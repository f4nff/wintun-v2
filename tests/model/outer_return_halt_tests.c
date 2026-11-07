/* SPDX-License-Identifier: GPL-2.0 */
/* Real outer callbacks are extracted verbatim by kernel-return-tests.ps1.
 * Session return/unregister/free are actual source; native Windows threads,
 * locks/events and guarded hosted DDI objects do not emulate NDIS or IRQL. */
#include "kernel_nbl_fixture.h"
#define Executive 0
#define KernelMode 0
typedef VOID *NDIS_HANDLE;
typedef NET_BUFFER_LIST *PNET_BUFFER_LIST;
typedef ULONG NDIS_HALT_ACTION;
typedef struct FILE_OBJECT { ULONG Unused; } FILE_OBJECT;
typedef VOID MINIPORT_RETURN_NET_BUFFER_LISTS(NDIS_HANDLE,PNET_BUFFER_LIST,ULONG);
typedef VOID MINIPORT_HALT(NDIS_HANDLE,NDIS_HALT_ACTION);
#define TUN_FORCE_UNREGISTRATION ((FILE_OBJECT *)-1)

typedef struct JOB {
    NET_BUFFER_LIST *Lists;
    HANDLE RefEntered,RefGate,SessionEntered,SessionGate,OuterEntered,OuterGate,Done;
    ULONG Windows,ExpectedWindows;
    BOOLEAN RefUsed;
} JOB;
static JOB Jobs[PB_MAX_QUEUES];
static TUN_CTX *Context;
static PB_KERNEL_SESSION *Current;
static VOID *Pool;
static NET_BUFFER_LIST *Reentrant;
static __declspec(thread) ULONG JobIndex,Depth;
static __declspec(thread) BOOLEAN LastWindow;
static volatile LONG Handles,Objects,WorkerHandles,Workers,Regions,SessionFreed,ContextFreed,PoolFreed;
static volatile LONG OuterAcquires,OuterReleases,OuterWaiting,Premature,Windows,Nested;
static ULONG JobCount,Scenarios,ExpectedWindows,ExpectedNested;
static HANDLE AllWait,SessionWait,SessionGone,OuterWait,PoolReached,PoolGate,HaltDone;
static __declspec(thread) BOOLEAN ResourceExclusive;

