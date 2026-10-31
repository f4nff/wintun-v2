/* SPDX-License-Identifier: GPL-2.0 */
/* Actual send lookup, rundown/reference helpers, fault quiescence and QUERY.
 * Real Windows threads/locks/events; NDIS and IRQL remain hosted. */
#include "../safety/guard_memory.h"
#include "kernel_nbl_fixture.h"
#include "../../include/packetbridge_send_diagnostics.h"
typedef LONG NTSTATUS;
typedef ULONG NDIS_STATUS;
typedef VOID *NDIS_HANDLE;
typedef struct NET_BUFFER {struct NET_BUFFER *Next;ULONG Length;} NET_BUFFER;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define NDIS_STATUS_PAUSED 1u
#define NDIS_STATUS_RESOURCES 2u
#define NDIS_STATUS_MEDIA_DISCONNECTED 3u
#define NDIS_STATUS_INVALID_LENGTH 4u
/* This incoming NBL has no offload OOB. Reuse two unused host info cells;
 * no receive NBL is constructed or published in these lookup-rejection tests. */
#define TcpLargeSendNetBufferListInfo NetBufferListFrameType
#define UdpSegmentationOffloadInfo TcpRecvSegCoalesceInfo
static NET_BUFFER Input={NULL,64};
#define NET_BUFFER_LIST_FIRST_NB(n) ((void)(n),&Input)
#define NET_BUFFER_NEXT_NB(n) ((n)->Next)
#define NET_BUFFER_DATA_LENGTH(n) ((n)->Length)
static HANDLE LookupEntered,LookupGate,CompletionEntered,CompletionGate,QueryDone,ExclusiveEntered,ExclusiveGate;
static volatile LONG QueryId,QueryMode,SharedAcquires,ExclusiveAcquires,Completions,Rejections;
static volatile LONG AllSharedAcquires,AllSharedReleases,AllExclusiveAcquires,AllExclusiveReleases;
static ULONG CallerLevel;
/* Model the transition DDI's saved-level/mode contract only. Host thread
 * scheduling, waits and page protection still run at ordinary user level. */
