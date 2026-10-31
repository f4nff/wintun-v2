/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL queue and SYS receive/return/reference/QUERY/free paths. Hosted
 * DDIs, real Windows locks/events/threads. No native NDIS, IRQL or worker
 * execution, or full DLL session open/close, is claimed. */
#include "kernel_nbl_fixture.h"
typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock) {AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockShared(lock);}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static ULONG64 KeQueryInterruptTime(VOID) {return GetTickCount64()*10000u;}
#ifndef PB_RETURN_RECLAIM_QUERY_SOURCE
#define PB_RETURN_RECLAIM_QUERY_SOURCE "../../driver/v2/session_query.h"
#endif
#include PB_RETURN_RECLAIM_QUERY_SOURCE
#include "../../driver/v2/session_fault_drain.h"
#include "../../driver/v2/session_recycle.h"
static TUN_CTX Context;
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/session_state.h"
static PB_RESULT StatusInternal(PB_SESSION *s,PB_V2_STATUS *status)
{(void)s;return PbV2Query(&Context,status)==STATUS_SUCCESS?PB_OK:PB_STOPPED;}
#include "../../api/v2/session_queue.h"
#define NDIS_STATUS_PAUSED 1u
static ULONG ReceiveMask;
static NET_BUFFER_LIST *Indicated;
static ULONG PbEffectiveMask(TUN_CTX *ctx) {(void)ctx;return ReceiveMask;}
static ULONG PbBeginIndication(TUN_CTX *ctx) {(void)ctx;return 0;}
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) {(void)ctx;CHECK(!bank);}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *nbls,ULONG count,ULONG flags)
{CHECK(adapter && nbls && count==1 && !flags && !Indicated);Indicated=nbls;}
#ifndef PB_RETURN_RECLAIM_RECEIVE_SOURCE
#define PB_RETURN_RECLAIM_RECEIVE_SOURCE "../../driver/v2/session_receive.h"
#endif
#include PB_RETURN_RECLAIM_RECEIVE_SOURCE
#define Executive 0
#define KernelMode 0
static PB_KERNEL_SESSION *Current;
static NET_BUFFER_LIST *Returned;
static volatile LONG Objects,LockedRegions,RundownStarted,RundownBlocked,KernelNotifies;
static ULONG GatePosition;
static BOOL TailGated;
static LONG ExpectedKernelNotifies=1;
static HANDLE TailEntered,TailGate,AllWaitEntered,RundownWaitEntered,ReturnDone,CleanupDone;
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    CHECK(Current && event==&Current->AllReturned && !reason && !mode && !alertable && !timeout);
    CHECK(SetEvent(AllWaitEntered) && WaitForSingleObject(event->Native,5000)==WAIT_OBJECT_0);
}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{CHECK(thread && !alertable && !timeout && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);}
static VOID ZwClose(HANDLE thread) {CHECK(CloseHandle(thread));}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    CHECK(Current && refs==&Current->Returns);
    /* Beginning rundown with an NBL not yet returned would reject its future
     * callback pin. This observer rejects omission of the AllReturned barrier. */
    CHECK(!ReadAcquire(&Current->Active));
    CHECK(!InterlockedExchange(&RundownStarted,1));
    if(!TryAcquireSRWLockExclusive(&refs->Lock)) {
        InterlockedExchange(&RundownBlocked,1);CHECK(SetEvent(RundownWaitEntered));
        AcquireSRWLockExclusive(&refs->Lock);
    }
    CHECK(!refs->References);ReleaseSRWLockExclusive(&refs->Lock);
}
static VOID ObDereferenceObject(KEVENT *event)
{
    /* Cleanup releases event objects before pages. Check the first physical
     * resource release so a negative control cannot close a still-used event. */
    CHECK(Current && !Current->Active);
    CHECK(!ReadAcquire(&Current->Returns.References));
    CHECK(event && CloseHandle(event->Native) && InterlockedDecrement(&Objects)>=0);free(event);
}
static VOID MmUnlockPages(MDL *mdl)
{
    CHECK(Current && mdl==Current->Mdl && Current->PagesLocked && LockedRegions==1 && !Current->Active);
    /* Last notification/return tail still uses queue/session objects even when
     * Active is zero. Reject omission of the rundown wait before touching memory. */
    CHECK(!ReadAcquire(&Current->Returns.References));
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(mdl->Address);
#else
    CHECK(VirtualFree(mdl->Address,0,MEM_RELEASE));
#endif
    mdl->Address=NULL;InterlockedDecrement(&LockedRegions);
}
#ifndef PB_RETURN_RECLAIM_CLEANUP_SOURCE
#define PB_RETURN_RECLAIM_CLEANUP_SOURCE "../../driver/v2/session_cleanup.h"
#endif
#include PB_RETURN_RECLAIM_CLEANUP_SOURCE
static HANDLE Event(BOOL manual,BOOL initial)
{HANDLE event=CreateEventW(NULL,manual,initial,NULL);CHECK(event);return event;}
static KEVENT *ObjectEvent(BOOL manual)
{KEVENT *event=calloc(1,sizeof(*event));CHECK(event);event->Native=Event(manual,FALSE);InterlockedIncrement(&Objects);return event;}
static VOID *Pool(SIZE_T size)
{VOID *memory=ExAllocatePool2(0,size,0);CHECK(memory);return memory;}
static VOID Freed(VOID *memory)
{
    if(memory!=Current) return;
    CHECK(!Pools && !Mdls && !Nbls && !Objects && !LockedRegions && RundownStarted && !Current->Active && !Current->Returns.References);
    CHECK(KernelNotifies==ExpectedKernelNotifies && CloseHandle(Current->StartEvent.Native) && CloseHandle(Current->StopEvent.Native) && CloseHandle(Current->AllReturned.Native));
    Current=NULL;
}
static VOID Gate(VOID)
{
    CHECK(Current && !Current->Active && Current->Returns.References==1 && !Nbls && Mdls==1 && !Current->Queues[0].CoalesceBusy[0]);
    CHECK(WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_OBJECT_0 && !TailGated);
    TailGated=TRUE;CHECK(SetEvent(TailEntered) && WaitForSingleObject(TailGate,5000)==WAIT_OBJECT_0);
}
static VOID EventObserved(KEVENT *event)
{
    if(Current && event==Current->Queues[0].KernelEvent) {
        CHECK(InterlockedIncrement(&KernelNotifies)==1);
        if(!GatePosition) Gate(); /* Before the actual final queue notification. */
    }
}
static VOID Releasing(EX_RUNDOWN_REF *refs)
{CHECK(Current && refs==&Current->Returns);if(GatePosition) Gate();}
static VOID Acquired(EX_RUNDOWN_REF *refs)
{CHECK(Current && refs==&Current->Returns && !ReadAcquire(&RundownStarted));}
static DWORD WINAPI ReturnThread(VOID *unused)
{
    (void)unused;NET_BUFFER_LIST *head=Returned;
    CHECK(PbV2ReturnNbls(&Context,&head,PB_MAX_BATCH) && !head && SetEvent(ReturnDone));return 0;
}
static DWORD WINAPI CleanupThread(VOID *session)
{PbV2Free(session);CHECK(SetEvent(CleanupDone));return 0;}
static HANDLE DetachAndCleanup(VOID)
{
    AcquireSRWLockExclusive(&Context.TransitionLock);CHECK(Context.V2==Current);Context.V2=NULL;ReleaseSRWLockExclusive(&Context.TransitionLock);
    HANDLE thread=CreateThread(NULL,0,CleanupThread,Current,0,NULL);CHECK(thread);return thread;
}
static PB_KERNEL_QUEUE *CreateSessionQueues(ULONG queues,ULONG originals,BOOL manual,BOOL wrap)
{
    Current=Pool(sizeof(*Current));Current->Ctx=&Context;Current->Epoch=42;Context.V2=Current;
    Context.MiniportAdapterHandle=Context.NblPool=&Context;Context.Running=1;
    CHECK(PbCalculateLayout(queues,originals,originals,&Current->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    Current->Region=PbTestGuardAlloc(Current->Layout.RegionSize);memset(Current->Region,0,Current->Layout.RegionSize);
#else
    Current->Region=VirtualAlloc(NULL,Current->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(Current->Region);
#endif
    InterlockedIncrement(&LockedRegions);Current->PagesLocked=TRUE;
    Current->Mdl=IoAllocateMdl(Current->Region,Current->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Current->Mdl);
    Current->Lifecycle=(PB_SHARED_LIFECYCLE *)(Current->Region+Current->Layout.LifecycleOffset);
    Current->ProgressEvent=ObjectEvent(FALSE);
    Current->StartEvent.Native=Event(TRUE,FALSE);Current->StopEvent.Native=Event(TRUE,FALSE);Current->AllReturned.Native=Event(TRUE,TRUE);
    for(ULONG index=0;index<queues;++index) {
        PB_KERNEL_QUEUE *q=Current->Queues+index;q->Session=Current;q->Index=index;
        q->TxSlots=Pool(originals*sizeof(*q->TxSlots));q->RxSlots=Pool(originals*sizeof(*q->RxSlots));q->Pending=Pool(originals*sizeof(*q->Pending));
        q->Scratch=Pool(PB_BLOCK_SIZE);q->CoalesceBuffers[0]=Pool(PB_BLOCK_SIZE);q->CoalesceBuffers[1]=Pool(PB_BLOCK_SIZE);
        q->KernelEvent=ObjectEvent(manual);q->UserEvent=ObjectEvent(FALSE);
        CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,originals,42,(USHORT)index)==PB_OK);
        for(ULONG ring=0;ring<PB_QUEUE_COUNT;++ring) {
            CHECK(PbAttachQueue(q->Rings+ring,Current->Region,&Current->Layout,index,ring,ring==PB_QUEUE_TX_READY || ring==PB_QUEUE_RX_COMPLETE)==PB_OK);
            uint64_t base=wrap && ring==PB_QUEUE_RX_COMPLETE?UINT64_MAX-1:0;
            q->Rings[ring].Cursor=q->Rings[ring].PeerCursor=q->Rings[ring].Shared->Producer=q->Rings[ring].Shared->Consumer=base;
        }
    }
    return Current->Queues;
}
static PB_KERNEL_QUEUE *CreateSession(ULONG originals,BOOL manual,BOOL wrap)
{return CreateSessionQueues(1,originals,manual,wrap);}
static VOID Run(BOOL fault,ULONG originals,ULONG gate,BOOL early,BOOL manual,BOOL wrap)
{
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions);
    memset(&Context,0,sizeof(Context));RundownStarted=RundownBlocked=KernelNotifies=0;TailGated=FALSE;GatePosition=gate;ExpectedKernelNotifies=1;
    PB_KERNEL_QUEUE *q=CreateSession(originals,manual,wrap);
    uint64_t base=wrap?UINT64_MAX-1:0;PB_QUEUE_VIEW *complete=q->Rings+PB_QUEUE_RX_COMPLETE;
    PB_DESCRIPTOR items[PB_MAX_BATCH]={0};
    for(ULONG i=0;i<originals;++i) {
        items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=40,.DataOffset=Current->Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
        Current->Region[items[i].DataOffset]=0x45;uint32_t mayComplete;
        CHECK(PbAdmitReceive(&Current->Layout,42,0,q->RxSlots,items+i,0,&mayComplete)==PB_OK);++q->Credits;
    }
    PB_COALESCE coal={.Length=80,.IpVersion=4,.Protocol=6,.SegmentSize=10,.PayloadLength=40};
    if(originals>1) {q->CoalesceBuffers[0][0]=0x45;q->CoalesceBusy[0]=1;}
    Returned=PbV2Receive(q,items,originals,originals>1?0:MAXULONG,originals>1?&coal:NULL);CHECK(Returned);
    PbV2RetainReceives(Current,1,originals);CHECK(Current->Active==1 && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
    Current->Draining=Current->DrainSnapshotReady=1;Current->Deadline=GetTickCount64()+30000;
    if(fault) {PbV2Fault(Current);PbV2QuiesceFaultedQueue(q);CHECK(q->FaultQuiesced);}
    PB_V2_STATUS status;CHECK(PbV2Query(&Context,&status)==STATUS_SUCCESS && status.ActiveReferences==1 && status.State!=PB_SESSION_STOPPED);
    CHECK(ResetEvent(q->KernelEvent->Native));
    TailEntered=Event(TRUE,FALSE);TailGate=Event(TRUE,FALSE);AllWaitEntered=Event(TRUE,FALSE);RundownWaitEntered=Event(TRUE,FALSE);
    ReturnDone=Event(TRUE,FALSE);CleanupDone=Event(TRUE,FALSE);
    BeforePoolFree=Freed;OnSetEvent=EventObserved;BeforeRundownRelease=Releasing;AfterRundownAcquire=Acquired;
    HANDLE cleanup=NULL;
    if(early) {
        cleanup=DetachAndCleanup();CHECK(WaitForSingleObject(AllWaitEntered,5000)==WAIT_OBJECT_0);
        CHECK(!RundownStarted && !Current->Returns.References && Current->Active==1 && LockedRegions==1 && WaitForSingleObject(CleanupDone,0)==WAIT_TIMEOUT);
    }
    HANDLE callback=CreateThread(NULL,0,ReturnThread,NULL,0,NULL);CHECK(callback && WaitForSingleObject(TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(Current && Current->PagesLocked && Current->Returns.References==1 && !q->Credits && !q->PendingCount && !q->RxSlots[0].Active);
    CHECK(Current->RxReturned==originals && Current->Completions==(fault?0:originals));
    CHECK(complete->Cursor==base+(fault?0:originals) && complete->Shared->Consumer==base);
    CHECK(WaitForSingleObject(q->KernelEvent->Native,0)==(gate?WAIT_OBJECT_0:WAIT_TIMEOUT));
    if(!early) {
        /* Actual QUERY can acknowledge STOPPED while valid completions remain
         * unread, and before the return tail has released its rundown pin. */
        CHECK(PbV2Query(&Context,&status)==STATUS_SUCCESS && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
        cleanup=DetachAndCleanup();
    }
    memset(&status,0xa5,sizeof(status));PB_V2_STATUS sentinel=status;
    CHECK(PbV2Query(&Context,&status)==STATUS_DEVICE_NOT_READY && !memcmp(&status,&sentinel,sizeof(status)));
    CHECK(WaitForSingleObject(RundownWaitEntered,5000)==WAIT_OBJECT_0 && RundownStarted && RundownBlocked && Current && LockedRegions==1);
    CHECK(WaitForSingleObject(CleanupDone,0)==WAIT_TIMEOUT && Current->Returns.References==1 && !Current->Active);
    CHECK(SetEvent(TailGate) && WaitForSingleObject(ReturnDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(CleanupDone,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(callback,5000)==WAIT_OBJECT_0 && WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0 && CloseHandle(callback) && CloseHandle(cleanup));
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && TailGated && KernelNotifies==1);
    CHECK(CloseHandle(TailEntered) && CloseHandle(TailGate) && CloseHandle(AllWaitEntered) && CloseHandle(RundownWaitEntered) && CloseHandle(ReturnDone) && CloseHandle(CleanupDone));
    BeforePoolFree=NULL;OnSetEvent=NULL;BeforeRundownRelease=NULL;AfterRundownAcquire=NULL;
}
static DWORD ReuseThreadId;
static ULONG ReuseGatePosition,ReuseGates,ReuseUserNotifies;
static VOID ReuseGate(VOID)
{
    CHECK(!ReuseGates++ && Current->Returns.References==1 && !Nbls && Mdls==1);
    CHECK(SetEvent(TailEntered) && WaitForSingleObject(TailGate,5000)==WAIT_OBJECT_0);
}
static VOID ReuseUnlocked(KSPIN_LOCK *lock)
{
    if(GetCurrentThreadId()!=ReuseThreadId) return;
    if((!ReuseGatePosition && lock==&Current->Queues[0].RxLock) ||
        (ReuseGatePosition==1 && lock==&Current->ReferenceLock)) ReuseGate();
}
static VOID ReuseReleasing(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Current->Returns);if(ReuseGatePosition==2 && GetCurrentThreadId()==ReuseThreadId) ReuseGate();}
static VOID ReuseEvent(KEVENT *event)
{
    if(event==Current->Queues[0].KernelEvent) InterlockedIncrement(&KernelNotifies);
    if(event==Current->Queues[0].UserEvent) ++ReuseUserNotifies;
}
static DWORD WINAPI ReuseReturnThread(VOID *unused)
{ReuseThreadId=GetCurrentThreadId();return ReturnThread(unused);}
static VOID Put16(UCHAR *p,ULONG value) {p[0]=(UCHAR)(value>>8);p[1]=(UCHAR)value;}
static VOID PrepareGeneration(PB_USER_QUEUE *q,ULONG originals,ULONG format,ULONG generation,PB_LEASE *leases)
{
    uint32_t count;
    CHECK(ReserveBatch(q,leases,originals,&count)==PB_OK && count==originals);
    for(ULONG i=0;i<originals;++i) {
        CHECK(leases[i].Descriptor.Token.Slot==i && leases[i].Descriptor.Token.Generation==generation);
        UCHAR *p=leases[i].Data;ULONG ip=format==2?40:20,transport=format==2?8:20,length=ip+transport+16;
        memset(p,0,length);
        if(format==2) {p[0]=0x60;Put16(p+4,length-40);p[6]=17;p[7]=64;p[8]=0x20;p[24]=0x30;Put16(p+ip+4,length-ip);}
        else {
            p[0]=0x45;Put16(p+2,length);p[8]=64;p[9]=6;p[12]=10;p[16]=11;
            ULONG sequence=1000*generation+16*i;Put16(p+ip+4,sequence>>16);Put16(p+ip+6,sequence);
            p[ip+12]=0x50;p[ip+13]=0x10;Put16(p+ip+14,8192);
        }
        Put16(p+ip,4000);Put16(p+ip+2,5000);memset(p+ip+transport,(int)(0x40+8*generation+i),16);
        CHECK(PbFinalizeChecksums(p,length,format!=2,format!=2,format==2)==PB_PACKET_OK);
        leases[i].Descriptor.Length=length;
    }
    CHECK(q->Loans==(LONG)originals && !q->Published);
}
static VOID CommitGeneration(PB_USER_QUEUE *q,ULONG originals,ULONG format,ULONG generation)
{
    PB_LEASE leases[4];uint32_t accepted;PrepareGeneration(q,originals,format,generation,leases);
    CHECK(CommitBatch(q,leases,originals,&accepted)==PB_OK && accepted==originals && !q->Loans && q->Published==(LONG)originals);
    CHECK(PbV2ReceiveBatch(Current->Queues+q->Index) && Indicated && !Indicated->Next);
    PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(Indicated)&~(ULONG_PTR)1);
    CHECK(record->Count==originals && record->CoalesceIndex==(format?0:MAXULONG));
}
static VOID PollGenerationLoans(PB_USER_QUEUE *q,ULONG originals,ULONG generation,LONG loans)
{
    PB_DESCRIPTOR items[4];uint32_t count;
    CHECK(PollCompletions(q,items,originals,&count)==PB_OK && count==originals && !q->Published && q->Loans==loans);
    for(ULONG i=0;i<originals;++i)
        CHECK(items[i].Token.Epoch==42 && items[i].Token.Generation==generation && items[i].Token.Slot==i && items[i].Token.Queue==q->Index && items[i].Status==PB_OK);
    CHECK(PollCompletions(q,items,originals,&count)==PB_EMPTY && !count);
}
static VOID PollGeneration(PB_USER_QUEUE *q,ULONG originals,ULONG generation)
{PollGenerationLoans(q,originals,generation,0);}
static VOID CheckGeneration(PB_KERNEL_QUEUE *q,ULONG originals,BOOLEAN active)
{
    for(ULONG i=0;i<originals;++i) CHECK(q->RxSlots[i].Generation==2 && q->RxSlots[i].Active==active);
}
static PB_SESSION *CreateUser(ULONG originals,uint64_t base)
{
    PB_SESSION *user=calloc(1,sizeof(*user));CHECK(user);
    user->Layout=Current->Layout;user->Epoch=42;user->Region=Current->Region;user->SharedLifecycle=Current->Lifecycle;
    user->ClosingEvent=Event(TRUE,FALSE);user->ProgressEvent=Event(FALSE,FALSE);
    for(ULONG index=0;index<Current->Layout.QueueCount;++index) {
        PB_KERNEL_QUEUE *native=Current->Queues+index;
        PB_USER_QUEUE *q=user->Queues+index;q->Session=user;q->Index=index;q->KernelEvent=native->KernelEvent->Native;q->UserEvent=native->UserEvent->Native;
        q->RxSlots=calloc(originals,sizeof(*q->RxSlots));q->TxSlots=calloc(originals,sizeof(*q->TxSlots));CHECK(q->RxSlots && q->TxSlots);
        CHECK(PbPoolInitialize(&q->Rx,q->RxSlots,originals,42,(USHORT)index)==PB_OK && PbPoolInitialize(&q->Tx,q->TxSlots,originals,42,(USHORT)index)==PB_OK);
        for(ULONG ring=0;ring<PB_QUEUE_COUNT;++ring) {
            CHECK(PbAttachQueue(q->Rings+ring,user->Region,&user->Layout,index,ring,ring==PB_QUEUE_RX_SUBMIT || ring==PB_QUEUE_TX_RECYCLE)==PB_OK);
            q->Rings[ring].Cursor=q->Rings[ring].PeerCursor=base;
            native->Rings[ring].Cursor=native->Rings[ring].PeerCursor=native->Rings[ring].Shared->Producer=native->Rings[ring].Shared->Consumer=base;
        }
    }
    return user;
}
/* Raw fixture teardown also permits native-acknowledged, undelivered fault
 * bookkeeping. It never represents production DLL resource release. */
static VOID FreeUserStorage(PB_SESSION *user)
{
    CHECK(CloseHandle(user->ClosingEvent) && CloseHandle(user->ProgressEvent));
    for(ULONG index=0;index<user->Layout.QueueCount;++index) {
        CHECK(!user->Queues[index].Loans);free(user->Queues[index].RxSlots);free(user->Queues[index].TxSlots);
    }
    free(user);
}
static VOID FreeUser(PB_SESSION *user)
{for(ULONG index=0;index<user->Layout.QueueCount;++index) CHECK(!user->Queues[index].Published);FreeUserStorage(user);}
static VOID RunReuse(ULONG format,ULONG gate,BOOL oldFirst,BOOL manual,BOOL wrap)
{
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && !Indicated);
    memset(&Context,0,sizeof(Context));RundownStarted=RundownBlocked=KernelNotifies=0;ExpectedKernelNotifies=2;
    FixtureRundownAcquires=FixtureRundownReleases=0;
    ReuseThreadId=ReuseGates=ReuseUserNotifies=0;ReuseGatePosition=gate;
    ULONG originals=format?4:1;PB_KERNEL_QUEUE *native=CreateSession(originals,manual,wrap);
    PB_SESSION *user=CreateUser(originals,wrap?UINT64_MAX-1:0);PB_USER_QUEUE *q=user->Queues;
    ReceiveMask=format==1?PB_RX_RSC4:format==2?PB_RX_URO:0;
    CommitGeneration(q,originals,format,1);Returned=Indicated;Indicated=NULL;
    uint32_t ready;CHECK(PbQueueArm(q->Rings+PB_QUEUE_RX_COMPLETE,&ready)==PB_OK && !ready);
    TailEntered=Event(TRUE,FALSE);TailGate=Event(TRUE,FALSE);ReturnDone=Event(TRUE,FALSE);
    AfterQueueUnlock=ReuseUnlocked;BeforeRundownRelease=ReuseReleasing;OnSetEvent=ReuseEvent;
    HANDLE callback=CreateThread(NULL,0,ReuseReturnThread,NULL,0,NULL);CHECK(callback && WaitForSingleObject(TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(Current->Active==(gate?0:1) && Current->Returns.References==1 && !native->Credits && !native->PendingCount);
    CHECK(WaitForSingleObject(Current->AllReturned.Native,0)==(gate?WAIT_OBJECT_0:WAIT_TIMEOUT));
    CHECK(!native->CoalesceBusy[0] && !native->CoalesceBusy[1]);
    for(ULONG i=0;i<originals;++i) CHECK(!native->RxSlots[i].Active);
    PollGeneration(q,originals,1);CHECK(WaitForSingleObject(q->KernelEvent,0)==WAIT_OBJECT_0);
    CommitGeneration(q,originals,format,2);NET_BUFFER_LIST *fresh=Indicated;Indicated=NULL;
    UCHAR bytes[256];MDL *data=fresh->FirstMdl->Next;CHECK(data->Length<=sizeof(bytes));ULONG length=data->Length;memcpy(bytes,data->Address,length);
    CheckGeneration(native,originals,TRUE);
    CHECK(Current->Active==(gate?1:2) && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
    CHECK(native->CoalesceBusy[0]==(LONG)(format!=0) && !native->CoalesceBusy[1]);
    /* The old generation is stale even while the new generation owns the slot. */
    PB_DESCRIPTOR stale={.Token={42,1,0,0}};uint32_t mayComplete;KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&native->RxLock,&lock);
    CHECK(PbAdmitReceive(&Current->Layout,42,0,native->RxSlots,&stale,0,&mayComplete)==PB_STALE && !mayComplete);
    KeReleaseInStackQueuedSpinLock(&lock);
    KIRQL irql=ExAcquireSpinLockExclusive(&Context.TransitionLock);PbV2BeginDrain(Current,GetTickCount64()+30000);ExReleaseSpinLockExclusive(&Context.TransitionLock,irql);
    user->Draining=1;PB_V2_STATUS status;CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_DRAINING && status.ActiveReferences==(ULONG)(gate?1:2));
    BOOL exclusive=TryAcquireSRWLockExclusive(&Current->Returns.Lock);if(exclusive) ReleaseSRWLockExclusive(&Current->Returns.Lock);CHECK(!exclusive);
    CHECK(PbQueueArm(q->Rings+PB_QUEUE_RX_COMPLETE,&ready)==PB_OK && !ready);
    if(!oldFirst) {
        CHECK(PbV2ReturnOne(&Context,fresh));PollGeneration(q,originals,2);CheckGeneration(native,originals,FALSE);
        CHECK(Current->Active==(gate?0:1) && Current->Returns.References==1);
        CHECK(StatusInternal(user,&status)==PB_OK && status.State==(gate?PB_SESSION_STOPPED:PB_SESSION_DRAINING));
    }
    CHECK(SetEvent(TailGate) && WaitForSingleObject(ReturnDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(callback,5000)==WAIT_OBJECT_0 && CloseHandle(callback));
    AfterQueueUnlock=NULL;BeforeRundownRelease=NULL;
    CHECK(Current->Active==(oldFirst?1:0) && !Current->Returns.References);
    if(oldFirst) {
        /* No old tail may clear a second-generation slot or copied buffer lease. */
        CheckGeneration(native,originals,TRUE);CHECK(native->CoalesceBusy[0]==(LONG)(format!=0));
        CHECK(!memcmp(bytes,data->Address,length) && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
        CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_DRAINING && status.ActiveReferences==1);
        CHECK(PbV2ReturnOne(&Context,fresh));PollGeneration(q,originals,2);
    }
    CheckGeneration(native,originals,FALSE);
    CHECK(!native->CoalesceBusy[0] && !native->CoalesceBusy[1] && !native->Credits && !native->PendingCount);
    CHECK(!q->Loans && !q->Published && !user->Fatal && Current->RxObjects==2*originals && Current->RxReturned==2*originals && Current->Completions==2*originals);
    for(ULONG i=0;i<originals;++i) CHECK(q->RxSlots[i].Generation==2 && q->RxSlots[i].State==PB_SLOT_FREE);
    CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    CHECK(WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_OBJECT_0 && ReuseGates==1 && ReuseUserNotifies==2 && KernelNotifies==2);
    CHECK(FixtureRundownAcquires==2 && FixtureRundownReleases==2);
    CHECK(CloseHandle(TailEntered) && CloseHandle(TailGate) && CloseHandle(ReturnDone));FreeUser(user);
    AllWaitEntered=Event(TRUE,FALSE);
    Context.V2=NULL;BeforePoolFree=Freed;PbV2Free(Current);BeforePoolFree=NULL;OnSetEvent=NULL;
    CHECK(WaitForSingleObject(AllWaitEntered,0)==WAIT_OBJECT_0 && CloseHandle(AllWaitEntered));
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions);
}
static VOID PublishTerminalTx(PB_USER_QUEUE *q,ULONG originals)
{
    PB_KERNEL_QUEUE *native=Current->Queues;PB_DESCRIPTOR items[4]={0};KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&native->TxLock,&lock);
    for(ULONG i=0;i<originals;++i) {
        native->TxSlots[i].Generation=UINT32_MAX-1;
        CHECK(PbPoolReserve(&native->TxPool,&items[i].Token)==PB_OK && items[i].Token.Slot==i && items[i].Token.Generation==UINT32_MAX);
        items[i].Length=20;items[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;
        memset(Current->Region+items[i].DataOffset,(int)(0x60+i),20);
        CHECK(PbPoolTransition(&native->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    uint32_t count,notify;CHECK(PbQueuePush(native->Rings+PB_QUEUE_TX_READY,items,originals,&count,&notify)==PB_OK && count==originals);
    native->TxPoolExhausted=TRUE;KeReleaseInStackQueuedSpinLock(&lock);
    PB_LEASE leases[4];CHECK(ReceiveBatch(q,leases,originals,&count)==PB_OK && count==originals && q->Loans==(LONG)originals);
    for(ULONG i=0;i<originals;++i)
        CHECK(!memcmp(&leases[i].Descriptor,&items[i],sizeof(items[i])) && leases[i].Data==Current->Region+items[i].DataOffset && leases[i].Data[0]==0x60+i);
}
static VOID ReturnTerminalTx(PB_USER_QUEUE *q,ULONG originals)
{
    PB_KERNEL_QUEUE *native=Current->Queues;PB_TOKEN tokens[4];uint32_t accepted,ready;KLOCK_QUEUE_HANDLE lock;
    for(ULONG i=0;i<originals;++i) tokens[i]=(PB_TOKEN){42,UINT32_MAX,(USHORT)i,0};
    KeClearEvent(native->KernelEvent);CHECK(PbQueueArm(native->Rings+PB_QUEUE_TX_RECYCLE,&ready)==PB_OK && !ready);
    CHECK(ReleaseBatch(q,tokens,originals,&accepted)==PB_OK && accepted==originals && !q->Loans);
    CHECK(WaitForSingleObject(native->KernelEvent->Native,0)==WAIT_OBJECT_0);
    KeAcquireInStackQueuedSpinLock(&native->TxLock,&lock);
    CHECK(PbV2RecycleLocked(native)==PB_OK && !native->TxPoolExhausted && !Current->Faulted);
    PB_TOKEN sentinel,unchanged;memset(&sentinel,0xa5,sizeof(sentinel));unchanged=sentinel;
    CHECK(PbPoolReserve(&native->TxPool,&sentinel)==PB_FULL && !memcmp(&sentinel,&unchanged,sizeof(sentinel)));
    CHECK(PbV2RecycleLocked(native)==PB_OK && !native->TxPool.FreeBits[0]);
    for(ULONG i=0;i<originals;++i) CHECK(native->TxSlots[i].Generation==UINT32_MAX && native->TxSlots[i].State==PB_SLOT_RETIRED);
    KeReleaseInStackQueuedSpinLock(&lock);
    for(ULONG i=0;i<originals;++i) CHECK(q->TxSlots[i].Generation==UINT32_MAX && q->TxSlots[i].State==PB_SLOT_FREE);
    uint64_t cursor=q->Rings[PB_QUEUE_TX_RECYCLE].Cursor;
    CHECK(ReleaseBatch(q,tokens,originals,&accepted)==PB_BUSY && !accepted && !q->Loans && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==cursor && !q->Session->Fatal);
}
static VOID ReturnTerminalRx(PB_USER_QUEUE *q,ULONG originals)
{
    LONG loans=q->Loans;
    CHECK(Indicated && PbV2ReturnOne(&Context,Indicated));Indicated=NULL;
    PollGenerationLoans(q,originals,UINT32_MAX,loans);
    CHECK(!Current->Active && !Current->Returns.References && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_OBJECT_0);
    for(ULONG i=0;i<originals;++i) CHECK(Current->Queues[0].RxSlots[i].Generation==UINT32_MAX && !Current->Queues[0].RxSlots[i].Active);
}
static VOID BeginTerminalDrain(PB_SESSION *user)
{
    KIRQL irql=ExAcquireSpinLockExclusive(&Context.TransitionLock);
    PbV2BeginDrain(Current,GetTickCount64()+30000);ExReleaseSpinLockExclusive(&Context.TransitionLock,irql);
    PbV2Progress(Current,TRUE);user->Draining=1;
}
static VOID RunTerminal(ULONG format,BOOL rxFirst,BOOL manual,BOOL wrap)
{
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && !Indicated);
    memset(&Context,0,sizeof(Context));RundownStarted=RundownBlocked=KernelNotifies=0;ExpectedKernelNotifies=1;ReuseUserNotifies=0;
    FixtureRundownAcquires=FixtureRundownReleases=0;
    ULONG originals=format?4:1;PB_KERNEL_QUEUE *native=CreateSession(originals,manual,wrap);
    PB_SESSION *user=CreateUser(originals,wrap?UINT64_MAX:0);PB_USER_QUEUE *q=user->Queues;
    /* Seed a valid, idle frontier rather than executing billions of rounds. */
    for(ULONG i=0;i<originals;++i) q->RxSlots[i].Generation=native->RxSlots[i].Generation=UINT32_MAX-1;
    ReceiveMask=format==1?PB_RX_RSC4:format==2?PB_RX_URO:0;
    CommitGeneration(q,originals,format,UINT32_MAX);PublishTerminalTx(q,originals);
    CHECK(Current->Active==1 && q->Published==(LONG)originals && q->Loans==(LONG)originals);
    uint32_t ready,count;CHECK(PbQueueArm(q->Rings+PB_QUEUE_RX_COMPLETE,&ready)==PB_OK && !ready);OnSetEvent=ReuseEvent;
    PB_V2_STATUS status;
    if(rxFirst) {
        ReturnTerminalRx(q,originals);
        /* Running DLL allocation permanently retires exhausted RX slots, with
         * no new loan or submission. TX leases remain independently owned. */
        PB_LEASE leases[4];uint64_t cursor=q->Rings[PB_QUEUE_RX_SUBMIT].Cursor;
        CHECK(ReserveBatch(q,leases,originals,&count)==PB_FULL && !count && q->Loans==(LONG)originals && !q->Rx.FreeBits[0] && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==cursor);
        BeginTerminalDrain(user);
        CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences && q->Loans==(LONG)originals);
        ReturnTerminalTx(q,originals);
    } else {
        ReturnTerminalTx(q,originals);BeginTerminalDrain(user);
        CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_DRAINING && status.ActiveReferences==1 && !q->Loans && q->Published==(LONG)originals);
        ReturnTerminalRx(q,originals);
    }
    /* Stopped admission must not reactivate or even retire additional slots. */
    PB_POOL pool=q->Rx;PB_SLOT slots[4];memcpy(slots,q->RxSlots,originals*sizeof(*slots));PB_LEASE leases[4];
    CHECK(ReserveBatch(q,leases,originals,&count)==PB_STOPPED && !count && !q->Loans && !memcmp(&pool,&q->Rx,sizeof(pool)) && !memcmp(slots,q->RxSlots,originals*sizeof(*slots)));
    for(ULONG i=0;i<originals;++i) CHECK(q->RxSlots[i].Generation==UINT32_MAX && q->RxSlots[i].State==(rxFirst?PB_SLOT_RETIRED:PB_SLOT_FREE));
    CHECK(!q->Published && !user->Fatal && !Current->Faulted && !native->Credits && !native->PendingCount && !native->CoalesceBusy[0] && !native->CoalesceBusy[1]);
    CHECK(Current->RxObjects==originals && Current->RxReturned==originals && Current->Completions==originals && FixtureRundownAcquires==1 && FixtureRundownReleases==1 && KernelNotifies==1 && ReuseUserNotifies==2);
    for(ULONG ring=0;ring<PB_QUEUE_COUNT;++ring) {
        uint64_t expected=(wrap?UINT64_MAX:0)+originals;
        CHECK(q->Rings[ring].Cursor==expected && native->Rings[ring].Cursor==expected && q->Rings[ring].Shared->Producer==expected && q->Rings[ring].Shared->Consumer==expected);
    }
    CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    FreeUser(user);AllWaitEntered=Event(TRUE,FALSE);Context.V2=NULL;BeforePoolFree=Freed;PbV2Free(Current);BeforePoolFree=NULL;OnSetEvent=NULL;
    CHECK(WaitForSingleObject(AllWaitEntered,0)==WAIT_OBJECT_0 && CloseHandle(AllWaitEntered) && !Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions);
}
typedef struct MULTI_JOB {
    ULONG Index;NET_BUFFER_LIST *Nbl;volatile LONG ThreadId;
    HANDLE RxEntered,RxGate,TailEntered,TailGate,Done;
} MULTI_JOB;
static MULTI_JOB MultiJobs[2];
static __declspec(thread) ULONG MultiIndex;
static volatile LONG MultiKernel[2],MultiUser[2],MultiReturnedSignals;
static VOID MultiUnlocked(KSPIN_LOCK *lock)
{
    for(ULONG i=0;i<2;++i) if((LONG)GetCurrentThreadId()==MultiJobs[i].ThreadId && lock==&Current->Queues[i].RxLock)
        CHECK(SetEvent(MultiJobs[i].RxEntered) && WaitForSingleObject(MultiJobs[i].RxGate,5000)==WAIT_OBJECT_0);
}
static VOID MultiReleasing(EX_RUNDOWN_REF *refs)
{
    MULTI_JOB *job=MultiJobs+MultiIndex;
    CHECK(refs==&Current->Returns && (LONG)GetCurrentThreadId()==job->ThreadId);
    CHECK(SetEvent(job->TailEntered) && WaitForSingleObject(job->TailGate,5000)==WAIT_OBJECT_0);
}
static VOID MultiEvent(KEVENT *event)
{
    if(event==&Current->AllReturned) InterlockedIncrement(&MultiReturnedSignals);
    for(ULONG i=0;i<2;++i) {
        if(event==Current->Queues[i].KernelEvent) {InterlockedIncrement(&KernelNotifies);InterlockedIncrement(MultiKernel+i);}
        if(event==Current->Queues[i].UserEvent) InterlockedIncrement(MultiUser+i);
    }
}
static DWORD WINAPI MultiReturn(VOID *opaque)
{
    MULTI_JOB *job=opaque;MultiIndex=job->Index;job->ThreadId=(LONG)GetCurrentThreadId();
    CHECK(PbV2ReturnOne(&Context,job->Nbl) && SetEvent(job->Done));return 0;
}
static VOID RunMulti(BOOL fault,ULONG firstReference,ULONG firstPin,BOOL manual,BOOL wrap)
{
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && !Indicated);
    memset(&Context,0,sizeof(Context));memset(MultiJobs,0,sizeof(MultiJobs));
    memset((VOID *)MultiKernel,0,sizeof(MultiKernel));memset((VOID *)MultiUser,0,sizeof(MultiUser));
    RundownStarted=RundownBlocked=KernelNotifies=MultiReturnedSignals=0;ExpectedKernelNotifies=fault?3:1;
    FixtureRundownAcquires=FixtureRundownReleases=0;
    CreateSessionQueues(2,4,manual,wrap);PB_SESSION *user=CreateUser(4,wrap?UINT64_MAX:0);
    for(ULONG i=0;i<2;++i) {
        MULTI_JOB *job=MultiJobs+i;job->Index=i;ReceiveMask=i?PB_RX_RSC4:0;
        CommitGeneration(user->Queues+i,i?4:1,i?1:0,1);job->Nbl=Indicated;Indicated=NULL;
        uint32_t ready;CHECK(PbQueueArm(user->Queues[i].Rings+PB_QUEUE_RX_COMPLETE,&ready)==PB_OK && !ready);
        job->RxEntered=Event(TRUE,FALSE);job->RxGate=Event(TRUE,FALSE);job->TailEntered=Event(TRUE,FALSE);job->TailGate=Event(TRUE,FALSE);job->Done=Event(TRUE,FALSE);
    }
    CHECK(Current->Active==2 && Current->RxObjects==5 && Nbls==2);
    OnSetEvent=MultiEvent;
    if(fault) PbV2Fault(Current);else BeginTerminalDrain(user);
    AfterQueueUnlock=MultiUnlocked;BeforeRundownRelease=MultiReleasing;AfterRundownAcquire=Acquired;
    HANDLE callbacks[2]={CreateThread(NULL,0,MultiReturn,MultiJobs,0,NULL),CreateThread(NULL,0,MultiReturn,MultiJobs+1,0,NULL)};
    CHECK(callbacks[0] && callbacks[1]);
    HANDLE entered[2]={MultiJobs[0].RxEntered,MultiJobs[1].RxEntered};CHECK(WaitForMultipleObjects(2,entered,TRUE,5000)==WAIT_OBJECT_0);
    /* Both queue transactions ended and both data objects are gone. The two
     * global references and two independent rundown pins still exist. */
    CHECK(Current->Active==2 && Current->Returns.References==2 && !Nbls && Mdls==1 && !MultiReturnedSignals && !Current->RxReturned);
    for(ULONG i=0;i<2;++i) CHECK(!Current->Queues[i].Credits && !Current->Queues[i].PendingCount && !Current->Queues[i].CoalesceBusy[0]);
    PB_V2_STATUS status;CHECK(StatusInternal(user,&status)==PB_OK && status.State!=PB_SESSION_STOPPED && status.ActiveReferences==2 && status.QueueCount==2);
    CHECK(SetEvent(MultiJobs[firstReference].RxGate) && WaitForSingleObject(MultiJobs[firstReference].TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(Current->Active==1 && Current->Returns.References==2 && !MultiReturnedSignals && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
    CHECK(StatusInternal(user,&status)==PB_OK && status.State!=PB_SESSION_STOPPED && status.ActiveReferences==1);
    CHECK(SetEvent(MultiJobs[firstReference^1].RxGate) && WaitForSingleObject(MultiJobs[firstReference^1].TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(!Current->Active && Current->Returns.References==2 && MultiReturnedSignals==1 && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_OBJECT_0);
    CHECK(Current->RxReturned==5 && Current->Completions==(fault?0:5) && Current->Ctx->Statistics.ifHCInUcastPkts==5 && !Nbls && Mdls==1);
    if(fault) {
        PbV2QuiesceFaultedQueue(Current->Queues);
        CHECK(StatusInternal(user,&status)==PB_OK && status.State!=PB_SESSION_STOPPED && !status.ActiveReferences && !Current->Queues[1].FaultQuiesced);
        PbV2QuiesceFaultedQueue(Current->Queues+1);
        CHECK(user->Queues[0].Published==1 && user->Queues[1].Published==4 && MultiUser[0]==1 && MultiUser[1]==1);
    } else {
        PollGeneration(user->Queues,1,1);PollGeneration(user->Queues+1,4,1);
        CHECK(MultiUser[0]==2 && MultiUser[1]==2);
    }
    CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    CHECK(MultiKernel[0]==(fault?1:0) && MultiKernel[1]==(fault?2:1));
    /* Fault bookkeeping is test-owned and undelivered; the actual STOPPED
     * query above authorizes fixture teardown, not a modeled DLL Close. */
    if(fault) FreeUserStorage(user);else FreeUser(user);
    AllWaitEntered=Event(TRUE,FALSE);RundownWaitEntered=Event(TRUE,FALSE);CleanupDone=Event(TRUE,FALSE);
    BeforePoolFree=Freed;HANDLE cleanup=DetachAndCleanup();
    CHECK(WaitForSingleObject(RundownWaitEntered,5000)==WAIT_OBJECT_0 && RundownStarted && RundownBlocked && Current && LockedRegions==1 && Current->Returns.References==2);
    CHECK(WaitForSingleObject(CleanupDone,0)==WAIT_TIMEOUT);
    CHECK(SetEvent(MultiJobs[firstPin].TailGate) && WaitForSingleObject(MultiJobs[firstPin].Done,5000)==WAIT_OBJECT_0 && WaitForSingleObject(callbacks[firstPin],5000)==WAIT_OBJECT_0);
    CHECK(Current && Current->Returns.References==1 && LockedRegions==1 && Objects==5 && Mdls==1 && WaitForSingleObject(CleanupDone,0)==WAIT_TIMEOUT);
    CHECK(SetEvent(MultiJobs[firstPin^1].TailGate) && WaitForSingleObject(MultiJobs[firstPin^1].Done,5000)==WAIT_OBJECT_0 && WaitForSingleObject(callbacks[firstPin^1],5000)==WAIT_OBJECT_0 && WaitForSingleObject(CleanupDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0);
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && FixtureRundownAcquires==2 && FixtureRundownReleases==2 && MultiReturnedSignals==1);
    CHECK(CloseHandle(callbacks[0]) && CloseHandle(callbacks[1]) && CloseHandle(cleanup) && CloseHandle(AllWaitEntered) && CloseHandle(RundownWaitEntered) && CloseHandle(CleanupDone));
    for(ULONG i=0;i<2;++i) CHECK(CloseHandle(MultiJobs[i].RxEntered) && CloseHandle(MultiJobs[i].RxGate) && CloseHandle(MultiJobs[i].TailEntered) && CloseHandle(MultiJobs[i].TailGate) && CloseHandle(MultiJobs[i].Done));
    AfterQueueUnlock=NULL;BeforeRundownRelease=NULL;AfterRundownAcquire=NULL;BeforePoolFree=NULL;OnSetEvent=NULL;
}
static VOID RunAllocation(ULONG format,ULONG failure,BOOL manual,BOOL wrap)
{
    CHECK(!Current && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions && !Indicated && !Failure);
    memset(&Context,0,sizeof(Context));memset((VOID *)MultiKernel,0,sizeof(MultiKernel));memset((VOID *)MultiUser,0,sizeof(MultiUser));
    RundownStarted=RundownBlocked=KernelNotifies=MultiReturnedSignals=0;
    FixtureRundownAcquires=FixtureRundownReleases=0;
    CreateSessionQueues(2,4,manual,wrap);PB_SESSION *user=CreateUser(4,wrap?UINT64_MAX-1:0);
    PB_USER_QUEUE *held=user->Queues,*q=held+1;PB_KERNEL_QUEUE *native=Current->Queues+1;
    ReceiveMask=0;CommitGeneration(held,1,0,1);NET_BUFFER_LIST *old=Indicated;Indicated=NULL;
    CHECK(Current->Active==1 && Mdls==3 && Nbls==1);LONG heldPools=Pools;
    ULONG originals=format?4:1;ReceiveMask=format==1?PB_RX_RSC4:format==2?PB_RX_URO:0;
    uint32_t ready;CHECK(PbQueueArm(q->Rings+PB_QUEUE_RX_COMPLETE,&ready)==PB_OK && !ready);
    PB_LEASE leases[4];uint32_t count,accepted;PrepareGeneration(q,originals,format,1,leases);
    CHECK(CommitBatch(q,leases,originals,&accepted)==PB_OK && accepted==originals && !q->Loans && q->Published==(LONG)originals);
    OnSetEvent=MultiEvent;Acquisitions=0;Failure=failure;
    CHECK(PbV2ReceiveBatch(native) && !Indicated && Acquisitions==failure);Failure=0;
    /* All four allocation failures unwind only the unbuilt group. They must
     * neither retain a reference nor return the NBL held on the other queue. */
    CHECK(!native->Credits && !native->PendingCount);
    CHECK(!native->CoalesceBusy[0] && !native->CoalesceBusy[1]);
    CHECK(Current->Active==1 && Current->RxObjects==1 && !Current->RxReturned && !Current->Returns.References);
    CHECK(!FixtureRundownAcquires && !FixtureRundownReleases && Pools==heldPools && Mdls==3 && Nbls==1);
    CHECK(Current->Queues[0].RxSlots[0].Active && !MultiReturnedSignals && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(q->KernelEvent,0)==WAIT_TIMEOUT);
    PB_DESCRIPTOR items[4];CHECK(PollCompletions(q,items,originals,&count)==PB_OK && count==originals && !q->Published && !q->Loans && !user->Fatal);
    for(ULONG i=0;i<originals;++i) CHECK(items[i].Token.Epoch==42 && items[i].Token.Queue==1 && items[i].Token.Slot==i && items[i].Token.Generation==1 && items[i].Status==PB_FULL && !native->RxSlots[i].Active);
    CHECK(MultiUser[1]==1 && !MultiKernel[1] && WaitForSingleObject(q->KernelEvent,0)==WAIT_OBJECT_0 && ResetEvent(q->KernelEvent));
    /* Reserve uses the same slots again, proving both DLL ownership and the
     * native/coalesced leases survived the failed generation correctly. */
    q->Rx.Next=0;CommitGeneration(q,originals,format,2);NET_BUFFER_LIST *fresh=Indicated;Indicated=NULL;
    CHECK(Current->Active==2 && Current->RxObjects==1+originals && !MultiReturnedSignals);
    KIRQL irql=ExAcquireSpinLockExclusive(&Context.TransitionLock);PbV2BeginDrain(Current,GetTickCount64()+30000);ExReleaseSpinLockExclusive(&Context.TransitionLock,irql);
    PB_V2_STATUS status;CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_DRAINING && status.ActiveReferences==2);
    CHECK(PbV2ReturnOne(&Context,fresh));PollGeneration(q,originals,2);
    CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_DRAINING && status.ActiveReferences==1 && held->Published==1 && Current->Queues[0].RxSlots[0].Active);
    CHECK(!MultiReturnedSignals && WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_TIMEOUT);
    CHECK(PbV2ReturnOne(&Context,old));PollGeneration(held,1,1);
    CHECK(StatusInternal(user,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    CHECK(Current->RxReturned==1+originals && Current->Completions==1+2*originals && !Current->Faulted && MultiReturnedSignals==1);
    CHECK(FixtureRundownAcquires==2 && FixtureRundownReleases==2);
    ExpectedKernelNotifies=KernelNotifies;CHECK(!MultiKernel[0] && MultiKernel[1]==(LONG)(format!=0));FreeUser(user);
    AllWaitEntered=Event(TRUE,FALSE);RundownWaitEntered=Event(TRUE,FALSE);CleanupDone=Event(TRUE,FALSE);
    BeforePoolFree=Freed;HANDLE cleanup=DetachAndCleanup();
    CHECK(WaitForSingleObject(CleanupDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0 && !Current && !RundownBlocked && !Pools && !Mdls && !Nbls && !Objects && !LockedRegions);
    CHECK(CloseHandle(cleanup) && CloseHandle(AllWaitEntered) && CloseHandle(RundownWaitEntered) && CloseHandle(CleanupDone));
    BeforePoolFree=NULL;OnSetEvent=NULL;
}
int main(int argc,char **argv)
{
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--reuse-only") || !strcmp(argv[1],"--terminal-only") || !strcmp(argv[1],"--multi-only") || !strcmp(argv[1],"--allocation-only"))));
    BOOL terminalOnly=argc==2 && !strcmp(argv[1],"--terminal-only");
    BOOL multiOnly=argc==2 && !strcmp(argv[1],"--multi-only");
    BOOL allocationOnly=argc==2 && !strcmp(argv[1],"--allocation-only");
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    /* Early cleanup first makes both isolated observer controls deterministic. */
    if(argc==1) for(ULONG schedule=0;schedule<64;++schedule)
        Run((schedule&1)!=0,(schedule&2)?4:1,(schedule>>2)&1,(schedule&8)==0,(schedule&16)!=0,(schedule&32)!=0);
    if(!terminalOnly && !multiOnly && !allocationOnly) for(ULONG format=0;format<3;++format) for(ULONG gate=0;gate<3;++gate) for(ULONG schedule=0;schedule<8;++schedule)
        RunReuse(format,gate,(schedule&1)!=0,(schedule&2)!=0,(schedule&4)!=0);
    if(argc==1 || terminalOnly) for(ULONG format=0;format<3;++format) for(ULONG schedule=0;schedule<8;++schedule)
        RunTerminal(format,(schedule&1)!=0,(schedule&2)!=0,(schedule&4)!=0);
    if(argc==1 || multiOnly) for(ULONG schedule=0;schedule<32;++schedule)
        RunMulti((schedule&16)!=0,schedule&1,(schedule>>1)&1,(schedule&4)!=0,(schedule&8)!=0);
    if(argc==1 || allocationOnly) for(ULONG format=0;format<3;++format) for(ULONG failure=1;failure<=4;++failure) for(ULONG schedule=0;schedule<4;++schedule)
        RunAllocation(format,failure,(schedule&1)!=0,(schedule&2)!=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    if(argc==1) puts("PASS: 64 actual return/QUERY/cleanup schedules: drain/fault, ordinary/coalesced NBL, pre-notify/pre-rundown tail, early/late detach, sync/manual event and cursor wrap; Active/AllReturned cannot replace rundown; unread completions can coexist with STOPPED; zero tracked resources/handles.");
    if(!terminalOnly && !multiOnly && !allocationOnly) puts("PASS: 72 actual DLL/SYS slot-reuse schedules: Reserve/Commit/Receive/Return/Poll/QUERY/Free, same slots generation 1 -> 2, plain/RSC4/URO6, pre-reference/post-reference/pre-rundown old tail, both return orders, sync/manual event and cursor wrap; exact ownership, completion generation, copied bytes, native wakes and balanced resources/handles.");
    if(argc==1 || terminalOnly) puts("PASS: 24 actual terminal-generation schedules: final UINT32_MAX RX/TX tokens, DLL receive/release/reserve/commit/poll, SYS receive/return/recycle/QUERY/free, plain/RSC4/URO6, both return orders, sync/manual event and cursor wrap; permanent retirement, unchanged full token, STOPPED with a user TX loan, stopped admission and balanced resources/handles.");
    if(argc==1 || multiOnly) puts("PASS: 32 actual multi-queue return schedules: concurrent ordinary/coalesced callbacks, independent last-reference/last-pin orders, drain/fault, sync/manual event and cursor wrap; both fault witnesses required, one AllReturned signal, cleanup blocks with two pins and with one, exact queue completions and balanced resources/handles.");
    if(argc==1 || allocationOnly) puts("PASS: 48 actual DLL/SYS allocation-failure schedules: all four NBL construction allocations, plain/RSC4/URO6, held NBL on another queue, sync/manual event and cursor wrap; PB_FULL completions retire only failed slots, same-slot generation 2 succeeds, held owner prevents STOPPED, exact reference/event/resource/handle balance.");return 0;
}
