/* SPDX-License-Identifier: GPL-2.0 */
/* Actual worker/receive/pause/completion with hosted DDIs. Gates schedule the
 * stale running->paused decision without altering the selected wait set. */
#include "kernel_nbl_fixture.h"
#include "../../include/packetbridge_probe.h"
typedef VOID KSTART_ROUTINE(PVOID);
#define STATUS_SUCCESS 0
#define NDIS_STATUS_PAUSED 1u
#define Executive 0
#define KernelMode 0
#define WaitAny 0
static __declspec(thread) LONG CriticalDepth;
static VOID KeEnterCriticalRegion(VOID) {CHECK(!CriticalDepth++);}
static VOID KeLeaveCriticalRegion(VOID) {CHECK(CriticalDepth--==1);}
static VOID ExAcquireResourceSharedLite(SRWLOCK *lock,BOOLEAN wait)
{CHECK(CriticalDepth==1&&wait);AcquireSRWLockShared(lock);}
static VOID ExReleaseResourceLite(SRWLOCK *lock)
{CHECK(CriticalDepth==1);ReleaseSRWLockShared(lock);}
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockShared(lock);}
static ULONG PbEffectiveMask(TUN_CTX *ctx) {(void)ctx;return 0;}
static volatile LONG UnexpectedIndications;
static ULONG PbBeginIndication(TUN_CTX *ctx) {(void)ctx;CHECK(!InterlockedIncrement(&UnexpectedIndications));return 0;}
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) {(void)ctx;(void)bank;CHECK(!InterlockedIncrement(&UnexpectedIndications));}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *lists,ULONG count,ULONG flags)
{(void)adapter;(void)lists;(void)count;(void)flags;CHECK(!InterlockedIncrement(&UnexpectedIndications));}
static PB_KERNEL_SESSION *Current;
static HANDLE ReceiveEntered,ReceiveGate,FirstWaitEntered,FirstWaitGate,FirstObserved,ObserveGate,NextWaitEntered;
static ULONG WaitCount,FirstWaitCount,NextWaitCount,FirstReady,Receives,KernelWakes,IndicationWaits,ReferenceWaits;
static BOOL GatedReceive;
static ULONG ExpectedPausePin=1;
static volatile LONG MemoryBlocks;
static HANDLE ReferenceWaitEntered,PauseDone;
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    CHECK(!reason && !mode && !alertable && !timeout);
    if(Current && event==&Current->AllReturned) {
        CHECK(Current->Returns.References==1);++ReferenceWaits;
        if(ReferenceWaitEntered) CHECK(SetEvent(ReferenceWaitEntered));
    }
    CHECK(WaitForSingleObject(event->Native,5000)==WAIT_OBJECT_0);
}
static VOID KeWaitForMultipleObjects(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{
    CHECK((n==2 || n==3) && !type && !reason && !mode && !alertable && !timeout && !blocks);
    HANDLE handles[3];for(ULONG i=0;i<n;++i) handles[i]=((KEVENT *)events[i])->Native;
    if(!WaitCount++) {
        FirstWaitCount=n;CHECK(SetEvent(FirstWaitEntered) && WaitForSingleObject(FirstWaitGate,5000)==WAIT_OBJECT_0);
        /* The zero-time wait consumes a ready event exactly as the blocking
         * wait would. If empty, continue with the same production wait set. */
        FirstReady=WaitForMultipleObjects(n,handles,FALSE,0);
        CHECK(SetEvent(FirstObserved) && WaitForSingleObject(ObserveGate,5000)==WAIT_OBJECT_0);
        if(FirstReady<WAIT_OBJECT_0+n) return;
        CHECK(FirstReady==WAIT_TIMEOUT);
    } else {NextWaitCount=n;CHECK(SetEvent(NextWaitEntered));}
    CHECK(WaitForMultipleObjects(n,handles,FALSE,5000)<WAIT_OBJECT_0+n);
}
static VOID PsTerminateSystemThread(LONG status) {CHECK(status==STATUS_SUCCESS);}
static VOID PbWaitIndications(TUN_CTX *ctx,ULONG bank)
{
    CHECK(Current && ctx==Current->Ctx && bank==IndicationWaits++ && Current->Returns.References==(LONG)ExpectedPausePin);
    CHECK(TryAcquireSRWLockExclusive(&ctx->TransitionLock));ReleaseSRWLockExclusive(&ctx->TransitionLock);
}
#define PbV2ReceiveBatch ActualReceiveBatch
#include "../../driver/v2/session_receive.h"
#undef PbV2ReceiveBatch
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q)
{
    ++Receives;
    if(!GatedReceive) {GatedReceive=TRUE;CHECK(SetEvent(ReceiveEntered) && WaitForSingleObject(ReceiveGate,5000)==WAIT_OBJECT_0);}
    return ActualReceiveBatch(q);
}
#include "../../driver/v2/session_recycle.h"
#include "../../driver/v2/session_worker.h"
#ifndef PB_PAUSE_SOURCE
#define PB_PAUSE_SOURCE "../../driver/v2/session_pause.h"
#endif
#include PB_PAUSE_SOURCE
static HANDLE Event(VOID) {HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(event);return event;}
static VOID *Memory(SIZE_T size,BOOL region)
{
#ifdef PB_GUARD_TESTS
    (void)region;VOID *memory=PbTestGuardAlloc(size);memset(memory,0,size);
#else
    VOID *memory=region?VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE):calloc(1,size);
