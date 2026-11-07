/* SPDX-License-Identifier: GPL-2.0 */
/* Actual PbV2Register/Free/Unregister with hosted DDIs. Caller event handles are
 * real Windows handles, including real DuplicateHandle aliases. No native
 * device, page locking, kernel worker, NDIS or IRQL execution is claimed. */
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PB_GUARD_TESTS
#include "../safety/guard_memory.h"
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Register tests require the shipping 64-character source fingerprint");
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
#define NT_ASSERT CHECK
#define MAXULONG MAXDWORD
static VOID InitializeListHead(LIST_ENTRY *head) {head->Flink=head->Blink=head;}
static BOOLEAN IsListEmpty(const LIST_ENTRY *head) {return head->Flink==head;}
static VOID InsertTailList(LIST_ENTRY *head,LIST_ENTRY *entry)
{entry->Flink=head;entry->Blink=head->Blink;head->Blink->Flink=entry;head->Blink=entry;}
static VOID RemoveEntryList(LIST_ENTRY *entry) {entry->Blink->Flink=entry->Flink;entry->Flink->Blink=entry->Blink;}
typedef struct EVENT_OBJECT {HANDLE Native;LONG References;} KEVENT,*PKEVENT;
typedef struct MDL {VOID *Address;ULONG Length;BOOLEAN Locked,Mapped;} MDL;
typedef struct {SRWLOCK Lock;} EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct FILE_OBJECT {ULONG Value;} FILE_OBJECT;
typedef struct _TUN_CTX {
    SRWLOCK TransitionLock;
    PB_KERNEL_SESSION *V2;
    struct {SRWLOCK RegistrationLock;FILE_OBJECT *OwningFileObject;HANDLE OwningProcessId;LIST_ENTRY Entry;KEVENT Disconnected;} Device;
    struct {volatile LONG V2SessionActive,AllowedMask;} Probe;
    VOID *MiniportAdapterHandle;
} TUN_CTX;
typedef struct {FILE_OBJECT *FileObject;struct {struct {ULONG InputBufferLength;} DeviceIoControl;} Parameters;} IO_STACK_LOCATION;
typedef struct {BOOLEAN Is32;UCHAR RequestorMode;struct {VOID *SystemBuffer;} AssociatedIrp;IO_STACK_LOCATION Stack;} IRP;
typedef UCHAR KIRQL;
typedef struct {SRWLOCK *Lock;} KLOCK_QUEUE_HANDLE;
#define NonPagedPool 0
#define TUN_MEMORY_TAG 0
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
#define UserMode 1
#define NotificationEvent 0
#define IoWriteAccess 1
#define MediaConnectStateConnected 1
#define try __try
#define except __except
static PB_KERNEL_SESSION *Current;
static TUN_CTX Context;
static FILE_OBJECT Owner={17},OtherOwner={19};
static SRWLOCK TunDispatchDeviceListLock;
static LIST_ENTRY TunDispatchDeviceList;
static HANDLE ProcessId;
static volatile LONG Handles,Pools,Mdls,Locked,Mapped,References,WorkerHandles,Workers;
static ULONG PoolCalls,MdlCalls,ProbeCalls,MapCalls,ObCalls,ThreadCalls,ThreadsMade,ThreadsExited;
static ULONG FailPool,FailMdl,FailOb,FailThread,Connected,Reports,Starts,StartBeforePublish,Scenarios;
static NTSTATUS ProbeError,ObError,ThreadError;
static BOOLEAN FailMap,EverPublished;
static __declspec(thread) BOOLEAN ResourceHeld;
static ULONG CleanupJoins,PlainChecks,Retries;
static PB_REGISTER_V2 Registration;
static IRP Request;
static PB_LAYOUT Layout;
static UCHAR *Region;
static struct {HANDLE Handle;PKEVENT Object;} HandleTable[2*(1+2*PB_MAX_QUEUES)];
static PKEVENT CallerObjects[1+2*PB_MAX_QUEUES];
static ULONG HandleCount,ObjectCount;

