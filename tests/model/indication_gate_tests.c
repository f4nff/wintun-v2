/* SPDX-License-Identifier: GPL-2.0 */
/* Executes the production generation gate with hosted synchronization objects. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while (0)
#define NT_ASSERT CHECK
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
typedef struct { HANDLE Native; } KEVENT;
typedef struct { CRITICAL_SECTION *Lock; } KLOCK_QUEUE_HANDLE;
typedef struct TUN_CTX {
    SRWLOCK Transition;
    struct { CRITICAL_SECTION IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2]; } Probe;
} TUN_CTX;
static VOID KeAcquireInStackQueuedSpinLock(CRITICAL_SECTION *lock,KLOCK_QUEUE_HANDLE *handle) { EnterCriticalSection(lock); handle->Lock=lock; }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) { LeaveCriticalSection(handle->Lock); }
static volatile LONG ClearCalls,SetCalls;
static VOID KeClearEvent(KEVENT *event) { CHECK(ResetEvent(event->Native));InterlockedIncrement(&ClearCalls); }
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait) { (void)priority; (void)wait; CHECK(SetEvent(event->Native));InterlockedIncrement(&SetCalls); }
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{ (void)reason; (void)mode; (void)alertable; (void)timeout; CHECK(WaitForSingleObject(event->Native,5000)==WAIT_OBJECT_0); }
#ifndef PB_INDICATION_SOURCE
#define PB_INDICATION_SOURCE "../../driver/v2/indication_gate.h"
#endif
#include PB_INDICATION_SOURCE
typedef struct JOB { TUN_CTX *Ctx; ULONG Bank; HANDLE Done; } JOB;
static DWORD WINAPI WaitPrior(VOID *arg) { JOB *job=arg; PbWaitIndications(job->Ctx,job->Bank); SetEvent(job->Done); return 0; }
static VOID OverlappingIndications(TUN_CTX *ctx)
{
    LONG clears=ClearCalls;ULONG original=0;
    for(ULONG i=0;i<64;++i) {AcquireSRWLockShared(&ctx->Transition);original=PbBeginIndication(ctx);ReleaseSRWLockShared(&ctx->Transition);}
    LONG overlapClears=ClearCalls-clears;
#ifdef PB_EVENT_POLICY_BASELINE
    CHECK(overlapClears==64);
#else
    CHECK(overlapClears==1);
#endif
    AcquireSRWLockExclusive(&ctx->Transition);ULONG prior=PbRotateIndications(ctx);ReleaseSRWLockExclusive(&ctx->Transition);CHECK(prior==original);
    JOB job={ctx,prior,CreateEventW(NULL,TRUE,FALSE,NULL)};CHECK(job.Done);HANDLE waiter=CreateThread(NULL,0,WaitPrior,&job,0,NULL);CHECK(waiter);
    CHECK(WaitForSingleObject(job.Done,20)==WAIT_TIMEOUT);
    AcquireSRWLockShared(&ctx->Transition);ULONG next=PbBeginIndication(ctx);ReleaseSRWLockShared(&ctx->Transition);CHECK(next!=original);
    for(ULONG i=0;i<63;++i) PbEndIndication(ctx,original);
    CHECK(WaitForSingleObject(job.Done,0)==WAIT_TIMEOUT && WaitForSingleObject(ctx->Probe.IndicationIdle[original].Native,0)==WAIT_TIMEOUT);
    PbEndIndication(ctx,original);CHECK(WaitForSingleObject(job.Done,5000)==WAIT_OBJECT_0);
    CHECK(ctx->Probe.IndicationCount[next]==1 && WaitForSingleObject(ctx->Probe.IndicationIdle[next].Native,0)==WAIT_TIMEOUT);
    PbEndIndication(ctx,next);CHECK(WaitForSingleObject(waiter,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(waiter) && CloseHandle(job.Done));
    printf("PASS: 64 overlapping indications: event clears=%ld; last old-generation exit releases waiter while new generation stays busy.\n",overlapClears);
}
static DWORD WINAPI CycleIndications(VOID *opaque)
{
    TUN_CTX *ctx=opaque;
    for(ULONG i=0;i<50000;++i) {
        AcquireSRWLockShared(&ctx->Transition);ULONG bank=PbBeginIndication(ctx);ReleaseSRWLockShared(&ctx->Transition);
        if(!(i&127)) SwitchToThread();PbEndIndication(ctx,bank);
    }
    return 0;
}
static VOID ConcurrentRotations(TUN_CTX *ctx)
{
    HANDLE threads[]={CreateThread(NULL,0,CycleIndications,ctx,0,NULL),CreateThread(NULL,0,CycleIndications,ctx,0,NULL)};
    CHECK(threads[0] && threads[1]);
    for(ULONG i=0;i<10000;++i) {
        AcquireSRWLockExclusive(&ctx->Transition);ULONG bank=PbRotateIndications(ctx);ReleaseSRWLockExclusive(&ctx->Transition);
        PbWaitIndications(ctx,bank);
    }
    CHECK(WaitForMultipleObjects(2,threads,TRUE,10000)==WAIT_OBJECT_0 && CloseHandle(threads[0]) && CloseHandle(threads[1]));
    for(ULONG i=0;i<2;++i) CHECK(!ctx->Probe.IndicationCount[i] && WaitForSingleObject(ctx->Probe.IndicationIdle[i].Native,0)==WAIT_OBJECT_0);
#ifndef PB_EVENT_POLICY_BASELINE
    CHECK(ClearCalls==SetCalls);
#endif
    puts("PASS: 100000 concurrent indications with 10000 serialized bank rotations; both idle events match zero counts.");
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD initial,final;CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial));
    TUN_CTX ctx={0}; InitializeSRWLock(&ctx.Transition); InitializeCriticalSection(&ctx.Probe.IndicationLock);
    for(unsigned i=0;i<2;++i) { ctx.Probe.IndicationIdle[i].Native=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(ctx.Probe.IndicationIdle[i].Native); }
    AcquireSRWLockShared(&ctx.Transition); ULONG original=PbBeginIndication(&ctx); ReleaseSRWLockShared(&ctx.Transition);
    AcquireSRWLockExclusive(&ctx.Transition); ULONG prior=PbRotateIndications(&ctx); ReleaseSRWLockExclusive(&ctx.Transition);
    CHECK(prior==original);
    JOB job={&ctx,prior,CreateEventW(NULL,TRUE,FALSE,NULL)}; CHECK(job.Done);
    HANDLE waiter=CreateThread(NULL,0,WaitPrior,&job,0,NULL); CHECK(waiter);
    CHECK(WaitForSingleObject(job.Done,30)==WAIT_TIMEOUT);
    /* A callback can reenter the send transition while the disable waits for its old generation. */
    CHECK(TryAcquireSRWLockShared(&ctx.Transition)); ULONG reentrant=PbBeginIndication(&ctx); ReleaseSRWLockShared(&ctx.Transition);
    CHECK(reentrant!=original);
    PbEndIndication(&ctx,original); CHECK(WaitForSingleObject(job.Done,5000)==WAIT_OBJECT_0);
    CHECK(ctx.Probe.IndicationCount[reentrant]==1); /* New traffic does not prolong the old disable barrier. */
    CHECK(WaitForSingleObject(waiter,5000)==WAIT_OBJECT_0); CloseHandle(waiter); CloseHandle(job.Done);
    PbEndIndication(&ctx,reentrant);
    OverlappingIndications(&ctx);ConcurrentRotations(&ctx);
    for (unsigned i=0;i<100000;++i) {
        AcquireSRWLockShared(&ctx.Transition); ULONG bank=PbBeginIndication(&ctx); ReleaseSRWLockShared(&ctx.Transition);
        PbEndIndication(&ctx,bank);
        AcquireSRWLockExclusive(&ctx.Transition); bank=PbRotateIndications(&ctx); ReleaseSRWLockExclusive(&ctx.Transition);
        PbWaitIndications(&ctx,bank);
    }
    CHECK(!ctx.Probe.IndicationCount[0] && !ctx.Probe.IndicationCount[1]);
    for(unsigned i=0;i<2;++i) CloseHandle(ctx.Probe.IndicationIdle[i].Native);
    DeleteCriticalSection(&ctx.Probe.IndicationLock);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&final) && initial==final);
    puts("PASS: actual indication gate, reentrant transition during disable, old generation only, 100000 balanced rotations.");
    return 0;
}