static HANDLE Event(BOOLEAN signaled)
{HANDLE event=CreateEventW(NULL,TRUE,signaled,NULL);CHECK(event);InterlockedIncrement(&Handles);return event;}
static VOID Close(HANDLE handle)
{CHECK(handle&&CloseHandle(handle)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE handle)
{CHECK(WaitForSingleObject(handle,10000)==WAIT_OBJECT_0);}
static HANDLE Start(LPTHREAD_START_ROUTINE run,VOID *input)
{HANDLE thread=CreateThread(NULL,0,run,input,0,NULL);CHECK(thread);InterlockedIncrement(&Handles);return thread;}
static VOID Join(HANDLE thread) {Await(thread);Close(thread);}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock) {AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static VOID KeEnterCriticalRegion(VOID) {}
static VOID KeLeaveCriticalRegion(VOID) {}
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait)
{CHECK(wait&&!ResourceExclusive);AcquireSRWLockExclusive(lock);ResourceExclusive=TRUE;}
static VOID ExReleaseResourceLite(SRWLOCK *lock)
{CHECK(ResourceExclusive);ResourceExclusive=FALSE;ReleaseSRWLockExclusive(lock);}
static VOID ExDeleteResourceLite(SRWLOCK *lock)
{CHECK(Context&&lock==&Context->Device.RegistrationLock&&!ResourceExclusive);}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{CHECK(!reason&&!mode&&!alertable&&!timeout);if(Current&&event==&Current->AllReturned)CHECK(SetEvent(AllWait));Await(event->Native);}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{CHECK(!alertable&&!timeout);Await(thread);}
static VOID ZwClose(HANDLE thread) {Close(thread);CHECK(InterlockedDecrement(&WorkerHandles)>=0);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    if(Current&&refs==&Current->Returns)CHECK(SetEvent(SessionWait));
    else {CHECK(Context&&refs==&Context->Probe.ReturnRundown);InterlockedIncrement(&OuterWaiting);CHECK(SetEvent(OuterWait));}
    AcquireSRWLockExclusive(&refs->Lock);CHECK(!refs->References);ReleaseSRWLockExclusive(&refs->Lock);
}
static VOID ObDereferenceObject(KEVENT *event)
{CHECK(event);Close(event->Native);free(event);CHECK(InterlockedDecrement(&Objects)>=0);}
static VOID MmUnlockPages(MDL *mdl)
{
    CHECK(Current&&mdl==Current->Mdl&&mdl->Address==Current->Region&&Regions==1);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(mdl->Address);
#else
    _aligned_free(mdl->Address);
#endif
    InterlockedDecrement(&Regions);
}
static VOID PbReportOffload(TUN_CTX *ctx)
{CHECK(ctx==Context&&!ctx->V2&&!ctx->Probe.AllowedMask&&!ctx->Probe.V2SessionActive);}
#include "../../driver/v2/session_cleanup.h"
#include "../../driver/v2/session_unregister.h"
static VOID TunRevokeContext(TUN_CTX *ctx) {CHECK(ctx==Context);WriteRelease(&ctx->Halting,1);}
static VOID TunUnregisterBuffers(TUN_CTX *ctx,FILE_OBJECT *owner)
{
    CHECK(ctx==Context&&owner==TUN_FORCE_UNREGISTRATION);KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&ctx->Device.RegistrationLock,TRUE);
    if(ctx->V2)PbV2Unregister(ctx);
    ExReleaseResourceLite(&ctx->Device.RegistrationLock);KeLeaveCriticalRegion();
}
static VOID NdisFreeNetBufferListPool(VOID *pool)
{
    CHECK(pool==Pool&&Context&&SessionFreed&&!Current&&!Nbls&&!Mdls&&!Workers&&!WorkerHandles);
    /* Observe a mutated Halt reaching a real free too early, then park the
     * stand-in so the test can join every callback before any unsafe free. */
    if(Context->Probe.ReturnRundown.References){InterlockedExchange(&Premature,1);CHECK(SetEvent(PoolReached));Await(PoolGate);}
    CHECK(!Context->Probe.ReturnRundown.References);InterlockedIncrement(&PoolFreed);
    ExFreePoolWithTag(pool,0);Pool=NULL;
}
static VOID OuterRelease(EX_RUNDOWN_REF *refs)
{
    CHECK(Context&&refs==&Context->Probe.ReturnRundown&&Depth);
    if(Depth==1){JOB *job=Jobs+JobIndex;CHECK(SetEvent(job->OuterEntered));Await(job->OuterGate);}
    else InterlockedIncrement(&Nested);
    --Depth;InterlockedIncrement(&OuterReleases);
    ExReleaseRundownProtection(refs); /* No Ctx/refs access after this call. */
}
static BOOLEAN ObservedReturn(TUN_CTX *ctx,NET_BUFFER_LIST **lists,ULONG limit)
{
    BOOLEAN previous=LastWindow;
    if(Depth==1){JOB *job=Jobs+JobIndex;LastWindow=++job->Windows==job->ExpectedWindows;}
    InterlockedIncrement(&Windows);BOOLEAN result=PbV2ReturnNbls(ctx,lists,limit);LastWindow=previous;return result;
}
#ifndef PB_OUTER_CALLBACK_SOURCE
#error Build this test with tools/build/kernel-return-tests.ps1 to extract actual callbacks
#endif
#define ExReleaseRundownProtection OuterRelease
#define PbV2ReturnNbls ObservedReturn
#pragma warning(push)
#pragma warning(disable:4100) /* Extracted NDIS ABI parameters are unused. */
#include PB_OUTER_CALLBACK_SOURCE
#pragma warning(pop)
#undef PbV2ReturnNbls
#undef ExReleaseRundownProtection

