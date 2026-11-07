/* SPDX-License-Identifier: GPL-2.0 */
#ifdef PB_API_TESTS
#error Drain fault capture requires actual shipping kernel paths
#endif
#ifndef NDEBUG
#error Drain fault capture requires NDEBUG build configuration
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Drain fault capture requires shipping fingerprint");
#ifndef PB_DRAIN_FAULT_CAPTURE_HOST
#error Bind the complete frozen drain-boundary host
#endif
/* Actual BeginDrain / Return->Flush->Fault / Query / worker quiescence.
 * Hosted ready session/MDLs; real SDK locks/events/joined OS threads. */
#define main UnusedDrainBoundaryMain
#include PB_DRAIN_FAULT_CAPTURE_HOST
#undef main
_Static_assert(sizeof(((FIXTURE *)0)->Slots)/sizeof(((FIXTURE *)0)->Slots[0])==PB_MAX_QUEUES,"All RX slot queue arrays must cover the supported maximum");
_Static_assert(sizeof(((FIXTURE *)0)->Pending)/sizeof(((FIXTURE *)0)->Pending[0])==PB_MAX_QUEUES,"All pending queue arrays must cover the supported maximum");
_Static_assert(sizeof(((FIXTURE *)0)->Kernel)/sizeof(KEVENT)==PB_MAX_QUEUES && sizeof(((FIXTURE *)0)->User)/sizeof(KEVENT)==PB_MAX_QUEUES,"All event arrays must cover the supported maximum");
enum {ROLE_DRAIN=1,ROLE_QUERY,ROLE_RETURN,ROLE_WORKER};
static __declspec(thread) ULONG Role;
static FIXTURE *Case;
static HANDLE CaptureEntered,CaptureGate,QueryEntered,QueryDone;
static volatile LONG Captures,QueueSignals,ProgressSignals,UserSignals;
static PB_V2_STATUS QueryResult;
static KEVENT Progress;
static ULONG HarnessHandles,Cases,Joins,Joined[5];
typedef struct DFC_THREAD {HANDLE Handle;DWORD Id;ULONG Role;} DFC_THREAD;
static VOID Await(HANDLE object){CHECK(WaitForSingleObject(object,10000)==WAIT_OBJECT_0);}
static HANDLE Event(BOOL manual){HANDLE event=CreateEventW(NULL,manual,FALSE,NULL);CHECK(event);++HarnessHandles;return event;}
static VOID Close(HANDLE object){CHECK(HarnessHandles && CloseHandle(object));--HarnessHandles;}
static VOID OnUnlock(KSPIN_LOCK *lock)
{
    if(Role!=ROLE_DRAIN || lock!=&Case->S.Queues[0].RxLock || InterlockedCompareExchange(&Captures,1,0))return;
    CHECK(Case->S.Queues[0].DrainProducer==1 && !Case->S.Queues[1].DrainProducer && !Case->S.DrainSnapshotReady && !Case->S.Draining && !Case->S.Faulted);
    CHECK(!TryAcquireSRWLockShared(&Case->Ctx.TransitionLock));
    /* Observer-only host gate. Native code never waits on these events while
     * holding a spin lock; this selects a legal other-CPU interleaving. */
    CHECK(SetEvent(CaptureEntered));Await(CaptureGate);
}
static VOID Signals(KEVENT *event)
{
    if(event==&Progress)InterlockedIncrement(&ProgressSignals);
    for(ULONG i=0;i<Case->S.Layout.QueueCount;++i){
        if(event==Case->S.Queues[i].KernelEvent)InterlockedIncrement(&QueueSignals);
        if(event==Case->S.Queues[i].UserEvent)InterlockedIncrement(&UserSignals);
    }
}
static DWORD WINAPI Draining(VOID *opaque)
{Role=ROLE_DRAIN;FIXTURE *f=opaque;AcquireSRWLockExclusive(&f->Ctx.TransitionLock);PbV2BeginDrain(&f->S,10000);ReleaseSRWLockExclusive(&f->Ctx.TransitionLock);return 0;}
static DWORD WINAPI Querying(VOID *opaque)
{Role=ROLE_QUERY;FIXTURE *f=opaque;CHECK(SetEvent(QueryEntered));CHECK(PbV2Query(&f->Ctx,&QueryResult)==STATUS_SUCCESS);CHECK(SetEvent(QueryDone));return 0;}
typedef struct RETURN_JOB {FIXTURE *F;NET_BUFFER_LIST *Nbl;} RETURN_JOB;
static DWORD WINAPI ReturnActual(VOID *opaque)
{Role=ROLE_RETURN;RETURN_JOB *job=opaque;CHECK(PbV2ReturnOne(&job->F->Ctx,job->Nbl));return 0;}
static DWORD WINAPI WorkerActual(VOID *opaque)
{Role=ROLE_WORKER;PbV2Worker(opaque);return 0;}
static DFC_THREAD Start(LPTHREAD_START_ROUTINE routine,VOID *arg,ULONG role)
{DFC_THREAD thread={0};CHECK(role>=ROLE_DRAIN && role<=ROLE_WORKER);thread.Role=role;thread.Handle=CreateThread(NULL,0,routine,arg,0,&thread.Id);CHECK(thread.Handle && thread.Id);++HarnessHandles;return thread;}
static VOID Join(DFC_THREAD thread)
{DWORD code;Await(thread.Handle);CHECK(GetExitCodeThread(thread.Handle,&code) && !code && GetThreadId(thread.Handle)==thread.Id);Close(thread.Handle);++Joins;++Joined[thread.Role];}
static NET_BUFFER_LIST *MakeOwned(FIXTURE *f,ULONG index)
{
    PB_KERNEL_QUEUE *q=f->S.Queues+index;PB_DESCRIPTOR d={.Token={42,1,0,(USHORT)index},.Length=20,.DataOffset=f->S.Layout.Queues[index].RxData};uint32_t complete;
    f->S.Region[d.DataOffset]=0x45;f->S.Region[d.DataOffset+3]=20;
    CHECK(PbAdmitReceive(&f->S.Layout,42,index,q->RxSlots,&d,0,&complete)==PB_OK);q->Credits++;
    NET_BUFFER_LIST *nbl=PbV2Receive(q,&d,1,MAXULONG,NULL);CHECK(nbl);PbV2RetainReceives(&f->S,1,1);return nbl;
}
static VOID Run(ULONG queues,BOOL activeFirst)
{
    CHECK((queues==2 || queues==8) && queues<=PB_MAX_QUEUES && !Case && !Pools && !Mdls && !Nbls && !HarnessHandles);Case=Create(queues,4,8,0);FIXTURE *f=Case;ULONG joined=Joins;
    Captures=QueueSignals=ProgressSignals=UserSignals=0;
    Progress.Native=Event(FALSE);f->S.ProgressEvent=&Progress;
    f->S.Lifecycle=(PB_SHARED_LIFECYCLE *)(f->S.Region+f->S.Layout.LifecycleOffset);
    CaptureEntered=Event(TRUE);CaptureGate=Event(TRUE);QueryEntered=Event(TRUE);QueryDone=Event(TRUE);
    NET_BUFFER_LIST *first=MakeOwned(f,0),*last=MakeOwned(f,queues-1);CHECK(f->S.Active==2);
    for(ULONG i=0;i<queues;++i){PB_QUEUE_VIEW producer;uint32_t count,notify;
        CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,i,PB_QUEUE_RX_SUBMIT,1)==PB_OK);
        PB_DESCRIPTOR d={.Token={42,1,1,(USHORT)i},.Length=20,.DataOffset=f->S.Layout.Queues[i].RxData+PB_BLOCK_SIZE};f->S.Region[d.DataOffset]=0x45;f->S.Region[d.DataOffset+3]=20;
        CHECK(PbQueuePush(&producer,&d,1,&count,&notify)==PB_OK && count==1);
    }
    /* Only a public untrusted consumer cursor is bad. The actual native return
     * must discover it through the production PushRing validation. */
    f->S.Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Shared->Consumer=1;
    AfterQueueUnlock=OnUnlock;OnSetEvent=Signals;
    DFC_THREAD drain=Start(Draining,f,ROLE_DRAIN);Await(CaptureEntered);
    DFC_THREAD query=Start(Querying,f,ROLE_QUERY);Await(QueryEntered);CHECK(WaitForSingleObject(QueryDone,0)==WAIT_TIMEOUT);
    RETURN_JOB job={f,first};Join(Start(ReturnActual,&job,ROLE_RETURN));first=NULL;
    CHECK(f->S.Faulted && f->S.Draining && !f->S.DrainSnapshotReady && f->S.Active==1 && !f->S.Returns.References);
    CHECK(!f->S.Queues[0].Credits && !f->S.Queues[0].PendingCount && !f->S.Queues[0].RxSlots[0].Active);
    CHECK(f->S.Lifecycle->Terminal && f->S.Lifecycle->Sequence && QueueSignals==(LONG)queues && UserSignals==(LONG)queues);
    CHECK(WaitForSingleObject(QueryDone,0)==WAIT_TIMEOUT && SetEvent(CaptureGate));Join(drain);Join(query);
    CHECK(Captures==1 && f->S.DrainSnapshotReady && QueryResult.State==PB_SESSION_DRAINING && QueryResult.ActiveReferences==1);
    for(ULONG i=0;i<queues;++i)CHECK(f->S.Queues[i].DrainProducer==1 && !f->S.Queues[i].Rings[PB_QUEUE_RX_SUBMIT].Cursor);
    for(ULONG i=0;i<queues-1;++i){Join(Start(WorkerActual,f->S.Queues+i,ROLE_WORKER));CHECK(f->S.Queues[i].FaultQuiesced);}
    CHECK(State(f,1)==PB_SESSION_DRAINING && !f->S.Queues[queues-1].FaultQuiesced);
    if(!activeFirst){Join(Start(WorkerActual,f->S.Queues+queues-1,ROLE_WORKER));CHECK(State(f,1)==PB_SESSION_DRAINING);}
    job.Nbl=last;Join(Start(ReturnActual,&job,ROLE_RETURN));last=NULL;
    CHECK(!f->S.Active && !f->S.Returns.References && !Pools && !Mdls && !Nbls && !f->S.Completions && f->S.RxReturned==2);
    if(activeFirst){CHECK(State(f,0)==PB_SESSION_DRAINING);Join(Start(WorkerActual,f->S.Queues+queues-1,ROLE_WORKER));}
    CHECK(State(f,0)==PB_SESSION_STOPPED && Exits==queues && QueueSignals==(LONG)queues && UserSignals==(LONG)queues && ProgressSignals>0);
    for(ULONG i=0;i<queues;++i)CHECK(f->S.Queues[i].FaultQuiesced && !f->S.Queues[i].Credits && !f->S.Queues[i].PendingCount && !f->S.Queues[i].RxSlots[0].Active && !f->S.Queues[i].RxSlots[1].Active);
    AfterQueueUnlock=NULL;OnSetEvent=NULL;Close(Progress.Native);f->S.ProgressEvent=NULL;
    Close(CaptureEntered);Close(CaptureGate);Close(QueryEntered);Close(QueryDone);Destroy(f);Case=NULL;
    CHECK(!Current && !Pools && !Mdls && !Nbls && !HarnessHandles && Joins-joined==queues+4);++Cases;
    printf("CASE: drain fault capture queues%lu activeFirst%u joins%lu workers%lu kernelWakes%ld userWakes%ld; snapshot alone rejected, native owners zero\n",queues,(unsigned)activeFirst,Joins-joined,queues,QueueSignals,UserSignals);
}
static VOID Matrix(VOID){Run(2,FALSE);Run(2,TRUE);Run(8,FALSE);Run(8,TRUE);}
int main(VOID)
{
    DWORD before,after;Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after && Cases==8 && Joins==72 && Joined[ROLE_DRAIN]==8 && Joined[ROLE_QUERY]==8 && Joined[ROLE_RETURN]==16 && Joined[ROLE_WORKER]==40);
    printf("PASS: %lu actual drain Return fault capture cases warm4 measure4; joined%lu drain%lu query%lu returns%lu workers%lu handles%lu/%lu; all NBL MDL pool references credits and harness owners zero\n",Cases,Joins,Joined[ROLE_DRAIN],Joined[ROLE_QUERY],Joined[ROLE_RETURN],Joined[ROLE_WORKER],before,after);return 0;
}