#endif
    CHECK(memory);InterlockedIncrement(&MemoryBlocks);return memory;
}
static VOID FreeMemory(VOID *memory,BOOL region)
{
#ifdef PB_GUARD_TESTS
    (void)region;PbTestGuardFree(memory);
#else
    if(region) CHECK(VirtualFree(memory,0,MEM_RELEASE));else free(memory);
#endif
    CHECK(InterlockedDecrement(&MemoryBlocks)>=0);
}
static VOID CountWake(KEVENT *event)
{if(Current) for(ULONG i=0;i<Current->Layout.QueueCount;++i) if(event==Current->Queues[i].KernelEvent) ++KernelWakes;}
static DWORD WINAPI Worker(VOID *queue) {PbV2Worker(queue);return 0;}
static VOID Run(BOOL restart,BOOL manual,BOOL wrap)
{
    CHECK(!Pools && !Mdls && !Nbls && !MemoryBlocks);PB_KERNEL_SESSION s={0};TUN_CTX ctx={0};Current=&s;s.Ctx=&ctx;s.Epoch=42;ctx.V2=&s;ctx.Running=1;
    const ULONG budget=4*PB_MAX_BATCH,total=budget+1;
    CHECK(PbCalculateLayout(1,total,512,&s.Layout)==PB_OK);s.Region=Memory(s.Layout.RegionSize,TRUE);
    s.StartEvent.Native=CreateEventW(NULL,TRUE,TRUE,NULL);s.StopEvent.Native=Event();s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);
    ctx.Probe.RunningEvent.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s.StartEvent.Native && s.AllReturned.Native && ctx.Probe.RunningEvent.Native);
    PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;q->KernelEvent=Memory(sizeof(KEVENT),FALSE);q->UserEvent=Memory(sizeof(KEVENT),FALSE);
    q->KernelEvent->Native=CreateEventW(NULL,manual,FALSE,NULL);q->UserEvent->Native=Event();CHECK(q->KernelEvent->Native);
    q->RxSlots=Memory(total*sizeof(*q->RxSlots),FALSE);q->Pending=Memory(512*sizeof(*q->Pending),FALSE);
    for(ULONG ring=0;ring<PB_QUEUE_COUNT;++ring) CHECK(PbAttachQueue(q->Rings+ring,s.Region,&s.Layout,0,ring,ring==PB_QUEUE_TX_READY || ring==PB_QUEUE_RX_COMPLETE)==PB_OK);
    uint64_t base=wrap?UINT64_MAX-63:0;PB_QUEUE_VIEW *complete=q->Rings+PB_QUEUE_RX_COMPLETE;
    complete->Cursor=complete->PeerCursor=complete->Shared->Producer=complete->Shared->Consumer=base;
    PB_QUEUE_VIEW consumer;CHECK(PbAttachQueue(&consumer,s.Region,&s.Layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);consumer.Cursor=consumer.PeerCursor=base;
    for(ULONG i=0;i<total;++i) {
        q->RxSlots[i].Generation=1;
        q->Pending[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=20,.DataOffset=s.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
    }
    q->PendingCount=q->Credits=total;PbV2FlushLocked(q);CHECK(q->PendingCount==1 && q->Credits==1 && s.Completions==budget);
    PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t count;
    for(ULONG batch=0;batch<4;++batch) CHECK(PbQueuePop(&consumer,items,PB_MAX_BATCH,&count)==PB_OK && count==PB_MAX_BATCH);
    KeSetEvent(q->KernelEvent,0,FALSE); /* Actual completion-consumer space wake. */
    ReceiveEntered=Event();ReceiveGate=Event();FirstWaitEntered=Event();FirstWaitGate=Event();FirstObserved=Event();ObserveGate=Event();NextWaitEntered=Event();
    WaitCount=FirstWaitCount=NextWaitCount=FirstReady=Receives=KernelWakes=IndicationWaits=ReferenceWaits=0;GatedReceive=FALSE;OnSetEvent=CountWake;
    HANDLE thread=CreateThread(NULL,0,Worker,q,0,NULL);CHECK(thread && WaitForSingleObject(ReceiveEntered,5000)==WAIT_OBJECT_0);
    AcquireSRWLockExclusive(&ctx.TransitionLock);WriteRelease(&ctx.Running,0);KeClearEvent(&ctx.Probe.RunningEvent);ReleaseSRWLockExclusive(&ctx.TransitionLock);
    PbV2Pause(&ctx);CHECK(IndicationWaits==2 && ReferenceWaits==1 && !s.Returns.References);
    CHECK(SetEvent(ReceiveGate) && WaitForSingleObject(FirstWaitEntered,5000)==WAIT_OBJECT_0 && FirstWaitCount==2 && q->PendingCount==1 && Receives==1);
    if(restart) {WriteRelease(&ctx.Running,1);KeSetEvent(&ctx.Probe.RunningEvent,0,FALSE);}
    CHECK(SetEvent(FirstWaitGate) && WaitForSingleObject(FirstObserved,5000)==WAIT_OBJECT_0);
    printf("TRACE: restart=%u manual=%u wrap=%u wait_set=%lu ready=%lu pause_wakes=%lu pending=%lu\n",restart,manual,wrap,FirstWaitCount,FirstReady,KernelWakes,q->PendingCount);
#ifdef PB_PAUSE_BASELINE
    CHECK(!KernelWakes && FirstReady==WAIT_TIMEOUT && q->PendingCount==1 && q->Credits==1);
    /* A later unrelated queue wake is required by the old implementation. */
    KeSetEvent(q->KernelEvent,0,FALSE);
#else
    CHECK(KernelWakes==1 && FirstReady==WAIT_OBJECT_0+1);
#endif
    CHECK(SetEvent(ObserveGate) && WaitForSingleObject(NextWaitEntered,5000)==WAIT_OBJECT_0);
    CHECK(!q->PendingCount && !q->Credits && s.Completions==total && !s.Active && !s.Faulted && !s.Draining && WaitCount==2);
    CHECK(NextWaitCount==(restart?2u:3u) && Receives==(restart?2u:1u));
    CHECK(!UnexpectedIndications);
    CHECK(PbQueuePop(&consumer,items,PB_MAX_BATCH,&count)==PB_OK && count==1 && items[0].Token.Slot==budget);
    InterlockedExchange(&s.Stopping,1);KeSetEvent(&s.StopEvent,0,FALSE);CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));
    OnSetEvent=NULL;Current=NULL;
    CHECK(CloseHandle(ReceiveEntered) && CloseHandle(ReceiveGate) && CloseHandle(FirstWaitEntered) && CloseHandle(FirstWaitGate) && CloseHandle(FirstObserved) && CloseHandle(ObserveGate) && CloseHandle(NextWaitEntered));
    CHECK(CloseHandle(s.StartEvent.Native) && CloseHandle(s.StopEvent.Native) && CloseHandle(s.AllReturned.Native) && CloseHandle(ctx.Probe.RunningEvent.Native));
    CHECK(CloseHandle(q->KernelEvent->Native) && CloseHandle(q->UserEvent->Native));FreeMemory(q->KernelEvent,FALSE);FreeMemory(q->UserEvent,FALSE);FreeMemory(q->RxSlots,FALSE);FreeMemory(q->Pending,FALSE);FreeMemory(s.Region,TRUE);
    CHECK(!MemoryBlocks && !Pools && !Mdls && !Nbls);
}
static VOID NoSession(VOID)
{
    TUN_CTX ctx={0};PB_KERNEL_SESSION s={0};Current=&s;s.Ctx=&ctx;ExpectedPausePin=0;
    IndicationWaits=ReferenceWaits=KernelWakes=0;PbV2Pause(&ctx);
    CHECK(IndicationWaits==2 && !ReferenceWaits && !KernelWakes && !s.Returns.References);
    Current=NULL;ExpectedPausePin=1;
}
static VOID TwoQueues(VOID)
{
    TUN_CTX ctx={0};PB_KERNEL_SESSION s={0};KEVENT events[2];Current=&s;s.Ctx=&ctx;ctx.V2=&s;s.Layout.QueueCount=2;
    s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s.AllReturned.Native);
    for(ULONG i=0;i<2;++i) {events[i].Native=CreateEventW(NULL,i!=0,FALSE,NULL);CHECK(events[i].Native);s.Queues[i].KernelEvent=events+i;}
    IndicationWaits=ReferenceWaits=KernelWakes=0;OnSetEvent=CountWake;PbV2Pause(&ctx);
    CHECK(IndicationWaits==2 && ReferenceWaits==1 && !s.Returns.References);