static VOID Acquired(EX_RUNDOWN_REF *refs)
{if(Context&&refs==&Context->Probe.ReturnRundown){++Depth;InterlockedIncrement(&OuterAcquires);}}
static VOID SessionRelease(EX_RUNDOWN_REF *refs)
{
    if(Context&&refs==&Context->Probe.ReturnRundown)return;
    CHECK(Current&&refs==&Current->Returns);
    if(Depth==1&&LastWindow){JOB *job=Jobs+JobIndex;CHECK(SetEvent(job->SessionEntered));Await(job->SessionGate);}
}
static VOID Unlocked(KSPIN_LOCK *lock)
{
    CHECK(Current&&Depth);JOB *job=Jobs+JobIndex;
    if(Depth==1&&Reentrant&&JobIndex==0){NET_BUFFER_LIST *nbl=Reentrant;Reentrant=NULL;TunReturnNetBufferLists(Context,nbl,0);}
    if(Depth==1&&!job->RefUsed){CHECK(lock==&Current->Queues[JobIndex<Current->Layout.QueueCount?JobIndex:0].RxLock);job->RefUsed=TRUE;CHECK(SetEvent(job->RefEntered));Await(job->RefGate);}
}
static VOID Freeing(VOID *memory)
{
    if(memory==Current){CHECK(!Current->Active&&!Current->Returns.References&&!Mdls&&!Nbls&&!Workers&&!WorkerHandles);Current=NULL;InterlockedIncrement(&SessionFreed);CHECK(SetEvent(SessionGone));}
    else if(memory==Context){
        CHECK(SessionFreed&&PoolFreed&&!Pool&&!Pools&&!Regions&&!Objects&&OuterAcquires==OuterReleases&&!Context->Probe.ReturnRundown.References);
        Close(Context->Probe.RunningEvent.Native);Close(Context->Device.Disconnected.Native);Context=NULL;InterlockedIncrement(&ContextFreed);
    }
}
static DWORD WINAPI Worker(VOID *input)
{PB_KERNEL_SESSION *s=input;Await(s->StopEvent.Native);CHECK(s->Stopping);InterlockedDecrement(&Workers);return 0;}
static DWORD WINAPI Callback(VOID *input)
{JobIndex=(ULONG)(ULONG_PTR)input;TunReturnNetBufferLists(Context,Jobs[JobIndex].Lists,0);CHECK(!Depth&&SetEvent(Jobs[JobIndex].Done));return 0;}
static DWORD WINAPI Halt(VOID *input)
{TunHaltEx(input,0);CHECK(SetEvent(HaltDone));return 0;}
static KEVENT *Object(BOOLEAN manual)
{KEVENT *event=calloc(1,sizeof(*event));CHECK(event);event->Native=CreateEventW(NULL,manual,FALSE,NULL);CHECK(event->Native);InterlockedIncrement(&Handles);InterlockedIncrement(&Objects);return event;}
static VOID SessionResourcesFree(VOID *memory)
{
    if(memory==Current){Close(Current->StartEvent.Native);Close(Current->StopEvent.Native);Close(Current->AllReturned.Native);}
    Freeing(memory);
}
static NET_BUFFER_LIST *Receive(ULONG queue,ULONG slot,ULONG originals)
{
    PB_KERNEL_QUEUE *q=Current->Queues+queue;PB_DESCRIPTOR descriptors[2]={0};CHECK(originals<=2);
    ULONG length=60;for(ULONG i=0;i<originals;++i){
        PB_DESCRIPTOR *d=descriptors+i;*d=(PB_DESCRIPTOR){.Token={42,1,(USHORT)(slot+i),(USHORT)queue},.Length=length,.DataOffset=Current->Layout.Queues[queue].RxData+(slot+i)*PB_BLOCK_SIZE};
        UCHAR *bytes=Current->Region+d->DataOffset;bytes[0]=0x45;bytes[3]=(UCHAR)length;bytes[9]=6;
        CHECK(!q->RxSlots[slot+i].Active);q->RxSlots[slot+i].Active=TRUE;q->Credits++;
    }
    PB_COALESCE coal={.Length=length,.Protocol=6,.IpVersion=4,.PayloadLength=16};
    if(originals>1){
        if(!q->CoalesceBuffers[0])q->CoalesceBuffers[0]=ExAllocatePool2(0,PB_BLOCK_SIZE,0);
        CHECK(q->CoalesceBuffers[0]&&!q->CoalesceBusy[0]);memcpy(q->CoalesceBuffers[0],Current->Region+descriptors[0].DataOffset,length);q->CoalesceBusy[0]=1;
    }
    NET_BUFFER_LIST *nbl=PbV2Receive(q,descriptors,originals,originals>1?0:MAXULONG,originals>1?&coal:NULL);CHECK(nbl);PbV2RetainReceives(Current,1,originals);return nbl;
}
static VOID Create(ULONG queues,ULONG shape,BOOLEAN manual)
{
    CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!WorkerHandles&&!Workers&&!Regions&&!Current&&!Context);
    memset(Jobs,0,sizeof(Jobs));SessionFreed=ContextFreed=PoolFreed=OuterAcquires=OuterReleases=OuterWaiting=Premature=Windows=Nested=0;
    Failure=Acquisitions=0;Context=ExAllocatePool2(0,sizeof(*Context),0);CHECK(Context);Context->Running=1;
    Pool=ExAllocatePool2(0,8,0);CHECK(Pool);Context->NblPool=Pool;Context->MiniportAdapterHandle=Context;
    Context->Probe.RunningEvent.Native=Event(TRUE);Context->Device.Disconnected.Native=Event(FALSE);
    Current=ExAllocatePool2(0,sizeof(*Current),0);CHECK(Current);Current->Ctx=Context;Current->Epoch=42;
    ULONG slots=shape==1?128:shape==2?4:2;CHECK(PbCalculateLayout(queues,slots,slots,&Current->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    Current->Region=PbTestGuardAlloc(Current->Layout.RegionSize);
#else
    Current->Region=_aligned_malloc(Current->Layout.RegionSize,64);CHECK(Current->Region);
#endif
    memset(Current->Region,0,Current->Layout.RegionSize);Current->Mdl=IoAllocateMdl(Current->Region,Current->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Current->Mdl);Current->PagesLocked=TRUE;Regions=1;
    Current->StartEvent.Native=Event(TRUE);Current->StopEvent.Native=Event(FALSE);Current->AllReturned.Native=Event(TRUE);
    for(ULONG i=0;i<queues;++i){
        PB_KERNEL_QUEUE *q=Current->Queues+i;q->Session=Current;q->Index=i;q->KernelEvent=Object(manual);q->UserEvent=Object(manual);
        q->RxSlots=ExAllocatePool2(0,slots*sizeof(*q->RxSlots),0);q->Pending=ExAllocatePool2(0,slots*sizeof(*q->Pending),0);CHECK(q->RxSlots&&q->Pending);
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_RX_COMPLETE,Current->Region,&Current->Layout,i,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
        InterlockedIncrement(&Workers);q->Thread=Start(Worker,Current);InterlockedIncrement(&WorkerHandles);
    }
    Context->V2=Current;Context->Probe.V2SessionActive=1;JobCount=shape?1:queues;ExpectedWindows=shape==1?2:shape==2?3:queues;ExpectedNested=shape==3?1:0;
    for(ULONG i=0;i<JobCount;++i){
        JOB *job=Jobs+i;job->RefEntered=Event(FALSE);job->RefGate=Event(FALSE);job->SessionEntered=Event(FALSE);job->SessionGate=Event(FALSE);job->OuterEntered=Event(FALSE);job->OuterGate=Event(FALSE);job->Done=Event(FALSE);
        job->ExpectedWindows=shape==1?2:shape==2?3:1;
        job->Lists=Receive(i,0,!shape&&(i&1)?2:1);
    }
    if(shape==1){NET_BUFFER_LIST *tail=Jobs[0].Lists;for(ULONG i=1;i<65;++i){tail->Next=Receive(0,i,1);tail=tail->Next;}}
    if(shape==2){NET_BUFFER_LIST *middle=Receive(1,0,1),*last=Receive(0,1,1);Jobs[0].Lists->Next=middle;middle->Next=last;}
    if(shape==3)Reentrant=Receive(0,1,1);else Reentrant=NULL;
    AllWait=Event(FALSE);SessionWait=Event(FALSE);SessionGone=Event(FALSE);OuterWait=Event(FALSE);PoolReached=Event(FALSE);PoolGate=Event(FALSE);HaltDone=Event(FALSE);
    AfterRundownAcquire=Acquired;BeforeRundownRelease=SessionRelease;AfterQueueUnlock=Unlocked;BeforePoolFree=SessionResourcesFree;
}
static BOOLEAN Run(ULONG queues,ULONG lastReference,ULONG lastSessionPin,ULONG lastOuterPin,ULONG shape,BOOLEAN manual,BOOLEAN fault)
{
    Create(queues,shape,manual);CHECK(lastReference<JobCount&&lastSessionPin<JobCount&&lastOuterPin<JobCount);
    if(fault)PbV2Fault(Current);
    HANDLE callbacks[PB_MAX_QUEUES];for(ULONG i=0;i<JobCount;++i)callbacks[i]=Start(Callback,(VOID *)(ULONG_PTR)i);
    for(ULONG i=0;i<JobCount;++i)Await(Jobs[i].RefEntered);
    /* Stop admission first and finish all ownership bookkeeping before Halt.
     * This models a completed Pause: callbacks can still own their private
     * session/outer rundown tails after AllReturned is set. It does not require
     * NDIS to start Halt while a Pause still waits for Active references. */
    TUN_CTX *ctx=Context;KIRQL irql=ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    WriteRelease(&ctx->Running,FALSE);KeClearEvent(&ctx->Probe.RunningEvent);
    ExReleaseSpinLockExclusive(&ctx->TransitionLock,irql);
    for(ULONG rank=0;rank<JobCount;++rank){ULONG i=(lastReference+rank+1)%JobCount;CHECK(SetEvent(Jobs[i].RefGate));Await(Jobs[i].SessionEntered);}
    CHECK(Current&&!Current->Active&&Current->Returns.References==(LONG)JobCount&&Context->Probe.ReturnRundown.References==(LONG)JobCount);
    HANDLE halt=Start(Halt,ctx);Await(AllWait);Await(SessionWait);CHECK(!Context->V2&&!ContextFreed);
    for(ULONG rank=0;rank<JobCount;++rank){
        ULONG i=(lastSessionPin+rank+1)%JobCount;CHECK(SetEvent(Jobs[i].SessionGate));Await(Jobs[i].OuterEntered);
        if(rank+1<JobCount)CHECK(!SessionFreed&&!ContextFreed&&!PoolFreed);
    }
    HANDLE reached[]={OuterWait,PoolReached};DWORD where=WaitForMultipleObjects(2,reached,FALSE,10000);CHECK(where==WAIT_OBJECT_0||where==WAIT_OBJECT_0+1);
    CHECK(SessionFreed&&!Current&&!Regions&&!Workers&&!WorkerHandles&&!Mdls&&!Nbls&&Context&&!PoolFreed&&!ContextFreed);
    CHECK(Context->Probe.ReturnRundown.References==(LONG)JobCount&&Windows==(LONG)(ExpectedWindows+ExpectedNested)&&Nested==(LONG)ExpectedNested);
    if(where==WAIT_OBJECT_0)CHECK(OuterWaiting==1&&!Premature);else CHECK(Premature&&!OuterWaiting);
    for(ULONG rank=0;rank<JobCount;++rank){
        ULONG i=(lastOuterPin+rank+1)%JobCount;CHECK(SetEvent(Jobs[i].OuterGate));Await(Jobs[i].Done);Join(callbacks[i]);
        if(rank+1<JobCount)CHECK(!PoolFreed&&!ContextFreed);
    }
    CHECK(SetEvent(PoolGate));Join(halt);Await(HaltDone);
    CHECK(ContextFreed==1&&SessionFreed==1&&PoolFreed==1&&!Context&&!Pool&&!Pools&&!Mdls&&!Nbls&&!Regions&&!Workers&&!WorkerHandles&&!Objects&&OuterAcquires==OuterReleases);
    for(ULONG i=0;i<JobCount;++i){JOB *job=Jobs+i;Close(job->RefEntered);Close(job->RefGate);Close(job->SessionEntered);Close(job->SessionGate);Close(job->OuterEntered);Close(job->OuterGate);Close(job->Done);}
    Close(AllWait);Close(SessionWait);Close(SessionGone);Close(OuterWait);Close(PoolReached);Close(PoolGate);Close(HaltDone);
    AfterRundownAcquire=NULL;BeforeRundownRelease=NULL;AfterQueueUnlock=NULL;BeforePoolFree=NULL;CHECK(!Handles);++Scenarios;return Premature!=0;
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(argc==2){
        CHECK(!strcmp(argv[1],"1"));BOOLEAN bad=Run(8,7,1,4,0,FALSE,FALSE);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        if(bad){puts("HALT_SKIPPED_OUTER_RETURN_RUNDOWN");return 96;}
        puts("PASS: actual Halt retains pool/Ctx after session free until the final outer Return pin leaves");return 0;
    }
    const ULONG counts[]={1,2,8};
    for(ULONG c=0;c<_countof(counts);++c)for(ULONG last=0;last<counts[c];++last)for(ULONG delta=0;delta<counts[c];++delta)
        for(ULONG outerShift=0;outerShift<(counts[c]>1?2u:1u);++outerShift)
            for(ULONG manual=0;manual<2;++manual)for(ULONG fault=0;fault<2;++fault)
                CHECK(!Run(counts[c],last,(last+delta)%counts[c],(last+delta+outerShift)%counts[c],0,(BOOLEAN)manual,(BOOLEAN)fault));
    for(ULONG shape=1;shape<=3;++shape)for(ULONG manual=0;manual<2;++manual)for(ULONG fault=0;fault<2;++fault)
        CHECK(!Run(shape==2?2:1,0,0,0,shape,(BOOLEAN)manual,(BOOLEAN)fault));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual outer Return/Halt schedules; 1/2/8 concurrent queue callbacks, different last-reference/session-pin/outer-pin orders, session already freed with Ctx pinned, 65-NBL bounded chain, alternating-queue chain and nested callback, fault/normal stop, auto/manual events; all hosted resources and threads reclaimed, no native NDIS/IRQL claim\n",Scenarios);return 0;
}
