/* SPDX-License-Identifier: GPL-2.0 */
/* Actual non-test DLL lifecycle/queue, SYS receive/return/fault/query/free.
 * REGISTER constructs a ready kernel peer; idle-only kernel workers, device
 * dispatch, NDIS and IRQL are hosted. DLL owns the shared region throughout. */
#ifdef PB_API_TESTS
#error Native reclamation requires shipping DLL state and lifecycle
#endif
#include "kernel_nbl_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping source fingerprint required");
typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define Executive 0
#define KernelMode 0
#define NDIS_STATUS_PAUSED 1u
static KIRQL ExAcquireSpinLockShared(SRWLOCK *l) {AcquireSRWLockShared(l);return 0;}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *l) {AcquireSRWLockExclusive(l);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *l,KIRQL saved) {CHECK(!saved);ReleaseSRWLockShared(l);}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *l,KIRQL saved) {CHECK(!saved);ReleaseSRWLockExclusive(l);}
static ULONG64 KeQueryInterruptTime(VOID) {return GetTickCount64()*10000u;}
#include "../../driver/v2/session_fault_drain.h"
#include "../../driver/v2/session_query.h"
#include "../../driver/v2/session_drain.h"
static NET_BUFFER_LIST *Indicated[PB_MAX_QUEUES];
static ULONG PbEffectiveMask(TUN_CTX *ctx) {(void)ctx;return 0;}
static ULONG PbBeginIndication(TUN_CTX *ctx) {(void)ctx;return 0;}
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) {(void)ctx;CHECK(!bank);}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *lists,ULONG count,ULONG flags)
{
    CHECK(adapter&&lists&&count==2&&!flags);PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(lists)&~(ULONG_PTR)1);
    CHECK(!Indicated[r->Queue->Index]);Indicated[r->Queue->Index]=lists;
}
#include "../../driver/v2/session_receive.h"
static TUN_CTX Context;
static PB_KERNEL_SESSION *Native;
static volatile LONG DllAllocations,Regions,DllEvents,Modules,Works,WorkPools,Devices;
static volatile LONG NativeObjects,NativeWorkers,LockedPages,OtherHandles;
static HANDLE CallbackThreads[32];
static volatile LONG CallbackThreadCount;
static PTP_WORK_CALLBACK WorkCallback;
static volatile LONG QueryCalls,LastQueryState,LastQueryReferences,CloseCalls,RundownWaits,UnsafeObserved;
static ULONG EventOrdinal;
static HANDLE Device,Progress,ParkEntered,ParkGate,TailEntered,TailGate,RundownEntered,UnsafeEntered,UnsafeRescue;
static BOOL ManualEvents,WrapCursors,GateTail;
static ULONG Scenarios;
static HANDLE Event(BOOL manual,BOOL initial)
{HANDLE h=CreateEventW(NULL,manual,initial,NULL);CHECK(h);InterlockedIncrement(&OtherHandles);return h;}
static VOID CloseOther(HANDLE h) {CHECK(h&&CloseHandle(h)&&InterlockedDecrement(&OtherHandles)>=0);}
static VOID Await(HANDLE h) {CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{CHECK(!reason&&!mode&&!alertable&&!timeout);Await(event->Native);}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{CHECK(thread&&!alertable&&!timeout);Await(thread);}
static VOID ZwClose(HANDLE thread) {CHECK(CloseHandle(thread)&&InterlockedDecrement(&NativeWorkers)>=0);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    CHECK(Native&&refs==&Native->Returns&&!Native->Active);InterlockedIncrement(&RundownWaits);
    if(!TryAcquireSRWLockExclusive(&refs->Lock)){CHECK(SetEvent(RundownEntered));AcquireSRWLockExclusive(&refs->Lock);}
    CHECK(!refs->References);ReleaseSRWLockExclusive(&refs->Lock);
}
static VOID ObDereferenceObject(KEVENT *event)
{
    CHECK(Native&&!Native->Active);
    if(ReadAcquire(&Native->Returns.References)){
        CHECK(!InterlockedExchange(&UnsafeObserved,1)&&SetEvent(UnsafeEntered));Await(UnsafeRescue);
    }
    CHECK(!Native->Returns.References&&event&&CloseHandle(event->Native)&&InterlockedDecrement(&NativeObjects)>=0);free(event);
}
static VOID MmUnlockPages(MDL *mdl)
{
    CHECK(Native&&mdl==Native->Mdl&&Native->PagesLocked&&!Native->Active&&!Native->Returns.References&&LockedPages==1);
    /* MmUnlockPages releases the SYS lock/map ownership, never DLL VirtualAlloc. */
    mdl->Address=NULL;InterlockedDecrement(&LockedPages);
}
#ifndef PB_NATIVE_RECLAIM_CLEANUP_SOURCE
#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "../../driver/v2/session_cleanup.h"
#endif
#include PB_NATIVE_RECLAIM_CLEANUP_SOURCE
static VOID PbReportOffload(TUN_CTX *ctx) {CHECK(ctx==&Context);}
#include "../../driver/v2/session_unregister.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/adapter.h"
static volatile LONG UnexpectedAdapter=1;
PB_RESULT WINAPI PbAdapterCreate(const wchar_t *a,const wchar_t *b,PB_ADAPTER **out)
{(void)a;(void)b;(void)out;CHECK(!UnexpectedAdapter);return PB_UNSUPPORTED;}
VOID WINAPI PbAdapterRelease(PB_ADAPTER *adapter) {CHECK(!adapter);}
VOID PbAdapterRetain(PB_ADAPTER *adapter) {(void)adapter;CHECK(!UnexpectedAdapter);}
const wchar_t *PbAdapterPath(PB_ADAPTER *adapter) {(void)adapter;CHECK(!UnexpectedAdapter);return NULL;}
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter,uint64_t *luid) {(void)adapter;(void)luid;CHECK(!UnexpectedAdapter);return PB_UNSUPPORTED;}
static VOID *TrackedCalloc(SIZE_T count,SIZE_T size)
{
    CHECK(!size||count<=SIZE_MAX/size);
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);
#else
    VOID *p=calloc(count,size);
