/* SPDX-License-Identifier: GPL-2.0 */
/* Execute production completion and paused-worker paths with deterministic hosted DDI stand-ins. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
#define NT_ASSERT CHECK
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
#define WaitAny 0
#define STATUS_SUCCESS 0
typedef struct { BOOLEAN Signaled; ULONG Sets; } KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct _TUN_CTX { volatile LONG Running; struct { volatile LONG V2SessionActive; KEVENT RunningEvent; } Probe; } TUN_CTX;
typedef struct { KSPIN_LOCK *Lock; } KLOCK_QUEUE_HANDLE;
typedef VOID KSTART_ROUTINE(PVOID);
static PB_KERNEL_SESSION *WorkerSession;
static ULONG Waits,UnexpectedReceives;
static PB_KERNEL_QUEUE *CompletionQueue;
static ULONG StagedDescriptors,DirectDescriptors;
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait) { (void)priority; (void)wait; event->Signaled=TRUE; event->Sets++; }
static VOID KeClearEvent(KEVENT *event) { event->Signaled=FALSE; }
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle) { CHECK(!*lock); *lock=1; handle->Lock=lock; }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) { CHECK(*handle->Lock); *handle->Lock=0; }
#include "../../driver/v2/session_fault.h"
#ifdef PB_COMPLETION_COPY_BASELINE
static PB_RESULT StagedPush(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    CHECK(view==CompletionQueue->Rings+PB_QUEUE_RX_COMPLETE && items!=CompletionQueue->Pending);
    StagedDescriptors+=count;return PbQueuePush(view,items,count,accepted,notify);
}
#define PbQueuePush StagedPush
#else
static PB_RESULT DirectPush(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t capacity,uint32_t head,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    CHECK(view==CompletionQueue->Rings+PB_QUEUE_RX_COMPLETE && items==CompletionQueue->Pending);
    CHECK(capacity==CompletionQueue->Session->Layout.RingCapacity && head==CompletionQueue->PendingHead);
    PB_RESULT result=PbQueuePushRing(view,items,capacity,head,count,accepted,notify);DirectDescriptors+=*accepted;return result;
}
#define PbQueuePushRing DirectPush
#endif
#ifndef PB_COMPLETION_SOURCE
#define PB_COMPLETION_SOURCE "../../driver/v2/session_completion.h"
#endif
#include PB_COMPLETION_SOURCE
#undef PbQueuePush
#undef PbQueuePushRing
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{ (void)reason; (void)mode; (void)alertable; (void)timeout; CHECK(event->Signaled); }
static VOID KeWaitForMultipleObjects(ULONG count,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{
    (void)type; (void)reason; (void)mode; (void)alertable; (void)timeout; (void)blocks; CHECK(count==3 && ++Waits<8);
    KEVENT *kernel=events[1]; if(kernel->Signaled) kernel->Signaled=FALSE; else WorkerSession->Stopping=1;
}
static PB_RESULT PbV2RecycleLocked(PB_KERNEL_QUEUE *q) { (void)q; return PB_OK; }
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q) { (void)q; UnexpectedReceives++; return FALSE; }
static VOID PsTerminateSystemThread(LONG status) { CHECK(status==STATUS_SUCCESS); }
#ifndef PB_WORKER_SOURCE
#define PB_WORKER_SOURCE "../../driver/v2/session_worker.h"
#endif
#include PB_WORKER_SOURCE
typedef struct { PB_KERNEL_SESSION S; TUN_CTX Ctx; PB_QUEUE_VIEW Consumer; KEVENT Kernel,User; UCHAR *Region; PB_RX_SLOT Slot; } FIXTURE;
static FIXTURE *Create(ULONG capacity)
{
    FIXTURE *f=calloc(1,sizeof(*f)); CHECK(f && PbCalculateLayout(1,1,capacity,&f->S.Layout)==PB_OK);
    f->S.Ctx=&f->Ctx; f->S.Epoch=42; f->Ctx.Probe.V2SessionActive=1;
    f->Region=_aligned_malloc(f->S.Layout.RegionSize,64); CHECK(f->Region); memset(f->Region,0,f->S.Layout.RegionSize);
    PB_KERNEL_QUEUE *q=f->S.Queues; q->Session=&f->S; q->RxSlots=&f->Slot; q->KernelEvent=&f->Kernel; q->UserEvent=&f->User;
    q->Pending=PbTestGuardAlloc(capacity*sizeof(*q->Pending)); CHECK(q->Pending);memset(q->Pending,0,capacity*sizeof(*q->Pending));
    CHECK(PbAttachQueue(&q->Rings[PB_QUEUE_RX_COMPLETE],f->Region,&f->S.Layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
    CHECK(PbAttachQueue(&q->Rings[PB_QUEUE_TX_RECYCLE],f->Region,&f->S.Layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
    CHECK(PbAttachQueue(&f->Consumer,f->Region,&f->S.Layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    CompletionQueue=q;StagedDescriptors=DirectDescriptors=0;return f;
}
static VOID Destroy(FIXTURE *f) { PbTestGuardFree(f->S.Queues[0].Pending); _aligned_free(f->Region); free(f);CompletionQueue=NULL; }
static VOID Add(FIXTURE *f,ULONG first,ULONG count)
{
    PB_KERNEL_QUEUE *q=f->S.Queues;
    for(ULONG i=0;i<count;++i) { PB_DESCRIPTOR d={.Token={42,first+i+1,0,0},.Length=20}; q->Credits++; PbV2CompleteLocked(q,&d,PB_OK); }
}
static ULONG Consume(FIXTURE *f,ULONG first)
{
    PB_DESCRIPTOR out[PB_MAX_BATCH]; uint32_t count; ULONG total=0; PB_RESULT result;
    while((result=PbQueuePop(&f->Consumer,out,PB_MAX_BATCH,&count))==PB_OK) {
        for(ULONG i=0;i<count;++i) CHECK(out[i].Token.Generation==first+total+i+1);
        total+=count;
    }
    CHECK(result==PB_EMPTY); return total;
}
static VOID BudgetAndWrap(BOOLEAN wrap)
{
    FIXTURE *f=Create(4096); PB_KERNEL_QUEUE *q=f->S.Queues;
    if(wrap) { q->PendingHead=4090; q->Rings[PB_QUEUE_RX_COMPLETE].Cursor=q->Rings[PB_QUEUE_RX_COMPLETE].PeerCursor=UINT64_MAX-63;
        f->Consumer.Cursor=f->Consumer.PeerCursor=q->Rings[PB_QUEUE_RX_COMPLETE].Cursor;
        q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Consumer=q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Producer=f->Consumer.Cursor; }
    Add(f,0,4096); ULONG consumed=0,passes=0;
    while(q->PendingCount) {
        uint32_t ready; CHECK(PbQueueArm(&f->Consumer,&ready)==PB_OK && !ready); f->Kernel.Signaled=FALSE;
        ULONG before=q->PendingCount; PbV2FlushLocked(q);
        CHECK(before-q->PendingCount<=4*PB_MAX_BATCH && before-q->PendingCount>0);
        ULONG n=Consume(f,consumed); CHECK(n==before-q->PendingCount && f->User.Signaled);
        consumed+=n; passes++;
        CHECK(f->Kernel.Signaled==(q->PendingCount!=0)); /* Bounded work must arrange its own next pass. */
    }
    CHECK(consumed==4096 && passes==4096/(4*PB_MAX_BATCH) && !q->Credits && f->S.Completions==4096 && f->User.Sets==passes);
