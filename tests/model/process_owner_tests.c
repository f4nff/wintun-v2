/* SPDX-License-Identifier: GPL-2.0 */
/* Extracted common process/FILE close callbacks + actual context gate and
 * session Unregister/Free. This models ready sessions and quiescent NDIS;
 * fake workers only wait for Stop. No process is terminated or device used. */
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
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"process owner tests require shipping source fingerprint");
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
#define NT_ASSERT CHECK
static VOID InitializeListHead(LIST_ENTRY *head){head->Flink=head->Blink=head;}
static BOOLEAN IsListEmpty(const LIST_ENTRY *head){return head->Flink==head;}
static VOID InsertTailList(LIST_ENTRY *head,LIST_ENTRY *item){item->Blink=head->Blink;item->Flink=head;head->Blink->Flink=item;head->Blink=item;}
static VOID RemoveEntryList(LIST_ENTRY *item){item->Blink->Flink=item->Flink;item->Flink->Blink=item->Blink;}
typedef struct {HANDLE Native;LONG References;} KEVENT,*PKEVENT;
typedef struct {VOID *Address;} MDL;
typedef struct {CRITICAL_SECTION Lock;LONG References;BOOLEAN Closing;HANDLE Idle;} EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct {VOID *Reserved;} DEVICE_OBJECT;
typedef struct {ULONG Identity;} FILE_OBJECT;
typedef struct {FILE_OBJECT *FileObject;} IO_STACK_LOCATION;
typedef struct {IO_STACK_LOCATION Stack;ULONG Forwarded;} IRP;
typedef VOID *NDIS_HANDLE;
typedef ULONG NDIS_HALT_ACTION;
typedef VOID MINIPORT_HALT(NDIS_HANDLE,NDIS_HALT_ACTION);
typedef NTSTATUS DRIVER_DISPATCH_PAGED(DEVICE_OBJECT *,IRP *);
typedef UCHAR KIRQL;
typedef struct _TUN_CTX {
    SRWLOCK TransitionLock;volatile LONG Running,Halting;EX_RUNDOWN_REF ControlRundown;
    DEVICE_OBJECT *FunctionalDeviceObject;PB_KERNEL_SESSION *V2;VOID *NblPool,*MiniportAdapterHandle;
    struct {SRWLOCK RegistrationLock;FILE_OBJECT *OwningFileObject;HANDLE OwningProcessId;LIST_ENTRY Entry;KEVENT Disconnected;} Device;
    struct {volatile LONG V2SessionActive,AllowedMask;EX_RUNDOWN_REF ReturnRundown;KEVENT RunningEvent;} Probe;
} TUN_CTX;
#define Executive 0
#define KernelMode 0
#define IO_NO_INCREMENT 0
#define TUN_MEMORY_TAG 0
#define MediaConnectStateDisconnected 0
#define PID_A ((HANDLE)(ULONG_PTR)0xa11)
#define PID_B ((HANDLE)(ULONG_PTR)0xb22)
#define PID_C ((HANDLE)(ULONG_PTR)0xc33)
enum {ROLE_MAIN,ROLE_NOTIFY,ROLE_CLOSE,ROLE_HALT};
typedef struct NODE {TUN_CTX *Ctx;DEVICE_OBJECT *Device;PB_KERNEL_SESSION *Session;VOID *Pool;FILE_OBJECT A,B,C,Late;LONG Frees,Disconnects,PoolFrees;} NODE;
static NODE Nodes[4];
static ULONG NodeCount,Scenarios;
static volatile LONG Handles,Pools,Mdls,Objects,Regions,Workers,WorkerHandles,SessionFrees,Forwarded;
static SRWLOCK TunDispatchCtxGuard,TunDispatchDeviceListLock;
static LIST_ENTRY TunDispatchDeviceList;
static NODE *Target;
static HANDLE LookupReady,LookupGate,TailReady,TailGate,ControlWaiting,PoolReached,PoolPermit;
static volatile LONG LookupCount,ExpectedLookups,Premature;
static BOOLEAN GateLookup,GateTail;
static __declspec(thread) ULONG Role,CriticalDepth,ResourceCount;
static __declspec(thread) BOOLEAN LookupUsed,PendingLookup,TailUsed;
static __declspec(thread) struct {SRWLOCK *Lock;BOOLEAN Exclusive;} Resource[8];

