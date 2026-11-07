/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL Commit/lifecycle and SYS finite DRAIN/QUERY/Free. Registration,
 * idle-only native workers, device IO and IRQL are hosted in the reused ledger. */
#ifdef PB_API_TESTS
#error Late Commit tests require shipping DLL paths
#endif
#if !defined(PB_LATE_RECLAIM_HOST) || !defined(PB_LATE_ARM_SOURCE)
#error Build through Build-LateCommitTests for explicit source binding
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../common/v2_core.h"
static PB_RESULT LatePush(PB_QUEUE_VIEW *,const PB_DESCRIPTOR *,uint32_t,uint32_t *,uint32_t *);
static VOID WINAPI LateRelease(PSRWLOCK);
static VOID WINAPI LateAcquire(PSRWLOCK);
#define main ReusedNativeReclaimMain
#include PB_LATE_RECLAIM_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"late Commit requires shipping source fingerprint");
#include PB_LATE_ARM_SOURCE
static HANDLE CommitEntered,CommitGate,BookEntered,BookGate,WriterEntered;
static volatile LONG OwnerId,Armed,PushFinished,BookHeld;
static ULONG Target,Batch,LateScenarios,UnexpectedReady;
static uint64_t BaseCursor;
static PB_RESULT LatePush(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    BOOL selected=User&&Armed&&(LONG)GetCurrentThreadId()==OwnerId&&view==User->Queues[Target].Rings+PB_QUEUE_RX_SUBMIT;
    if(selected){CHECK(count==Batch&&!User->CloseRequested&&!User->Draining&&User->Queues[Target].Loans==(LONG)Batch&&!User->Queues[Target].Published);CHECK(SetEvent(CommitEntered));Await(CommitGate);}
    PB_RESULT result=PbQueuePush(view,items,count,accepted,notify);
    if(selected){CHECK(result==PB_OK&&*accepted==Batch&&User->CloseRequested&&Native->DrainSnapshotReady&&Native->Draining);InterlockedExchange(&PushFinished,1);}
    return result;
}
static VOID WINAPI LateRelease(PSRWLOCK lock)
{
    if(User&&lock==&User->DataGate&&(LONG)GetCurrentThreadId()==OwnerId&&PushFinished&&InterlockedCompareExchange(&Armed,0,1)==1){
        CHECK(!User->Queues[Target].Loans&&User->Queues[Target].Published==(LONG)Batch&&!TryAcquireSRWLockExclusive(lock));
        InterlockedExchange(&BookHeld,1);CHECK(SetEvent(BookEntered));Await(BookGate);InterlockedExchange(&BookHeld,0);
    }
    ReleaseSRWLockShared(lock);
}
static VOID WINAPI LateAcquire(PSRWLOCK lock)
{
    if(User&&lock==&User->DataGate&&Armed){CHECK(!TryAcquireSRWLockExclusive(lock));CHECK(SetEvent(WriterEntered));}
    AcquireSRWLockExclusive(lock);
}
static DWORD WINAPI LateOwner(VOID *unused)
{
    (void)unused;OwnerId=(LONG)GetCurrentThreadId();PB_USER_QUEUE *q;PB_LEASE leases[64];uint32_t count;
    CHECK(Api.OpenQueue(User,Target,&q)==PB_OK&&Api.ReserveBatch(q,leases,Batch,&count)==PB_OK&&count==Batch);
    for(ULONG i=0;i<Batch;++i){leases[i].Descriptor.Length=20;memset(leases[i].Data,0,20);leases[i].Data[0]=0x45;leases[i].Data[3]=20;}
    CHECK(Api.CommitBatch(q,leases,Batch,&count)==PB_OK&&count==Batch);
    /* Lifecycle can already have reconciled Published after QueueLeave.
     * Only caller-owned outputs and further valid same-owner calls are used. */
    PB_LEASE rejected;memset(&rejected,0xa5,sizeof(rejected));CHECK(Api.ReserveBatch(q,&rejected,1,&count)==PB_STOPPED&&!count);
    CHECK(Api.Wait(q,0)==PB_STOPPED);return 0;
}
static VOID OpenPeer(ULONG queues,BOOL manual,BOOL wrap)
{
    CHECK(!User&&!Native&&!Context.V2&&!DllAllocations&&!Regions&&!DllEvents&&!Modules&&!Works&&!WorkPools&&!Devices&&
        !NativeObjects&&!NativeWorkers&&!LockedPages&&!OtherHandles&&!CallbackThreadCount&&!WorkCallback&&!Pools&&!Mdls&&!Nbls&&!RuntimeCount&&!RuntimeBytes&&!RuntimeAdapterCount);
    memset(&Context,0,sizeof(Context));memset(Indicated,0,sizeof(Indicated));ManualEvents=manual;WrapCursors=wrap;GateTail=FALSE;EventOrdinal=0;
    Context.Running=1;Context.NblPool=Context.MiniportAdapterHandle=&Context;Context.Device.Disconnected.Native=Event(TRUE,FALSE);
    QueryCalls=LastQueryState=LastQueryReferences=CloseCalls=RundownWaits=UnsafeObserved=0;Progress=Device=NULL;
    Failure=Acquisitions=0;BeforePoolFree=NativeFreed;BeforeRundownRelease=ReturnTail;
    ParkEntered=Event(FALSE,FALSE);ParkGate=Event(FALSE,FALSE);TailEntered=Event(TRUE,FALSE);TailGate=Event(TRUE,FALSE);
    RundownEntered=Event(TRUE,FALSE);UnsafeEntered=Event(TRUE,FALSE);UnsafeRescue=Event(TRUE,FALSE);
    PB_CONFIG config={sizeof(config),queues,64,128,0,0};CHECK(Api.Open(L"hosted-late-commit",&config,&User)==PB_OK);Retain(User);
    BaseCursor=wrap?UINT64_MAX-1:0;
    for(ULONG i=0;i<queues;++i)for(ULONG ring=PB_QUEUE_RX_SUBMIT;ring<=PB_QUEUE_RX_COMPLETE;++ring){
        PB_QUEUE_VIEW *u=User->Queues[i].Rings+ring,*n=Native->Queues[i].Rings+ring;
        u->Cursor=u->PeerCursor=n->Cursor=n->PeerCursor=u->Shared->Producer=u->Shared->Consumer=BaseCursor;
    }
}
static VOID FinishPeer(VOID)
{
    CHECK(Api.Close(User,5000)==PB_OK);
    CHECK(!Native&&!Context.V2&&!NativeObjects&&!NativeWorkers&&!LockedPages&&!Pools&&!Mdls&&!Nbls&&CloseCalls==1&&Devices==0&&User->ResourcesClosed&&!User->Region);
    /* All public callers joined. The original fixture internal owner protects
     * this final work/pool observation; no consumed public handle is reused. */
    WaitForThreadpoolWorkCallbacks(User->Work,FALSE);DropReference(User);User=NULL;Progress=NULL;
    CHECK(CallbackThreadCount>0);for(LONG i=0;i<CallbackThreadCount;++i){Join(CallbackThreads[i]);CallbackThreads[i]=NULL;}
    CallbackThreadCount=0;WorkCallback=NULL;
    CloseOther(Context.Device.Disconnected.Native);Context.Device.Disconnected.Native=NULL;
    CloseOther(ParkEntered);CloseOther(ParkGate);CloseOther(TailEntered);CloseOther(TailGate);CloseOther(RundownEntered);CloseOther(UnsafeEntered);CloseOther(UnsafeRescue);
    ParkEntered=ParkGate=TailEntered=TailGate=RundownEntered=UnsafeEntered=UnsafeRescue=NULL;
    BeforePoolFree=NULL;BeforeRundownRelease=NULL;GateTail=FALSE;
    CHECK(!DllAllocations&&!Regions&&!DllEvents&&!Modules&&!Works&&!WorkPools&&!OtherHandles&&!RuntimeCount&&!RuntimeBytes&&!RuntimeAdapterCount);
}
static VOID RunLate(ULONG queues,ULONG batch,BOOL manual,BOOL wrap)
{
    Target=queues-1;Batch=batch;OwnerId=Armed=PushFinished=BookHeld=0;OpenPeer(queues,manual,wrap);
    CommitEntered=Event(TRUE,FALSE);CommitGate=Event(TRUE,FALSE);BookEntered=Event(TRUE,FALSE);BookGate=Event(TRUE,FALSE);WriterEntered=Event(TRUE,FALSE);Armed=1;
    HANDLE owner=Start(LateOwner,NULL);Await(CommitEntered);CHECK(!Native->Draining&&User->Queues[Target].Loans==(LONG)Batch);
    CHECK(Life.BeginStop(User)==PB_OK);Await(WriterEntered);
    CHECK(Native->Draining&&Native->DrainSnapshotReady&&!Native->Active&&!QueryCalls&&User->Queues[Target].Loans==(LONG)Batch);
    for(ULONG i=0;i<queues;++i)CHECK(Native->Queues[i].DrainProducer==BaseCursor&&Native->Queues[i].Rings[PB_QUEUE_RX_SUBMIT].Cursor==BaseCursor);
    PB_V2_STATUS status;CHECK(PbV2Query(&Context,&status)==STATUS_SUCCESS&&status.State==PB_SESSION_STOPPED&&!status.ActiveReferences);
    CHECK(SetEvent(CommitGate));Await(BookEntered);
    PB_USER_QUEUE *q=User->Queues+Target;PB_KERNEL_QUEUE *n=Native->Queues+Target;
    CHECK(BookHeld&&!q->Loans&&q->Published==(LONG)Batch&&q->Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer==BaseCursor+Batch&&!QueryCalls);
    CHECK(!PbV2ReceiveBatch(n)&&n->Rings[PB_QUEUE_RX_SUBMIT].Cursor==BaseCursor&&n->DrainProducer==BaseCursor&&!Native->RxObjects&&!Native->Completions&&!Native->Indications&&!Native->Active);
    /* Actual worker arm helper, under its required RxLock. Only a bounded
     * decision is executed: do not run a deliberately spinning worker loop. */
    KLOCK_QUEUE_HANDLE lock;uint32_t ready=UINT32_MAX;KeAcquireInStackQueuedSpinLock(&n->RxLock,&lock);
    CHECK(!PbV2SubmissionRoom(n,PB_MAX_BATCH)&&SelectedArmSubmissions(n,&ready)==PB_OK&&ready<=1);
    CHECK(n->Rings[PB_QUEUE_RX_SUBMIT].Cursor==BaseCursor&&n->DrainProducer==BaseCursor&&!n->Credits);
    KeReleaseInStackQueuedSpinLock(&lock);if(ready)++UnexpectedReady;
    for(ULONG i=0;i<Batch;++i)CHECK(q->RxSlots[i].State==PB_SLOT_PUBLISHED&&q->RxSlots[i].Generation==1&&!n->RxSlots[i].Active&&!n->RxSlots[i].Generation);
    CHECK(PbV2Query(&Context,&status)==STATUS_SUCCESS&&status.State==PB_SESSION_STOPPED);
    CHECK(SetEvent(BookGate));Join(owner);CHECK(Api.PrepareClose(User,5000)==PB_OK&&QueryCalls&&LastQueryState==PB_SESSION_STOPPED);
    CHECK(!q->Loans&&!q->Published&&!User->Fatal&&!Native->RxObjects&&!Native->Completions&&n->Rings[PB_QUEUE_RX_SUBMIT].Shared->Consumer==BaseCursor);
    CHECK(q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==BaseCursor+Batch&&q->Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer==BaseCursor+Batch);
    for(ULONG i=0;i<Batch;++i)CHECK(q->RxSlots[i].State==PB_SLOT_FREE&&q->RxSlots[i].Generation==1);
    CloseOther(CommitEntered);CloseOther(CommitGate);CloseOther(BookEntered);CloseOther(BookGate);CloseOther(WriterEntered);CommitEntered=CommitGate=BookEntered=BookGate=WriterEntered=NULL;
    FinishPeer();++LateScenarios;
}
static VOID Matrix(VOID)
{const ULONG queues[]={1,2,8},batches[]={1,2,7,63,64};for(ULONG qi=0;qi<3;++qi)for(ULONG bi=0;bi<5;++bi)for(ULONG manual=0;manual<2;++manual)for(ULONG wrap=0;wrap<2;++wrap)RunLate(queues[qi],batches[bi],(BOOL)manual,(BOOL)wrap);}
int main(int argc,char **argv)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK&&PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    BOOL selected=argc==2&&!strcmp(argv[1],"124");CHECK(argc==1||selected);
#ifdef PB_LATE_COMMIT_WITHOUT_GUARD
    BOOL negative=TRUE;CHECK(selected);
