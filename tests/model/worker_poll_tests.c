/* SPDX-License-Identifier: GPL-2.0 */
/* Execute the production worker with deterministic event/fault interleavings, no SYS loaded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
#define NT_ASSERT CHECK
typedef struct { BOOLEAN Signaled; } KEVENT, *PKEVENT, MDL, EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
#define PbQueueArm ActualQueueArm
#include "../../common/v2_core.c"
#undef PbQueueArm
typedef struct _TUN_CTX { volatile LONG Running; struct { volatile LONG V2SessionActive; KEVENT RunningEvent; } Probe; } TUN_CTX;
typedef VOID KSTART_ROUTINE(PVOID);
typedef struct { KSPIN_LOCK *Lock; } KLOCK_QUEUE_HANDLE;
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
#define WaitAny 0
#define STATUS_SUCCESS 0
enum { NO_FAULT, BEFORE_CLEAR, AFTER_CLEAR, IN_RECYCLE, IN_RECEIVE, IN_ARM_RX, IN_ARM_TX };
static ULONG Injection, Clears, Arms, Sleeps, LostFaultWaits, Exits, Receives;
static PB_KERNEL_SESSION *Session;
static BOOLEAN RealRecycle,PublishedReturn;
static PB_QUEUE_VIEW ReturnProducer;
static PB_DESCRIPTOR Returned;
static ULONG LostRecycleWaits;
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait) { (void)priority; (void)wait; event->Signaled=TRUE; }
#include "../../driver/v2/session_fault.h"
#define PbV2RecycleLocked ActualRecycle
#include "../../driver/v2/session_recycle.h"
#undef PbV2RecycleLocked
static VOID PublishReturn(VOID)
{
    if(PublishedReturn) return;
    uint32_t count,notify;
    CHECK(PbQueuePush(&ReturnProducer,&Returned,1,&count,&notify)==PB_OK && count==1);
    PublishedReturn=TRUE;
    if(notify) KeSetEvent(Session->Queues[0].KernelEvent,0,FALSE);
}
static VOID KeClearEvent(KEVENT *event)
{
    CHECK(++Clears<4);
    if(RealRecycle) {
        if(Injection==0) PublishReturn();
        event->Signaled=FALSE;
        if(Injection==1) PublishReturn();
        return;
    }
    if (Injection==BEFORE_CLEAR) PbV2Fault(Session);
    event->Signaled=FALSE;
    if (Injection==AFTER_CLEAR) PbV2Fault(Session);
}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{ (void)reason; (void)mode; (void)alertable; (void)timeout; CHECK(event->Signaled); }
static VOID KeWaitForMultipleObjects(ULONG count,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{
    (void)type; (void)reason; (void)mode; (void)alertable; (void)timeout; (void)blocks; CHECK(count==2 || count==3); Sleeps++;
    KEVENT *kernel=events[1];
    if(RealRecycle && PublishedReturn && Session->Queues[0].TxSlots[0].State!=PB_SLOT_FREE && !kernel->Signaled)
        LostRecycleWaits++;
    if (Session->Faulted && !kernel->Signaled) LostFaultWaits++;
    if (kernel->Signaled) kernel->Signaled=FALSE;
    else Session->Stopping=1; /* A real worker would remain asleep here; keep a failing test bounded. */
}
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{ CHECK(!*lock); *lock=1; handle->Lock=lock; }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) { CHECK(*handle->Lock==1); *handle->Lock=0; }
static PB_RESULT PbV2RecycleLocked(PB_KERNEL_QUEUE *q)
{
    if(RealRecycle) {PB_RESULT result=ActualRecycle(q);if(Injection==2) PublishReturn();return result;}
    if(Injection==IN_RECYCLE) {PbV2Fault(q->Session);return PB_CORRUPT;}return PB_OK;
}
static VOID PbV2FlushLocked(PB_KERNEL_QUEUE *q) { (void)q; }
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q) { (void)q; Receives++;if(!RealRecycle && Injection==IN_RECEIVE) PbV2Fault(Session); return FALSE; }
PB_RESULT PbQueueArm(PB_QUEUE_VIEW *view,uint32_t *ready)
{
    (void)view; CHECK(++Arms<5); *ready=0;
    if(RealRecycle) {
        if(view==Session->Queues[0].Rings+PB_QUEUE_TX_RECYCLE) {
            if(Injection==3) PublishReturn();
            PB_RESULT result=ActualQueueArm(view,ready);
            if(Injection==4) PublishReturn();
            return result;
        }
        return PB_OK;
    }
    if((Arms==1 && Injection==IN_ARM_RX) || (Arms==2 && Injection==IN_ARM_TX)) PbV2Fault(Session);
    return PB_OK;
}
static VOID PsTerminateSystemThread(LONG status) { CHECK(status==STATUS_SUCCESS); Exits++; }
#ifndef PB_WORKER_SOURCE
#define PB_WORKER_SOURCE "../../driver/v2/session_worker.h"
#endif
#include PB_WORKER_SOURCE
static VOID RecycleWakeSchedules(VOID)
{
    for(ULONG paused=0;paused<2;++paused)
    for(ULONG injection=0;injection<5;++injection) {
#ifdef PB_SEND_FAULT_BASELINE
        if(paused) continue; /* Historical send-fault A/B predates paused recycling. */
#endif
        TUN_CTX ctx={0};PB_KERNEL_SESSION s={0};KEVENT kernel={0},user={0};PB_SLOT slot;
        CHECK(PbCalculateLayout(1,1,2,&s.Layout)==PB_OK);
        UCHAR *region=VirtualAlloc(NULL,s.Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(region);
        Session=&s;s.Ctx=&ctx;s.StartEvent.Signaled=TRUE;ctx.Running=!paused;
        PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;q->KernelEvent=&kernel;q->UserEvent=&user;q->TxSlots=&slot;
        CHECK(PbPoolInitialize(&q->TxPool,&slot,1,42,0)==PB_OK);
        CHECK(PbPoolReserve(&q->TxPool,&Returned.Token)==PB_OK);
        CHECK(PbPoolTransition(&q->TxPool,&Returned.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        q->TxPoolExhausted=TRUE;
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_TX_RECYCLE,region,&s.Layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
        CHECK(PbAttachQueue(&ReturnProducer,region,&s.Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
        q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Armed=injection<=1?1u:0u;
        RealRecycle=TRUE;PublishedReturn=FALSE;Injection=injection;
        Clears=Arms=Sleeps=LostFaultWaits=LostRecycleWaits=Exits=Receives=0;
        PbV2Worker(q);
        CHECK(PublishedReturn && !LostRecycleWaits && !LostFaultWaits && Exits==1 && !s.Faulted);
        CHECK(slot.State==PB_SLOT_FREE && !q->TxPoolExhausted && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==1);
        CHECK(!paused || !Receives);
        CHECK(Sleeps<=2 && VirtualFree(region,0,MEM_RELEASE));RealRecycle=FALSE;
    }
#ifdef PB_SEND_FAULT_BASELINE
    puts("PASS: historical running worker+recycle+ring, return before/after clear, after empty drain, before/after arm; no lost return wake.");
#else
    puts("PASS: actual running/paused worker+recycle+ring, return before/after clear, after empty drain, before/after arm; no lost return wake, no paused receive, full-pool proof cleared only by valid retirement.");
#endif
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);
    ULONG cases=0;
    for(ULONG injection=NO_FAULT;injection<=IN_ARM_TX;++injection) {
        TUN_CTX ctx={0}; PB_KERNEL_SESSION s={0}; KEVENT kernel={0},user={0};
        Session=&s; s.Ctx=&ctx; s.Layout.QueueCount=1; s.Layout.RingCapacity=32; s.StartEvent.Signaled=TRUE;
        ctx.Running=1; ctx.Probe.V2SessionActive=1; s.Queues[0].Session=&s; s.Queues[0].KernelEvent=&kernel; s.Queues[0].UserEvent=&user;
        Injection=injection; Clears=Arms=Sleeps=LostFaultWaits=Exits=Receives=0;
        PbV2Worker(s.Queues);
        CHECK(Exits==1 && !LostFaultWaits);
        if(injection!=NO_FAULT) CHECK(s.Faulted && user.Signaled && !ctx.Probe.V2SessionActive);
        if(injection==IN_RECYCLE) {
#ifdef PB_SEND_FAULT_BASELINE
            CHECK(Receives==1 && Sleeps==1 && Arms==1); /* Draining bypasses the RX arm. */
#else
            CHECK(!Receives && !Sleeps && !Arms);
#endif
            CHECK(s.Queues[0].FaultQuiesced);
            printf("PASS: worker recycle fault: receive attempts=%lu waits=%lu arms=%lu, private quiescence witnessed.\n",Receives,Sleeps,Arms);
        }
        cases++;
    }
    puts("PASS: actual worker, seven deterministic fault/event schedules; no indefinite fault wait after clear, recycle, receive or arm.");
    CHECK(cases==7);RecycleWakeSchedules();return 0;
}