static HANDLE Event(BOOLEAN manual,BOOLEAN signaled)
{HANDLE event=CreateEventW(NULL,manual,signaled,NULL);CHECK(event);InterlockedIncrement(&Handles);return event;}
static VOID Close(HANDLE handle){CHECK(handle&&CloseHandle(handle)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE handle){CHECK(WaitForSingleObject(handle,10000)==WAIT_OBJECT_0);}
static HANDLE Thread(LPTHREAD_START_ROUTINE run,VOID *input)
{HANDLE thread=CreateThread(NULL,0,run,input,0,NULL);CHECK(thread);InterlockedIncrement(&Handles);return thread;}
static VOID Join(HANDLE thread){Await(thread);Close(thread);}
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
static VOID *PoolAlloc(SIZE_T bytes){VOID *memory=HostAlloc(bytes);InterlockedIncrement(&Pools);return memory;}
static NODE *NodeCtx(TUN_CTX *ctx){for(ULONG i=0;i<NodeCount;++i)if(Nodes[i].Ctx==ctx)return Nodes+i;CHECK(FALSE);return NULL;}
static NODE *NodeRefs(EX_RUNDOWN_REF *refs)
{for(ULONG i=0;i<NodeCount;++i)if(Nodes[i].Ctx&&refs==&Nodes[i].Ctx->ControlRundown)return Nodes+i;return NULL;}
static VOID InitRundown(EX_RUNDOWN_REF *refs)
{InitializeCriticalSection(&refs->Lock);refs->Idle=Event(TRUE,TRUE);}
static VOID DestroyRundown(EX_RUNDOWN_REF *refs)
{CHECK(!refs->References);DeleteCriticalSection(&refs->Lock);Close(refs->Idle);}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *refs)
{
    EnterCriticalSection(&refs->Lock);BOOLEAN acquired=!refs->Closing;
    if(acquired){if(!refs->References++)CHECK(ResetEvent(refs->Idle));}
    LeaveCriticalSection(&refs->Lock);
    if(acquired&&Role==ROLE_NOTIFY&&GateLookup&&!LookupUsed&&NodeRefs(refs)==Target){PendingLookup=TRUE;LookupUsed=TRUE;}
    return acquired;
}
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *refs)
{
    if(GateTail&&!TailUsed&&(Role==ROLE_NOTIFY||Role==ROLE_CLOSE)&&NodeRefs(refs)==Target){TailUsed=TRUE;CHECK(SetEvent(TailReady));Await(TailGate);}
    EnterCriticalSection(&refs->Lock);CHECK(refs->References>0);
    if(!--refs->References)CHECK(SetEvent(refs->Idle));
    LeaveCriticalSection(&refs->Lock); /* No refs/containing-Ctx access after this. */
}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    EnterCriticalSection(&refs->Lock);refs->Closing=TRUE;LeaveCriticalSection(&refs->Lock);
    if(NodeRefs(refs)==Target&&Target)CHECK(SetEvent(ControlWaiting));
    Await(refs->Idle);
    /* The event may be set while the last release still owns its CS. Join its
     * actual unlock before permitting the containing object to be destroyed. */
    EnterCriticalSection(&refs->Lock);CHECK(!refs->References);LeaveCriticalSection(&refs->Lock);
}
static VOID KeEnterCriticalRegion(VOID){++CriticalDepth;}
static VOID KeLeaveCriticalRegion(VOID)
{
    CHECK(CriticalDepth&&!ResourceCount);--CriticalDepth;
    if(Role==ROLE_NOTIFY&&PendingLookup&&CriticalDepth==1){
        PendingLookup=FALSE;if(InterlockedIncrement(&LookupCount)==ExpectedLookups)CHECK(SetEvent(LookupReady));Await(LookupGate);
    }
}
static VOID AcquireResource(SRWLOCK *lock,BOOLEAN wait,BOOLEAN exclusive)
{
    CHECK(CriticalDepth&&wait&&ResourceCount<_countof(Resource));
    if(exclusive)AcquireSRWLockExclusive(lock);else AcquireSRWLockShared(lock);
    Resource[ResourceCount].Lock=lock;Resource[ResourceCount++].Exclusive=exclusive;
}
static VOID ExAcquireResourceSharedLite(SRWLOCK *lock,BOOLEAN wait){AcquireResource(lock,wait,FALSE);}
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait){AcquireResource(lock,wait,TRUE);}
static VOID ExReleaseResourceLite(SRWLOCK *lock)
{CHECK(ResourceCount&&Resource[ResourceCount-1].Lock==lock);if(Resource[--ResourceCount].Exclusive)ReleaseSRWLockExclusive(lock);else ReleaseSRWLockShared(lock);}
static VOID ExDeleteResourceLite(SRWLOCK *lock){CHECK(lock&&!ResourceCount);}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock){AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql){CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait){CHECK(event&&!priority&&!wait&&SetEvent(event->Native));}
static VOID KeClearEvent(KEVENT *event){CHECK(event&&ResetEvent(event->Native));}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{CHECK(!reason&&!mode&&!alertable&&!timeout);Await(event->Native);}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout){CHECK(!alertable&&!timeout);Await(thread);}
static VOID ZwClose(HANDLE thread){Close(thread);CHECK(InterlockedDecrement(&WorkerHandles)>=0);}
static VOID ObDereferenceObject(PKEVENT event)
{CHECK(event&&event->References==1);Close(event->Native);free(event);CHECK(InterlockedDecrement(&Objects)>=0);}
static VOID MmUnlockPages(MDL *mdl)
{
    CHECK(mdl&&mdl->Address);
#ifdef PB_GUARD_TESTS
    HostFree(mdl->Address);
#else
    _aligned_free(mdl->Address);
#endif
    mdl->Address=NULL;CHECK(InterlockedDecrement(&Regions)>=0);
}
static VOID IoFreeMdl(MDL *mdl){CHECK(mdl&&!mdl->Address);HostFree(mdl);CHECK(InterlockedDecrement(&Mdls)>=0);}
static VOID ExFreePoolWithTag(VOID *memory,ULONG tag)
{
    CHECK(memory&&!tag&&Pools>0);
    for(ULONG i=0;i<NodeCount;++i){NODE *node=Nodes+i;
        if(memory==node->Session){
            CHECK(!node->Session->Active);Close(node->Session->StartEvent.Native);Close(node->Session->StopEvent.Native);Close(node->Session->AllReturned.Native);DestroyRundown(&node->Session->Returns);node->Session=NULL;InterlockedIncrement(&SessionFrees);
        }else if(memory==node->Ctx){
            CHECK(!node->Session&&!node->Pool&&node->PoolFrees==1&&!node->Ctx->ControlRundown.References&&!node->Ctx->Probe.ReturnRundown.References&&!node->Device->Reserved);
            Close(node->Ctx->Device.Disconnected.Native);Close(node->Ctx->Probe.RunningEvent.Native);DestroyRundown(&node->Ctx->ControlRundown);DestroyRundown(&node->Ctx->Probe.ReturnRundown);node->Ctx=NULL;InterlockedIncrement(&node->Frees);
        }
    }
    InterlockedDecrement(&Pools);HostFree(memory);
}
static VOID PbReportOffload(TUN_CTX *ctx){CHECK(ctx&&!ctx->V2&&!ctx->Probe.V2SessionActive&&!ctx->Probe.AllowedMask);}
static VOID TunIndicateStatus(VOID *adapter,ULONG status){NODE *node=NodeCtx(adapter);CHECK(status==MediaConnectStateDisconnected);InterlockedIncrement(&node->Disconnects);}
static IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(IRP *irp){CHECK(irp);return &irp->Stack;}
static NTSTATUS OriginalClose(DEVICE_OBJECT *device,IRP *irp)
{CHECK(device&&irp&&CriticalDepth==0&&!ResourceCount);++irp->Forwarded;InterlockedIncrement(&Forwarded);return STATUS_SUCCESS;}
static DRIVER_DISPATCH_PAGED *NdisDispatchClose=OriginalClose;
static VOID NdisFreeNetBufferListPool(VOID *pool)
{
    NODE *node=NULL;for(ULONG i=0;i<NodeCount;++i)if(Nodes[i].Pool==pool)node=Nodes+i;CHECK(node&&node->Ctx&&!node->Session);
    if(node->Ctx->ControlRundown.References){CHECK(node==Target);InterlockedExchange(&Premature,1);CHECK(SetEvent(PoolReached));Await(PoolPermit);}
    CHECK(!node->Ctx->ControlRundown.References&&!node->Ctx->Probe.ReturnRundown.References);node->Pool=NULL;InterlockedIncrement(&node->PoolFrees);ExFreePoolWithTag(pool,0);
}
#ifndef PB_PROCESS_CONTEXT_SOURCE
#error Build with tools/build/kernel-process-tests.ps1
#endif
#include PB_PROCESS_CONTEXT_SOURCE
#include "../../driver/v2/session_cleanup.h"
#include "../../driver/v2/session_unregister.h"
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_PROCESS_CALLBACK_SOURCE
#pragma warning(pop)