#endif
    CHECK(p);InterlockedIncrement(&DllAllocations);return p;
}
static VOID TrackedFree(VOID *p)
{
    if(!p)return;CHECK(InterlockedDecrement(&DllAllocations)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID *WINAPI TrackedRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{
    CHECK(!address&&type==(MEM_RESERVE|MEM_COMMIT)&&protection==PAGE_READWRITE);
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(size);memset(p,0,size);
#else
    VOID *p=VirtualAlloc(NULL,size,type,protection);
#endif
    CHECK(p);InterlockedIncrement(&Regions);return p;
}
static BOOL WINAPI TrackedUnmap(VOID *p,SIZE_T size,DWORD type)
{
    CHECK(p&&!size&&type==MEM_RELEASE&&!Native&&!NativeObjects&&!NativeWorkers&&!LockedPages&&!Pools&&!Mdls&&!Nbls);
    CHECK(InterlockedDecrement(&Regions)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);return TRUE;
#else
    return VirtualFree(p,0,type);
#endif
}
static HANDLE WINAPI TrackedEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name)
{if(++EventOrdinal>2)manual=ManualEvents;HANDLE h=CreateEventW(attributes,manual,initial,name);CHECK(h);InterlockedIncrement(&DllEvents);return h;}
static BOOL WINAPI TrackedModule(DWORD flags,LPCWSTR address,HMODULE *module)
{BOOL ok=GetModuleHandleExW(flags,address,module);CHECK(ok);InterlockedIncrement(&Modules);return ok;}
static BOOL WINAPI TrackedLibrary(HMODULE module) {CHECK(InterlockedDecrement(&Modules)>=0);return FreeLibrary(module);}
static VOID WINAPI TrackedTail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{CHECK(InterlockedDecrement(&Modules)>=0);FreeLibraryWhenCallbackReturns(instance,module);}
static PTP_POOL WINAPI TrackedPool(VOID *reserved)
{PTP_POOL p=CreateThreadpool(reserved);CHECK(p);InterlockedIncrement(&WorkPools);return p;}
static VOID WINAPI TrackedPoolClose(PTP_POOL p) {CHECK(InterlockedDecrement(&WorkPools)>=0);CloseThreadpool(p);}
static void CALLBACK LifecycleWorker(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
static VOID CALLBACK DispatchWork(PTP_CALLBACK_INSTANCE instance,PVOID context,PTP_WORK work)
{
    LONG index=InterlockedIncrement(&CallbackThreadCount)-1;CHECK(index>=0&&index<32);
    CHECK(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),CallbackThreads+index,SYNCHRONIZE,FALSE,0));
    InterlockedIncrement(&OtherHandles);WorkCallback(instance,context,work);
}
static PTP_WORK WINAPI TrackedWork(PTP_WORK_CALLBACK callback,VOID *context,PTP_CALLBACK_ENVIRON environment)
{CHECK(callback==LifecycleWorker&&!WorkCallback&&!CallbackThreadCount);WorkCallback=callback;
    PTP_WORK w=CreateThreadpoolWork(DispatchWork,context,environment);CHECK(w);InterlockedIncrement(&Works);return w;}