#else
    BOOL negative=FALSE;
#endif
    /* Two checked selected cases cover the same CNG first-use initialization
     * reached naturally by the full warm matrix. Never close SDK-owned handles. */
    if(selected){RunLate(2,2,FALSE,TRUE);RunLate(2,2,FALSE,TRUE);}else Matrix();
    ULONG warmed=LateScenarios,warmReady=UnexpectedReady;LateScenarios=UnexpectedReady=0;
    CHECK(warmReady==(negative?warmed:0));DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)RunLate(2,2,FALSE,TRUE);
    else Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));if(before!=after)fprintf(stderr,"handles %lu/%lu\n",before,after);CHECK(before==after);
    CHECK(UnexpectedReady==(negative?LateScenarios:0));
    printf("PASS: %lu actual nonPB late Commit/SYS DRAIN/QUERY/lifecycle reconciliation/free schedules plus %lu checked warmup; post-boundary Producer untouched, real DataGate writer waits bookkeeping, STOPPED ack retires Published without generation reuse; all owners/callbacks/workers joined, resources zero; handles %lu/%lu; unexpected arm ready %lu/%lu\n",LateScenarios,warmed,before,after,UnexpectedReady,warmReady);
    if(negative){puts("LATE_COMMIT_DRAIN_ARM_GUARD_BYPASSED: selected actual arm reports ready for post-boundary tail despite zero finite room; bounded observation only, real owner/lifecycle/native cleanup and all thread joins precede124, zero repair");return 124;}
    return 0;
}