static DWORD WINAPI Worker(VOID *input)
{PB_KERNEL_SESSION *session=input;Await(session->StartEvent.Native);Await(session->StopEvent.Native);CHECK(session->Stopping);InterlockedDecrement(&Workers);return 0;}
static PKEVENT Object(BOOLEAN manual)
{PKEVENT event=calloc(1,sizeof(*event));CHECK(event);event->Native=Event(manual,FALSE);event->References=1;InterlockedIncrement(&Objects);return event;}
static VOID NewSession(NODE *node,FILE_OBJECT *file,HANDLE pid,ULONG queues,BOOLEAN manual)
{
    TUN_CTX *ctx=node->Ctx;CHECK(ctx&&!ctx->V2&&!ctx->Device.OwningFileObject&&!node->Session);
    KeEnterCriticalRegion();ExAcquireResourceExclusiveLite(&ctx->Device.RegistrationLock,TRUE);
    PB_KERNEL_SESSION *session=PoolAlloc(sizeof(*session));node->Session=session;session->Ctx=ctx;session->Epoch=42;
    CHECK(PbCalculateLayout(queues,2,2,&session->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    session->Region=HostAlloc(session->Layout.RegionSize);
#else
    session->Region=_aligned_malloc(session->Layout.RegionSize,64);CHECK(session->Region);memset(session->Region,0,session->Layout.RegionSize);
#endif
    CHECK(!((ULONG_PTR)session->Region&63));memcpy(session->Region,&session->Layout,sizeof(session->Layout));InterlockedIncrement(&Regions);
    session->Mdl=HostAlloc(sizeof(*session->Mdl));session->Mdl->Address=session->Region;session->PagesLocked=TRUE;InterlockedIncrement(&Mdls);
    session->Lifecycle=(PB_SHARED_LIFECYCLE *)(session->Region+session->Layout.LifecycleOffset);session->ProgressEvent=Object(manual);
    session->StartEvent.Native=Event(TRUE,FALSE);session->StopEvent.Native=Event(TRUE,FALSE);session->AllReturned.Native=Event(TRUE,TRUE);InitRundown(&session->Returns);
    for(ULONG i=0;i<queues;++i){
        PB_KERNEL_QUEUE *queue=session->Queues+i;queue->Session=session;queue->Index=i;queue->KernelEvent=Object(manual);queue->UserEvent=Object(manual);
        queue->TxSlots=PoolAlloc(2*sizeof(*queue->TxSlots));queue->RxSlots=PoolAlloc(2*sizeof(*queue->RxSlots));queue->Pending=PoolAlloc(2*sizeof(*queue->Pending));
        InterlockedIncrement(&Workers);queue->Thread=Thread(Worker,session);InterlockedIncrement(&WorkerHandles);
    }
    ctx->Device.OwningFileObject=file;ctx->Device.OwningProcessId=pid;InitializeListHead(&ctx->Device.Entry);
    ExAcquireResourceExclusiveLite(&TunDispatchDeviceListLock,TRUE);InsertTailList(&TunDispatchDeviceList,&ctx->Device.Entry);ExReleaseResourceLite(&TunDispatchDeviceListLock);
    KIRQL irql=ExAcquireSpinLockExclusive(&ctx->TransitionLock);ctx->V2=session;ctx->Probe.V2SessionActive=1;ctx->Probe.AllowedMask=0;KeClearEvent(&ctx->Device.Disconnected);ExReleaseSpinLockExclusive(&ctx->TransitionLock,irql);
    KeSetEvent(&session->StartEvent,0,FALSE);ExReleaseResourceLite(&ctx->Device.RegistrationLock);KeLeaveCriticalRegion();
}
static NODE *Create(HANDLE pid,ULONG queues,BOOLEAN manual)
{
    CHECK(NodeCount<_countof(Nodes));NODE *node=Nodes+NodeCount++;node->Device=HostAlloc(sizeof(*node->Device));node->Ctx=PoolAlloc(sizeof(*node->Ctx));
    TUN_CTX *ctx=node->Ctx;ctx->FunctionalDeviceObject=node->Device;ctx->MiniportAdapterHandle=ctx;ctx->Running=1;
    InitRundown(&ctx->ControlRundown);InitRundown(&ctx->Probe.ReturnRundown);ctx->Device.Disconnected.Native=Event(TRUE,TRUE);ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);
    node->Pool=PoolAlloc(8);ctx->NblPool=node->Pool;node->A.Identity=11;node->B.Identity=22;node->C.Identity=33;node->Late.Identity=44;
    KeEnterCriticalRegion();ExAcquireResourceExclusiveLite(&TunDispatchCtxGuard,TRUE);node->Device->Reserved=ctx;ExReleaseResourceLite(&TunDispatchCtxGuard);KeLeaveCriticalRegion();
    NewSession(node,&node->A,pid,queues,manual);return node;
}
static VOID Begin(VOID)
{
    CHECK(!Handles&&!Pools&&!Mdls&&!Objects&&!Regions&&!Workers&&!WorkerHandles&&!CriticalDepth&&!ResourceCount);
    memset(Nodes,0,sizeof(Nodes));NodeCount=0;InitializeListHead(&TunDispatchDeviceList);Target=NULL;GateLookup=GateTail=FALSE;LookupCount=ExpectedLookups=Premature=0;SessionFrees=Forwarded=0;
    LookupReady=Event(TRUE,FALSE);LookupGate=Event(TRUE,FALSE);TailReady=Event(TRUE,FALSE);TailGate=Event(TRUE,FALSE);ControlWaiting=Event(TRUE,FALSE);PoolReached=Event(TRUE,FALSE);PoolPermit=Event(TRUE,FALSE);
}
static VOID Notify(HANDLE pid,BOOLEAN create)
{ULONG previous=Role;Role=ROLE_NOTIFY;CriticalDepth=1;LookupUsed=PendingLookup=TailUsed=FALSE;TunProcessNotification(NULL,pid,create);CHECK(CriticalDepth==1&&!ResourceCount);CriticalDepth=0;Role=previous;}
static NTSTATUS FileClose(NODE *node,FILE_OBJECT *file)
{IRP irp={.Stack={file}};ULONG previous=Role;Role=ROLE_CLOSE;TailUsed=FALSE;NTSTATUS status=TunDispatchClose(node->Device,&irp);CHECK(!CriticalDepth&&!ResourceCount&&irp.Forwarded==1);Role=previous;return status;}
static VOID PausedBoundary(TUN_CTX *ctx)
{
    /* Establish the state a completed Pause would leave. This fixture never
     * constructs/indicates an NBL and its workers perform no packet work. */
    KIRQL irql=ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    WriteRelease(&ctx->Running,FALSE);KeClearEvent(&ctx->Probe.RunningEvent);
    CHECK(!ctx->Probe.ReturnRundown.References);
    if(ctx->V2)CHECK(!ctx->V2->Active&&!ctx->V2->Indications);
    ExReleaseSpinLockExclusive(&ctx->TransitionLock,irql);
}
static DWORD WINAPI NotifyThread(VOID *input){CHECK(input);Notify(input,FALSE);return 0;}
static DWORD WINAPI CloseThread(VOID *input){NODE *node=input;CHECK(NT_SUCCESS(FileClose(node,&node->A)));return 0;}
static DWORD WINAPI HaltThread(VOID *input){Role=ROLE_HALT;PausedBoundary(input);TunHaltEx(input,0);CHECK(!CriticalDepth&&!ResourceCount);return 0;}
static VOID End(VOID)
{
    GateLookup=GateTail=FALSE;CHECK(SetEvent(LookupGate)&&SetEvent(TailGate)&&SetEvent(PoolPermit));
    for(ULONG i=0;i<NodeCount;++i){NODE *node=Nodes+i;if(node->Ctx){PausedBoundary(node->Ctx);TunHaltEx(node->Ctx,0);}CHECK(node->Frees==1&&node->PoolFrees==1&&!node->Session&&!node->Pool&&!node->Device->Reserved);CHECK(NT_SUCCESS(FileClose(node,&node->Late)));HostFree(node->Device);node->Device=NULL;}
    CHECK(IsListEmpty(&TunDispatchDeviceList)&&!Pools&&!Mdls&&!Objects&&!Regions&&!Workers&&!WorkerHandles);
    Close(LookupReady);Close(LookupGate);Close(TailReady);Close(TailGate);Close(ControlWaiting);Close(PoolReached);Close(PoolPermit);CHECK(!Handles&&!CriticalDepth&&!ResourceCount);++Scenarios;
}
static VOID UnlockedGlobals(VOID)
{CHECK(TryAcquireSRWLockExclusive(&TunDispatchCtxGuard));ReleaseSRWLockExclusive(&TunDispatchCtxGuard);CHECK(TryAcquireSRWLockExclusive(&TunDispatchDeviceListLock));ReleaseSRWLockExclusive(&TunDispatchDeviceListLock);}
static BOOLEAN Replacement(ULONG queues,BOOLEAN manual,ULONG notifications)
{
    Begin();NODE *node=Create(PID_A,queues,manual);Target=node;GateLookup=TRUE;ExpectedLookups=1;
    HANDLE threads[2];threads[0]=Thread(NotifyThread,PID_A);Await(LookupReady);UnlockedGlobals();
    CHECK(node->Ctx->ControlRundown.References==1&&NT_SUCCESS(FileClose(node,&node->A))&&!node->Session);
    if(notifications==2){
        /* Two different process deletions can overlap while this adapter is
         * reassigned A -> C -> B. Never invent duplicate exit notifications for
         * one process, nor a second CLOSE of the same FILE object. */
        CHECK(ResetEvent(LookupReady));ExpectedLookups=2;NewSession(node,&node->C,PID_C,queues,manual);
        threads[1]=Thread(NotifyThread,PID_C);Await(LookupReady);UnlockedGlobals();
        CHECK(node->Ctx->ControlRundown.References==2&&NT_SUCCESS(FileClose(node,&node->C))&&!node->Session);
    }
    NewSession(node,&node->B,PID_B,queues,manual);PB_KERNEL_SESSION *replacement=node->Session;CHECK(SetEvent(LookupGate));
    for(ULONG i=0;i<notifications;++i)Join(threads[i]);GateLookup=FALSE;
    BOOLEAN bad=node->Session!=replacement||node->Ctx->Device.OwningFileObject!=&node->B;
    if(!bad)CHECK(node->Ctx->Device.OwningProcessId==PID_B&&node->Disconnects==(LONG)notifications&&!node->Ctx->ControlRundown.References);
    End();return bad;
}
static BOOLEAN StaleFile(ULONG queues,BOOLEAN manual)
{
    Begin();NODE *node=Create(PID_A,queues,manual);Notify(PID_A,FALSE);CHECK(!node->Session&&node->Disconnects==1);
    NewSession(node,&node->B,PID_B,queues,manual);PB_KERNEL_SESSION *replacement=node->Session;
    CHECK(NT_SUCCESS(FileClose(node,&node->A)));BOOLEAN bad=node->Session!=replacement||node->Ctx->Device.OwningFileObject!=&node->B;
    if(!bad)CHECK(node->Disconnects==1&&node->Ctx->Device.OwningProcessId==PID_B);End();return bad;
}
static BOOLEAN ControlTail(ULONG queues,BOOLEAN manual,BOOLEAN close)
{
    Begin();NODE *node=Create(PID_A,queues,manual);Target=node;GateTail=TRUE;
    HANDLE callback=Thread(close?CloseThread:NotifyThread,close?(VOID *)node:PID_A);Await(TailReady);
    CHECK(!node->Session&&!node->Ctx->V2&&node->Ctx->ControlRundown.References==1&&!node->Ctx->Probe.ReturnRundown.References&&SessionFrees==1);
    TUN_CTX *ctx=node->Ctx;HANDLE halt=Thread(HaltThread,ctx);HANDLE observed[]={ControlWaiting,PoolReached};
    DWORD reached=WaitForMultipleObjects(2,observed,FALSE,10000);CHECK(reached==WAIT_OBJECT_0||reached==WAIT_OBJECT_0+1);
    CHECK(ctx->Halting&&!node->Device->Reserved&&!node->Frees&&!node->PoolFrees&&ctx->ControlRundown.References==1);
    UnlockedGlobals();CHECK(!TunReferenceDeviceCtx(node->Device));
    CHECK(SetEvent(TailGate));Join(callback);CHECK(SetEvent(PoolPermit));Join(halt);
    CHECK(node->Frees==1&&!node->Ctx);BOOLEAN bad=Premature!=0;End();return bad;
}
static VOID Multiple(ULONG owners,ULONG queues,BOOLEAN manual)
{
    Begin();for(ULONG i=0;i<owners;++i)Create(PID_A,queues,manual);NODE *other=Create(PID_B,queues,manual);
    Notify(PID_A,TRUE);Notify((HANDLE)(ULONG_PTR)0xd44,FALSE);CHECK(!SessionFrees);
    Notify(PID_A,FALSE);CHECK(SessionFrees==(LONG)owners&&other->Session&&other->Ctx->Device.OwningProcessId==PID_B);
    for(ULONG i=0;i<owners;++i)CHECK(!Nodes[i].Session&&Nodes[i].Disconnects==1&&!Nodes[i].Ctx->ControlRundown.References);
    Notify(PID_A,FALSE);CHECK(SessionFrees==(LONG)owners);End();
}
static VOID Competing(ULONG queues,BOOLEAN manual)
{
    Begin();NODE *node=Create(PID_A,queues,manual),*other=Create(PID_C,queues,manual);
    HANDLE threads[]={Thread(NotifyThread,PID_A),Thread(NotifyThread,PID_C),Thread(CloseThread,node)};
    for(ULONG i=0;i<_countof(threads);++i)Join(threads[i]);
    CHECK(SessionFrees==2&&node->Disconnects==1&&other->Disconnects==1&&!node->Session&&!other->Session&&!node->Ctx->ControlRundown.References&&!other->Ctx->ControlRundown.References);End();
}
static VOID HaltingCandidate(ULONG queues,BOOLEAN manual)
{
    Begin();NODE *halting=Create(PID_A,queues,manual),*live=Create(PID_A,queues,manual),*other=Create(PID_B,queues,manual);Target=halting;
    TUN_CTX *held=TunReferenceDeviceCtx(halting->Device);CHECK(held==halting->Ctx);
    HANDLE halt=Thread(HaltThread,held);Await(ControlWaiting);
    CHECK(held->Halting&&!halting->Device->Reserved&&halting->Session);Notify(PID_A,FALSE);
    /* One revoked candidate must not prevent the scan reaching another live
     * adapter of this PID. Halt retains ownership of the skipped candidate. */
    CHECK(halting->Session&&!live->Session&&other->Session&&SessionFrees==1);
    TunDereferenceCtx(held);Join(halt);CHECK(!halting->Ctx);End();
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);
    if(argc==2){
        ULONG kind=(ULONG)strtoul(argv[1],NULL,10);CHECK(kind>=1&&kind<=3);
        BOOLEAN bad=kind==1?Replacement(2,FALSE,2):kind==2?StaleFile(2,FALSE):ControlTail(2,FALSE,TRUE);
        if(bad){puts(kind==1?"PROCESS_NOTIFY_REMOVED_REPLACEMENT_OWNER":kind==2?"OLD_FILE_CLOSE_REMOVED_NEW_OWNER":"HALT_SKIPPED_CONTROL_RUNDOWN");return 103+(int)kind;}
        puts("PASS: actual process/FILE identity and ControlRundown boundary");return 0;
    }
    /* Warm real lock/handle paths before process-wide native handle comparison.
     * Exact fixture ownership still returns to zero after every warm-up case. */
    CHECK(!ControlTail(1,FALSE,FALSE));Scenarios=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    const ULONG queues[]={1,2,8};
    for(ULONG repeat=0;repeat<3;++repeat)for(ULONG q=0;q<_countof(queues);++q)for(ULONG manual=0;manual<2;++manual){
        for(ULONG n=1;n<=2;++n)CHECK(!Replacement(queues[q],(BOOLEAN)manual,n));
        CHECK(!StaleFile(queues[q],(BOOLEAN)manual));
        for(ULONG close=0;close<2;++close)CHECK(!ControlTail(queues[q],(BOOLEAN)manual,(BOOLEAN)close));
        for(ULONG n=1;n<=3;++n)Multiple(n,queues[q],(BOOLEAN)manual);Competing(queues[q],(BOOLEAN)manual);HaltingCandidate(queues[q],(BOOLEAN)manual);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual process/FILE-owner/ControlRundown schedules plus one checked warmup; PID replacement, stale FILE close, process/close tails versus actual Halt, multiple matching adapters, create/no-match and racing notifications/close, 1/2/8 queues, auto/manual events; all hosted objects and threads reclaimed; ready sessions/fake workers, no native process exit/NDIS/IRQL claim\n",Scenarios);return 0;
}
