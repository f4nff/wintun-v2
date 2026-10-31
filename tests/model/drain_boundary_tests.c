/* SPDX-License-Identifier: GPL-2.0 */
/* Execute actual receive/worker/query functions with a producer that keeps writing
 * after DRAIN. Deterministic DDI stand-ins bound a failing old worker to eight turns. */
#include "kernel_nbl_fixture.h"
#include "../safety/guard_memory.h"
#include "../../include/packetbridge_probe.h"
typedef UCHAR KIRQL;
typedef LONG NTSTATUS;
typedef VOID KSTART_ROUTINE(PVOID);
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define NDIS_STATUS_PAUSED 1u
#define Executive 0
#define KernelMode 0
#define WaitAny 0
static ULONG Calls,Idle,Exits,SubmittedCompletions,NativeCompletions,MaskReads,JunkCompletions;
static ULONG EnabledMask,StatWrites,StatRecordReads,KernelWakes;
static ULONG64 SnapshotBytes;
static ULONG64 SubmissionEpoch;
static BOOLEAN Append,ReturnCredit,AutoConsume,ConsumeAtIdle;
static volatile LONG UnexpectedIndication=1;
static PB_QUEUE_VIEW Submit,Complete;
static NET_BUFFER_LIST *Returning;
static NET_BUFFER_LIST *Indicated;
static BOOLEAN ReturnDuringIndication;
static VOID (*IndicationHook)(VOID);
static BOOLEAN MonitorReceive;
static ULONG ReceiveLocks;
static PB_KERNEL_SESSION *Current;
static HANDLE WaitEntered,WaitGate;
static BOOLEAN RealWait;
static PB_QUEUE_VIEW *RecycleProducer;
static PB_DESCRIPTOR RecycleItem;
static ULONG RecycleSchedule;
static VOID ReturnOneAtSchedule(ULONG schedule)
{
    if(RecycleSchedule!=schedule) return;
    uint32_t accepted,notify;RecycleSchedule=0;
    CHECK(PbQueuePush(RecycleProducer,&RecycleItem,1,&accepted,&notify)==PB_OK && accepted==1);
    CHECK(notify==(schedule==2?1u:0u));
    if(notify) KeSetEvent(Current->Queues[0].KernelEvent,0,FALSE);
}
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock); return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL saved) {(void)saved; ReleaseSRWLockShared(lock);}
#define ExAcquireSpinLockExclusive(lock) (AcquireSRWLockExclusive(lock),(KIRQL)0)
#define ExReleaseSpinLockExclusive(lock,irql) ((void)(irql),ReleaseSRWLockExclusive(lock))
static ULONG64 KeQueryInterruptTime(VOID) {return 5000ull*10000ull;}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{(void)reason;(void)mode;(void)alertable;(void)timeout; CHECK(WaitForSingleObject(event->Native,0)==WAIT_OBJECT_0);}
static VOID KeWaitForMultipleObjects(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks);
static ULONG PbEffectiveMask(TUN_CTX *ctx) {(void)ctx; MaskReads++;return EnabledMask;}
static ULONG PbBeginIndication(TUN_CTX *ctx) {(void)ctx; CHECK(!UnexpectedIndication); return 0;}
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) {(void)ctx;(void)bank;CHECK(!UnexpectedIndication);}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *nbls,ULONG count,ULONG flags)
{
    (void)adapter;(void)count;(void)flags;CHECK(!UnexpectedIndication && !Indicated);Indicated=nbls;
    if(IndicationHook) IndicationHook();
    if(ReturnDuringIndication) CHECK(PbV2ReturnNbls(Current->Ctx,&Indicated,PB_MAX_BATCH) && !Indicated);
}
#ifndef PB_DRAIN_BASELINE
static VOID CheckViewLock(PB_QUEUE_VIEW *view)
{
    for(ULONG i=0;i<Current->Layout.QueueCount;++i) {
        PB_KERNEL_QUEUE *q=Current->Queues+i;
        SRWLOCK *lock=view==q->Rings+PB_QUEUE_RX_SUBMIT?(SRWLOCK *)&q->RxLock:
            view==q->Rings+PB_QUEUE_TX_RECYCLE?(SRWLOCK *)&q->TxLock:NULL;
        if(lock) {
            BOOLEAN unlocked=TryAcquireSRWLockExclusive(lock);
            if(unlocked) ReleaseSRWLockExclusive(lock);
            CHECK(!unlocked); return;
        }
    }
    CHECK(FALSE);
}
static PB_RESULT SnapshotLocked(PB_QUEUE_VIEW *view,uint64_t *producer)
{CheckViewLock(view);return PbQueueSnapshotProducer(view,producer);}
static PB_RESULT ArmLocked(PB_QUEUE_VIEW *view,uint32_t *ready)
{
    CheckViewLock(view);
    if(view==Current->Queues[0].Rings+PB_QUEUE_TX_RECYCLE) ReturnOneAtSchedule(1);
    return PbQueueArm(view,ready);
}
#define PbQueueSnapshotProducer SnapshotLocked
#define PbQueueArm ArmLocked
#endif
#ifndef PB_RECEIVE_SOURCE
#define PB_CURRENT_RECEIVE_TAIL 1
#define PB_RECEIVE_SOURCE "../../driver/v2/session_receive.h"
#endif
static VOID CountReceiveLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{if(MonitorReceive) ReceiveLocks++;KeAcquireInStackQueuedSpinLock(lock,handle);}
static BOOLEAN StatTarget(volatile LONG64 *value)
{
    return value==&Current->Ctx->Probe.RscPackets || value==&Current->Ctx->Probe.RscBytes ||
        value==&Current->Ctx->Probe.RscEvents || value==&Current->Ctx->Probe.UroPackets || value==&Current->Ctx->Probe.UroEvents;
}
static LONG64 CountStatAdd(volatile LONG64 *value,LONG64 delta)
{if(StatTarget(value)) StatWrites++;return InterlockedAdd64(value,delta);}
static LONG64 CountStatIncrement(volatile LONG64 *value)
{if(StatTarget(value)) StatWrites++;return InterlockedIncrement64(value);}
static VOID *CountRecordRead(NET_BUFFER_LIST *nbl) {StatRecordReads++;return nbl->Reserved[1];}
static VOID CountSnapshot(VOID *dst,const VOID *src,SIZE_T length)
{if(Current && dst==Current->Queues[0].Scratch) SnapshotBytes+=length;memcpy(dst,src,length);}
#pragma push_macro("InterlockedAdd64")
#pragma push_macro("InterlockedIncrement64")
#pragma push_macro("NET_BUFFER_LIST_NEXT_NBL_EX")
#pragma push_macro("RtlCopyMemory")
#undef InterlockedAdd64
#undef InterlockedIncrement64
#undef NET_BUFFER_LIST_NEXT_NBL_EX
#undef RtlCopyMemory
#define InterlockedAdd64 CountStatAdd
#define InterlockedIncrement64 CountStatIncrement
#define NET_BUFFER_LIST_NEXT_NBL_EX(n) CountRecordRead(n)
#define RtlCopyMemory CountSnapshot
#define KeAcquireInStackQueuedSpinLock CountReceiveLock
#define PbV2ReceiveBatch ActualReceiveBatch
#include PB_RECEIVE_SOURCE
#undef PbV2ReceiveBatch
#undef KeAcquireInStackQueuedSpinLock
#pragma pop_macro("NET_BUFFER_LIST_NEXT_NBL_EX")
#pragma pop_macro("RtlCopyMemory")
#pragma pop_macro("InterlockedIncrement64")
#pragma pop_macro("InterlockedAdd64")
#include "../../driver/v2/session_recycle.h"
static VOID SubmitOne(PB_KERNEL_SESSION *s)
{
    USHORT slot=(USHORT)(Submit.Cursor%s->Layout.SlotCount);
    uint32_t generation=(uint32_t)((Submit.Cursor/s->Layout.SlotCount)&255u)+1;
    PB_DESCRIPTOR d={.Token={SubmissionEpoch,generation,slot,0},.Length=20,.DataOffset=s->Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE}; uint32_t n,notify;
    s->Region[d.DataOffset]=0x45; s->Region[d.DataOffset+3]=20;
    CHECK(PbQueuePush(&Submit,&d,1,&n,&notify)==PB_OK && n==1);
    if(notify) KeSetEvent(s->Queues[0].KernelEvent,0,FALSE);
}
static VOID ConsumeCompletions(VOID)
{
    PB_DESCRIPTOR items[32]; uint32_t n;
    for(;;) {
        PB_RESULT r=PbQueuePop(&Complete,items,32,&n); if(r==PB_EMPTY) break; CHECK(r==PB_OK);
        for(ULONG i=0;i<n;++i) {
            if(items[i].Token.Epoch==40) {CHECK(items[i].Status==PB_INVALID);JunkCompletions++;}
            else if(items[i].Token.Epoch==41) {CHECK(items[i].Status==PB_INVALID); SubmittedCompletions++;}
            else {
                CHECK(items[i].Token.Epoch==42);
                if(items[i].Status==PB_STOPPED) SubmittedCompletions++;
                else {CHECK(items[i].Status==PB_OK); NativeCompletions++;}
            }
        }
    }
}
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q)
{
    BOOLEAN work=ActualReceiveBatch(q); Calls++; if(AutoConsume) ConsumeCompletions();
    if(ReturnCredit) {
        CHECK(Returning && PbV2ReturnNbls(q->Session->Ctx,&Returning,PB_MAX_BATCH) && !Returning);
        ReturnCredit=FALSE; ConsumeCompletions();
    }
    if(Append) SubmitOne(q->Session);
    if(Calls==8) q->Session->Stopping=1;
    CHECK(Calls<=8); return work;
}
static VOID KeWaitForMultipleObjects(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{
    ReturnOneAtSchedule(2);
    (void)type;(void)reason;(void)mode;(void)alertable;(void)timeout;(void)blocks;
    CHECK(n==2 || n==3); KEVENT *kernel=events[1];
    if(RealWait) {
        HANDLE handles[3];for(ULONG i=0;i<n;++i) handles[i]=((KEVENT *)events[i])->Native;
        Idle++;CHECK(SetEvent(WaitEntered));
        if(WaitGate) CHECK(WaitForSingleObject(WaitGate,5000)==WAIT_OBJECT_0);
        CHECK(WaitForMultipleObjects(n,handles,FALSE,5000)<WAIT_OBJECT_0+n);return;
    }
    if(ConsumeAtIdle) {
        CHECK(WaitForSingleObject(kernel->Native,0)==WAIT_TIMEOUT); /* A full completion ring must not self-poll. */
        PB_DESCRIPTOR items[32];uint32_t count;
        for(ULONG batch=0;batch<4;++batch) CHECK(PbQueuePop(&Complete,items,32,&count)==PB_OK && count==32 && items[0].Token.Epoch==40);
        JunkCompletions+=128;ConsumeAtIdle=FALSE;AutoConsume=TRUE;KeSetEvent(kernel,0,FALSE);
    }
    if(WaitForSingleObject(kernel->Native,0)==WAIT_OBJECT_0) return;
    Idle++; Current->Stopping=1;
}
static VOID PsTerminateSystemThread(LONG status) {CHECK(status==STATUS_SUCCESS); Exits++;}
#ifndef PB_WORKER_SOURCE
#define PB_WORKER_SOURCE "../../driver/v2/session_worker.h"
#endif
#include PB_WORKER_SOURCE
#ifndef PB_QUERY_SOURCE
#define PB_QUERY_SOURCE "../../driver/v2/session_query.h"
#endif
#include PB_QUERY_SOURCE
typedef struct FIXTURE {TUN_CTX Ctx;PB_KERNEL_SESSION S;MDL Parent;PB_RX_SLOT Slots[2][PB_MAX_SLOTS];PB_DESCRIPTOR Pending[2][512];KEVENT Kernel[2],User[2];} FIXTURE;
static FIXTURE *Create(ULONG queues,ULONG slots,ULONG capacity,ULONG64 base)
{
    FIXTURE *f=calloc(1,sizeof(*f)); CHECK(f && !Pools && !Mdls && !Nbls);
    PB_KERNEL_SESSION *s=&f->S; Current=s; s->Ctx=&f->Ctx; s->Epoch=42; f->Ctx.V2=s; f->Ctx.Running=1; f->Ctx.NblPool=f;
    CHECK(PbCalculateLayout(queues,slots,capacity,&s->Layout)==PB_OK);
    s->Region=VirtualAlloc(NULL,s->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(s->Region);
    f->Parent.Address=s->Region; f->Parent.Length=s->Layout.RegionSize; s->Mdl=&f->Parent;
    s->StartEvent.Native=CreateEventW(NULL,TRUE,TRUE,NULL); s->StopEvent.Native=CreateEventW(NULL,TRUE,FALSE,NULL);
    s->AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL); f->Ctx.Probe.RunningEvent.Native=CreateEventW(NULL,TRUE,TRUE,NULL);
    CHECK(s->StartEvent.Native && s->StopEvent.Native && s->AllReturned.Native && f->Ctx.Probe.RunningEvent.Native);
    for(ULONG i=0;i<queues;++i) {
        f->Kernel[i].Native=CreateEventW(NULL,FALSE,FALSE,NULL); f->User[i].Native=CreateEventW(NULL,FALSE,FALSE,NULL); CHECK(f->Kernel[i].Native && f->User[i].Native);
        PB_KERNEL_QUEUE *q=s->Queues+i; q->Session=s; q->Index=i; q->RxSlots=f->Slots[i]; q->Pending=f->Pending[i]; q->KernelEvent=f->Kernel+i; q->UserEvent=f->User+i;
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r) CHECK(PbAttachQueue(q->Rings+r,s->Region,&s->Layout,i,r,r==PB_QUEUE_TX_READY || r==PB_QUEUE_RX_COMPLETE)==PB_OK);
    }
    CHECK(PbAttachQueue(&Submit,s->Region,&s->Layout,0,PB_QUEUE_RX_SUBMIT,1)==PB_OK);
    CHECK(PbAttachQueue(&Complete,s->Region,&s->Layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    PB_QUEUE_VIEW *rx=s->Queues[0].Rings+PB_QUEUE_RX_SUBMIT;
    rx->Cursor=rx->PeerCursor=Submit.Cursor=Submit.PeerCursor=base; rx->Shared->Producer=rx->Shared->Consumer=base;
    Calls=Idle=Exits=SubmittedCompletions=NativeCompletions=MaskReads=JunkCompletions=0; SubmissionEpoch=42; Append=ReturnCredit=ConsumeAtIdle=FALSE;AutoConsume=TRUE; Returning=Indicated=NULL;
    EnabledMask=StatWrites=StatRecordReads=KernelWakes=0;SnapshotBytes=0;UnexpectedIndication=1;ReturnDuringIndication=FALSE;
    IndicationHook=NULL;MonitorReceive=FALSE;ReceiveLocks=0;
    RecycleProducer=NULL;RecycleSchedule=0;return f;
}
static VOID Drain(FIXTURE *f)
{
#ifdef PB_DRAIN_BASELINE
    f->S.Deadline=0; f->S.Draining=1;
#else
    AcquireSRWLockExclusive(&f->Ctx.TransitionLock); PbV2BeginDrain(&f->S,0); ReleaseSRWLockExclusive(&f->Ctx.TransitionLock);
#endif
}
static ULONG State(FIXTURE *f,ULONG refs)
{PB_V2_STATUS s; CHECK(PbV2Query(&f->Ctx,&s)==STATUS_SUCCESS && s.ActiveReferences==refs);return s.State;}
static VOID Destroy(FIXTURE *f)
{
    CHECK(!Pools && !Mdls && !Nbls && !f->S.Active && !f->S.Returns.References && !f->S.Queues[0].Credits && !f->S.Queues[0].PendingCount);
    CHECK(CloseHandle(f->S.StartEvent.Native) && CloseHandle(f->S.StopEvent.Native) && CloseHandle(f->S.AllReturned.Native) && CloseHandle(f->Ctx.Probe.RunningEvent.Native));
    for(ULONG i=0;i<f->S.Layout.QueueCount;++i) {CHECK(!f->S.Queues[i].Credits && !f->S.Queues[i].PendingCount && CloseHandle(f->Kernel[i].Native) && CloseHandle(f->User[i].Native));}
    CHECK(VirtualFree(f->S.Region,0,MEM_RELEASE)); free(f); Current=NULL;
}
static VOID Tail(ULONG initial,ULONG64 base,BOOLEAN paused)
{
    FIXTURE *f=Create(1,4,8,base); for(ULONG i=0;i<initial;++i) SubmitOne(&f->S); Drain(f);
    if(paused) f->Ctx.Running=0;
    Append=TRUE; PbV2Worker(f->S.Queues); CHECK(Exits==1 && !f->S.Faulted);
#ifdef PB_DRAIN_BASELINE
    CHECK(Calls==8 && !Idle && State(f,0)==PB_SESSION_QUARANTINED && SubmittedCompletions>initial);
#else
    CHECK(Calls<=2 && Idle==1 && State(f,0)==PB_SESSION_STOPPED && SubmittedCompletions==initial);
    CHECK(f->S.Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Cursor==base+initial);
    f->S.Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer=UINT64_MAX/2;
    CHECK(State(f,0)==PB_SESSION_STOPPED); /* A late shared write cannot extend private DRAIN. */
#endif
    printf("PASS: post-DRAIN tail initial=%lu wrap=%u paused=%u: receive calls=%lu idle waits=%lu submitted completions=%lu state=%lu, native refs=0.\n",initial,base!=0,paused,Calls,Idle,SubmittedCompletions,State(f,0)); Destroy(f);
}
#ifndef PB_DRAIN_BASELINE
static VOID EmptyReceiveProgress(VOID)
{
    FIXTURE *f=Create(1,4,512,0);FixtureLocks=0;
    CHECK(!ActualReceiveBatch(f->S.Queues));
#ifdef PB_EMPTY_BASELINE
    CHECK(FixtureLocks==3 && MaskReads==1);
#else
    CHECK(FixtureLocks==1 && !MaskReads);
#endif
    printf("PASS: empty receive locks=%ld mask_reads=%lu.\n",FixtureLocks,MaskReads);Destroy(f);
    f=Create(1,4,512,0);PB_KERNEL_QUEUE *q=f->S.Queues;
    PB_DESCRIPTOR d={.Token={41,1,0,0},.Status=PB_INVALID};
    for(ULONG i=0;i<257;++i) {q->Credits++;PbV2CompleteLocked(q,&d,PB_INVALID);}
    CHECK(!ActualReceiveBatch(q));
#ifdef PB_EMPTY_BASELINE
    CHECK(q->PendingCount==1);
#else
    CHECK(q->PendingCount==129); /* One bounded flush, with a scheduled continuation. */
#endif
    ConsumeCompletions();PbV2Worker(q);
    CHECK(Idle==1 && Exits==1 && SubmittedCompletions==257 && f->S.Completions==257);Destroy(f);
    f=Create(1,4,512,0);q=f->S.Queues;PB_DESCRIPTOR junk[32]={0};uint32_t count,notify;
    for(ULONG i=0;i<32;++i) {junk[i].Token.Epoch=40;junk[i].Status=PB_INVALID;}
    for(ULONG batch=0;batch<16;++batch) CHECK(PbQueuePush(q->Rings+PB_QUEUE_RX_COMPLETE,junk,32,&count,&notify)==PB_OK && count==32);
    for(ULONG i=0;i<32;++i) {q->Credits++;PbV2CompleteLocked(q,&d,PB_INVALID);}
    AutoConsume=FALSE;ConsumeAtIdle=TRUE;PbV2Worker(q);
    CHECK(!ConsumeAtIdle && Idle==1 && Calls==2 && SubmittedCompletions==32 && JunkCompletions==512 && f->S.Completions==32);Destroy(f);
    puts("PASS: actual empty receive/worker: 257 pending completions progress through bounded continuations; full completion ring parks until consumer wake, then pending/credits clear.");
}
static VOID NormalIdleArm(VOID)
{
    FIXTURE *f=Create(1,4,8,0); PbV2Worker(f->S.Queues);
    CHECK(Calls==1 && Idle==1 && Exits==1 && State(f,0)==PB_SESSION_RUNNING && !f->S.Draining);
    CHECK(WaitForSingleObject(f->Kernel[0].Native,0)==WAIT_TIMEOUT);
    SubmitOne(&f->S);
    CHECK(WaitForSingleObject(f->Kernel[0].Native,0)==WAIT_OBJECT_0); /* Live producer still wakes an armed idle worker. */
    Destroy(f); puts("PASS: normal empty worker arms RX/TX under their view locks and parks; a later live submission still produces its wake.");
}
static VOID RecycleIdleWake(VOID)
{
    for(ULONG paused=0;paused<2;++paused)
    for(ULONG schedule=1;schedule<=3;++schedule) {
        FIXTURE *f=Create(1,4,8,0);PB_KERNEL_QUEUE *q=f->S.Queues;PB_SLOT slots[4];PB_QUEUE_VIEW producer;
        CHECK(PbPoolInitialize(&q->TxPool,slots,4,f->S.Epoch,0)==PB_OK);
        CHECK(PbPoolReserve(&q->TxPool,&RecycleItem.Token)==PB_OK);
        CHECK(PbPoolTransition(&q->TxPool,&RecycleItem.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
        if(paused) {f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);SubmitOne(&f->S);}
        q->TxPoolExhausted=TRUE;
        RecycleProducer=&producer;RecycleSchedule=schedule;
        if(schedule==3) ReturnOneAtSchedule(3);
        PbV2Worker(q);
        CHECK(!RecycleSchedule && !f->S.Faulted && Exits==1 && Idle==1);
        CHECK(slots[RecycleItem.Token.Slot].State==PB_SLOT_FREE && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==1);
        CHECK(!q->TxPoolExhausted && (!paused || (!Calls && !q->Rings[PB_QUEUE_RX_SUBMIT].Cursor)));
        CHECK(WaitForSingleObject(q->KernelEvent->Native,0)==WAIT_TIMEOUT);
        Destroy(f);
    }
    puts("PASS: actual running/paused TX recycle/worker: return before initial drain, between drain and arm, or between arm and wait; no lost wake, no self-poll, no paused submission consumed.");
}
static VOID PausedRecycleBudget(ULONG64 base)
{
    FIXTURE *f=Create(1,160,256,0);PB_KERNEL_QUEUE *q=f->S.Queues;PB_SLOT slots[160];PB_QUEUE_VIEW producer;
    CHECK(PbPoolInitialize(&q->TxPool,slots,160,f->S.Epoch,0)==PB_OK);
    CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    producer.Cursor=producer.PeerCursor=q->Rings[PB_QUEUE_TX_RECYCLE].Cursor=q->Rings[PB_QUEUE_TX_RECYCLE].PeerCursor=base;
    producer.Shared->Producer=producer.Shared->Consumer=base;
    for(ULONG batch=0;batch<5;++batch) {
        PB_DESCRIPTOR items[PB_MAX_BATCH]={0};uint32_t n,notify;
        for(ULONG i=0;i<PB_MAX_BATCH;++i) {
            CHECK(PbPoolReserve(&q->TxPool,&items[i].Token)==PB_OK);
            CHECK(PbPoolTransition(&q->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        }
        CHECK(PbQueuePush(&producer,items,PB_MAX_BATCH,&n,&notify)==PB_OK && n==PB_MAX_BATCH && !notify);
    }
    q->TxPoolExhausted=TRUE;f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);SubmitOne(&f->S);
    PbV2Worker(q);
    CHECK(Idle==1 && Exits==1 && !Calls && !f->S.Faulted && !q->TxPoolExhausted);
    CHECK(q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==base+160 && q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Armed==1);
    for(ULONG i=0;i<160;++i) CHECK(slots[i].State==PB_SLOT_FREE && slots[i].Generation==1);
    CHECK(!q->Rings[PB_QUEUE_RX_SUBMIT].Cursor && WaitForSingleObject(q->KernelEvent->Native,0)==WAIT_TIMEOUT);
    printf("PASS: paused recycle wrap=%u: 160 unique slots retired across the 128-token budget, one idle wait, zero receive scans, ready submission untouched.\n",base!=0);Destroy(f);
}
static VOID MultipleQueuesAndFault(VOID)
{
    FIXTURE *f=Create(2,4,8,0); SubmitOne(&f->S); PB_QUEUE_VIEW second,completed;
    CHECK(PbAttachQueue(&second,f->S.Region,&f->S.Layout,1,PB_QUEUE_RX_SUBMIT,1)==PB_OK && PbAttachQueue(&completed,f->S.Region,&f->S.Layout,1,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    PB_DESCRIPTOR d={.Token={41,1,0,1},.Length=20,.DataOffset=f->S.Layout.Queues[1].RxData}; uint32_t n,notify;
    for(ULONG i=0;i<3;++i) CHECK(PbQueuePush(&second,&d,1,&n,&notify)==PB_OK && n==1);
    Drain(f); CHECK(f->S.DrainSnapshotReady && f->S.Queues[0].DrainProducer==1 && f->S.Queues[1].DrainProducer==3 && State(f,0)==PB_SESSION_QUARANTINED);
    CHECK(ActualReceiveBatch(f->S.Queues)); ConsumeCompletions(); CHECK(State(f,0)==PB_SESSION_QUARANTINED);
    CHECK(PbQueuePush(&second,&d,1,&n,&notify)==PB_OK && n==1);
    AcquireSRWLockExclusive(&f->Ctx.TransitionLock); PbV2BeginDrain(&f->S,999); ReleaseSRWLockExclusive(&f->Ctx.TransitionLock);
    CHECK(f->S.Queues[1].DrainProducer==3 && !f->S.Deadline); /* Repeated DRAIN never renews targets/deadline. */
    CHECK(ActualReceiveBatch(f->S.Queues+1) && PbQueuePop(&completed,&d,1,&n)==PB_OK && n==1);
    for(ULONG i=0;i<2;++i) CHECK(PbQueuePop(&completed,&d,1,&n)==PB_OK && n==1 && d.Status==PB_INVALID);
    CHECK(State(f,0)==PB_SESSION_STOPPED && f->S.Queues[1].Rings[PB_QUEUE_RX_SUBMIT].Cursor==3 && second.Cursor==4); Destroy(f);
    f=Create(2,4,8,0); SubmitOne(&f->S); f->S.Queues[1].Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer=9; Drain(f);
    CHECK(f->S.Faulted && !f->S.DrainSnapshotReady && State(f,0)==PB_SESSION_QUARANTINED);
    PbV2Worker(f->S.Queues); PbV2Worker(f->S.Queues+1);
    CHECK(Exits==2 && State(f,0)==PB_SESSION_STOPPED); Destroy(f);
    puts("PASS: all queue drain targets, idempotent boundary/deadline, partial snapshot corruption enters fault and requires both private worker witnesses.");
}
static NET_BUFFER_LIST *NativeReceive(FIXTURE *f,ULONG slot)
{
    PB_DESCRIPTOR d={.Token={42,1,(USHORT)slot,0},.Length=20,.DataOffset=f->S.Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE}; uint32_t complete;
    f->S.Region[d.DataOffset]=0x45;
    CHECK(PbAdmitReceive(&f->S.Layout,42,0,f->Slots[0],&d,0,&complete)==PB_OK); f->S.Queues[0].Credits++;
    NET_BUFFER_LIST *nbl=PbV2Receive(f->S.Queues,&d,1,MAXULONG,NULL); CHECK(nbl); PbV2RetainReceives(&f->S,1,1); return nbl;
}
static LONG CancelLockStart,CancelLockEnd;
static VOID FaultDuringBuild(VOID) {AfterNblAllocate=NULL;PbV2Fault(Current);}
static VOID CountKernelWake(KEVENT *event) {if(event==Current->Queues[0].KernelEvent) KernelWakes++;}
static VOID MarkCancelStart(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Current->Returns);if(CancelLockStart<0) CancelLockStart=FixtureLocks;}
static VOID MarkCancelEnd(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Current->Returns);CancelLockEnd=FixtureLocks;}
static VOID LateFaultCancel(BOOLEAN failedAllocation,BOOLEAN held)
{
    FIXTURE *f=Create(1,64,64,0);PB_KERNEL_QUEUE *q=f->S.Queues;NET_BUFFER_LIST *old=held?NativeReceive(f,32):NULL;
    PB_DESCRIPTOR items[32]={0};uint32_t n,notify;
    for(ULONG i=0;i<32;++i) {
        items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=20,.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
        f->S.Region[items[i].DataOffset]=0x45;
    }
    CHECK(PbQueuePush(&Submit,items,32,&n,&notify)==PB_OK && n==32);
    Acquisitions=0;Failure=failedAllocation?8:0;FixtureLocks=FixtureRundownAcquires=FixtureRundownReleases=0;CancelLockStart=CancelLockEnd=-1;
    AfterNblAllocate=FaultDuringBuild;AfterRundownAcquire=MarkCancelStart;BeforeRundownRelease=MarkCancelEnd;OnSetEvent=CountKernelWake;
    CHECK(ActualReceiveBatch(q) && !AfterNblAllocate && f->S.Faulted && !f->S.Indications && !StatWrites);
    AfterRundownAcquire=BeforeRundownRelease=NULL;OnSetEvent=NULL;Failure=0;ULONG made=failedAllocation?31:32,windows=1;
#if defined(PB_RX_BATCH_BASELINE) || defined(PB_RX_CANCEL_BASELINE) || defined(PB_EMPTY_BASELINE)
    windows=made;
#endif
    /* Measure the return windows themselves, independently of any later
     * receive-tail flush in current or frozen historical implementations. */
    LONG returnLocks=CancelLockEnd-CancelLockStart;
    CHECK(CancelLockStart>=0 && returnLocks==(LONG)(2*windows));
    CHECK(FixtureRundownAcquires==(LONG)windows && FixtureRundownReleases==(LONG)windows);
#ifdef PB_RETURN_WAKE_BASELINE
    CHECK(KernelWakes==1+windows);
#else
    CHECK(KernelWakes==1); /* Capacity 64 never exhausted; Fault owns the one wake. */
#endif
    CHECK(f->S.RxObjects==made+held && f->S.RxReturned==made && f->S.Active==held && q->Credits==held && !q->PendingCount);
    CHECK(Pools==(LONG)held && Mdls==2*(LONG)held && Nbls==(LONG)held && !f->S.Returns.References);
    for(ULONG i=0;i<32;++i) CHECK(!q->RxSlots[i].Active);
    CHECK(State(f,held)==PB_SESSION_QUARANTINED && !q->FaultQuiesced);
    printf("PASS: late fault private cancel: allocation_failure=%u held=%u nbls=%lu return_locks=%ld rundown_pairs=%ld kernel_wakes=%lu, no indication/stats; witness still required.\n",failedAllocation,held,made,returnLocks,FixtureRundownAcquires,KernelWakes);
    PbV2Worker(q);CHECK(q->FaultQuiesced && State(f,held)==(held?PB_SESSION_QUARANTINED:PB_SESSION_STOPPED));
    if(held) {CHECK(q->RxSlots[32].Active);PbV2ReturnOne(&f->Ctx,old);CHECK(State(f,0)==PB_SESSION_STOPPED);}
    Destroy(f);
}
static VOID PacketForStats(UCHAR *p,ULONG protocol,ULONG flow,ULONG sequence)
{
    memset(p,0,64);p[0]=0x45;p[3]=64;p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;
    p[20]=(UCHAR)(flow>>8);p[21]=(UCHAR)flow;p[22]=0x13;p[23]=0x88;
    if(protocol==6) {p[24]=(UCHAR)(sequence>>24);p[25]=(UCHAR)(sequence>>16);p[26]=(UCHAR)(sequence>>8);p[27]=(UCHAR)sequence;p[32]=0x50;p[33]=0x10;p[34]=0x20;}
    else p[25]=44;
    memset(p+(protocol==6?40:28),0x5a,protocol==6?24:36);
    CHECK(PbFinalizeChecksums(p,64,1,protocol==6,protocol==17)==PB_PACKET_OK);
}
/* mode: plain, two TCP groups, two UDP groups, mixed, one full TCP group. */
static VOID IndicationStats(ULONG mode,BOOLEAN failure,BOOLEAN synchronousReturn,BOOLEAN lateFault)
{
    FIXTURE *f=Create(1,32,32,0);PB_KERNEL_QUEUE *q=f->S.Queues;PB_DESCRIPTOR items[32]={0};uint32_t n,notify;
    EnabledMask=mode?PB_RX_RSC4|PB_RX_URO:0;
    if(mode) {q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG b=0;b<2;++b) q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);}
    for(ULONG i=0;i<32;++i) {
        ULONG protocol=mode==2 || (mode==3 && i>=2)?17:6;
        ULONG flow=mode==4?4000:i<2?4000:i<4?5000:6000+i;
        items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=64,.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
        PacketForStats(f->S.Region+items[i].DataOffset,protocol,flow,100+i*24);
    }
    CHECK(PbQueuePush(&Submit,items,32,&n,&notify)==PB_OK && n==32);
    f->Ctx.Probe.RscPackets=7;f->Ctx.Probe.RscBytes=11;f->Ctx.Probe.RscEvents=13;f->Ctx.Probe.UroPackets=17;f->Ctx.Probe.UroEvents=19;
    Acquisitions=0;Failure=failure?1:0;UnexpectedIndication=0;ReturnDuringIndication=synchronousReturn;
    if(lateFault) {AfterNblAllocate=FaultDuringBuild;UnexpectedIndication=1;}
    ReceiveLocks=0;MonitorReceive=TRUE;
    CHECK(ActualReceiveBatch(q) && f->S.Indications==!lateFault && !!f->S.Faulted==lateFault);Failure=0;MonitorReceive=FALSE;
#if defined(PB_CURRENT_RECEIVE_TAIL) || defined(PB_RX_TAIL_TESTS)
#ifdef PB_RX_TAIL_BASELINE
    CHECK(ReceiveLocks==3+(ULONG)failure);
#else
    CHECK(ReceiveLocks==2+2*(ULONG)failure);
#endif
#endif
    ULONG rscEvents=mode==1?(failure?1:2):mode==3 || mode==4?1:0,uroEvents=mode==2?2:mode==3?1:0;
    ULONG rscPackets=mode==4?32:rscEvents*2,uroPackets=uroEvents*2;
    CHECK(f->Ctx.Probe.RscPackets==7+(lateFault?0:rscPackets) && f->Ctx.Probe.RscBytes==11+(lateFault?0:rscPackets*24) && f->Ctx.Probe.RscEvents==13+(lateFault?0:rscEvents));
    CHECK(f->Ctx.Probe.UroPackets==17+(lateFault?0:uroPackets) && f->Ctx.Probe.UroEvents==19+(lateFault?0:uroEvents));
    ULONG objects=failure?30:32,nbls=mode==4?1:objects-rscEvents-uroEvents;
#if defined(PB_RX_BATCH_BASELINE) || defined(PB_RX_CANCEL_BASELINE) || defined(PB_EMPTY_BASELINE)
    CHECK(StatRecordReads==(lateFault?0:nbls) && StatWrites==(lateFault?0:rscEvents*3+uroEvents*2));
#else
    /* Only the exceptional pre-indication cancellation walks each private
     * record to mark its origin. Ordinary receive statistic work is unchanged. */
    CHECK(StatRecordReads==rscEvents+uroEvents+(lateFault?nbls:0) && StatWrites==(lateFault?0:(rscEvents?3u:0)+(uroEvents?2u:0)));
#endif
    CHECK(f->S.RxObjects==objects && f->S.Active==((synchronousReturn || lateFault)?0:(LONG)nbls));
    if(!synchronousReturn && !lateFault) CHECK(Indicated && PbV2ReturnNbls(&f->Ctx,&Indicated,PB_MAX_BATCH) && !Indicated);
    CHECK(!Pools && !Mdls && !Nbls && !q->Credits && !q->PendingCount && f->S.RxReturned==objects);
    PB_DESCRIPTOR completed[32];
    if(lateFault) {
        CHECK(!Indicated && !AfterNblAllocate && !f->S.Completions && PbQueuePop(&Complete,completed,32,&n)==PB_EMPTY);
        CHECK(State(f,0)==PB_SESSION_QUARANTINED);PbV2Worker(q);CHECK(State(f,0)==PB_SESSION_STOPPED);
    } else {
        CHECK(PbQueuePop(&Complete,completed,32,&n)==PB_OK && n==32);
        for(ULONG i=0;i<32;++i) CHECK(completed[i].Status==(ULONG)(failure && i<2?PB_FULL:PB_OK));
    }
    printf("PASS: receive stats mode=%lu allocation_failure=%u synchronous_return=%u late_fault=%u nbls=%lu record_reads=%lu atomic_writes=%lu rsc_events=%lu uro_events=%lu receive_rx_locks=%lu.\n",mode,failure,synchronousReturn,lateFault,nbls,StatRecordReads,StatWrites,rscEvents,uroEvents,ReceiveLocks);
    if(mode) {PbTestGuardFree(q->Scratch);for(ULONG b=0;b<2;++b) {CHECK(!q->CoalesceBusy[b]);PbTestGuardFree(q->CoalesceBuffers[b]);}}
    Destroy(f);
}
#if defined(PB_CURRENT_RECEIVE_TAIL) || defined(PB_RX_TAIL_TESTS)
static BOOLEAN TailConsumerWake;
static VOID ConsumeDuringIndication(VOID)
{
    CHECK(Current->Queues[0].PendingCount==1 && Current->Active==1);
    ConsumeCompletions();CHECK(JunkCompletions==8);
    if(TailConsumerWake) KeSetEvent(Current->Queues[0].KernelEvent,0,FALSE);
    IndicationHook=NULL;
}
static VOID ReceiveTailPending(BOOLEAN full,BOOLEAN wake)
{
    FIXTURE *f=Create(1,4,full?8:512,UINT64_MAX-1);PB_KERNEL_QUEUE *q=f->S.Queues;uint32_t n,notify;
    if(full) {
        PB_DESCRIPTOR junk[8]={0};for(ULONG i=0;i<8;++i) {junk[i].Token.Epoch=40;junk[i].Status=PB_INVALID;}
        CHECK(PbQueuePush(q->Rings+PB_QUEUE_RX_COMPLETE,junk,8,&n,&notify)==PB_OK && n==8);
    }
    ULONG pending=full?1:257;PB_DESCRIPTOR d={.Token={41,1,0,0},.Status=PB_INVALID};
    for(ULONG i=0;i<pending;++i) {q->Credits++;PbV2CompleteLocked(q,&d,PB_INVALID);}
    SubmitOne(&f->S);UnexpectedIndication=0;AutoConsume=FALSE;
    if(full) {TailConsumerWake=wake;IndicationHook=ConsumeDuringIndication;}
    ReceiveLocks=0;MonitorReceive=TRUE;CHECK(ActualReceiveBatch(q) && Indicated && !IndicationHook);MonitorReceive=FALSE;
    CHECK(f->S.Active==1 && q->RxSlots[2].Active && !f->S.Faulted);
#ifdef PB_RX_TAIL_BASELINE
    CHECK(ReceiveLocks==3 && q->PendingCount==(full?0u:1u));
#else
    CHECK(ReceiveLocks==2 && q->PendingCount==(full?1u:129u));
#endif
    ULONG saved=q->PendingCount;AutoConsume=TRUE;PbV2Worker(q);
    CHECK(Idle==1 && Exits==1 && Calls<=2 && SubmittedCompletions==pending && f->S.Completions==pending);
    CHECK(!q->PendingCount && q->Credits==1 && f->S.Active==1 && q->RxSlots[2].Active);
    CHECK(PbV2ReturnNbls(&f->Ctx,&Indicated,PB_MAX_BATCH) && !Indicated);ConsumeCompletions();
    CHECK(NativeCompletions==1 && f->S.Completions==pending+1 && !q->Credits && !f->S.Active);
    printf("PASS: receive tail full=%u consumer_wake=%u: receive RxLock holds=%lu, pending_at_return=%lu; bounded next worker turns clear prior pending before one idle wait, native owner stays live until return.\n",full,wake,ReceiveLocks,saved);
    Destroy(f);
}
static HANDLE TailReturnEntered,TailReturnContinue,TailReceiveDone,TailReturnThread;
static NET_BUFFER_LIST *TailReturnLists;
static volatile LONG TailReturnThreadId;
static VOID GateReturnLock(KSPIN_LOCK *lock)
{
    if(lock!=&Current->Queues[0].RxLock || (LONG)GetCurrentThreadId()!=ReadAcquire(&TailReturnThreadId)) return;
    AfterQueueLock=NULL;
    CHECK(SetEvent(TailReturnEntered) && WaitForSingleObject(TailReturnContinue,5000)==WAIT_OBJECT_0);
}
static DWORD WINAPI TailReturn(VOID *unused)
{
    (void)unused;InterlockedExchange(&TailReturnThreadId,(LONG)GetCurrentThreadId());
    CHECK(PbV2ReturnNbls(Current->Ctx,&TailReturnLists,PB_MAX_BATCH) && !TailReturnLists);return 0;
}
static VOID StartTailReturn(VOID)
{
    IndicationHook=NULL;TailReturnLists=Indicated;Indicated=NULL;
    TailReturnThread=CreateThread(NULL,0,TailReturn,NULL,0,NULL);
    CHECK(TailReturnThread && WaitForSingleObject(TailReturnEntered,5000)==WAIT_OBJECT_0);
}
static DWORD WINAPI TailReceive(VOID *unused)
{(void)unused;CHECK(ActualReceiveBatch(Current->Queues) && SetEvent(TailReceiveDone));return 0;}
static VOID ReceiveTailContendedReturn(VOID)
{
    FIXTURE *f=Create(1,4,8,0);SubmitOne(&f->S);UnexpectedIndication=0;
    TailReturnEntered=CreateEventW(NULL,TRUE,FALSE,NULL);TailReturnContinue=CreateEventW(NULL,TRUE,FALSE,NULL);
    TailReceiveDone=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(TailReturnEntered && TailReturnContinue && TailReceiveDone);
    TailReturnThreadId=0;AfterQueueLock=GateReturnLock;IndicationHook=StartTailReturn;MonitorReceive=TRUE;
    HANDLE receive=CreateThread(NULL,0,TailReceive,NULL,0,NULL);CHECK(receive && WaitForSingleObject(TailReturnEntered,5000)==WAIT_OBJECT_0);
#ifdef PB_RX_TAIL_BASELINE
    CHECK(WaitForSingleObject(TailReceiveDone,30)==WAIT_TIMEOUT);
#else
    CHECK(WaitForSingleObject(TailReceiveDone,2000)==WAIT_OBJECT_0 && ReceiveLocks==2);
#endif
    /* The real return callback freed its NBL/MDLs, but still owns the private
     * record, active slot, credit, native reference and Returns tail pin. */
    CHECK(!Nbls && !Mdls && Pools==1 && f->S.Active==1 && f->S.Returns.References==1 &&
        f->S.Queues[0].Credits==1 && f->S.Queues[0].RxSlots[0].Active);
    CHECK(SetEvent(TailReturnContinue) && WaitForSingleObject(receive,5000)==WAIT_OBJECT_0 && WaitForSingleObject(TailReturnThread,5000)==WAIT_OBJECT_0);
    MonitorReceive=FALSE;CHECK(!AfterQueueLock && !IndicationHook && !TailReturnLists);
    CHECK(CloseHandle(receive) && CloseHandle(TailReturnThread) && CloseHandle(TailReturnEntered) && CloseHandle(TailReturnContinue) && CloseHandle(TailReceiveDone));
    TailReturnThread=TailReturnEntered=TailReturnContinue=TailReceiveDone=NULL;
    ConsumeCompletions();CHECK(NativeCompletions==1 && f->S.RxReturned==1 && !f->S.Active && !f->S.Returns.References);
    printf("PASS: real concurrent return holds RxLock: receive RxLock holds=%lu; unchanged native slot/credit/reference/rundown ownership until callback continues, exact final completion and joined threads.\n",ReceiveLocks);
    Destroy(f);
}
#endif
static VOID UdpTerminalPacket(UCHAR *p,ULONG length,ULONG ip,UCHAR pattern)
{
    ULONG l4=ip==4?20:40;memset(p,0,length);p[0]=(UCHAR)(ip==4?0x45:0x60);
    if(ip==4) {p[2]=(UCHAR)(length>>8);p[3]=(UCHAR)length;p[8]=64;p[9]=17;p[12]=10;p[16]=11;}
    else {ULONG payload=length-40;p[4]=(UCHAR)(payload>>8);p[5]=(UCHAR)payload;p[6]=17;p[7]=64;p[8]=0x20;p[24]=0x30;}
    p[l4]=0x0f;p[l4+1]=0xa0;p[l4+2]=0x13;p[l4+3]=0x88;
    p[l4+4]=(UCHAR)((length-l4)>>8);p[l4+5]=(UCHAR)(length-l4);
    memset(p+l4+8,pattern,length-l4-8);CHECK(PbFinalizeChecksums(p,length,ip==4,0,1)==PB_PACKET_OK);
}
/* Exercise the real receive transaction: three packets total 121000 bytes,
 * within its 128 KiB budget. Only the first two form one short-final URO NBL. */
static VOID UdpTerminalReceive(ULONG ip,ULONG allocationFailure,BOOLEAN synchronousReturn,BOOLEAN lateFault)
{
    FIXTURE *f=Create(1,4,8,0);PB_KERNEL_QUEUE *q=f->S.Queues;EnabledMask=PB_RX_URO;
    q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG b=0;b<2;++b) q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);
    const ULONG lengths[]={60000,1000,60000};PB_DESCRIPTOR items[3]={0};uint32_t n,notify;
    for(ULONG i=0;i<3;++i) {
        items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=lengths[i],.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
        UdpTerminalPacket(f->S.Region+items[i].DataOffset,lengths[i],ip,(UCHAR)(0x50+i));
    }
    UCHAR *expected=PbTestGuardAlloc(PB_BLOCK_SIZE);PB_COALESCE oracle={0};
    CHECK(PbCoalesceBegin(&oracle,expected,65535,f->S.Region+items[0].DataOffset,60000)==PB_PACKET_OK);
    CHECK(PbCoalesceAppend(&oracle,f->S.Region+items[1].DataOffset,1000)==PB_PACKET_OK && PbCoalesceFinish(&oracle)==PB_PACKET_OK);
    CHECK(PbQueuePush(&Submit,items,3,&n,&notify)==PB_OK && n==3);
    Acquisitions=0;Failure=allocationFailure;UnexpectedIndication=lateFault;ReturnDuringIndication=synchronousReturn;
    if(lateFault) AfterNblAllocate=FaultDuringBuild;
    CHECK(ActualReceiveBatch(q) && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==3);Failure=0;
#if defined(PB_COALESCE_TERMINAL_BASELINE) || defined(PB_EMPTY_BASELINE)
    CHECK(SnapshotBytes==121000);
#else
    CHECK(SnapshotBytes==61000);
#endif
    ULONG objects=allocationFailure==1?1:allocationFailure?2:3,nbls=allocationFailure?1:2;
    CHECK(f->S.RxObjects==objects && f->S.Active==((synchronousReturn || lateFault)?0:(LONG)nbls));
    CHECK(f->Ctx.Probe.UroPackets==((allocationFailure==1 || lateFault)?0:2) && f->Ctx.Probe.UroEvents==((allocationFailure==1 || lateFault)?0:1) && !f->Ctx.Probe.RscAborts);
    if(!synchronousReturn && !lateFault) {
        CHECK(Indicated);NET_BUFFER_LIST *raw=Indicated;
        if(allocationFailure!=1) {
            PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(Indicated)&~(ULONG_PTR)1);
            NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=NET_BUFFER_LIST_INFO(Indicated,UdpRecvSegCoalesceOffloadInfo)};
            NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum={.Value=NET_BUFFER_LIST_INFO(Indicated,TcpIpChecksumNetBufferListInfo)};
            CHECK(record->Count==2 && record->CoalesceIndex<2 && Indicated->Length==oracle.Length+PB_ETH_HEADER);
            CHECK(info.Receive.SegCount==2 && info.Receive.SegSize==60000-(ip==4?28:48));
            CHECK(checksum.Receive.UdpChecksumSucceeded && checksum.Receive.IpChecksumSucceeded==(ULONG)(ip==4) && checksum.Receive.IpChecksumValueInvalid==(ULONG)(ip==4));
            CHECK(!memcmp(q->CoalesceBuffers[record->CoalesceIndex],expected,oracle.Length));raw=Indicated->Next;
        }
        if(allocationFailure<2) CHECK(raw && !raw->Next && raw->Length==60000+PB_ETH_HEADER && !NET_BUFFER_LIST_INFO(raw,UdpRecvSegCoalesceOffloadInfo) && !NET_BUFFER_LIST_INFO(raw,TcpIpChecksumNetBufferListInfo));
        else CHECK(!raw);
        CHECK(PbV2ReturnNbls(&f->Ctx,&Indicated,PB_MAX_BATCH) && !Indicated);
    }
    CHECK(!Pools && !Mdls && !Nbls && !q->Credits && !q->PendingCount && !f->S.Active && f->S.RxReturned==objects);
    if(lateFault) {
        CHECK(!Indicated && !AfterNblAllocate && !f->S.Completions && State(f,0)==PB_SESSION_QUARANTINED);
        PbV2Worker(q);CHECK(State(f,0)==PB_SESSION_STOPPED);
    } else {
        PB_DESCRIPTOR out[3];CHECK(PbQueuePop(&Complete,out,3,&n)==PB_OK && n==3);ULONG seen=0;
        for(ULONG i=0;i<3;++i) {
            ULONG slot=out[i].Token.Slot;CHECK(slot<3 && !(seen&(1u<<slot)));seen|=1u<<slot;
            ULONG status=allocationFailure==1 && slot<2?PB_FULL:allocationFailure>1 && slot==2?PB_FULL:PB_OK;
            CHECK(out[i].Status==status && !memcmp(&out[i].Token,&items[slot].Token,sizeof(PB_TOKEN)));
        }
        CHECK(seen==7 && f->S.Completions==3);
    }
    printf("PASS: actual short-final receive ip=%lu allocation_failure=%lu synchronous_return=%u late_fault=%u snapshot_bytes=%llu: exact URO/raw OOB, payload, unique completions, stats and returned ownership.\n",ip,allocationFailure,synchronousReturn,lateFault,SnapshotBytes);
    for(ULONG b=0;b<2;++b) {CHECK(!q->CoalesceBusy[b]);PbTestGuardFree(q->CoalesceBuffers[b]);}
    PbTestGuardFree(expected);PbTestGuardFree(q->Scratch);Destroy(f);
}
static VOID RecycleFaultWithNativeReceive(BOOLEAN paused)
{
    FIXTURE *f=Create(1,4,8,0);PB_KERNEL_QUEUE *q=f->S.Queues;NET_BUFFER_LIST *held=NativeReceive(f,3);
    if(paused) {f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);}
    q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer=9;PbV2Worker(q);
    CHECK(f->S.Faulted && q->FaultQuiesced && Exits==1 && !Calls && !Idle);
    CHECK(q->Credits==1 && q->RxSlots[3].Active && f->S.Active==1 && Pools==1 && Mdls==2 && Nbls==1);
    CHECK(State(f,1)==PB_SESSION_QUARANTINED);
    PbV2ReturnOne(&f->Ctx,held);CHECK(State(f,0)==PB_SESSION_STOPPED);Destroy(f);
    printf("PASS: recycle counter fault paused=%u reaches worker witness without RX; existing NDIS receive owns slot/credit/resources and prevents STOPPED until real return.\n",paused);
}
static VOID RejectedBeforeIndication(BOOLEAN held)
{
    FIXTURE *f=Create(1,4,8,0);PB_KERNEL_QUEUE *q=f->S.Queues;
    NET_BUFFER_LIST *old=held?NativeReceive(f,3):NULL;
    const USHORT slots[2][3]={{0,0,1},{3,0,1}};PB_DESCRIPTOR items[3]={0};uint32_t n,notify;
    for(ULONG i=0;i<3;++i) {
        USHORT slot=slots[held][i];items[i].Token=(PB_TOKEN){42,1,slot,0};items[i].Length=20;
        items[i].DataOffset=f->S.Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE;
        f->S.Region[items[i].DataOffset]=0x45;
    }
    CHECK(PbQueuePush(&Submit,items,3,&n,&notify)==PB_OK && n==3);
    Acquisitions=0;FixtureRundownAcquires=FixtureRundownReleases=0;
    CHECK(ActualReceiveBatch(q) && f->S.Faulted && q->Credits==held);
    CHECK(!q->RxSlots[0].Active && !q->RxSlots[1].Active && q->RxSlots[3].Active==held);
    CHECK(f->S.Active==held && Pools==(LONG)held && Mdls==2*(LONG)held && Nbls==(LONG)held);
#if defined(PB_RX_CANCEL_BASELINE) || defined(PB_EMPTY_BASELINE)
    CHECK(Acquisitions==8 && FixtureRundownAcquires==2 && FixtureRundownReleases==2);
    CHECK(f->S.RxObjects==held+2 && f->S.RxReturned==2 && f->Ctx.Statistics.ifInErrors==2);
#else
    CHECK(!Acquisitions && !FixtureRundownAcquires && !FixtureRundownReleases);
    CHECK(f->S.RxObjects==held && !f->S.RxReturned && !f->Ctx.Statistics.ifInErrors);
#endif
    CHECK(State(f,held)==PB_SESSION_QUARANTINED && !q->FaultQuiesced);
    printf("PASS: fault before indication held=%u: allocation attempts=%lu rundown pairs=%ld active=%ld; STOPPED still requires worker witness.\n",held,Acquisitions,FixtureRundownAcquires,f->S.Active);
    PbV2Worker(q);CHECK(q->FaultQuiesced && Exits==1);
    CHECK(State(f,held)==(held?PB_SESSION_QUARANTINED:PB_SESSION_STOPPED));
    if(held) {
        CHECK(q->RxSlots[3].Active);PbV2ReturnOne(&f->Ctx,old);
        CHECK(!q->RxSlots[3].Active && State(f,0)==PB_SESSION_STOPPED);
    }
    Destroy(f);
}
static VOID CreditReturnBeforeIdle(VOID)
{
    FIXTURE *f=Create(1,4,4,0); NET_BUFFER_LIST *first=NativeReceive(f,0),*last=NativeReceive(f,2);
    first->Next=NativeReceive(f,1); last->Next=NativeReceive(f,3);
    SubmissionEpoch=41; /* Raw invalid submissions can coexist with every valid slot held by NDIS. */
    for(ULONG i=0;i<3;++i) SubmitOne(&f->S); Drain(f); Returning=first; ReturnCredit=Append=TRUE;
    PbV2Worker(f->S.Queues);
    CHECK(Idle==1 && Calls==4 && SubmittedCompletions==3 && NativeCompletions==2 && State(f,2)==PB_SESSION_QUARANTINED && f->S.Queues[0].Credits==2);
    CHECK(PbV2ReturnNbls(&f->Ctx,&last,PB_MAX_BATCH) && !last); ConsumeCompletions();
    CHECK(NativeCompletions==4 && State(f,0)==PB_SESSION_STOPPED); Destroy(f);
    puts("PASS: full receive credits return as one window between failed receive and idle check; finite drain progresses, post-boundary tail parks, actual remaining NDIS refs still prevent STOPPED.");
}
static DWORD WINAPI RunWaitingWorker(VOID *opaque) {PbV2Worker(opaque);return 0;}
static HANDLE StartWaitingWorker(FIXTURE *f,BOOLEAN gated)
{
    WaitEntered=CreateEventW(NULL,TRUE,FALSE,NULL);WaitGate=gated?CreateEventW(NULL,TRUE,FALSE,NULL):NULL;
    CHECK(WaitEntered && (!gated || WaitGate));RealWait=TRUE;OnSetEvent=CountKernelWake;
    HANDLE thread=CreateThread(NULL,0,RunWaitingWorker,f->S.Queues,0,NULL);
    CHECK(thread && WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==1);
    CHECK(ResetEvent(WaitEntered));return thread;
}
static VOID StopWaitingWorker(FIXTURE *f,HANDLE thread)
{
    InterlockedExchange(&f->S.Stopping,1);CHECK(SetEvent(f->S.StopEvent.Native));
    if(WaitGate) CHECK(SetEvent(WaitGate));
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && Exits==1 && CloseHandle(thread));
    CHECK(CloseHandle(WaitEntered));if(WaitGate) CHECK(CloseHandle(WaitGate));
    WaitEntered=WaitGate=NULL;RealWait=FALSE;OnSetEvent=NULL;
}
static VOID PausedRecycleWaiting(BOOLEAN gated)
{
    FIXTURE *f=Create(1,4,8,0);PB_KERNEL_QUEUE *q=f->S.Queues;PB_SLOT slots[4];PB_QUEUE_VIEW producer;
    CHECK(PbPoolInitialize(&q->TxPool,slots,4,f->S.Epoch,0)==PB_OK);
    CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);SubmissionEpoch=41;SubmitOne(&f->S);
    HANDLE worker=StartWaitingWorker(f,gated);CHECK(!Calls);
    for(ULONG i=0;i<16;++i) {
        PB_DESCRIPTOR d={0};uint32_t n,notify;KLOCK_QUEUE_HANDLE lock;
        KeAcquireInStackQueuedSpinLock(&q->TxLock,&lock);
        CHECK(PbPoolReserve(&q->TxPool,&d.Token)==PB_OK);
        CHECK(PbPoolTransition(&q->TxPool,&d.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        KeReleaseInStackQueuedSpinLock(&lock);
        CHECK(PbQueuePush(&producer,&d,1,&n,&notify)==PB_OK && n==1 && notify==1);
        KeSetEvent(q->KernelEvent,0,FALSE);if(WaitGate) CHECK(SetEvent(WaitGate));
        CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==i+2 && !Calls && KernelWakes==i+1);
        CHECK(slots[d.Token.Slot].State==PB_SLOT_FREE && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==i+1);
        CHECK(ResetEvent(WaitEntered));
    }
    CHECK(WaitForSingleObject(WaitEntered,30)==WAIT_TIMEOUT && !q->Rings[PB_QUEUE_RX_SUBMIT].Cursor);
    WriteRelease(&f->Ctx.Running,1);KeSetEvent(&f->Ctx.Probe.RunningEvent,0,FALSE);
    CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==18 && Calls==2 && SubmittedCompletions==1 && KernelWakes==16);
    StopWaitingWorker(f,worker);
    printf("PASS: paused real waiter before_wait=%u: 16 returns cause exactly 16 wake cycles, no idle spin/RX scan; restart alone resumes the queued submission.\n",gated);Destroy(f);
}
static VOID PausedStopWaiting(BOOLEAN manual,BOOLEAN gated,BOOLEAN wrap,BOOLEAN fault)
{
    ULONG64 base=wrap?UINT64_MAX-1:0;
    FIXTURE *f=Create(1,4,8,base);PB_KERNEL_QUEUE *q=f->S.Queues;PB_SLOT slots[4];PB_QUEUE_VIEW producer;
    if(manual) {CHECK(CloseHandle(q->KernelEvent->Native));q->KernelEvent->Native=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(q->KernelEvent->Native);}
    CHECK(PbPoolInitialize(&q->TxPool,slots,4,f->S.Epoch,0)==PB_OK);
    PB_TOKEN token={0};
    for(ULONG i=0;i<4;++i) {
        PB_TOKEN reserved;CHECK(PbPoolReserve(&q->TxPool,&reserved)==PB_OK && PbPoolTransition(&q->TxPool,&reserved,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        if(!i) token=reserved;
    }
    CHECK(!q->TxPool.FreeBits[0]); /* Establish the full-pool proof from actual ownership. */
    CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    PB_QUEUE_VIEW *recycle=q->Rings+PB_QUEUE_TX_RECYCLE;
    recycle->Cursor=recycle->PeerCursor=producer.Cursor=producer.PeerCursor=base;
    recycle->Shared->Producer=recycle->Shared->Consumer=base;q->TxPoolExhausted=TRUE;
    f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);
    for(ULONG i=0;i<3;++i) SubmitOne(&f->S);
    HANDLE worker=StartWaitingWorker(f,gated);CHECK(!Calls && !KernelWakes);
    if(!fault) {Drain(f);CHECK(q->DrainProducer==base+3);}
    for(ULONG i=0;i<3;++i) SubmitOne(&f->S);
    /* Park the raw return publisher after publication but before SetEvent.
     * DRAIN/Fault's own wake must suffice; do not inject another return wake. */
    uint32_t accepted,notify;
    CHECK(PbQueuePushTokens(&producer,&token,1,&accepted,&notify)==PB_OK && accepted==1 && notify==1);
    if(fault) PbV2Fault(&f->S);else PbV2Wake(&f->S);
    if(WaitGate) CHECK(SetEvent(WaitGate));
    if(fault) {
        CHECK(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && Exits==1 && Idle==1 && !Calls);
        CHECK(q->FaultQuiesced && recycle->Cursor==base && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==base);
        /* Fault quiescence is not evidence that queued TX tokens retired.
         * No native slot may become reusable merely because QUERY is STOPPED. */
        CHECK(slots[token.Slot].State==PB_SLOT_PUBLISHED && q->TxPoolExhausted && !(q->TxPool.FreeBits[0]&(1u<<token.Slot)));
        CHECK(!SubmittedCompletions && !NativeCompletions);
    } else {
        CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==2 && Calls==2);
        CHECK(SubmittedCompletions==3 && !NativeCompletions && !f->S.Faulted && !q->FaultQuiesced);
        CHECK(q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==base+3 && recycle->Cursor==base+1);
        CHECK(slots[token.Slot].State==PB_SLOT_FREE && !q->TxPoolExhausted && q->TxPool.FreeBits[0]==(1u<<token.Slot));
        CHECK(ResetEvent(WaitEntered) && WaitForSingleObject(WaitEntered,30)==WAIT_TIMEOUT);
    }
    CHECK(KernelWakes==1 && slots[token.Slot].Generation==token.Generation && !f->S.Active && !q->Credits && !q->PendingCount);
    for(ULONG i=0;i<4;++i) CHECK(slots[i].Generation==1 && slots[i].State==(!fault && i==token.Slot?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
    CHECK(State(f,0)==PB_SESSION_STOPPED);
    /* Shared tails cannot revoke either private stopping conclusion. The
     * normal worker remains asleep until the separate physical stop below. */
    q->Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer=UINT64_MAX/2;
    recycle->Shared->Producer=UINT64_MAX/2;CHECK(State(f,0)==PB_SESSION_STOPPED);
    StopWaitingWorker(f,worker);Destroy(f);
}
static VOID IdleReturnWake(BOOLEAN paused,BOOLEAN gated)
{
    FIXTURE *f=Create(1,4,8,0);NET_BUFFER_LIST *nbl=NativeReceive(f,0);
    if(paused) {f->Ctx.Running=0;KeClearEvent(&f->Ctx.Probe.RunningEvent);}
    HANDLE worker=StartWaitingWorker(f,gated);
    CHECK(PbV2ReturnOne(&f->Ctx,nbl));if(WaitGate) CHECK(SetEvent(WaitGate));
#ifdef PB_RETURN_WAKE_BASELINE
    CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==2 && KernelWakes==1);
    CHECK(Calls==(paused?0u:2u));
#else
    CHECK(WaitForSingleObject(WaitEntered,30)==WAIT_TIMEOUT && Idle==1 && !KernelWakes);
    CHECK(Calls==(paused?0u:1u));
#endif
    StopWaitingWorker(f,worker);ConsumeCompletions();
    CHECK(NativeCompletions==1 && !f->S.Active && !f->S.Queues[0].Credits);
    printf("PASS: idle return paused=%u before_wait=%u: kernel wakes=%lu worker idle entries=%lu receive scans=%lu, completion/last-reference progress without new admission.\n",paused,gated,KernelWakes,Idle,Calls);Destroy(f);
}
static VOID FullCreditWait(BOOLEAN draining,BOOLEAN gated)
{
    FIXTURE *f=Create(1,4,4,0);NET_BUFFER_LIST *first=NativeReceive(f,0),*last=NativeReceive(f,2);
    first->Next=NativeReceive(f,1);last->Next=NativeReceive(f,3);SubmissionEpoch=41;
    for(ULONG i=0;i<3;++i) SubmitOne(&f->S);if(draining) Drain(f);
    HANDLE worker=StartWaitingWorker(f,gated);CHECK(Calls==1 && f->S.Queues[0].Credits==4);
    /* Producer is already ready, so its arm is cleared. Only the return's
     * full -> usable transition can wake this worker; no producer/poll wake. */
    CHECK(!f->S.Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Shared->Armed);
    CHECK(PbV2ReturnNbls(&f->Ctx,&first,PB_MAX_BATCH) && !first && KernelWakes==1);
    if(WaitGate) CHECK(SetEvent(WaitGate));
    CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==2 && Calls==4);
    CHECK(SubmittedCompletions==3 && NativeCompletions==2 && f->S.Active==2 && f->S.Queues[0].Credits==2);
    if(draining) CHECK(State(f,2)==PB_SESSION_QUARANTINED);
    StopWaitingWorker(f,worker);CHECK(PbV2ReturnNbls(&f->Ctx,&last,PB_MAX_BATCH) && !last);ConsumeCompletions();
    CHECK(NativeCompletions==4);if(draining) CHECK(State(f,0)==PB_SESSION_STOPPED);
    printf("PASS: full-credit return draining=%u before_wait=%u: one wake resumes three already-queued submissions, second wait reached; native owners still block drain.\n",draining,gated);Destroy(f);
}
static VOID FullCompletionCreditWait(BOOLEAN draining)
{
    FIXTURE *f=Create(1,4,4,0);PB_KERNEL_QUEUE *q=f->S.Queues;PB_DESCRIPTOR junk[4]={0};uint32_t n,notify;
    for(ULONG i=0;i<4;++i) {junk[i].Token.Epoch=40;junk[i].Status=PB_INVALID;}
    CHECK(PbQueuePush(q->Rings+PB_QUEUE_RX_COMPLETE,junk,4,&n,&notify)==PB_OK && n==4);
    NET_BUFFER_LIST *first=NativeReceive(f,0),*last=NativeReceive(f,2);
    first->Next=NativeReceive(f,1);last->Next=NativeReceive(f,3);SubmissionEpoch=41;AutoConsume=FALSE;
    for(ULONG i=0;i<3;++i) SubmitOne(&f->S);if(draining) Drain(f);
    HANDLE worker=StartWaitingWorker(f,FALSE);CHECK(Calls==1 && q->Credits==4);
    CHECK(PbV2ReturnNbls(&f->Ctx,&first,PB_MAX_BATCH) && !first && q->Credits==4 && q->PendingCount==2);
#ifdef PB_RETURN_WAKE_BASELINE
    CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Idle==2 && Calls==2 && KernelWakes==1);
    CHECK(ResetEvent(WaitEntered));