#ifdef PB_PAUSE_BASELINE
    CHECK(!KernelWakes);DWORD expected=WAIT_TIMEOUT;
#else
    CHECK(KernelWakes==2);DWORD expected=WAIT_OBJECT_0;
#endif
    for(ULONG i=0;i<2;++i) CHECK(WaitForSingleObject(events[i].Native,0)==expected && CloseHandle(events[i].Native));
    CHECK(CloseHandle(s.AllReturned.Native));Current=NULL;OnSetEvent=NULL;
}
static DWORD WINAPI PauseThread(VOID *ctx) {PbV2Pause(ctx);CHECK(SetEvent(PauseDone));return 0;}
static VOID ActiveBarrier(BOOL detach)
{
    TUN_CTX ctx={0};PB_KERNEL_SESSION s={0};Current=&s;s.Ctx=&ctx;ctx.V2=&s;
    s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s.AllReturned.Native);PbV2RetainReferences(&s,1);
    ReferenceWaitEntered=Event();PauseDone=Event();IndicationWaits=ReferenceWaits=KernelWakes=0;
    HANDLE thread=CreateThread(NULL,0,PauseThread,&ctx,0,NULL);CHECK(thread && WaitForSingleObject(ReferenceWaitEntered,5000)==WAIT_OBJECT_0);
    CHECK(s.Active==1 && s.Returns.References==1 && IndicationWaits==2 && ReferenceWaits==1 && WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
    if(detach) {AcquireSRWLockExclusive(&ctx.TransitionLock);ctx.V2=NULL;ReleaseSRWLockExclusive(&ctx.TransitionLock);}
    PbV2ReleaseReference(&s);CHECK(WaitForSingleObject(PauseDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(!s.Active && !s.Returns.References && CloseHandle(thread) && CloseHandle(ReferenceWaitEntered) && CloseHandle(PauseDone) && CloseHandle(s.AllReturned.Native));
    ReferenceWaitEntered=PauseDone=NULL;Current=NULL;
}
int main(VOID)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(ULONG schedule=0;schedule<8;++schedule) Run((schedule&1)!=0,(schedule&2)!=0,(schedule&4)!=0);
    NoSession();TwoQueues();ActiveBarrier(FALSE);ActiveBarrier(TRUE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("PASS: 12 actual pause/worker scenarios: 8 stale-decision schedules plus no-session, two-queue and active/detach barriers; frozen wait has no ready event until explicit rescue, current pause wakes progress without polling.");return 0;
}
