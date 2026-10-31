/* SPDX-License-Identifier: GPL-2.0 */
/* Actual queue/lifecycle APIs and real Windows events, with a held native ack. */
#define PB_API_TESTS 1
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#include "kernel_nbl_fixture.h"
#undef CHECK
#define CHECK SAFETY_CHECK
typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) {(void)irql;ReleaseSRWLockShared(lock);}
#define ExAcquireSpinLockExclusive(lock) (AcquireSRWLockExclusive(lock),(KIRQL)0)
#define ExReleaseSpinLockExclusive(lock,irql) ((void)(irql),ReleaseSRWLockExclusive(lock))
static ULONG64 KeQueryInterruptTime(VOID) {return 10000;}
#include "../../driver/v2/session_drain.h"
#include "../../driver/v2/session_query.h"
#include "../../driver/v2/session_recycle.h"
static TUN_CTX *NativePeer;
static BOOL WINAPI NativeIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    if(NativePeer && code==PB_V2_IOCTL_QUERY && outputSize==sizeof(PB_V2_STATUS)) {
        CHECK(PbV2Query(NativePeer,output)==STATUS_SUCCESS);*returned=sizeof(PB_V2_STATUS);return TRUE;
    }
    return DeviceIoControl(device,code,input,inputSize,output,outputSize,returned,overlapped);
}
static BOOL WINAPI ObserveEvent(HANDLE);
static DWORD WINAPI ObserveWait(HANDLE,DWORD);
static volatile LONG GuardAllocations;
#ifdef PB_GUARD_TESTS
static VOID *GuardCalloc(SIZE_T count,SIZE_T size)
{CHECK(!size || count<=SIZE_MAX/size);VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);InterlockedIncrement(&GuardAllocations);return p;}
static VOID GuardFree(VOID *p) {if(p){PbTestGuardFree(p);CHECK(InterlockedDecrement(&GuardAllocations)>=0);}}
static VOID *WINAPI GuardRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{CHECK(!address && type==(MEM_RESERVE|MEM_COMMIT) && protection==PAGE_READWRITE);return GuardCalloc(1,size);}
static BOOL WINAPI GuardUnmap(VOID *p,SIZE_T size,DWORD type)
{CHECK(!size && type==MEM_RELEASE);GuardFree(p);return TRUE;}
#define calloc GuardCalloc
#define free GuardFree
#define VirtualAlloc GuardRegion
#define VirtualFree GuardUnmap
#endif
#define SetEvent ObserveEvent
#define WaitForSingleObject ObserveWait
#define DeviceIoControl NativeIo
#ifndef PB_CLOSE_WAKE_SESSION_SOURCE
#define PB_CLOSE_WAKE_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_CLOSE_WAKE_SESSION_SOURCE
#undef SetEvent
#undef WaitForSingleObject
#undef DeviceIoControl
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
static HANDLE Progress,Kernel,ParkEntered,ParkGate,WakeSeen;
static volatile LONG Recording,ProgressSignals,KernelSignals,Parks,EventWakes,Timeouts;
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static BOOL WINAPI ObserveEvent(HANDLE event)
{
    if(ReadAcquire(&Recording)) {
        if(event==Progress) InterlockedIncrement(&ProgressSignals);
        if(event==Kernel) InterlockedIncrement(&KernelSignals);
    }
    return SetEvent(event);
}
static DWORD WINAPI ObserveWait(HANDLE event,DWORD timeout)
{
    BOOLEAN parked=event==Progress && timeout==1000;
    if(parked) {
        InterlockedIncrement(&Parks);CHECK(SetEvent(ParkEntered));
        CHECK(WaitForSingleObject(ParkGate,5000)==WAIT_OBJECT_0);
    }
    DWORD result=WaitForSingleObject(event,timeout);
    if(parked && result==WAIT_OBJECT_0) {InterlockedIncrement(&EventWakes);CHECK(SetEvent(WakeSeen));}
    if(parked && result==WAIT_TIMEOUT) InterlockedIncrement(&Timeouts);
    return result;
}
typedef struct FIXTURE {PB_SESSION *S;PB_USER_QUEUE *Q;HANDLE DrainEntered,DrainGate,QueryEntered,QueryGate;} FIXTURE;
static HANDLE Event(BOOL manual)
{HANDLE event=CreateEventW(NULL,manual,FALSE,NULL);CHECK(event);return event;}
static FIXTURE Create(VOID)
{
    FIXTURE f={0};PB_CONFIG config={sizeof(config),1,8,8,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&f.S,&region,&layout,&epoch)==PB_OK && Api.OpenQueue(f.S,0,&f.Q)==PB_OK);
    PB_DESCRIPTOR d;uint32_t n;CHECK(Api.PollCompletions(f.Q,&d,1,&n)==PB_EMPTY && !n); /* Claim this owner. */
    f.DrainEntered=Event(FALSE);f.DrainGate=Event(TRUE);f.QueryEntered=Event(FALSE);f.QueryGate=Event(TRUE);
    ParkEntered=Event(FALSE);ParkGate=Event(TRUE);WakeSeen=Event(TRUE);Progress=f.S->ProgressEvent;Kernel=f.Q->KernelEvent;
    Recording=ProgressSignals=KernelSignals=Parks=EventWakes=Timeouts=0;PbTestActive(f.S,1);return f;
}
static VOID CloseFixture(FIXTURE *f)
{
    InterlockedExchange(&Recording,0);CHECK(SetEvent(f->DrainGate) && SetEvent(f->QueryGate) && SetEvent(ParkGate));
    PbTestActive(f->S,0);ULONGLONG start=GetTickCount64();PB_RESULT prepared;
    do {
        prepared=Api.PrepareClose(f->S,100);
        CHECK((prepared==PB_OK || prepared==PB_CORRUPT || prepared==PB_QUARANTINED) &&
            !f->S->ResourcesClosed && GetTickCount64()-start<5000);
        if(prepared!=PB_OK) SwitchToThread();
    } while(prepared!=PB_OK);
    Retain(f->S);CHECK(Api.Close(f->S,2000)==PB_OK && f->S->ResourcesClosed);
    WaitForThreadpoolWorkCallbacks(f->S->Work,FALSE);Progress=Kernel=NULL;DropReference(f->S);
    CHECK(CloseHandle(f->DrainEntered) && CloseHandle(f->DrainGate) && CloseHandle(f->QueryEntered) && CloseHandle(f->QueryGate));
    CHECK(CloseHandle(ParkEntered) && CloseHandle(ParkGate) && CloseHandle(WakeSeen));ParkEntered=ParkGate=WakeSeen=NULL;
    PB_RUNTIME_STATUS status;CHECK(Life.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !GuardAllocations);
}
static VOID StopAtDrain(FIXTURE *f)
{
    PbTestBlock(f->S,PB_TEST_DRAIN,f->DrainEntered,f->DrainGate);CHECK(Life.BeginStop(f->S)==PB_OK);
    CHECK(WaitForSingleObject(f->DrainEntered,2000)==WAIT_OBJECT_0 && ResetEvent(Progress));
}
static VOID StopAtQuery(FIXTURE *f)
{
    PbTestBlock(f->S,PB_TEST_QUERY,f->QueryEntered,f->QueryGate);StopAtDrain(f);
    CHECK(SetEvent(f->DrainGate) && WaitForSingleObject(f->QueryEntered,2000)==WAIT_OBJECT_0 && ResetEvent(Progress));
}
static VOID CheckRetained(FIXTURE *f)
{
    PB_CLOSE_PROGRESS p;CHECK(Life.GetCloseProgress(f->S,&p)==PB_OK && p.Phase<PB_LIFE_PREPARED &&
        p.RetainedBytes && !f->S->ResourcesClosed && f->S->TestActive==1);
}
static VOID Record(VOID)
{ProgressSignals=KernelSignals=0;InterlockedExchange(&Recording,1);}
static VOID NoOwnershipProgress(VOID)
{
    FIXTURE f=Create();StopAtQuery(&f);Record();
    PB_TOKEN stale={f.S->Epoch,1,0,0};PB_LEASE lease={0};PB_DESCRIPTOR d;uint32_t n;
    for(ULONG i=0;i<100;++i) {
        CHECK(Api.PollCompletions(f.Q,&d,1,&n)==PB_EMPTY && !n);
        CHECK(Api.ReceiveBatch(f.Q,&lease,1,&n)==PB_STOPPED && !n);
        CHECK(Api.ReserveBatch(f.Q,&lease,1,&n)==PB_STOPPED && !n);
        CHECK(Api.CommitBatch(f.Q,&lease,1,&n)==PB_STOPPED && !n);
        CHECK(Api.CancelBatch(f.Q,&stale,1)==PB_STALE);
        CHECK(Api.ReleaseBatch(f.Q,&stale,1,&n)==PB_STALE && !n);
    }
    InterlockedExchange(&Recording,0);CHECK(!KernelSignals && !f.Q->Loans && !f.Q->Published && !f.S->Fatal);CheckRetained(&f);
#ifdef PB_CLOSE_WAKE_BASELINE
    CHECK(ProgressSignals==600 && WaitForSingleObject(Progress,0)==WAIT_OBJECT_0);
    CHECK(SetEvent(Progress)); /* Restore the real coalesced signal consumed by inspection. */
#else
    CHECK(!ProgressSignals && WaitForSingleObject(Progress,0)==WAIT_TIMEOUT);
#endif
    CHECK(SetEvent(f.QueryGate) && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);
    PbTestBlock(f.S,PB_TEST_QUERY,NULL,NULL);CHECK(SetEvent(ParkGate));
#ifdef PB_CLOSE_WAKE_BASELINE
    CHECK(WaitForSingleObject(WakeSeen,2000)==WAIT_OBJECT_0 && EventWakes>=1 && PbTestQueries(f.S)>=2);
#else
    CHECK(WaitForSingleObject(WakeSeen,50)==WAIT_TIMEOUT && !EventWakes);
#endif
    CheckRetained(&f);
    printf("PASS: 600 empty/stopped/stale actual queue calls: progress SetEvent=%ld kernel SetEvent=%ld native-event wakes=%ld diagnostic timeouts=%ld; ownership unchanged, native ack still blocks close.\n",ProgressSignals,KernelSignals,EventWakes,Timeouts);
    CloseFixture(&f);
}
static VOID LoanReturn(BOOLEAN transmit)
{
    FIXTURE f=Create();PB_LEASE lease;PB_TOKEN token;uint32_t n,notify;
    if(transmit) {
        CHECK(PbPoolReserve(f.S->TestTxPools,&token)==PB_OK && PbPoolTransition(f.S->TestTxPools,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        PB_DESCRIPTOR d={.Token=token,.Length=20,.DataOffset=f.S->Layout.Queues[0].TxData+token.Slot*PB_BLOCK_SIZE};
        CHECK(PbQueuePush(&f.S->TestRings[0][PB_QUEUE_TX_READY],&d,1,&n,&notify)==PB_OK && n==1);
        CHECK(Api.ReceiveBatch(f.Q,&lease,1,&n)==PB_OK && n==1);
        CHECK(PbQueueArm(&f.S->TestRings[0][PB_QUEUE_TX_RECYCLE],&n)==PB_OK && !n);
    } else {CHECK(Api.ReserveBatch(f.Q,&lease,1,&n)==PB_OK && n==1);token=lease.Descriptor.Token;}
    StopAtDrain(&f);CHECK(SetEvent(f.DrainGate) && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);
    Record();
    if(transmit) CHECK(Api.ReleaseBatch(f.Q,&token,1,&n)==PB_OK && n==1);
    else CHECK(Api.CancelBatch(f.Q,&token,1)==PB_OK);
    InterlockedExchange(&Recording,0);CHECK(ProgressSignals==1 && KernelSignals==(LONG)transmit && !f.Q->Loans);CheckRetained(&f);
    CHECK(SetEvent(ParkGate) && WaitForSingleObject(WakeSeen,2000)==WAIT_OBJECT_0 && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);
    PB_CLOSE_PROGRESS p;CHECK(Life.GetCloseProgress(f.S,&p)==PB_OK && p.NativeReferences==1);CheckRetained(&f);
    if(transmit) {
        PB_TOKEN returned;CHECK(PbQueuePopTokens(&f.S->TestRings[0][PB_QUEUE_TX_RECYCLE],&returned,1,&n)==PB_OK && n==1 && !memcmp(&returned,&token,sizeof(token)));
        CHECK(PbPoolTransition(f.S->TestTxPools,&returned,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);
    }
    printf("PASS: close-time %s: one actual progress wake resumes the lease-blocked worker; kernel notification policy and independent native barrier preserved.\n",transmit?"TX release":"RX cancel");
    CloseFixture(&f);
}
static VOID CompletionOrFault(BOOLEAN corrupt,ULONG status)
{
    FIXTURE f=Create();PB_LEASE lease;uint32_t n,notify;PB_DESCRIPTOR d,output;
    CHECK(Api.ReserveBatch(f.Q,&lease,1,&n)==PB_OK && n==1);lease.Descriptor.Length=20;
    CHECK(Api.CommitBatch(f.Q,&lease,1,&n)==PB_OK && n==1);
    CHECK(PbQueuePopBudget(&f.S->TestRings[0][PB_QUEUE_RX_SUBMIT],&d,1,128u*1024u,&n)==PB_OK && n==1);
    d.Status=status;StopAtQuery(&f);if(corrupt) d.Token.Epoch++;
    Record();ULONG calls=corrupt?17:1;
    for(ULONG i=0;i<calls;++i) {
        CHECK(PbQueuePush(&f.S->TestRings[0][PB_QUEUE_RX_COMPLETE],&d,1,&n,&notify)==PB_OK && n==1);
        CHECK(Api.PollCompletions(f.Q,&output,1,&n)==(corrupt?PB_CORRUPT:PB_OK) && n==(corrupt?0u:1u));
        if(!corrupt) CHECK(!memcmp(&d,&output,sizeof(d)));
    }
    InterlockedExchange(&Recording,0);
#ifdef PB_CLOSE_WAKE_BASELINE
    CHECK(ProgressSignals==(corrupt?18:1));
#else
    CHECK(ProgressSignals==1);
#endif
#ifdef PB_CLOSE_WAKE_BASELINE
    CHECK(KernelSignals==(corrupt?0:1));
#else
    CHECK(KernelSignals==(LONG)calls); /* Every successful Pop released ring space. */
#endif
    CHECK(f.Q->Published==(corrupt?1:0) && !!f.S->Fatal==corrupt);CheckRetained(&f);
    CHECK(SetEvent(f.QueryGate) && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);CheckRetained(&f);
    CHECK(SetEvent(ParkGate) && WaitForSingleObject(WakeSeen,2000)==WAIT_OBJECT_0 && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);
    CHECK(f.Q->Published==(corrupt?1:0));CheckRetained(&f);
    printf("PASS: close-time %s packet_status=%lu calls=%lu: progress signals=%ld kernel signals=%ld; first fault/success wakes retained, repeated failures cannot authorize retirement, STOPPED acknowledgement still required.\n",corrupt?"corrupt completion":"valid completion",status,calls,ProgressSignals,KernelSignals);
    CloseFixture(&f);
}
static DWORD WINAPI KernelTerminal(VOID *opaque)
{
    PB_SESSION *s=opaque;ULONGLONG start=GetTickCount64();
    while(!PbTestWaiters(s)) {CHECK(GetTickCount64()-start<2000);SwitchToThread();}
    PbTestFault(s);return 0;
}
static VOID FirstTerminalObservation(VOID)
{
    FIXTURE f=Create();CHECK(ResetEvent(Progress));Record();
    HANDLE terminal=CreateThread(NULL,0,KernelTerminal,f.S,0,NULL);CHECK(terminal);
    CHECK(Api.Wait(f.Q,INFINITE)==PB_STOPPED && WaitForSingleObject(terminal,2000)==WAIT_OBJECT_0 && CloseHandle(terminal));
    InterlockedExchange(&Recording,0);
    /* One progress event is the hosted native terminal publication, the other
     * is Wait's first local CloseRequested transition. Both remain required. */
    CHECK(ProgressSignals==2 && !KernelSignals && f.S->CloseRequested && !f.S->Fatal);CheckRetained(&f);
    CloseFixture(&f);puts("PASS: actual Wait(INFINITE) sees the first kernel terminal hint: native and first local-stop progress notifications preserved, no premature native close.");
}
static VOID RetrySnapshot(VOID)
{
    FIXTURE f=Create();PbTestFail(f.S,PB_TEST_DRAIN,TRUE);CHECK(Life.BeginStop(f.S)==PB_OK);
    CHECK(WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0 && Api.PrepareClose(f.S,2000)==PB_CORRUPT);
    PB_CLOSE_PROGRESS p;CHECK(Life.GetCloseProgress(f.S,&p)==PB_OK && p.Result==PB_CORRUPT && p.LastError==ERROR_DEVICE_NOT_CONNECTED);CheckRetained(&f);
    /* Queue failure completion is not involved: advance the actual unchanged
     * lifecycle worker from its failed snapshot into a controlled retry. */
    PbTestBlock(f.S,PB_TEST_DRAIN,f.DrainEntered,f.DrainGate);
    CHECK(SetEvent(ParkGate) && WaitForSingleObject(f.DrainEntered,2000)==WAIT_OBJECT_0);
    CHECK(Life.GetCloseProgress(f.S,&p)==PB_OK && p.Result==PB_QUARANTINED && !p.LastError);CheckRetained(&f);
    CHECK(ResetEvent(ParkGate) && SetEvent(f.DrainGate) && WaitForSingleObject(ParkEntered,2000)==WAIT_OBJECT_0);
    CHECK(Life.GetCloseProgress(f.S,&p)==PB_OK && p.Result==PB_CORRUPT && p.LastError==ERROR_DEVICE_NOT_CONNECTED);CheckRetained(&f);
    PbTestFail(f.S,PB_TEST_DRAIN,FALSE);CloseFixture(&f);
    puts("PASS: deterministic asynchronous retry snapshots: failed error -> in-flight quarantine -> failed error, mapping/native barrier retained throughout; progress reads need not match an earlier Close result.");
}
static HANDLE SpaceReady,SpaceDone;
static DWORD WINAPI FlushNativePeer(VOID *opaque)
{
    PB_KERNEL_QUEUE *q=opaque;CHECK(SetEvent(SpaceReady));
    CHECK(WaitForSingleObject(q->KernelEvent->Native,5000)==WAIT_OBJECT_0);
    KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->RxLock,&lock);PbV2FlushLocked(q);KeReleaseInStackQueuedSpinLock(&lock);
    CHECK(!q->Credits && !q->PendingCount && SetEvent(SpaceDone));return 0;
}
static VOID InvalidCompletionSpaceWake(VOID)
{
    FIXTURE f=Create();f.S->Test=0; /* Use actual native QUERY, not TestActive as its acknowledgement. */
    PB_LEASE lease;uint32_t n,notify;CHECK(Api.ReserveBatch(f.Q,&lease,1,&n)==PB_OK && n==1);
    lease.Descriptor.Length=20;((UCHAR *)lease.Data)[0]=0x45;((UCHAR *)lease.Data)[3]=20;
    CHECK(Api.CommitBatch(f.Q,&lease,1,&n)==PB_OK && n==1);
    PB_KERNEL_SESSION native={0};TUN_CTX ctx={0};PB_RX_SLOT slots[8]={0};PB_DESCRIPTOR pending[8]={0};
    MDL parent={.Address=f.S->Region,.Length=f.S->Layout.RegionSize};
    KEVENT kernel={f.Q->KernelEvent},progress={f.S->ProgressEvent},event={f.Q->UserEvent};
    native.Ctx=&ctx;native.Layout=f.S->Layout;native.Epoch=f.S->Epoch;native.Region=f.S->Region;native.Mdl=&parent;
    native.Lifecycle=f.S->SharedLifecycle;native.ProgressEvent=&progress;native.AllReturned.Native=Event(TRUE);CHECK(SetEvent(native.AllReturned.Native));
    ctx.V2=&native;ctx.Running=1;ctx.NblPool=&ctx;NativePeer=&ctx;
    PB_KERNEL_QUEUE *q=native.Queues;q->Session=&native;q->RxSlots=slots;q->Pending=pending;q->KernelEvent=&kernel;q->UserEvent=&event;
    for(ULONG r=0;r<PB_QUEUE_COUNT;++r) CHECK(PbAttachQueue(q->Rings+r,f.S->Region,&f.S->Layout,0,r,r==PB_QUEUE_TX_READY || r==PB_QUEUE_RX_COMPLETE)==PB_OK);
    PB_DESCRIPTOR d;CHECK(PbQueuePopBudget(q->Rings+PB_QUEUE_RX_SUBMIT,&d,1,128u*1024u,&n)==PB_OK && n==1);
    uint32_t complete;CHECK(PbAdmitReceive(&native.Layout,native.Epoch,0,slots,&d,0,&complete)==PB_OK);q->Credits++;
    PbV2RetainReferences(&native,1);PbV2ReleaseReference(&native);
    NET_BUFFER_LIST *nbl=PbV2Receive(q,&d,1,MAXULONG,NULL);CHECK(nbl);PbV2RetainReceives(&native,1,1);
    /* Raw ABI invalid-epoch submissions are echoed as rejected completions,
     * while the original valid NBL still belongs to the protocol stack. */
    PB_DESCRIPTOR bad=d;bad.Token.Epoch++;
    for(ULONG i=0;i<8;++i) {
        CHECK(PbQueuePush(f.Q->Rings+PB_QUEUE_RX_SUBMIT,&bad,1,&n,&notify)==PB_OK && n==1);
        PB_DESCRIPTOR input;CHECK(PbQueuePopBudget(q->Rings+PB_QUEUE_RX_SUBMIT,&input,1,128u*1024u,&n)==PB_OK && n==1);
        q->Credits++;CHECK(PbAdmitReceive(&native.Layout,native.Epoch,0,slots,&input,0,&complete)==PB_INVALID && complete);
        PbV2CompleteLocked(q,&input,PB_INVALID);PbV2FlushLocked(q);
    }
    AcquireSRWLockExclusive(&ctx.TransitionLock);PbV2BeginDrain(&native,1000);ReleaseSRWLockExclusive(&ctx.TransitionLock);
    InterlockedExchange(&f.S->Draining,1); /* The native DRAIN boundary is already acknowledged. */
    CHECK(PbV2ReturnOne(&ctx,nbl) && !native.Active && !native.Returns.References && q->Credits==1 && q->PendingCount==1);
    CHECK(ResetEvent(f.Q->KernelEvent));SpaceReady=Event(FALSE);SpaceDone=Event(TRUE);
    HANDLE worker=CreateThread(NULL,0,FlushNativePeer,q,0,NULL);CHECK(worker && WaitForSingleObject(SpaceReady,2000)==WAIT_OBJECT_0);
    Record();CHECK(Api.PollCompletions(f.Q,&d,1,&n)==PB_CORRUPT && !n && f.Q->Published==1);InterlockedExchange(&Recording,0);
    PB_V2_STATUS status;
#ifdef PB_CLOSE_WAKE_BASELINE
    CHECK(!KernelSignals && WaitForSingleObject(SpaceDone,50)==WAIT_TIMEOUT);
    CHECK(PbV2Query(&ctx,&status)==STATUS_SUCCESS && status.State!=PB_SESSION_STOPPED && !status.ActiveReferences);
    CHECK(Api.PrepareClose(f.S,30)==PB_QUARANTINED && !f.S->ResourcesClosed && f.Q->Published==1);
    CHECK(PbV2Query(&ctx,&status)==STATUS_SUCCESS && status.State!=PB_SESSION_STOPPED && q->Credits==1 && q->PendingCount==1);
    CHECK(SetEvent(f.Q->KernelEvent)); /* Explicit recovery after reproducing the old stall. */
#else
    CHECK(KernelSignals==1);
#endif
    CHECK(WaitForSingleObject(SpaceDone,2000)==WAIT_OBJECT_0 && WaitForSingleObject(worker,2000)==WAIT_OBJECT_0);
    CHECK(PbV2Query(&ctx,&status)==STATUS_SUCCESS && status.State==PB_SESSION_STOPPED && !status.ActiveReferences && native.Completions==9);
    CHECK(SetEvent(ParkGate) && Api.PrepareClose(f.S,2000)==PB_OK && !f.Q->Published && !f.S->ResourcesClosed);
    CHECK(!Pools && !Mdls && !Nbls);CloseFixture(&f);NativePeer=NULL;
    CHECK(CloseHandle(worker) && CloseHandle(SpaceReady) && CloseHandle(SpaceDone) && CloseHandle(native.AllReturned.Native));SpaceReady=SpaceDone=NULL;
#ifdef PB_CLOSE_WAKE_BASELINE
    puts("PASS: expected old corruption-space stall reproduced after DRAIN: actual native refs=0, pending=1, credit=1 keep close quarantined until explicit recovery wake.");
#else
    puts("PASS: invalid completion space release wakes actual SYS completion/query paths: pending and credit drain, native STOPPED authorizes local retirement, no manual recovery wake or early mapping release.");
#endif
}
static VOID NativeFaultFullRecycle(VOID)
{
    FIXTURE f=Create();PB_TOKEN token;PB_LEASE lease;uint32_t n,notify;
    CHECK(PbPoolReserve(f.S->TestTxPools,&token)==PB_OK && PbPoolTransition(f.S->TestTxPools,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    PB_DESCRIPTOR d={.Token=token,.Length=20,.DataOffset=f.S->Layout.Queues[0].TxData+token.Slot*PB_BLOCK_SIZE};
    CHECK(PbQueuePush(&f.S->TestRings[0][PB_QUEUE_TX_READY],&d,1,&n,&notify)==PB_OK && n==1);
    CHECK(Api.ReceiveBatch(f.Q,&lease,1,&n)==PB_OK && n==1);
    PB_TOKEN junk[8];for(ULONG i=0;i<8;++i) {junk[i]=token;junk[i].Epoch++;}
    CHECK(PbQueuePushTokens(f.Q->Rings+PB_QUEUE_TX_RECYCLE,junk,8,&n,&notify)==PB_OK && n==8);
    /* Preserve ordinary running backpressure: no local release or fault. */
    CHECK(Api.ReleaseBatch(f.Q,&token,1,&n)==PB_FULL && !n && f.Q->Loans==1 && !f.S->Fatal && !f.S->CloseRequested);
    PbTestFault(f.S); /* Native-only terminal hint, with a noncooperating full recycle ring. */
#if defined(PB_CLOSE_WAKE_BASELINE) || defined(PB_CLOSE_FULL_BASELINE)
    CHECK(Api.ReleaseBatch(f.Q,&token,1,&n)==PB_FULL && !n && f.Q->Loans==1 && !f.S->Fatal);
    CHECK(Api.PrepareClose(f.S,0)==PB_BUSY && !f.S->ResourcesClosed);
    /* Explicit recovery: make the old ring detectably corrupt so its existing
     * local-fatal fallback can finally retire the view for fixture teardown. */
    InterlockedExchange64((volatile LONG64 *)&f.Q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Consumer,9);
    CHECK(Api.ReleaseBatch(f.Q,&token,1,&n)==PB_OK && n==1 && f.S->Fatal);
#else
    Record();CHECK(Api.ReleaseBatch(f.Q,&token,1,&n)==PB_OK && n==1 && !f.Q->Loans && !f.S->Fatal);InterlockedExchange(&Recording,0);
    CHECK(!KernelSignals && f.Q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==8);
#endif
    CHECK(f.Q->TxSlots[token.Slot].State==PB_SLOT_FREE && f.S->TestTxSlots[0][token.Slot].State==PB_SLOT_PUBLISHED);
    CHECK(Api.PrepareClose(f.S,30)==PB_QUARANTINED && !f.S->ResourcesClosed);CheckRetained(&f);
    CloseFixture(&f);
#if defined(PB_CLOSE_WAKE_BASELINE) || defined(PB_CLOSE_FULL_BASELINE)
    puts("PASS: expected old native-only fault/full recycle stall: valid user loan remains busy until explicit local-fatal recovery; ordinary running backpressure preserved.");
#else
    puts("PASS: native-only fault/full recycle ring: stopped admission allows local view retirement without recycling/reusing native slot; active native barrier still blocks close, ordinary running backpressure unchanged.");
#endif
}
typedef struct RELEASE_JOB {PB_USER_QUEUE *Queue;const PB_TOKEN *Tokens;PB_RESULT Result;uint32_t Accepted;} RELEASE_JOB;
static DWORD WINAPI ForeignRelease(VOID *opaque)
{
    RELEASE_JOB *job=opaque;job->Result=Api.ReleaseBatch(job->Queue,job->Tokens,4,&job->Accepted);return 0;
}
/* A full recycle ring cannot occur with four outstanding loans and capacity
 * eight under the ordinary producer contract. Raw junk models a noncooperating
 * peer. Every retirement still has to validate all four real private loans. */
static VOID StoppedReleaseBoundaries(VOID)
{
    ULONG scenarios=0,invalidBatches=0;
    for(ULONG nativeFault=0;nativeFault<2;++nativeFault)
    for(ULONG room=0;room<=2;room+=2)
    for(ULONG wrap=0;wrap<2;++wrap) {
        FIXTURE f=Create();PB_DESCRIPTOR ready[4];PB_LEASE leases[4];uint32_t n,notify;
        PB_TEST_GUARD input=PbTestGuardCreate(4*sizeof(PB_TOKEN));
        PB_TOKEN *tokens=(PB_TOKEN *)PbTestGuardSpan(&input,4*sizeof(PB_TOKEN));
        for(ULONG i=0;i<4;++i) {
            CHECK(PbPoolReserve(f.S->TestTxPools,tokens+i)==PB_OK &&
                PbPoolTransition(f.S->TestTxPools,tokens+i,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
            ready[i]=(PB_DESCRIPTOR){.Token=tokens[i],.Length=20,
                .DataOffset=f.S->Layout.Queues[0].TxData+tokens[i].Slot*PB_BLOCK_SIZE};
        }
        CHECK(PbQueuePush(&f.S->TestRings[0][PB_QUEUE_TX_READY],ready,4,&n,&notify)==PB_OK && n==4);
        CHECK(Api.ReceiveBatch(f.Q,leases,4,&n)==PB_OK && n==4 && f.Q->Loans==4);
        PB_QUEUE_VIEW *producer=f.Q->Rings+PB_QUEUE_TX_RECYCLE;
        PB_QUEUE_VIEW *consumer=&f.S->TestRings[0][PB_QUEUE_TX_RECYCLE];
        ULONG64 base=wrap?UINT64_MAX-3:0;
        producer->Cursor=producer->PeerCursor=consumer->Cursor=consumer->PeerCursor=base;
        producer->Shared->Producer=producer->Shared->Consumer=base;
        PB_TOKEN junk[8];for(ULONG i=0;i<8;++i) {junk[i]=tokens[0];junk[i].Epoch++;}
        CHECK(PbQueuePushTokens(producer,junk,8-room,&n,&notify)==PB_OK && n==8-room);
        if(nativeFault) {
            PbTestFault(f.S);CHECK(Api.ReceiveBatch(f.Q,leases,4,&n)==PB_STOPPED && !n && !f.S->Fatal);
        }
        StopAtDrain(&f);CHECK(!f.S->Fatal);
        /* An arm window may still be in flight with existing data. Only actual
         * publication exchanges Armed; validation and local retirement must not. */
        producer->Shared->Armed=1;
        PB_POOL pool=f.Q->Tx;PB_SLOT slots[8];memcpy(slots,f.Q->TxSlots,sizeof(slots));
        PB_QUEUE_SHARED shared=*producer->Shared;PB_DESCRIPTOR entries[8];memcpy(entries,producer->Entries,sizeof(entries));
        ULONG64 cursor=producer->Cursor,peer=producer->PeerCursor;
        for(ULONG fault=0;fault<9;++fault) {
            PB_TOKEN saved=tokens[3];uint32_t count=4;const PB_TOKEN *items=tokens;PB_RESULT expected=PB_STALE;
            if(fault==0) tokens[3].Epoch++;
            if(fault==1) tokens[3].Generation++;
            if(fault==2) tokens[3].Slot=8;
            if(fault==3) tokens[3].Queue=1;
            if(fault==4) tokens[3].Generation=0;
            if(fault==5) {tokens[3]=tokens[0];expected=PB_INVALID;}
            if(fault==6) {count=0;expected=PB_INVALID;}
            if(fault==7) {count=PB_MAX_BATCH+1;expected=PB_INVALID;}
            if(fault==8) {items=NULL;expected=PB_INVALID;}
            Record();n=123;CHECK(Api.ReleaseBatch(f.Q,items,count,&n)==expected && !n);InterlockedExchange(&Recording,0);
            tokens[3]=saved;
            CHECK(!KernelSignals && !f.S->Fatal && f.Q->Loans==4 &&
                !memcmp(&pool,&f.Q->Tx,sizeof(pool)) && !memcmp(slots,f.Q->TxSlots,sizeof(slots)) &&
                !memcmp(&shared,producer->Shared,sizeof(shared)) && !memcmp(entries,producer->Entries,sizeof(entries)) &&
                producer->Cursor==cursor && producer->PeerCursor==peer);
#ifdef PB_CLOSE_WAKE_BASELINE
            CHECK(ProgressSignals==1);
#else
            CHECK(!ProgressSignals);
#endif
            invalidBatches++;
        }
        RELEASE_JOB job={f.Q,tokens,PB_OK,123};Record();
        HANDLE thread=CreateThread(NULL,0,ForeignRelease,&job,0,NULL);CHECK(thread && WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);
        InterlockedExchange(&Recording,0);CHECK(CloseHandle(thread) && job.Result==PB_BUSY && !job.Accepted &&
            !ProgressSignals && !KernelSignals && f.Q->Loans==4 && !memcmp(slots,f.Q->TxSlots,sizeof(slots)));
        ULONG published=0;
        if(room) {
            Record();CHECK(Api.ReleaseBatch(f.Q,tokens,4,&n)==PB_OK && n==room);InterlockedExchange(&Recording,0);
            CHECK(ProgressSignals==1 && KernelSignals==1 && producer->Cursor==base+8 && f.Q->Loans==2 && !f.S->Fatal);
            for(ULONG i=0;i<4;++i) CHECK(f.Q->TxSlots[tokens[i].Slot].State==(i<room?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
            published=room;
        }
        Record();PB_RESULT result=Api.ReleaseBatch(f.Q,tokens+published,4-published,&n);InterlockedExchange(&Recording,0);
#ifdef PB_CLOSE_WAKE_BASELINE
        CHECK(result==PB_FULL && !n && f.Q->Loans==(LONG)(4-published) && !f.S->Fatal && !KernelSignals);
        /* Old code's expected stall is recovered only for teardown. The real
         * native consumer below must still see the unchanged original ring. */
        producer->Shared->Consumer=base+9;
        CHECK(Api.ReleaseBatch(f.Q,tokens+published,4-published,&n)==PB_OK && n==4-published && f.S->Fatal);
        producer->Shared->Consumer=base;
#else
        CHECK(result==PB_OK && n==4-published && !f.Q->Loans && !f.S->Fatal && !KernelSignals && ProgressSignals==1);
#endif
        CHECK(producer->Cursor==base+8 && !f.Q->Loans && producer->Shared->Armed==(room?0u:1u));
        pool=f.Q->Tx;memcpy(slots,f.Q->TxSlots,sizeof(slots));
        Record();CHECK(Api.ReleaseBatch(f.Q,tokens,4,&n)==PB_BUSY && !n);InterlockedExchange(&Recording,0);
        CHECK(!KernelSignals && !f.Q->Loans && !memcmp(&pool,&f.Q->Tx,sizeof(pool)) && !memcmp(slots,f.Q->TxSlots,sizeof(slots)));
#ifdef PB_CLOSE_WAKE_BASELINE
        CHECK(ProgressSignals==1);
#else
        CHECK(!ProgressSignals);
#endif
        /* Drop only the deliberately invalid raw inputs, then use the actual
         * native recycler for the prefix that was really published. A local
         * return must leave every unpublished native slot PUBLISHED. */
        PB_TOKEN discarded[8];CHECK(PbQueuePopTokens(consumer,discarded,8-room,&n)==PB_OK && n==8-room && !memcmp(discarded,junk,n*sizeof(*junk)));
        TUN_CTX ctx={0};PB_KERNEL_SESSION native={0};native.Ctx=&ctx;native.Layout=f.S->Layout;
        PB_KERNEL_QUEUE *q=native.Queues;q->Session=&native;q->TxPool=f.S->TestTxPools[0];q->TxPoolExhausted=TRUE;
        q->Rings[PB_QUEUE_TX_RECYCLE]=*consumer;
        CHECK(PbV2RecycleLocked(q)==PB_OK && !native.Faulted && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==base+8 &&
            q->TxPoolExhausted==!published && q->TxPool.FreeBits[0]==(0xf0u|((1u<<published)-1u)) && f.Q->Tx.FreeBits[0]==0xffu);
        for(ULONG i=0;i<4;++i) CHECK(f.Q->TxSlots[tokens[i].Slot].State==PB_SLOT_FREE &&
            f.S->TestTxSlots[0][tokens[i].Slot].State==(i<published?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
        CHECK(Api.PrepareClose(f.S,0)==PB_QUARANTINED && !f.S->ResourcesClosed);CheckRetained(&f);
        PbTestGuardDestroy(&input);CloseFixture(&f);scenarios++;
    }
    CHECK(scenarios==8 && invalidBatches==72);
    puts("PASS: eight actual stopped-release boundary scenarios, 72 all-or-nothing invalid batches with guarded token inputs, wrong-thread rejection, normal/native-only stop, full/partial wrapped publication and repeated return; actual SYS recycler frees only the published prefix, native acknowledgement still blocks reclamation.");
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    NoOwnershipProgress();LoanReturn(FALSE);LoanReturn(TRUE);CompletionOrFault(FALSE,PB_OK);
    CompletionOrFault(FALSE,PB_RX_REJECTED);CompletionOrFault(TRUE,PB_OK);
    FirstTerminalObservation();RetrySnapshot();InvalidCompletionSpaceWake();NativeFaultFullRecycle();StoppedReleaseBoundaries();return 0;
}