static VOID WINAPI TrackedWorkClose(PTP_WORK w) {CHECK(InterlockedDecrement(&Works)>=0);CloseThreadpoolWork(w);}
static HANDLE WINAPI HostedFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL WINAPI HostedIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static BOOL WINAPI HostedClose(HANDLE);
static DWORD WINAPI GatedWait(HANDLE,DWORD);
#define calloc TrackedCalloc
#define free TrackedFree
#define VirtualAlloc TrackedRegion
#define VirtualFree TrackedUnmap
#define CreateEventW TrackedEvent
#define CloseHandle HostedClose
#define GetModuleHandleExW TrackedModule
#define FreeLibrary TrackedLibrary
#define FreeLibraryWhenCallbackReturns TrackedTail
#define CreateThreadpool TrackedPool
#define CloseThreadpool TrackedPoolClose
#define CreateThreadpoolWork TrackedWork
#define CloseThreadpoolWork TrackedWorkClose
#define CreateFileW HostedFile
#define DeviceIoControl HostedIo
#define WaitForSingleObject GatedWait
#include "../../api/v2/session.c"
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CloseHandle
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef CreateFileW
#undef DeviceIoControl
#undef WaitForSingleObject
static PB_SESSION *User;
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static KEVENT *DuplicateEvent(HANDLE source)
{
    KEVENT *event=malloc(sizeof(*event));CHECK(event&&DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&event->Native,0,FALSE,DUPLICATE_SAME_ACCESS));
    InterlockedIncrement(&NativeObjects);return event;
}
static DWORD WINAPI IdleWorker(VOID *argument)
{PB_KERNEL_QUEUE *q=argument;Await(q->Session->StartEvent.Native);Await(q->Session->StopEvent.Native);return 0;}
static VOID NativeFreed(VOID *pointer)
{
    if(pointer!=Native)return;
    CHECK(!Pools&&!Mdls&&!Nbls&&!NativeObjects&&!NativeWorkers&&!LockedPages);
    CloseOther(Native->StartEvent.Native);CloseOther(Native->StopEvent.Native);CloseOther(Native->AllReturned.Native);Native=NULL;
}
static VOID ReadyPeer(const PB_REGISTER_V2 *r)
{
    CHECK(!Native&&!Context.V2&&r->Size==sizeof(*r)&&!strcmp(r->SourceFingerprint,PB_SOURCE_STRING));
    Native=ExAllocatePool2(0,sizeof(*Native),0);CHECK(Native);PB_KERNEL_SESSION *s=Native;s->Ctx=&Context;s->Epoch=r->Epoch;
    CHECK(PbCalculateLayout(r->QueueCount,r->SlotCount,r->RingCapacity,&s->Layout)==PB_OK&&s->Layout.RegionSize==r->RegionSize);
    s->Region=(UCHAR *)(uintptr_t)r->RegionAddress;s->Lifecycle=(PB_SHARED_LIFECYCLE *)(s->Region+s->Layout.LifecycleOffset);
    s->Mdl=IoAllocateMdl(s->Region,s->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(s->Mdl);s->PagesLocked=TRUE;InterlockedIncrement(&LockedPages);
    s->ProgressEvent=DuplicateEvent((HANDLE)(uintptr_t)r->ProgressEvent);Progress=(HANDLE)(uintptr_t)r->ProgressEvent;
    s->StartEvent.Native=Event(TRUE,FALSE);s->StopEvent.Native=Event(TRUE,FALSE);s->AllReturned.Native=Event(TRUE,TRUE);
    for(ULONG i=0;i<s->Layout.QueueCount;++i){PB_KERNEL_QUEUE *q=s->Queues+i;q->Session=s;q->Index=i;
        q->TxSlots=ExAllocatePool2(0,s->Layout.SlotCount*sizeof(*q->TxSlots),0);q->RxSlots=ExAllocatePool2(0,s->Layout.SlotCount*sizeof(*q->RxSlots),0);
        q->Pending=ExAllocatePool2(0,s->Layout.RingCapacity*sizeof(*q->Pending),0);CHECK(q->TxSlots&&q->RxSlots&&q->Pending);
        CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,s->Layout.SlotCount,s->Epoch,(USHORT)i)==PB_OK);
        q->UserEvent=DuplicateEvent((HANDLE)(uintptr_t)r->UserEvents[i]);q->KernelEvent=DuplicateEvent((HANDLE)(uintptr_t)r->KernelEvents[i]);
        for(ULONG ring=0;ring<PB_QUEUE_COUNT;++ring)CHECK(PbAttachQueue(q->Rings+ring,s->Region,&s->Layout,i,ring,ring==PB_QUEUE_TX_READY||ring==PB_QUEUE_RX_COMPLETE)==PB_OK);
        q->Thread=CreateThread(NULL,0,IdleWorker,q,0,NULL);CHECK(q->Thread);InterlockedIncrement(&NativeWorkers);
    }
    Context.V2=s;Context.Probe.V2SessionActive=1;CHECK(SetEvent(s->StartEvent.Native));
}
static HANDLE WINAPI HostedFile(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD create,DWORD flags,HANDLE template)
{
    CHECK(path&&access==(GENERIC_READ|GENERIC_WRITE)&&sharing==(FILE_SHARE_READ|FILE_SHARE_WRITE)&&!security&&create==OPEN_EXISTING&&!flags&&!template&&!Device);
    Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);InterlockedIncrement(&Devices);return Device;
}
static BOOL WINAPI HostedIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    CHECK(device==Device&&Device&&!overlapped&&returned);*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY){CHECK(!input&&!inputSize&&outputSize==sizeof(PB_PROBE_STATUS));PB_PROBE_STATUS *p=output;memset(p,0,sizeof(*p));
        p->Size=sizeof(*p);p->Magic=PB_PROBE_MAGIC;p->Schema=PB_PROBE_SCHEMA;p->DataAbi=PB_ABI_MAJOR;p->FileVersion=PB_VERSION_PACKED;
        memcpy(p->SourceFingerprint,PB_SOURCE_STRING,65);*returned=sizeof(*p);return TRUE;}
    if(code==PB_V2_IOCTL_REGISTER){CHECK(inputSize==sizeof(PB_REGISTER_V2)&&!output&&!outputSize);ReadyPeer(input);return TRUE;}
    CHECK(Native&&Context.V2==Native);
    if(code==PB_V2_IOCTL_DRAIN){CHECK(!input&&!inputSize&&!output&&!outputSize);KIRQL irql=ExAcquireSpinLockExclusive(&Context.TransitionLock);
        PbV2BeginDrain(Native,GetTickCount64()+30000);WriteRelease(&Context.Probe.V2SessionActive,0);ExReleaseSpinLockExclusive(&Context.TransitionLock,irql);
        PbV2Progress(Native,TRUE);PbV2Wake(Native);return TRUE;}
    CHECK(code==PB_V2_IOCTL_QUERY&&!input&&!inputSize&&outputSize==sizeof(PB_V2_STATUS));
    CHECK(PbV2Query(&Context,output)==STATUS_SUCCESS);PB_V2_STATUS *status=output;
    InterlockedExchange(&LastQueryState,(LONG)status->State);InterlockedExchange(&LastQueryReferences,(LONG)status->ActiveReferences);InterlockedIncrement(&QueryCalls);
    *returned=sizeof(*status);return TRUE;
}
static BOOL WINAPI HostedClose(HANDLE handle)
{
    if(handle==Device){CHECK(Native&&User&&!Loans(User));InterlockedIncrement(&CloseCalls);
        AcquireSRWLockExclusive(&Context.Device.RegistrationLock);PbV2Unregister(&Context);ReleaseSRWLockExclusive(&Context.Device.RegistrationLock);
        CHECK(!Native&&CloseHandle(Device)&&InterlockedDecrement(&Devices)==0);Device=NULL;return TRUE;}
    CHECK(CloseHandle(handle)&&InterlockedDecrement(&DllEvents)>=0);return TRUE;
}
static DWORD WINAPI GatedWait(HANDLE event,DWORD timeout)
{
    if(event==Progress&&timeout==1000){CHECK(SetEvent(ParkEntered));Await(ParkGate);}
    return WaitForSingleObject(event,timeout);
}
static HANDLE Start(LPTHREAD_START_ROUTINE entry,VOID *argument)
{HANDLE h=CreateThread(NULL,0,entry,argument,0,NULL);CHECK(h);InterlockedIncrement(&OtherHandles);return h;}
static VOID Join(HANDLE h) {Await(h);CloseOther(h);}
static DWORD WINAPI Submit(VOID *unused)
{
    (void)unused;
    for(ULONG i=0;i<User->Layout.QueueCount;++i){PB_USER_QUEUE *q;PB_LEASE leases[2];uint32_t n;
        CHECK(Api.OpenQueue(User,i,&q)==PB_OK&&Api.ReserveBatch(q,leases,2,&n)==PB_OK&&n==2);
        for(ULONG j=0;j<2;++j){leases[j].Descriptor.Length=20;memset(leases[j].Data,0,20);leases[j].Data[0]=0x45;leases[j].Data[3]=20;}
        CHECK(Api.CommitBatch(q,leases,2,&n)==PB_OK&&n==2&&!q->Loans&&q->Published==2);
    }
    return 0;
}
static DWORD WINAPI Returning(VOID *argument)
{
    NET_BUFFER_LIST *lists=argument;
    CHECK(PbV2ReturnNbls(&Context,&lists,PB_MAX_BATCH)&&!lists);return 0;
}
static VOID ReturnTail(EX_RUNDOWN_REF *refs)
{
    if(!GateTail)return;
    CHECK(Native&&refs==&Native->Returns&&refs->References==1&&!Native->Active&&!Nbls&&Mdls==1);
    CHECK(SetEvent(TailEntered));Await(TailGate);
}
static VOID Parked(VOID)
{Await(ParkEntered);CHECK(QueryCalls&&LastQueryState!=PB_SESSION_STOPPED&&ResetEvent(Progress));}
static VOID Advance(VOID)
{CHECK(SetEvent(Progress)&&SetEvent(ParkGate));Parked();}
static ULONG PublishedCount(VOID)
{ULONG count=0;AcquireSRWLockShared(&User->DataGate);for(ULONG i=0;i<User->Layout.QueueCount;++i)count+=(ULONG)User->Queues[i].Published;ReleaseSRWLockShared(&User->DataGate);return count;}
static VOID Retained(ULONG published)
{
    CHECK(User&&Native&&User->Region==Native->Region&&Regions==1&&LockedPages==1&&!CloseCalls&&!User->ResourcesClosed&&PublishedCount()==published);
    AcquireSRWLockShared(&User->State);CHECK(User->Progress.Phase<PB_LIFE_PREPARED);ReleaseSRWLockShared(&User->State);
}
static VOID Begin(ULONG queues,BOOL manual,BOOL wrap)
{
    CHECK(!User&&!Native&&!Context.V2&&!DllAllocations&&!Regions&&!DllEvents&&!Modules&&!Works&&!WorkPools&&!Devices&&
        !NativeObjects&&!NativeWorkers&&!LockedPages&&!OtherHandles&&!CallbackThreadCount&&!WorkCallback&&!Pools&&!Mdls&&!Nbls&&!RuntimeCount&&!RuntimeBytes&&!RuntimeAdapterCount);
    memset(&Context,0,sizeof(Context));memset(Indicated,0,sizeof(Indicated));ManualEvents=manual;WrapCursors=wrap;GateTail=FALSE;EventOrdinal=0;
    Context.Running=1;Context.NblPool=Context.MiniportAdapterHandle=&Context;Context.Device.Disconnected.Native=Event(TRUE,FALSE);
    QueryCalls=LastQueryState=LastQueryReferences=CloseCalls=RundownWaits=UnsafeObserved=0;Progress=Device=NULL;
    Failure=Acquisitions=0;BeforePoolFree=NativeFreed;BeforeRundownRelease=ReturnTail;
    ParkEntered=Event(FALSE,FALSE);ParkGate=Event(FALSE,FALSE);TailEntered=Event(TRUE,FALSE);TailGate=Event(TRUE,FALSE);
    RundownEntered=Event(TRUE,FALSE);UnsafeEntered=Event(TRUE,FALSE);UnsafeRescue=Event(TRUE,FALSE);
    PB_CONFIG config={sizeof(config),queues,4,4,0,0};CHECK(Api.Open(L"hosted-native-reclaim",&config,&User)==PB_OK);Retain(User);
    for(ULONG i=0;i<queues;++i)for(ULONG ring=PB_QUEUE_RX_SUBMIT;ring<=PB_QUEUE_RX_COMPLETE;++ring){
        PB_QUEUE_VIEW *u=User->Queues[i].Rings+ring,*n=Native->Queues[i].Rings+ring;uint64_t base=WrapCursors?UINT64_MAX-1:0;
        u->Cursor=u->PeerCursor=n->Cursor=n->PeerCursor=u->Shared->Producer=u->Shared->Consumer=base;
    }
    /* This is the only public queue owner. It has ended and its handle is
     * joined before PrepareClose/Close; lifecycle polls under DataGate. */
    Join(Start(Submit,NULL));
    for(ULONG i=0;i<queues;++i)CHECK(PbV2ReceiveBatch(Native->Queues+i)&&Indicated[i]);
    CHECK(Native->Active==(LONG)(2*queues)&&Native->RxObjects==2*queues&&Nbls==(LONG)(2*queues));
    CHECK(Api.PrepareClose(User,0)==PB_QUARANTINED);Parked();Retained(2*queues);
}
static VOID End(VOID)
{
    CHECK(!Native&&!Context.V2&&!NativeObjects&&!NativeWorkers&&!LockedPages&&!Pools&&!Mdls&&!Nbls&&CloseCalls==1&&Devices==0&&User->ResourcesClosed&&!User->Region);
    /* Internal retained core protects these observations after public calls
     * have stopped. No public progress/status call runs during final Close. */
    CHECK(Api.Close(User,2000)==PB_OK);DropReference(User);User=NULL;Progress=NULL;
    /* ClosePool has retired the private pool. Callback completion alone is not
     * OS-thread termination: join each retained thread handle individually,
     * including duplicates when two callbacks used the same worker thread. */
    CHECK(CallbackThreadCount>0);for(LONG i=0;i<CallbackThreadCount;++i){Join(CallbackThreads[i]);CallbackThreads[i]=NULL;}
    CallbackThreadCount=0;WorkCallback=NULL;
    CloseOther(Context.Device.Disconnected.Native);Context.Device.Disconnected.Native=NULL;
    CloseOther(ParkEntered);CloseOther(ParkGate);CloseOther(TailEntered);CloseOther(TailGate);CloseOther(RundownEntered);CloseOther(UnsafeEntered);CloseOther(UnsafeRescue);
    ParkEntered=ParkGate=TailEntered=TailGate=RundownEntered=UnsafeEntered=UnsafeRescue=NULL;
    BeforePoolFree=NULL;BeforeRundownRelease=NULL;GateTail=FALSE;
    CHECK(!DllAllocations&&!Regions&&!DllEvents&&!Modules&&!Works&&!WorkPools&&!OtherHandles&&!RuntimeCount&&!RuntimeBytes&&!RuntimeAdapterCount);
}
static VOID Run(ULONG queues,ULONG mode,BOOL manual,BOOL wrap,ULONG lastQueue,BOOL control)
{
    Begin(queues,manual,wrap);BOOL fault=mode!=0;
    if(fault){PbV2Fault(Native);
        if(mode==2)Native->Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Shared->Producer=(wrap?UINT64_MAX-1:0)+5;
        for(ULONG i=0;i<queues;++i)if(i!=lastQueue)PbV2QuiesceFaultedQueue(Native->Queues+i);
        Advance();Retained(2*queues);CHECK(!Native->Queues[lastQueue].FaultQuiesced);
    }
    NET_BUFFER_LIST *last=NULL;
    for(ULONG i=0;i<queues;++i){NET_BUFFER_LIST *head=Indicated[i];Indicated[i]=NULL;CHECK(head&&head->Next&&!head->Next->Next);
        if(i==lastQueue){last=head->Next;head->Next=NULL;}
        Join(Start(Returning,head));
    }
    CHECK(last&&Native->Active==1);Advance();Retained(fault?2*queues:1);
    GateTail=TRUE;HANDLE callback=Start(Returning,last);Await(TailEntered);
    CHECK(!Native->Active&&Native->Returns.References==1&&Native->RxReturned==2*queues);
    if(fault){Advance();Retained(2*queues);CHECK(!LastQueryReferences&&!Native->Queues[lastQueue].FaultQuiesced);
        PbV2QuiesceFaultedQueue(Native->Queues+lastQueue);CHECK(Native->Queues[lastQueue].FaultQuiesced);}
    CHECK(SetEvent(Progress)&&SetEvent(ParkGate));CHECK(Api.PrepareClose(User,2000)==PB_OK);
    CHECK(LastQueryState==PB_SESSION_STOPPED&&!LastQueryReferences&&!PublishedCount()&&Native->Returns.References==1&&Regions==1&&LockedPages==1&&!CloseCalls);
    CHECK(Native->Completions==(fault?0:2*queues));
    for(ULONG i=0;i<queues;++i){CHECK(User->Queues[i].Rings[PB_QUEUE_RX_SUBMIT].Cursor==(wrap?0:2));
        if(!fault)CHECK(User->Queues[i].Rings[PB_QUEUE_RX_COMPLETE].Cursor==(wrap?0:2));}
    /* Other public queue/control calls are quiescent. Close(0) can return
     * while the independently owned lifecycle waits for native Returns. */
    CHECK(Api.Close(User,0)==PB_QUARANTINED);
    HANDLE waits[]={RundownEntered,UnsafeEntered};DWORD observed=WaitForMultipleObjects(2,waits,FALSE,5000);CHECK(observed<WAIT_OBJECT_0+2);
    CHECK(Native&&Native->Returns.References==1&&!Native->Active&&NativeObjects&&LockedPages==1&&Regions==1&&!User->ResourcesClosed&&User->Region);
    AcquireSRWLockShared(&User->State);CHECK(User->Progress.Phase==PB_LIFE_RELEASING&&User->Progress.BlockStage==PB_BLOCK_DEVICE_CLOSE);ReleaseSRWLockShared(&User->State);
    BOOL missing=observed==WAIT_OBJECT_0+1;CHECK(!missing||control);CHECK(missing?(!RundownWaits&&UnsafeObserved):(RundownWaits==1&&!UnsafeObserved));
    CHECK(SetEvent(TailGate));Join(callback);GateTail=FALSE;
    if(missing)CHECK(SetEvent(UnsafeRescue));
    WaitForThreadpoolWorkCallbacks(User->Work,FALSE);End();Scenarios++;
}
int main(int argc,char **argv)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK&&PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    if(argc==2){CHECK(!strcmp(argv[1],"1"));Run(2,1,FALSE,TRUE,1,TRUE);
        if(UnsafeObserved){puts("NATIVE_CLOSE_SKIPPED_RETURNS_BARRIER: pre-free observer parked, return rescued/joined, every resource reclaimed");return 109;}
        puts("PASS: native Close Returns barrier control; callback joined, every resource reclaimed.");return 0;}
    CHECK(argc==1);Run(8,2,TRUE,TRUE,7,FALSE);Run(8,0,TRUE,TRUE,7,FALSE);Scenarios=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(ULONG qi=0;qi<3;++qi)for(ULONG mode=0;mode<3;++mode)for(ULONG manual=0;manual<2;++manual)for(ULONG wrap=0;wrap<2;++wrap){ULONG queues=qi==0?1:qi==1?2:8;
        Run(queues,mode,(BOOL)manual,(BOOL)wrap,0,FALSE);if(queues>1)Run(queues,mode,(BOOL)manual,(BOOL)wrap,queues-1,FALSE);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
    if(before!=after)fprintf(stderr,"Process handles before=%lu after=%lu; fixture counters=%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld\n",before,after,OtherHandles,DllEvents,Devices,NativeWorkers,NativeObjects,Works,WorkPools,Modules,Regions);
    CHECK(before==after);
    printf("PASS: %lu actual non-test DLL/SYS native reclamation schedules: 1/2/8 queues, drain/fault/corrupt completion, private witnesses, Active/Returns separation, real region ownership and joined owners/callbacks/workers; tracked heap/region/object/work/pool/module/handle counts zero, process handles stable (%lu).\n",Scenarios,after);return 0;
}
