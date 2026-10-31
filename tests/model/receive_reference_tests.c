/* SPDX-License-Identifier: GPL-2.0 */
/* Production batch-reference helpers, hosted locks/events and concurrent return/acquire schedules. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
#define NT_ASSERT CHECK
#define IO_NO_INCREMENT 0
typedef struct { HANDLE Native; } KEVENT, *PKEVENT, MDL, EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef UCHAR KIRQL;
typedef LONG NTSTATUS;
typedef struct _TUN_CTX {SRWLOCK TransitionLock;PB_KERNEL_SESSION *V2;} TUN_CTX;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
typedef struct { SRWLOCK *Lock; } KLOCK_QUEUE_HANDLE;
static volatile LONG LockCalls,ClearCalls,SetCalls,ReferenceRmws,ReferenceStores;
static PB_KERNEL_SESSION *Observed;
static __declspec(thread) KSPIN_LOCK *HeldLock;
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{ CHECK(!HeldLock);handle->Lock=(SRWLOCK *)lock; AcquireSRWLockExclusive(handle->Lock);HeldLock=lock;InterlockedIncrement(&LockCalls); }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) { CHECK(HeldLock==(KSPIN_LOCK *)handle->Lock);HeldLock=NULL;ReleaseSRWLockExclusive(handle->Lock); }
static VOID KeClearEvent(KEVENT *event)
{ CHECK(Observed && event==&Observed->AllReturned && HeldLock==&Observed->ReferenceLock && ReadAcquire(&Observed->Active)>0);CHECK(ResetEvent(event->Native));InterlockedIncrement(&ClearCalls); }
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait)
{ (void)priority;(void)wait;CHECK(Observed && event==&Observed->AllReturned && HeldLock==&Observed->ReferenceLock && !ReadAcquire(&Observed->Active));CHECK(SetEvent(event->Native));InterlockedIncrement(&SetCalls); }
#ifndef PB_REFERENCE_SOURCE
#define PB_REFERENCE_SOURCE "../../driver/v2/session_references.h"
#endif
#if defined(PB_REFERENCE_RMW_BASELINE) || defined(PB_EVENT_POLICY_BASELINE)
static LONG CountReferenceExchangeAdd(volatile LONG *value,LONG delta)
{
    CHECK(Observed && value==&Observed->Active && HeldLock==&Observed->ReferenceLock);
    InterlockedIncrement(&ReferenceRmws);return InterlockedExchangeAdd(value,delta);
}
static LONG CountReferenceAdd(volatile LONG *value,LONG delta)
{
    CHECK(Observed && value==&Observed->Active && HeldLock==&Observed->ReferenceLock);
    InterlockedIncrement(&ReferenceRmws);return InterlockedAdd(value,delta);
}
#else
static VOID CountReferenceStore(volatile LONG *value,LONG next)
{
    CHECK(Observed && value==&Observed->Active && HeldLock==&Observed->ReferenceLock);
    InterlockedIncrement(&ReferenceStores);WriteRelease(value,next);
}
#endif
#pragma push_macro("InterlockedExchangeAdd")
#pragma push_macro("InterlockedAdd")
#pragma push_macro("WriteRelease")
#undef InterlockedExchangeAdd
#undef InterlockedAdd
#undef WriteRelease
#define InterlockedExchangeAdd CountReferenceExchangeAdd
#define InterlockedAdd CountReferenceAdd
#define WriteRelease CountReferenceStore
#include PB_REFERENCE_SOURCE
#pragma pop_macro("WriteRelease")
#pragma pop_macro("InterlockedAdd")
#pragma pop_macro("InterlockedExchangeAdd")
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) {(void)irql;ReleaseSRWLockShared(lock);}
#define ExAcquireSpinLockExclusive(lock) (AcquireSRWLockExclusive(lock),(KIRQL)0)
#define ExReleaseSpinLockExclusive(lock,irql) ((void)(irql),ReleaseSRWLockExclusive(lock))
static ULONG64 KeQueryInterruptTime(VOID) {return 10000;}
#include "../../driver/v2/session_query.h"
static VOID CheckSnapshot(PB_KERNEL_SESSION *s,ULONG held)
{
    KLOCK_QUEUE_HANDLE lock; KeAcquireInStackQueuedSpinLock(&s->ReferenceLock,&lock);
    CHECK(s->Active>=(LONG)held);
    CHECK((WaitForSingleObject(s->AllReturned.Native,0)==WAIT_OBJECT_0)==(s->Active==0));
    KeReleaseInStackQueuedSpinLock(&lock);
}
typedef struct { PB_KERNEL_SESSION *Session; ULONG Nbls,Objects; BOOLEAN Batch; } JOB;
static DWORD WINAPI Cycle(VOID *arg)
{
    JOB *job=arg;
    for(ULONG i=0;i<50000;++i) {
        PbV2RetainReceives(job->Session,job->Nbls,job->Objects); CheckSnapshot(job->Session,job->Nbls);
        if(job->Batch) PbV2ReleaseReferences(job->Session,job->Nbls);
        else for(ULONG n=0;n<job->Nbls;++n) PbV2ReleaseReference(job->Session);
        if(!(i&127)) SwitchToThread();
    }
    return 0;
}
typedef struct WAIT_JOB {PB_KERNEL_SESSION *Session;HANDLE Entered,Done;} WAIT_JOB;
static DWORD WINAPI WaitReturned(VOID *opaque)
{
    WAIT_JOB *job=opaque;CHECK(SetEvent(job->Entered));
    CHECK(WaitForSingleObject(job->Session->AllReturned.Native,5000)==WAIT_OBJECT_0);
    CHECK(SetEvent(job->Done));return 0;
}
static VOID OverlappingOwners(PB_KERNEL_SESSION *s)
{
    LONG clears=ClearCalls,sets=SetCalls,rmws=ReferenceRmws,stores=ReferenceStores;
    for(ULONG i=0;i<64;++i) {PbV2RetainReferences(s,1);CheckSnapshot(s,i+1);}
    LONG overlapClears=ClearCalls-clears;
#ifdef PB_EVENT_POLICY_BASELINE
    CHECK(overlapClears==64);
#else
    CHECK(overlapClears==1);
#endif
    WAIT_JOB job={s,CreateEventW(NULL,TRUE,FALSE,NULL),CreateEventW(NULL,TRUE,FALSE,NULL)};
    CHECK(job.Entered && job.Done);HANDLE waiter=CreateThread(NULL,0,WaitReturned,&job,0,NULL);CHECK(waiter);
    CHECK(WaitForSingleObject(job.Entered,5000)==WAIT_OBJECT_0 && WaitForSingleObject(job.Done,20)==WAIT_TIMEOUT);
    for(ULONG i=0;i<63;++i) PbV2ReleaseReference(s);
    PbV2RetainReferences(s,1);PbV2ReleaseReference(s);CheckSnapshot(s,1);
    CHECK(WaitForSingleObject(job.Done,0)==WAIT_TIMEOUT && SetCalls==sets);
    PbV2ReleaseReference(s);
    CHECK(WaitForSingleObject(job.Done,5000)==WAIT_OBJECT_0 && WaitForSingleObject(waiter,5000)==WAIT_OBJECT_0 && SetCalls==sets+1);
    CHECK(CloseHandle(waiter) && CloseHandle(job.Entered) && CloseHandle(job.Done));
    CheckSnapshot(s,0);
    printf("PASS: 64 overlapping references: initial event clears=%ld; waiter stays blocked through partial release/reacquire, final release signals once.\n",overlapClears);
    LONG updates=130; /* 64 retains + 63 releases + retain/release + final release. */