static __declspec(thread) KIRQL ModelLevel,SavedLevel;
static __declspec(thread) SRWLOCK *HeldTransition;
static __declspec(thread) ULONG HeldMode;
static PB_KERNEL_SESSION *Current;
static PB_V2_STATUS Snapshot;
static NTSTATUS QueryResult;
static volatile LONG UnexpectedWork=1;
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock)
{
    CHECK(!HeldTransition && !HeldMode && ModelLevel<=2);KIRQL saved=ModelLevel;ModelLevel=2;
    if((LONG)GetCurrentThreadId()==QueryId) InterlockedIncrement(&SharedAcquires);
    AcquireSRWLockShared(lock);HeldTransition=lock;HeldMode=1;SavedLevel=saved;InterlockedIncrement(&AllSharedAcquires);return saved;
}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql)
{
    CHECK(HeldTransition==lock && HeldMode==1 && ModelLevel==2 && irql==SavedLevel);
    HeldTransition=NULL;HeldMode=0;ReleaseSRWLockShared(lock);ModelLevel=irql;InterlockedIncrement(&AllSharedReleases);
}
KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock)
{
    CHECK(!HeldTransition && !HeldMode && ModelLevel<=2);KIRQL saved=ModelLevel;ModelLevel=2;
    if((LONG)GetCurrentThreadId()==QueryId) {
        InterlockedIncrement(&ExclusiveAcquires);CHECK(SetEvent(ExclusiveEntered));
        if(QueryMode==2) CHECK(WaitForSingleObject(ExclusiveGate,5000)==WAIT_OBJECT_0);
    }
    AcquireSRWLockExclusive(lock);HeldTransition=lock;HeldMode=2;SavedLevel=saved;InterlockedIncrement(&AllExclusiveAcquires);return saved;
}
VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql)
{
    CHECK(HeldTransition==lock && HeldMode==2 && ModelLevel==2 && irql==SavedLevel);
    HeldTransition=NULL;HeldMode=0;ReleaseSRWLockExclusive(lock);ModelLevel=irql;InterlockedIncrement(&AllExclusiveReleases);
}
static VOID CheckCallerLevel(VOID) {CHECK(!HeldTransition && !HeldMode && ModelLevel==(KIRQL)CallerLevel);}
static ULONG64 KeQueryInterruptTime(VOID) {return 10000;}
#ifndef PB_FAULT_QUERY_SOURCE
#define PB_FAULT_QUERY_SOURCE "../../driver/v2/session_query.h"
#endif
#include PB_FAULT_QUERY_SOURCE
#include "../../driver/v2/session_fault_drain.h"
#include "../../driver/v2/session_recycle.h"
static BOOLEAN PbV2Read(NET_BUFFER *nb,ULONG skip,UCHAR *bytes,ULONG length)
{(void)nb;(void)skip;(void)bytes;(void)length;CHECK(!UnexpectedWork);return (BOOLEAN)!UnexpectedWork;}
static ULONG PbEffectiveMask(TUN_CTX *ctx) {(void)ctx;return 0;}
static ULONG PbBeginIndication(TUN_CTX *ctx) {(void)ctx;CHECK(!UnexpectedWork);return (ULONG)UnexpectedWork+1;}
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) {(void)ctx;(void)bank;CHECK(!UnexpectedWork);}
static NDIS_STATUS PbV2ControlReply(TUN_CTX *ctx,const UCHAR *bytes,ULONG length)
{(void)ctx;(void)bytes;(void)length;CHECK(!UnexpectedWork);return (NDIS_STATUS)UnexpectedWork;}
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG skip,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)
{(void)ctx;(void)nbl;(void)nb;(void)skip;(void)d;(void)bytes;(void)failure;CHECK(!UnexpectedWork);return (NDIS_STATUS)UnexpectedWork;}
static VOID PbV2RejectSend(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,
    ULONG prior,ULONG queue,ULONG ethernet,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{
    (void)ctx;(void)epoch;(void)nbl;(void)nb;(void)prior;(void)queue;(void)ethernet;(void)accepted;(void)attempted;(void)scope;
    CHECK(status==NDIS_STATUS_PAUSED && failure->Reason==PB_SEND_ADMISSION && !q && !pending);InterlockedIncrement(&Rejections);
}
static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls) {(void)nbls;}
static VOID PbCompleteSend(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbls)
{
    CHECK(adapter && nbls->Status==NDIS_STATUS_PAUSED && Current->Active==1 && Current->Returns.References==1);
    /* The real completion upcall is unlocked. Faulted QUERY must remain
     * reentrant here, and the send reference must cover the entire callback. */
    CheckCallerLevel();KIRQL saved=ModelLevel;ModelLevel=2;
    PB_V2_STATUS status;CHECK(PbV2Query(Current->Ctx,&status)==STATUS_SUCCESS && status.State!=PB_SESSION_STOPPED && status.ActiveReferences==1);
    CHECK(ModelLevel==2 && !HeldTransition && !HeldMode);ModelLevel=saved;
    InterlockedIncrement(&Completions);CHECK(SetEvent(CompletionEntered) && WaitForSingleObject(CompletionGate,5000)==WAIT_OBJECT_0);
}
#include "../../driver/v2/session_send.h"
static HANDLE Event(VOID) {HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(event);return event;}
typedef struct FIXTURE {TUN_CTX Ctx;PB_KERNEL_SESSION *S;PB_RX_SLOT Slots[2];KEVENT Kernel,User,Progress;} FIXTURE;
static VOID Init(FIXTURE *f)
{
    memset(f,0,sizeof(*f));f->S=PbTestGuardAlloc(sizeof(*f->S));memset(f->S,0,sizeof(*f->S));
    PB_KERNEL_SESSION *s=f->S;Current=s;s->Ctx=&f->Ctx;s->Epoch=42;s->Deadline=1000;
    CHECK(PbCalculateLayout(1,2,2,&s->Layout)==PB_OK);f->Ctx.V2=s;f->Ctx.Running=1;f->Ctx.MiniportAdapterHandle=f;
    s->Queues[0].Session=s;s->Queues[0].RxSlots=f->Slots;
    f->Kernel.Native=Event();f->User.Native=Event();f->Progress.Native=Event();
    s->Queues[0].KernelEvent=&f->Kernel;s->Queues[0].UserEvent=&f->User;s->ProgressEvent=&f->Progress;
    s->AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s->AllReturned.Native);
    LookupEntered=Event();LookupGate=Event();CompletionEntered=Event();CompletionGate=Event();
    QueryDone=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(QueryDone);ExclusiveEntered=Event();ExclusiveGate=Event();
    QueryId=QueryMode=SharedAcquires=ExclusiveAcquires=Completions=Rejections=0;memset(&Snapshot,0,sizeof(Snapshot));
    AllSharedAcquires=AllSharedReleases=AllExclusiveAcquires=AllExclusiveReleases=0;AfterQueueUnlock=NULL;
}
static VOID Destroy(FIXTURE *f)
{
    PB_KERNEL_SESSION *s=f->S;AfterRundownAcquire=NULL;AfterQueueUnlock=NULL;CheckCallerLevel();
    CHECK(AllSharedAcquires==AllSharedReleases && AllExclusiveAcquires==AllExclusiveReleases);
    CHECK(!s->Active && !s->Returns.References && !Pools && !Mdls && !Nbls && WaitForSingleObject(s->AllReturned.Native,0)==WAIT_OBJECT_0);
    CHECK(CloseHandle(s->AllReturned.Native) && CloseHandle(f->Kernel.Native) && CloseHandle(f->User.Native) && CloseHandle(f->Progress.Native));
    CHECK(CloseHandle(LookupEntered) && CloseHandle(LookupGate) && CloseHandle(CompletionEntered) && CloseHandle(CompletionGate) &&
        CloseHandle(QueryDone) && CloseHandle(ExclusiveEntered) && CloseHandle(ExclusiveGate));
    PbTestGuardFree(s);f->S=Current=NULL;
}
static VOID ParkLookup(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Current->Returns && !Current->Active && HeldTransition==&Current->Ctx->TransitionLock && HeldMode==1 && ModelLevel==2);
 CHECK(SetEvent(LookupEntered) && WaitForSingleObject(LookupGate,5000)==WAIT_OBJECT_0);}