static VOID *HostAlloc(SIZE_T bytes)
{
#ifdef PB_GUARD_TESTS
    VOID *memory=PbTestGuardAlloc(bytes);memset(memory,0,bytes);
#else
    VOID *memory=calloc(1,bytes);
#endif
    CHECK(memory);return memory;
}
static VOID HostFree(VOID *memory)
{
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(memory);
#else
    free(memory);
#endif
}
static HANDLE Event(BOOLEAN manual,BOOLEAN signaled)
{HANDLE handle=CreateEventW(NULL,manual,signaled,NULL);CHECK(handle);InterlockedIncrement(&Handles);return handle;}
static VOID Close(HANDLE handle) {CHECK(CloseHandle(handle)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE handle) {CHECK(WaitForSingleObject(handle,10000)==WAIT_OBJECT_0);}
static VOID *ExAllocatePoolZero(ULONG kind,SIZE_T bytes,ULONG tag)
{
    CHECK(!kind&&!tag);++PoolCalls;if(PoolCalls==FailPool)return NULL;
    VOID *memory=HostAlloc(bytes);InterlockedIncrement(&Pools);
    if(PoolCalls==1){CHECK(bytes==sizeof(PB_KERNEL_SESSION)&&!Current);Current=memory;}
    return memory;
}
static VOID ExFreePoolWithTag(VOID *memory,ULONG tag)
{
    CHECK(memory&&!tag&&Pools>0);
    if(memory==Current){
        CHECK(!Workers&&!WorkerHandles&&!References&&!Mdls&&!Locked&&!Mapped&&Pools==1);
        Close(Current->StartEvent.Native);Close(Current->StopEvent.Native);Close(Current->AllReturned.Native);Current=NULL;
    }
    InterlockedDecrement(&Pools);HostFree(memory);
}
static VOID KeInitializeEvent(KEVENT *event,ULONG kind,BOOLEAN signaled)
{CHECK(kind==NotificationEvent&&!event->Native);event->Native=Event(TRUE,signaled);}
static VOID KeSetEvent(KEVENT *event,ULONG priority,BOOLEAN wait)
{
    CHECK(event&&event->Native&&!priority&&!wait);
    if(Current&&event==&Current->StartEvent&&!Current->Stopping){
        ++Starts;
        if(Context.V2!=Current||Context.Device.OwningFileObject!=&Owner||Context.Device.OwningProcessId!=ProcessId||
            TunDispatchDeviceList.Flink!=&Context.Device.Entry||TunDispatchDeviceList.Blink!=&Context.Device.Entry||
            !Context.Probe.V2SessionActive||Context.Probe.AllowedMask||WaitForSingleObject(Context.Device.Disconnected.Native,0)!=WAIT_TIMEOUT)++StartBeforePublish;
        else EverPublished=TRUE;
    }
    CHECK(SetEvent(event->Native));
}
static VOID KeClearEvent(KEVENT *event) {CHECK(event&&ResetEvent(event->Native));}
static VOID KeInitializeSpinLock(KSPIN_LOCK *lock) {memset(lock,0,sizeof(*lock));}
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{handle->Lock=(SRWLOCK *)lock;AcquireSRWLockExclusive(handle->Lock);}
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) {ReleaseSRWLockExclusive(handle->Lock);}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock) {AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait)
{CHECK(wait);AcquireSRWLockExclusive(lock);if(lock==&Context.Device.RegistrationLock){CHECK(!ResourceHeld);ResourceHeld=TRUE;}}
static VOID ExReleaseResourceLite(SRWLOCK *lock)
{if(lock==&Context.Device.RegistrationLock){CHECK(ResourceHeld);ResourceHeld=FALSE;}ReleaseSRWLockExclusive(lock);}
static VOID ExInitializeRundownProtection(EX_RUNDOWN_REF *refs) {InitializeSRWLock(&refs->Lock);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{AcquireSRWLockExclusive(&refs->Lock);ReleaseSRWLockExclusive(&refs->Lock);}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{CHECK(!reason&&!mode&&!alertable&&!timeout);Await(event->Native);}
static MDL *IoAllocateMdl(VOID *address,ULONG length,BOOLEAN secondary,BOOLEAN quota,VOID *irp)
{
    CHECK(address==Region&&length==Layout.RegionSize&&!secondary&&!quota&&!irp);++MdlCalls;if(FailMdl)return NULL;
    MDL *mdl=HostAlloc(sizeof(*mdl));mdl->Address=address;mdl->Length=length;InterlockedIncrement(&Mdls);return mdl;
}
static VOID MmProbeAndLockPages(MDL *mdl,UCHAR mode,ULONG access)
{
    CHECK(mdl&&!mdl->Locked&&mode==UserMode&&access==IoWriteAccess);++ProbeCalls;
    if(ProbeError)RaiseException((DWORD)ProbeError,0,0,NULL);
    mdl->Locked=TRUE;InterlockedIncrement(&Locked);
}
static UCHAR *PbV2MapMdl(MDL *mdl)
{
    CHECK(mdl&&mdl->Locked&&!mdl->Mapped);++MapCalls;if(FailMap)return NULL;
    mdl->Mapped=TRUE;InterlockedIncrement(&Mapped);return mdl->Address;
}
static VOID MmUnlockPages(MDL *mdl)
{CHECK(mdl&&mdl->Locked);mdl->Locked=FALSE;InterlockedDecrement(&Locked);if(mdl->Mapped){mdl->Mapped=FALSE;InterlockedDecrement(&Mapped);}}
static VOID IoFreeMdl(MDL *mdl) {CHECK(mdl&&!mdl->Locked&&!mdl->Mapped);HostFree(mdl);InterlockedDecrement(&Mdls);}
static VOID *EventType=(VOID *)(ULONG_PTR)0x1234;
static VOID **ExEventObjectType=&EventType;
static NTSTATUS ObReferenceObjectByHandle(HANDLE handle,ACCESS_MASK access,VOID *type,UCHAR mode,PKEVENT *output,VOID *info)
{
    ++ObCalls;CHECK(type==EventType&&mode==UserMode&&output&&!*output&&!info);
    CHECK(access==(ACCESS_MASK)(ObCalls>1&&!(ObCalls&1)?SYNCHRONIZE|EVENT_MODIFY_STATE:EVENT_MODIFY_STATE));
    if(ObCalls==FailOb)return ObError;
    for(ULONG i=0;i<HandleCount;++i)if(HandleTable[i].Handle==handle){*output=HandleTable[i].Object;InterlockedIncrement(&(*output)->References);InterlockedIncrement(&References);return STATUS_SUCCESS;}
    return STATUS_INVALID_HANDLE;
}
static VOID ObDereferenceObject(PKEVENT event)
{CHECK(event&&event->References>0);InterlockedDecrement(&event->References);CHECK(InterlockedDecrement(&References)>=0);}
static VOID PbV2Worker(PVOID input)
{
    PB_KERNEL_QUEUE *queue=input;PB_KERNEL_SESSION *session=queue->Session;
    /* This stand-in performs no queue/data access. Start is its first wait;
     * rollback sets both events and joins it before freeing any queue/object. */
    Await(session->StartEvent.Native);Await(session->StopEvent.Native);CHECK(session->Stopping);
    InterlockedIncrement((LONG *)&ThreadsExited);InterlockedDecrement(&Workers);
}
typedef struct {VOID (*Routine)(PVOID);PVOID Input;} THREAD_START;
static DWORD WINAPI WorkerEntry(VOID *input)
{THREAD_START start=*(THREAD_START *)input;free(input);start.Routine(start.Input);return 0;}
static NTSTATUS PsCreateSystemThread(HANDLE *thread,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,HANDLE process,VOID *id,VOID (*routine)(PVOID),PVOID input)
{
    ++ThreadCalls;CHECK(thread&&!*thread&&access==THREAD_ALL_ACCESS&&attributes&&attributes->Attributes==OBJ_KERNEL_HANDLE&&!process&&!id&&routine==PbV2Worker);
    if(ThreadCalls==FailThread)return ThreadError;
    THREAD_START *start=malloc(sizeof(*start));CHECK(start);*start=(THREAD_START){routine,input};InterlockedIncrement(&Workers);
    *thread=CreateThread(NULL,0,WorkerEntry,start,0,NULL);CHECK(*thread);InterlockedIncrement(&Handles);InterlockedIncrement(&WorkerHandles);++ThreadsMade;return STATUS_SUCCESS;
}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout) {CHECK(!alertable&&!timeout);Await(thread);++CleanupJoins;}
static VOID ZwClose(HANDLE thread) {Close(thread);CHECK(InterlockedDecrement(&WorkerHandles)>=0);}
static HANDLE PsGetCurrentProcessId(VOID) {return ProcessId;}
static IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(IRP *irp) {CHECK(irp==&Request);return &irp->Stack;}
static BOOLEAN IoIs32bitProcess(IRP *irp) {CHECK(irp==&Request);return irp->Is32;}
static SIZE_T ComparePrefix(const VOID *left,const VOID *right,SIZE_T bytes)
{SIZE_T i=0;for(;i<bytes&&((const UCHAR *)left)[i]==((const UCHAR *)right)[i];++i){}return i;}
#define RtlCompareMemory ComparePrefix
static VOID PbReportOffload(TUN_CTX *ctx) {CHECK(ctx==&Context);++Reports;}
static VOID TunIndicateStatus(VOID *adapter,ULONG state)
{CHECK(adapter==Context.MiniportAdapterHandle&&state==MediaConnectStateConnected&&Context.V2==Current&&Context.Device.OwningFileObject==&Owner);++Connected;}
#include "../../driver/v2/session_fault.h"
#include "../../driver/v2/session_completion.h"
#include "../../driver/v2/session_recycle.h"
#include "../../driver/v2/session_cleanup.h"
#include "../../driver/v2/session_unregister.h"
#ifndef PB_REGISTER_SOURCE
#error Build with tools/build/kernel-register-tests.ps1 to extract actual PbV2Register
#endif
#include PB_REGISTER_SOURCE