#if defined(PB_REFERENCE_RMW_BASELINE) || defined(PB_EVENT_POLICY_BASELINE)
    CHECK(ReferenceRmws-rmws==updates && ReferenceStores==stores);
#else
    CHECK(ReferenceRmws==rmws && ReferenceStores-stores==updates);
#endif
    printf("PASS: overlapping ownership updates=%ld, extra reference RMWs=%ld release stores=%ld, every write owns ReferenceLock.\n",updates,ReferenceRmws-rmws,ReferenceStores-stores);
}
static DWORD WINAPI QueryOwned(VOID *opaque)
{
    TUN_CTX *ctx=opaque;
    for(ULONG i=0;i<100000;++i) {
        PB_V2_STATUS status;CHECK(!HeldLock && PbV2Query(ctx,&status)==STATUS_SUCCESS);
        CHECK(status.State==PB_SESSION_DRAINING && status.ActiveReferences>=1 && status.ActiveReferences<=65);
        if(!(i&127)) SwitchToThread();
    }
    return 0;
}
static VOID ConcurrentQueries(PB_KERNEL_SESSION *s)
{
    TUN_CTX ctx={0};ctx.V2=s;s->Ctx=&ctx;s->Layout.QueueCount=1;s->Layout.RingCapacity=32;
    s->Draining=s->Faulted=1;s->Deadline=2000;s->Queues[0].FaultQuiesced=1;
    PbV2RetainReferences(s,1);LONG64 before=s->RxObjects;
    JOB a={s,32,32,TRUE},b={s,32,32,TRUE};
    HANDLE threads[]={CreateThread(NULL,0,Cycle,&a,0,NULL),CreateThread(NULL,0,Cycle,&b,0,NULL),CreateThread(NULL,0,QueryOwned,&ctx,0,NULL)};
    CHECK(threads[0] && threads[1] && threads[2] && WaitForMultipleObjects(3,threads,TRUE,20000)==WAIT_OBJECT_0);
    CHECK(s->Active==1 && s->RxObjects==before+3200000 && WaitForSingleObject(s->AllReturned.Native,0)==WAIT_TIMEOUT);
    PB_V2_STATUS status;CHECK(PbV2Query(&ctx,&status)==STATUS_SUCCESS && status.State==PB_SESSION_DRAINING && status.ActiveReferences==1);
    PbV2ReleaseReference(s);
    CHECK(PbV2Query(&ctx,&status)==STATUS_SUCCESS && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    CheckSnapshot(s,0);for(ULONG i=0;i<_countof(threads);++i) CHECK(CloseHandle(threads[i]));s->Ctx=NULL;
    puts("PASS: actual QUERY runs 100000 times beside 100000 concurrent retain/release cycles; a sentinel owner prevents STOPPED and only final release completes the boundary.");
}
int main(VOID)
{
    _Static_assert(sizeof(KSPIN_LOCK)==sizeof(SRWLOCK),"hosted lock storage");
    DWORD initialHandles,finalHandles; CHECK(GetProcessHandleCount(GetCurrentProcess(),&initialHandles));
    PB_KERNEL_SESSION s={0};Observed=&s;s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(s.AllReturned.Native);
    /* A 32-NBL chain is private until this one retain; cancellation returns it through the same releases. */
    PbV2RetainReceives(&s,32,32); CHECK(s.Active==32 && s.RxObjects==32 && LockCalls==1 && ClearCalls==1);
    for(ULONG i=0;i<31;++i) { PbV2ReleaseReference(&s); CHECK(WaitForSingleObject(s.AllReturned.Native,0)==WAIT_TIMEOUT); }
    PbV2ReleaseReference(&s); CheckSnapshot(&s,0);
    /* One coalesced NBL retains 32 object completions but only one NDIS return reference. */
    PbV2RetainReceives(&s,1,32); CHECK(s.Active==1 && s.RxObjects==64); PbV2ReleaseReference(&s);
    OverlappingOwners(&s);
    for(ULONG mode=0;mode<2;++mode) {
        LONG clears=ClearCalls;
        JOB a={&s,32,32,(BOOLEAN)mode},b={&s,1,32,FALSE};
        HANDLE one=CreateThread(NULL,0,Cycle,&a,0,NULL),two=CreateThread(NULL,0,Cycle,&b,0,NULL); CHECK(one && two);
        HANDLE threads[]={one,two}; CHECK(WaitForMultipleObjects(2,threads,TRUE,10000)==WAIT_OBJECT_0);
        CheckSnapshot(&s,0); CHECK(s.Active==0 && s.RxObjects==64+(LONG64)(mode+1)*3200000);
#ifdef PB_EVENT_POLICY_BASELINE
        CHECK(ClearCalls==clears+100000 && SetCalls<=ClearCalls);
#else
        CHECK(ClearCalls==SetCalls && ClearCalls>clears && ClearCalls<=clears+100000);
#endif
        CloseHandle(one); CloseHandle(two);
    }
    ConcurrentQueries(&s);CloseHandle(s.AllReturned.Native);Observed=NULL;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&finalHandles) && initialHandles==finalHandles);
    puts("PASS: production receive references, batch/cancel/coalesced counts, 200000 concurrent retain cycles with single and batched releases, balanced AllReturned, zero handle delta.");
    return 0;
}