#else
    CHECK(WaitForSingleObject(WaitEntered,30)==WAIT_TIMEOUT && Idle==1 && Calls==1 && !KernelWakes);
#endif
    /* Returning NBLs did not release completion credits. Only the completion
     * consumer's actual space release and its existing wake can make progress. */
    ConsumeCompletions();CHECK(JunkCompletions==4);AutoConsume=TRUE;KeSetEvent(q->KernelEvent,0,FALSE);
    CHECK(WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && SubmittedCompletions==3 && NativeCompletions==2 && q->Credits==2 && f->S.Active==2);
    StopWaitingWorker(f,worker);CHECK(PbV2ReturnNbls(&f->Ctx,&last,PB_MAX_BATCH) && !last);ConsumeCompletions();
    CHECK(NativeCompletions==4);if(draining) CHECK(State(f,0)==PB_SESSION_STOPPED);
    printf("PASS: full completion/credit wait draining=%u: no credit released until consumer space/wake; pending plus three submissions then progress, with native ownership retained.\n",draining);Destroy(f);
}
#endif
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0); DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Tail(3,0,FALSE); Tail(3,UINT64_MAX-1,TRUE); Tail(0,0,FALSE);
#ifndef PB_DRAIN_BASELINE
    EmptyReceiveProgress(); NormalIdleArm(); RecycleIdleWake(); PausedRecycleBudget(0); PausedRecycleBudget(UINT64_MAX-63); MultipleQueuesAndFault(); CreditReturnBeforeIdle();
    RejectedBeforeIndication(FALSE); RejectedBeforeIndication(TRUE);
    RecycleFaultWithNativeReceive(FALSE); RecycleFaultWithNativeReceive(TRUE);
    PausedRecycleWaiting(FALSE); PausedRecycleWaiting(TRUE);
    for(ULONG schedule=0;schedule<16;++schedule)
        PausedStopWaiting((BOOLEAN)((schedule&1)!=0),(BOOLEAN)((schedule&2)!=0),(BOOLEAN)((schedule&4)!=0),(BOOLEAN)((schedule&8)!=0));
    puts("PASS: 16 paused stop/return schedules: sync/manual events, gated/ungated waits, ordinary/wrapped cursors, DRAIN/Fault. One stop wake suffices; finite DRAIN retires exactly one of four published TX slots and the initial RX boundary without idle spin; fault exits without fabricated TX reuse, shared tails cannot revoke STOPPED.");
    for(ULONG gated=0;gated<2;++gated) {IdleReturnWake(FALSE,(BOOLEAN)gated);IdleReturnWake(TRUE,(BOOLEAN)gated);FullCreditWait(FALSE,(BOOLEAN)gated);FullCreditWait(TRUE,(BOOLEAN)gated);}
    FullCompletionCreditWait(FALSE);FullCompletionCreditWait(TRUE);
    LateFaultCancel(FALSE,FALSE);LateFaultCancel(TRUE,FALSE);LateFaultCancel(FALSE,TRUE);
    for(ULONG mode=0;mode<=4;++mode) IndicationStats(mode,FALSE,FALSE,FALSE);
    IndicationStats(1,TRUE,FALSE,FALSE);IndicationStats(3,FALSE,TRUE,FALSE);
    IndicationStats(1,TRUE,TRUE,FALSE);IndicationStats(3,FALSE,FALSE,TRUE);
#if defined(PB_CURRENT_RECEIVE_TAIL) || defined(PB_RX_TAIL_TESTS)
    ReceiveTailPending(TRUE,FALSE);ReceiveTailPending(TRUE,TRUE);ReceiveTailPending(FALSE,FALSE);
    ReceiveTailContendedReturn();
#endif
    for(ULONG ip=4;ip<=6;ip+=2) {UdpTerminalReceive(ip,0,FALSE,FALSE);UdpTerminalReceive(ip,1,FALSE,FALSE);UdpTerminalReceive(ip,5,FALSE,FALSE);UdpTerminalReceive(ip,0,TRUE,FALSE);UdpTerminalReceive(ip,0,FALSE,TRUE);}
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after); return 0;
}