#ifdef PB_COMPLETION_COPY_BASELINE
    CHECK(StagedDescriptors==4096 && !DirectDescriptors);
#else
    CHECK(!StagedDescriptors && DirectDescriptors==4096);
#endif
    printf("PASS: actual completion flush wrap=%u: completions=%lu staged_bytes=%zu direct_descriptors=%lu, <=%u/pass, identical continuation and wake counts.\n",wrap,consumed,StagedDescriptors*sizeof(PB_DESCRIPTOR),DirectDescriptors,4*PB_MAX_BATCH);
    Destroy(f);
}
static VOID BackpressureAndCorruption(BOOLEAN corrupt)
{
    FIXTURE *f=Create(4); PB_KERNEL_QUEUE *q=f->S.Queues;
    Add(f,0,4); PbV2FlushLocked(q); CHECK(!q->PendingCount && !q->Credits);
    Add(f,4,1); f->Kernel.Signaled=FALSE;
    if(corrupt) q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Consumer=q->Rings[PB_QUEUE_RX_COMPLETE].Cursor+1;
    PbV2FlushLocked(q);
    if(corrupt) { CHECK(f->S.Faulted && f->Kernel.Signaled && f->User.Signaled && !q->PendingCount && !q->Credits); }
    else {
        CHECK(q->PendingCount==1 && q->Credits==1);
        CHECK(!f->Kernel.Signaled && !f->S.Faulted); /* Full completion ring must park, not self-spin. */
        CHECK(Consume(f,0)==4); PbV2FlushLocked(q); CHECK(Consume(f,4)==1 && !q->Credits && !q->PendingCount);
    }
    Destroy(f);
}
static VOID PartialWrap(VOID)
{
    FIXTURE *f=Create(8);PB_KERNEL_QUEUE *q=f->S.Queues;
    q->PendingHead=7;q->Rings[PB_QUEUE_RX_COMPLETE].Cursor=q->Rings[PB_QUEUE_RX_COMPLETE].PeerCursor=UINT64_MAX-2;
    f->Consumer.Cursor=f->Consumer.PeerCursor=UINT64_MAX-2;
    q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Producer=q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Consumer=f->Consumer.Cursor;
    Add(f,0,7);PbV2FlushLocked(q);CHECK(!q->Credits && Consume(f,0)==7);
    /* Occupy seven destinations, then offer a pending source wrapping at 8.
     * Only its first descriptor fits. The remaining private prefix must stay
     * ordered and retain its credits until consumer space is returned. */
    Add(f,7,7);PbV2FlushLocked(q);CHECK(!q->Credits);
    q->PendingHead=7;Add(f,14,4);f->Kernel.Signaled=FALSE;PbV2FlushLocked(q);
    CHECK(q->PendingCount==3 && q->Credits==3 && q->PendingHead==0 && !f->Kernel.Signaled);
    CHECK(Consume(f,7)==8);PbV2FlushLocked(q);CHECK(Consume(f,15)==3 && !q->Credits && !q->PendingCount && q->PendingHead==3);
    Destroy(f);puts("PASS: actual flush partial acceptance across independent pending/destination/uint64 wraps, stable remaining credits/order and full-ring parking.");
}
static VOID PausedProgress(VOID)
{
    FIXTURE *f=Create(4); PB_KERNEL_QUEUE *q=f->S.Queues;
    /* Legal error responses can fill the ring independently of the one usable payload slot. */
    PB_DESCRIPTOR bad={.Token={41,1,0,0},.Length=20,.DataOffset=f->S.Layout.Queues[0].RxData};
    for(ULONG i=0;i<4;++i) { uint32_t complete; PB_RESULT status=PbAdmitReceive(&f->S.Layout,42,0,&f->Slot,&bad,0,&complete);
        CHECK(status==PB_INVALID && complete); q->Credits++; PbV2CompleteLocked(q,&bad,status); }
    PbV2FlushLocked(q); CHECK(!q->Credits && !q->PendingCount);
    PB_DESCRIPTOR good=bad; good.Token.Epoch=42; uint32_t complete;
    CHECK(PbAdmitReceive(&f->S.Layout,42,0,&f->Slot,&good,0,&complete)==PB_OK); q->Credits++;
    f->Slot.Active=0; PbV2CompleteLocked(q,&good,PB_OK); PbV2FlushLocked(q); CHECK(q->PendingCount==1);
    PB_DESCRIPTOR old[4]; uint32_t n; CHECK(PbQueuePop(&f->Consumer,old,4,&n)==PB_OK && n==4);
    uint32_t ready; CHECK(PbQueueArm(&f->Consumer,&ready)==PB_OK && !ready);
    KeSetEvent(&f->Kernel,0,FALSE); /* PollCompletions signals newly freed completion space. */
    f->S.StartEvent.Signaled=TRUE; WorkerSession=&f->S; Waits=UnexpectedReceives=0; PbV2Worker(q);
    CHECK(!q->PendingCount && !q->Credits && !f->S.Faulted && f->User.Signaled && !UnexpectedReceives);
    CHECK(PbQueuePop(&f->Consumer,old,4,&n)==PB_OK && n==1 && old[0].Token.Epoch==42 && old[0].Status==PB_OK);
    Destroy(f);
}
static VOID PausedBudget(VOID)
{
    const ULONG total=12*PB_MAX_BATCH;
    FIXTURE *f=Create(1024); PB_KERNEL_QUEUE *q=f->S.Queues; Add(f,0,total);
    uint32_t ready; CHECK(PbQueueArm(&f->Consumer,&ready)==PB_OK && !ready);
    f->S.StartEvent.Signaled=TRUE; WorkerSession=&f->S; Waits=UnexpectedReceives=0; PbV2Worker(q);
    CHECK(!q->PendingCount && !q->Credits && f->S.Completions==total && Waits==3 && f->Kernel.Sets==2 && !UnexpectedReceives);
    CHECK(Consume(f,0)==total); Destroy(f);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--paused")) { PausedProgress(); puts("PASS: actual paused worker drains completion backlog after consumer wake."); return 0; }
    BudgetAndWrap(FALSE); BudgetAndWrap(TRUE); BackpressureAndCorruption(FALSE); BackpressureAndCorruption(TRUE); PartialWrap(); PausedProgress(); PausedBudget();
    printf("PASS: actual completion/worker paths, seven cases, 4096 completions per normal/wrap case, <=%u per pass, partial acceptance, reschedule, full-ring quiescence, corruption and paused progress across three budgets.\n",4*PB_MAX_BATCH);
    return 0;
}