static DWORD WINAPI Send(VOID *opaque)
{ModelLevel=(KIRQL)CallerLevel;FIXTURE *f=opaque;NET_BUFFER_LIST nbl={0};CHECK(PbV2Send(&f->Ctx,&nbl) && nbl.Status==NDIS_STATUS_PAUSED);CheckCallerLevel();return 0;}
static DWORD WINAPI Query(VOID *opaque)
{
    ModelLevel=(KIRQL)CallerLevel;QueryId=(LONG)GetCurrentThreadId();QueryResult=PbV2Query(opaque,&Snapshot);CheckCallerLevel();CHECK(SetEvent(QueryDone));return 0;
}
static VOID LookupFault(ULONG level)
{
    FIXTURE f;Init(&f);CallerLevel=level;ModelLevel=(KIRQL)level;PB_KERNEL_SESSION *s=f.S;AfterRundownAcquire=ParkLookup;
    HANDLE send=CreateThread(NULL,0,Send,&f,0,NULL);CHECK(send && WaitForSingleObject(LookupEntered,5000)==WAIT_OBJECT_0);
    CHECK(!s->Active && s->Returns.References==1 && WaitForSingleObject(s->AllReturned.Native,0)==WAIT_OBJECT_0);
    PbV2Fault(s);PbV2QuiesceFaultedQueue(s->Queues);CHECK(s->Faulted && s->Queues[0].FaultQuiesced && !s->Active);
    QueryMode=1;HANDLE query=CreateThread(NULL,0,Query,&f.Ctx,0,NULL);CHECK(query);
#ifdef PB_FAULT_QUERY_BASELINE
    CHECK(WaitForSingleObject(QueryDone,5000)==WAIT_OBJECT_0 && QueryResult==STATUS_SUCCESS && Snapshot.State==PB_SESSION_STOPPED && !Snapshot.ActiveReferences);
    CHECK(!ExclusiveAcquires);
#else
    CHECK(WaitForSingleObject(ExclusiveEntered,5000)==WAIT_OBJECT_0 && WaitForSingleObject(QueryDone,0)==WAIT_TIMEOUT);
#endif
    CHECK(SetEvent(LookupGate) && WaitForSingleObject(CompletionEntered,5000)==WAIT_OBJECT_0 && WaitForSingleObject(QueryDone,5000)==WAIT_OBJECT_0);
    CHECK(s->Active==1 && s->Returns.References==1 && WaitForSingleObject(s->AllReturned.Native,0)==WAIT_TIMEOUT);
#ifndef PB_FAULT_QUERY_BASELINE
    CHECK(QueryResult==STATUS_SUCCESS && Snapshot.State!=PB_SESSION_STOPPED && Snapshot.ActiveReferences==1 && ExclusiveAcquires==1);
#endif
    PB_V2_STATUS later;CHECK(PbV2Query(&f.Ctx,&later)==STATUS_SUCCESS && later.State!=PB_SESSION_STOPPED && later.ActiveReferences==1);
    CheckCallerLevel();
    CHECK(SetEvent(CompletionGate) && WaitForSingleObject(send,5000)==WAIT_OBJECT_0 && WaitForSingleObject(query,5000)==WAIT_OBJECT_0);
    CHECK(Completions==1 && Rejections==1 && !s->TxObjects && !s->RxObjects && PbV2Query(&f.Ctx,&later)==STATUS_SUCCESS &&
        later.State==PB_SESSION_STOPPED && !later.ActiveReferences);
    CheckCallerLevel();
    CHECK(CloseHandle(send) && CloseHandle(query));Destroy(&f);
#ifdef PB_FAULT_QUERY_BASELINE
    printf("PASS: modeled caller level=%lu, expected old faulted QUERY false STOPPED: actual send lookup still owns rundown, later Active becomes 1 through the unlocked completion callback; physical reference/rundown pins remain.\n",level);
#else
    printf("PASS: modeled caller level=%lu, faulted QUERY joins the actual send lookup; matching mode/level restoration, Active=1 through reentrant callback at modeled level 2, no packet work/reuse, only final release permits STOPPED.\n",level);
#endif
}
static VOID FaultAtSnapshot(KSPIN_LOCK *lock)
{
    CHECK(lock==&Current->Queues[0].RxLock && HeldTransition==&Current->Ctx->TransitionLock && HeldMode==1 && ModelLevel==2);
    AfterQueueUnlock=NULL;PbV2Fault(Current);PbV2QuiesceFaultedQueue(Current->Queues);
}
static VOID CommonQuery(VOID)
{
    for(ULONG level=0;level<=2;++level) {
        FIXTURE f;Init(&f);CallerLevel=level;ModelLevel=(KIRQL)level;QueryId=(LONG)GetCurrentThreadId();PB_V2_STATUS out,before;
        memset(&out,0xa5,sizeof(out));before=out;
        f.Ctx.V2=NULL;CHECK(PbV2Query(&f.Ctx,&out)==STATUS_DEVICE_NOT_READY && !memcmp(&before,&out,sizeof(out)));CheckCallerLevel();f.Ctx.V2=f.S;
        CHECK(PbV2Query(&f.Ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_RUNNING && !ExclusiveAcquires);CheckCallerLevel();
        f.S->Draining=f.S->DrainSnapshotReady=1;
        CHECK(PbV2Query(&f.Ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED && !ExclusiveAcquires);CheckCallerLevel();
        AfterQueueUnlock=FaultAtSnapshot;
        CHECK(PbV2Query(&f.Ctx,&out)==STATUS_SUCCESS && out.State!=PB_SESSION_STOPPED && !ExclusiveAcquires && !AfterQueueUnlock);CheckCallerLevel();
        CHECK(PbV2Query(&f.Ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED);CheckCallerLevel();
#ifdef PB_FAULT_QUERY_BASELINE
        CHECK(SharedAcquires==5 && !ExclusiveAcquires);
#else
        CHECK(SharedAcquires==5 && ExclusiveAcquires==1);
#endif
        QueryId=0;Destroy(&f);
    }
    puts("PASS: modeled caller levels 0/1/2: no-session output unchanged, normal queries remain shared, a fault during the shared snapshot cannot acknowledge STOPPED, next query joins fault mode; every release matches mode and saved level.");
}
#ifndef PB_FAULT_QUERY_BASELINE
static VOID DetachDuringEscalation(VOID)
{
    for(ULONG level=0;level<=2;++level) for(ULONG replace=0;replace<2;++replace) {
        FIXTURE f;Init(&f);CallerLevel=level;ModelLevel=(KIRQL)level;PbV2Fault(f.S);PbV2QuiesceFaultedQueue(f.S->Queues);QueryMode=2;
        memset(&Snapshot,0xa5,sizeof(Snapshot));PB_V2_STATUS before=Snapshot;
        HANDLE query=CreateThread(NULL,0,Query,&f.Ctx,0,NULL);CHECK(query && WaitForSingleObject(ExclusiveEntered,5000)==WAIT_OBJECT_0);
        PB_KERNEL_SESSION *next=NULL;
        if(replace) {next=PbTestGuardAlloc(sizeof(*next));memset(next,0,sizeof(*next));next->Epoch=84;next->Layout=f.S->Layout;}
        AcquireSRWLockExclusive(&f.Ctx.TransitionLock);f.Ctx.V2=next;ReleaseSRWLockExclusive(&f.Ctx.TransitionLock);
        DWORD old;CHECK(VirtualProtect(f.S,sizeof(*f.S),PAGE_NOACCESS,&old));
        CHECK(SetEvent(ExclusiveGate) && WaitForSingleObject(QueryDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(query,5000)==WAIT_OBJECT_0);
        CHECK(ExclusiveAcquires==1);
        if(replace) CHECK(QueryResult==STATUS_SUCCESS && Snapshot.Size==sizeof(Snapshot) && Snapshot.Epoch==84 && Snapshot.State==PB_SESSION_RUNNING);
        else CHECK(QueryResult==STATUS_DEVICE_NOT_READY && !memcmp(&before,&Snapshot,sizeof(Snapshot)));
        DWORD ignored;CHECK(VirtualProtect(f.S,sizeof(*f.S),old,&ignored) && CloseHandle(query));Destroy(&f);
        if(next) PbTestGuardFree(next);
    }
    puts("PASS: six modeled level x detach/replacement escalation cases: old session PAGE_NOACCESS, quiet DEVICE_NOT_READY or fresh epoch 84; matched exclusive release restores the requested level, no stale pointer/data.");
}
#endif
int main(VOID)
{
    CommonQuery();for(ULONG level=0;level<=2;++level) LookupFault(level);
#ifndef PB_FAULT_QUERY_BASELINE
    DetachDuringEscalation();
#endif
    return 0;
}