static uint64_t *EventField(ULONG index)
{return !index?&Registration.ProgressEvent:index&1?Registration.KernelEvents+(index-1)/2:Registration.UserEvents+(index-2)/2;}
static VOID Begin(ULONG queues,BOOLEAN manual)
{
    CHECK(!Current&&!Pools&&!Mdls&&!Locked&&!Mapped&&!References&&!WorkerHandles&&!Workers&&!Handles&&!ObjectCount&&!HandleCount);
    memset(&Context,0,sizeof(Context));memset(&Registration,0,sizeof(Registration));memset(&Request,0,sizeof(Request));
    PoolCalls=MdlCalls=ProbeCalls=MapCalls=ObCalls=ThreadCalls=ThreadsMade=ThreadsExited=FailPool=FailMdl=FailOb=FailThread=Connected=Reports=Starts=StartBeforePublish=CleanupJoins=0;
    ProbeError=0;ObError=STATUS_ACCESS_DENIED;ThreadError=STATUS_INSUFFICIENT_RESOURCES;FailMap=EverPublished=FALSE;
    ProcessId=(HANDLE)(ULONG_PTR)GetCurrentProcessId();InitializeListHead(&TunDispatchDeviceList);Context.MiniportAdapterHandle=&Context;
    Context.Device.Disconnected.Native=Event(TRUE,TRUE);CHECK(PbCalculateLayout(queues,2,2,&Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    Region=PbTestGuardAlloc(Layout.RegionSize);
#else
    Region=_aligned_malloc(Layout.RegionSize,64);CHECK(Region);
#endif
    memset(Region,0,Layout.RegionSize);
    Registration.Size=sizeof(Registration);Registration.Major=PB_ABI_MAJOR;Registration.Minor=PB_ABI_MINOR;Registration.Epoch=42;
    Registration.RegionAddress=(uint64_t)(ULONG_PTR)Region;Registration.RegionSize=Layout.RegionSize;Registration.QueueCount=queues;Registration.SlotCount=2;Registration.RingCapacity=2;
    memcpy(Registration.SourceFingerprint,PB_SOURCE_STRING,sizeof(Registration.SourceFingerprint));
    for(ULONG i=0;i<1+2*queues;++i){
        PKEVENT object=calloc(1,sizeof(*object));CHECK(object);object->Native=Event(manual,FALSE);CallerObjects[ObjectCount++]=object;
        HandleTable[HandleCount].Handle=object->Native;HandleTable[HandleCount++].Object=object;*EventField(i)=(uint64_t)(ULONG_PTR)object->Native;
    }
    Request.RequestorMode=UserMode;Request.AssociatedIrp.SystemBuffer=&Registration;Request.Stack.FileObject=&Owner;Request.Stack.Parameters.DeviceIoControl.InputBufferLength=sizeof(Registration);
}
static VOID Alias(ULONG first,ULONG second,BOOLEAN duplicate)
{
    HANDLE original=(HANDLE)(ULONG_PTR)*EventField(first),alias=original;PKEVENT object=NULL;
    for(ULONG i=0;i<HandleCount;++i)if(HandleTable[i].Handle==original)object=HandleTable[i].Object;
    CHECK(object&&first<second);
    if(duplicate){CHECK(DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS));InterlockedIncrement(&Handles);CHECK(alias!=original);HandleTable[HandleCount].Handle=alias;HandleTable[HandleCount++].Object=object;}
    *EventField(second)=(uint64_t)(ULONG_PTR)alias;
}
static VOID Cleanup(VOID)
{
    if(Context.V2){
        CHECK(Context.V2==Current&&Context.Device.OwningFileObject==&Owner&&!IsListEmpty(&TunDispatchDeviceList));
        ExAcquireResourceExclusiveLite(&Context.Device.RegistrationLock,TRUE);
        Context.Device.OwningFileObject=NULL;RemoveEntryList(&Context.Device.Entry);PbV2Unregister(&Context);
        ExReleaseResourceLite(&Context.Device.RegistrationLock);
    }
    CHECK(!Current&&!Context.V2&&!Context.Device.OwningFileObject&&IsListEmpty(&TunDispatchDeviceList)&&!Pools&&!Mdls&&!Locked&&!Mapped&&!References&&!WorkerHandles&&!Workers&&ThreadsMade==ThreadsExited&&ThreadsMade==CleanupJoins);
    for(ULONG i=0;i<HandleCount;++i)Close(HandleTable[i].Handle);
    for(ULONG i=0;i<ObjectCount;++i){CHECK(!CallerObjects[i]->References);free(CallerObjects[i]);CallerObjects[i]=NULL;}
    HandleCount=ObjectCount=0;Close(Context.Device.Disconnected.Native);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(Region);
#else
    _aligned_free(Region);
#endif
    Region=NULL;CHECK(!Handles);++Scenarios;
}
static NTSTATUS Register(VOID) {return PbV2Register(&Context,&Request);}
static VOID CheckFailure(NTSTATUS expected)
{
    CHECK(Register()==expected&&!Current&&!Context.V2&&!Context.Device.OwningFileObject&&!Context.Device.OwningProcessId&&
        !Context.Probe.V2SessionActive&&!Context.Probe.AllowedMask&&IsListEmpty(&TunDispatchDeviceList)&&
        WaitForSingleObject(Context.Device.Disconnected.Native,0)==WAIT_OBJECT_0&&!Connected&&!Reports&&!Starts&&!EverPublished&&!StartBeforePublish);
}
static VOID PlainQueues(ULONG index)
{
    PB_KERNEL_QUEUE *q=Current->Queues+index;PB_TOKEN token;CHECK(PbPoolReserve(&q->TxPool,&token)==PB_OK);
    PB_DESCRIPTOR sent={.Token=token,.Length=20,.DataOffset=Layout.Queues[index].TxData+token.Slot*PB_BLOCK_SIZE};
    CHECK(PbPoolTransition(&q->TxPool,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    uint32_t count,notify;CHECK(PbQueuePush(q->Rings+PB_QUEUE_TX_READY,&sent,1,&count,&notify)==PB_OK&&count==1);
    PB_QUEUE_VIEW consumer,recycler;CHECK(PbAttachQueue(&consumer,Region,&Layout,index,PB_QUEUE_TX_READY,0)==PB_OK);
    PB_DESCRIPTOR output;CHECK(PbQueuePop(&consumer,&output,1,&count)==PB_OK&&count==1&&!memcmp(&output,&sent,sizeof(sent)));
    CHECK(PbAttachQueue(&recycler,Region,&Layout,index,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    CHECK(PbQueuePushTokens(&recycler,&token,1,&count,&notify)==PB_OK&&count==1);
    CHECK(PbV2RecycleLocked(q)==PB_OK&&q->TxSlots[token.Slot].State==PB_SLOT_FREE);
    PB_QUEUE_VIEW submit,complete;CHECK(PbAttachQueue(&submit,Region,&Layout,index,PB_QUEUE_RX_SUBMIT,1)==PB_OK);
    CHECK(PbAttachQueue(&complete,Region,&Layout,index,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    PB_DESCRIPTOR received={.Token={42,1,0,(USHORT)index},.Length=20,.DataOffset=Layout.Queues[index].RxData};
    CHECK(PbQueuePush(&submit,&received,1,&count,&notify)==PB_OK&&count==1);
    CHECK(PbQueuePop(q->Rings+PB_QUEUE_RX_SUBMIT,&output,1,&count)==PB_OK&&count==1);
    uint32_t needed;CHECK(PbAdmitReceive(&Layout,42,index,q->RxSlots,&output,FALSE,&needed)==PB_OK);q->Credits++;
    q->RxSlots[0].Active=FALSE;PbV2CompleteLocked(q,&output,PB_OK);PbV2FlushLocked(q);
    CHECK(PbQueuePop(&complete,&output,1,&count)==PB_OK&&count==1&&!output.Status&&!q->Credits&&!q->PendingCount);++PlainChecks;
}
static VOID RetryAfterFailure(VOID)
{
    CHECK(!Current&&!Pools&&!Mdls&&!Locked&&!Mapped&&!References&&!WorkerHandles&&!Workers&&ThreadsMade==ThreadsExited&&ThreadsMade==CleanupJoins);
    /* Keep this adapter, caller region and all caller handles. A new attempt
     * must not depend on constructing a fresh Ctx after failure rollback. */
    FailPool=FailMdl=FailOb=FailThread=0;ProbeError=0;FailMap=FALSE;
    PoolCalls=MdlCalls=ProbeCalls=MapCalls=ObCalls=ThreadCalls=ThreadsMade=ThreadsExited=CleanupJoins=0;
    CHECK(NT_SUCCESS(Register())&&Current&&Context.V2==Current&&Connected==1&&Starts==1&&EverPublished&&!StartBeforePublish);
    CHECK(!Current->Lifecycle->Terminal);++Retries;
}
static VOID Aliases(ULONG queues,BOOLEAN manual)
{
    ULONG objects=1+2*queues;
    for(ULONG first=0;first<objects;++first)for(ULONG second=first+1;second<objects;++second)for(ULONG duplicate=0;duplicate<2;++duplicate){
        Begin(queues,manual);Alias(first,second,(BOOLEAN)duplicate);CheckFailure(STATUS_INVALID_PARAMETER);CHECK(ObCalls==second+1+(second&1));
        *EventField(second)=(uint64_t)(ULONG_PTR)CallerObjects[second]->Native;RetryAfterFailure();Cleanup();
    }
}
static VOID Resources(ULONG queues,BOOLEAN manual)
{
    for(ULONG at=1;at<=1+6*queues;++at){
        Begin(queues,manual);FailPool=at;BOOLEAN optional=at>1&&(at-2)%6>=3;
        if(optional){CHECK(NT_SUCCESS(Register())&&Connected==1&&Starts==1&&EverPublished&&!StartBeforePublish);PlainQueues((at-2)/6);}
        else {CheckFailure(STATUS_INSUFFICIENT_RESOURCES);RetryAfterFailure();}Cleanup();
    }
    Begin(queues,manual);FailMdl=1;CheckFailure(STATUS_INSUFFICIENT_RESOURCES);CHECK(MdlCalls==1&&!ProbeCalls);RetryAfterFailure();Cleanup();
    const NTSTATUS probe[]={STATUS_ACCESS_VIOLATION,STATUS_INSUFFICIENT_RESOURCES};
    for(ULONG i=0;i<_countof(probe);++i){Begin(queues,manual);ProbeError=probe[i];CheckFailure(probe[i]);CHECK(ProbeCalls==1&&!MapCalls);RetryAfterFailure();Cleanup();}
    Begin(queues,manual);FailMap=TRUE;CheckFailure(STATUS_INSUFFICIENT_RESOURCES);CHECK(ProbeCalls==1&&MapCalls==1&&!ObCalls);RetryAfterFailure();Cleanup();
    const NTSTATUS errors[]={STATUS_INVALID_HANDLE,STATUS_ACCESS_DENIED,STATUS_OBJECT_TYPE_MISMATCH};
    for(ULONG at=1;at<=1+2*queues;++at)for(ULONG e=0;e<_countof(errors);++e){Begin(queues,manual);FailOb=at;ObError=errors[e];CheckFailure(ObError);CHECK(ObCalls==at);RetryAfterFailure();Cleanup();}
    for(ULONG at=1;at<=queues;++at)for(ULONG e=0;e<2;++e){Begin(queues,manual);FailThread=at;ThreadError=e?STATUS_ACCESS_DENIED:STATUS_INSUFFICIENT_RESOURCES;CheckFailure(ThreadError);CHECK(ThreadCalls==at&&ThreadsMade==at-1);RetryAfterFailure();Cleanup();}
}
static VOID Invalid(ULONG queues)
{
    for(ULONG kind=0;kind<29;++kind){
        Begin(queues,FALSE);
        switch(kind){
        case 0:Request.Is32=TRUE;break;case 1:Request.Stack.Parameters.DeviceIoControl.InputBufferLength=0;break;
        case 2:Request.Stack.Parameters.DeviceIoControl.InputBufferLength=sizeof(Registration)-1;break;
        case 3:Request.Stack.Parameters.DeviceIoControl.InputBufferLength=sizeof(Registration)+1;break;
        case 4:Registration.Size--;break;case 5:Registration.Major++;break;case 6:Registration.Minor++;break;
        case 7:Registration.Epoch=0;break;case 8:Registration.RegionAddress=0;break;case 9:Registration.RegionAddress++;break;
        case 10:Registration.RegionSize--;break;case 11:Registration.RegionSize++;break;
        case 12:Registration.QueueCount=0;break;case 13:Registration.QueueCount=PB_MAX_QUEUES+1;break;
        case 14:Registration.SlotCount=0;break;case 15:Registration.SlotCount=PB_MAX_SLOTS+1;break;
        case 16:Registration.RingCapacity=0;break;case 17:Registration.RingCapacity=3;break;
        case 18:Registration.SourceFingerprint[0]^=1;break;case 19:Registration.SourceFingerprint[64]=1;break;
        case 20:Request.Stack.Parameters.DeviceIoControl.InputBufferLength=MAXULONG;break;
        case 21:Registration.RegionAddress=UINT64_MAX;break;
        default:Registration.Reserved[kind-22]=1;break;
        }
        CheckFailure(STATUS_INVALID_PARAMETER);CHECK(!PoolCalls&&!MdlCalls&&!ProbeCalls&&!MapCalls&&!ObCalls&&!ThreadCalls);Cleanup();
    }
}
static VOID Success(ULONG queues,BOOLEAN manual)
{
    Begin(queues,manual);CHECK(NT_SUCCESS(Register())&&Context.V2==Current&&Connected==1&&Reports==1&&EverPublished&&Starts==1&&!StartBeforePublish);
    CHECK(References==(LONG)(1+2*queues)&&ThreadsMade==queues&&Current->Epoch==42&&ComparePrefix(Region,&Layout,sizeof(Layout))==sizeof(Layout));
    for(ULONG q=0;q<queues;++q)PlainQueues(q);
    ULONG allocations=PoolCalls;CHECK(Register()==STATUS_ALREADY_INITIALIZED&&PoolCalls==allocations&&Connected==1);
    Request.Stack.FileObject=&OtherOwner;CHECK(Register()==STATUS_ALREADY_INITIALIZED&&PoolCalls==allocations&&Context.Device.OwningFileObject==&Owner);Request.Stack.FileObject=&Owner;Cleanup();
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(argc==2){
        CHECK(!strcmp(argv[1],"1")||!strcmp(argv[1],"2"));BOOLEAN alias=!strcmp(argv[1],"1");Begin(8,FALSE);
        if(alias)Alias(0,16,TRUE);NTSTATUS status=Register();BOOLEAN bad=alias?NT_SUCCESS(status):StartBeforePublish!=0;
        if(alias)CHECK(status==(bad?STATUS_SUCCESS:STATUS_INVALID_PARAMETER));else CHECK(NT_SUCCESS(status));
        Cleanup();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        if(bad){puts(alias?"REGISTER_ACCEPTED_EVENT_OBJECT_ALIAS":"REGISTER_SIGNALED_START_BEFORE_PUBLICATION");return alias?100:101;}
        puts("PASS: actual register rejects object alias and signals Start only after publication");return 0;
    }
    const ULONG queues[]={1,2,8};
    for(ULONG i=0;i<_countof(queues);++i){Invalid(queues[i]);for(ULONG manual=0;manual<2;++manual){Aliases(queues[i],(BOOLEAN)manual);Resources(queues[i],(BOOLEAN)manual);Success(queues[i],(BOOLEAN)manual);}}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual Register/Free/Unregister schedules, %lu successful same-Ctx/region/handles retries; 1/2/8 queues, every event pair with same/duplicate handle aliases, input rejects, Probe exceptions/Map/ObRef/thread/pool failures, %lu plain shared-queue checks after optional failure/success, all partial workers joined and references/handles/mappings/pools balanced; no native DDI/NDIS claim\n",Scenarios,Retries,PlainChecks);return 0;
}
