/* SPDX-License-Identifier: GPL-2.0 */
/* Execute the actual query with hosted locks. This does not emulate kernel IRQL. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
typedef struct { ULONG Unused; } KEVENT, *PKEVENT, MDL, EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef UCHAR KIRQL;
typedef LONG NTSTATUS;
typedef struct { KSPIN_LOCK *Lock; } KLOCK_QUEUE_HANDLE;
typedef struct _TUN_CTX { SRWLOCK TransitionLock; PB_KERNEL_SESSION *V2; } TUN_CTX;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
static ULONG64 Now;
static ULONG QueueLocks;
static PB_KERNEL_SESSION *MutateSession;
static BOOLEAN RetainBeforeRx,FaultAfterRx;
static HANDLE InsideRx, ResumeRx, DetachStarted, DetachDone;
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) { AcquireSRWLockShared(lock); return 0; }
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) { (void)irql; ReleaseSRWLockShared(lock); }
#define ExAcquireSpinLockExclusive(lock) (AcquireSRWLockExclusive(lock),(KIRQL)0)
#define ExReleaseSpinLockExclusive(lock,irql) ((void)(irql),ReleaseSRWLockExclusive(lock))
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{
    if (InsideRx) { CHECK(SetEvent(InsideRx)); CHECK(WaitForSingleObject(ResumeRx,5000)==WAIT_OBJECT_0); }
    CHECK(!*lock); *lock=1; handle->Lock=lock; QueueLocks++;
    if(RetainBeforeRx) {
        RetainBeforeRx=FALSE;
        /* A worker retained its private NBL chain after Query's first Active
         * read, then published the queue's quiescence before Query acquired RxLock. */
        MutateSession->Active=1; MutateSession->Queues[0].FaultQuiesced=1;
    }
}
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle)
{ CHECK(*handle->Lock==1); *handle->Lock=0; if(FaultAfterRx) { FaultAfterRx=FALSE; MutateSession->Faulted=1; } }
static ULONG64 KeQueryInterruptTime(VOID) { return Now*10000u; }
#ifndef PB_QUERY_SOURCE
#define PB_QUERY_SOURCE "../../driver/v2/session_query.h"
#endif
#include PB_QUERY_SOURCE
static DWORD WINAPI Query(VOID *arg)
{
    PB_V2_STATUS status;
    CHECK(PbV2Query(arg,&status)==STATUS_SUCCESS && status.Epoch==42 && status.State==PB_SESSION_STOPPED);
    return 0;
}
static DWORD WINAPI Detach(VOID *arg)
{
    TUN_CTX *ctx=arg; CHECK(SetEvent(DetachStarted));
    AcquireSRWLockExclusive(&ctx->TransitionLock); ctx->V2=NULL; ReleaseSRWLockExclusive(&ctx->TransitionLock);
    CHECK(SetEvent(DetachDone)); return 0;
}
int main(VOID)
{
    TUN_CTX ctx={SRWLOCK_INIT,NULL}; PB_V2_STATUS out={0}; PB_KERNEL_SESSION s={0}; PB_QUEUE_SHARED ring={0};
    CHECK(PbV2Query(&ctx,&out)==STATUS_DEVICE_NOT_READY);
    ctx.V2=&s; s.Layout.QueueCount=1; s.Epoch=42; s.TxObjects=71; s.RxObjects=69; s.RxReturned=68; s.Indications=3; s.Completions=4;
    s.Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Shared=&ring;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_RUNNING);
    CHECK(out.Size==sizeof(out) && out.QueueCount==1 && out.Epoch==42 && out.TxObjects==71 && out.RxObjects==69 && out.RxReturned==68);
    CHECK(out.Indications==3 && out.Completions==4 && !QueueLocks);
    /* There is deliberately no RegistrationLock in this context: a callback query cannot wait for it. */
    s.Draining=s.DrainSnapshotReady=1; s.Deadline=2000; Now=1; s.Active=1;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING && out.ActiveReferences==1);
    Now=2000; CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_QUARANTINED);
    s.Active=0; CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED);
    Now=1; s.Queues[0].Credits=1; CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING);
    s.Queues[0].Credits=0; ring.Producer=s.Queues[0].DrainProducer=1; CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING);
    s.Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Cursor=1;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED && !s.Queues[0].RxLock && QueueLocks==6);
    ring.Producer=UINT64_MAX;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED); /* Private normal-drain boundary is stable. */
    s.DrainSnapshotReady=0;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING); /* Partial/faulting snapshot cannot acknowledge. */
    s.DrainSnapshotReady=1;
    MutateSession=&s; s.Faulted=1;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING); /* No witness, despite zero Active/credits. */
    RetainBeforeRx=TRUE;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING && out.ActiveReferences==1);
    s.Active=0; ring.Producer=UINT64_MAX;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_STOPPED); /* Ignore faulted user producer. */
    s.Faulted=0; s.Queues[0].FaultQuiesced=0; ring.Producer=1; FaultAfterRx=TRUE;
    CHECK(PbV2Query(&ctx,&out)==STATUS_SUCCESS && out.State==PB_SESSION_DRAINING); /* Fault invalidates normal snapshot. */
    s.Faulted=0;
    InsideRx=CreateEventW(NULL,TRUE,FALSE,NULL); ResumeRx=CreateEventW(NULL,TRUE,FALSE,NULL);
    DetachStarted=CreateEventW(NULL,TRUE,FALSE,NULL); DetachDone=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(InsideRx && ResumeRx && DetachStarted && DetachDone);
    HANDLE query=CreateThread(NULL,0,Query,&ctx,0,NULL); CHECK(query && WaitForSingleObject(InsideRx,5000)==WAIT_OBJECT_0);
    HANDLE detach=CreateThread(NULL,0,Detach,&ctx,0,NULL); CHECK(detach && WaitForSingleObject(DetachStarted,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(DetachDone,30)==WAIT_TIMEOUT); /* Detach cannot release a session mid-snapshot. */
    CHECK(SetEvent(ResumeRx) && WaitForSingleObject(query,5000)==WAIT_OBJECT_0 && WaitForSingleObject(detach,5000)==WAIT_OBJECT_0);
    CloseHandle(query); CloseHandle(detach); CloseHandle(InsideRx); CloseHandle(ResumeRx); CloseHandle(DetachStarted); CloseHandle(DetachDone);
    CHECK(PbV2Query(&ctx,&out)==STATUS_DEVICE_NOT_READY);
    puts("PASS: actual session query, running/draining/quarantine/stopped, credits and queued descriptors, private fault witnesses, references retained during snapshot, concurrent fault invalidates normal snapshot; concurrent detach waits for snapshot without RegistrationLock.");
    return 0;
}
