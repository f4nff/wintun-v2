/* SPDX-License-Identifier: GPL-2.0 */
/* Actual NBL ownership, fault completion retirement, worker, query and cleanup.
 * Native DDI stand-ins do not emulate kernel IRQL or NDIS scheduling. */
#include "kernel_nbl_fixture.h"
typedef UCHAR KIRQL;
typedef LONG NTSTATUS;
typedef VOID KSTART_ROUTINE(PVOID);
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define Executive 0
#define KernelMode 0
#define WaitAny 0
static LONG ThreadHandles,ObjectRefs,LockedPages;
static PB_KERNEL_SESSION *CleanupSession;
static HANDLE TailEntered,TailGate,AtRundown,AtAllReturned,CleanupDone;
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) { AcquireSRWLockShared(lock); return 0; }
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL saved) { (void)saved; ReleaseSRWLockShared(lock); }
#define ExAcquireSpinLockExclusive(lock) (AcquireSRWLockExclusive(lock),(KIRQL)0)
#define ExReleaseSpinLockExclusive(lock,irql) ((void)(irql),ReleaseSRWLockExclusive(lock))
static ULONG64 KeQueryInterruptTime(VOID) { return 5000ull*10000ull; }
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    (void)reason;(void)mode;(void)alertable;(void)timeout;
    if(AtAllReturned && event==&CleanupSession->AllReturned) CHECK(SetEvent(AtAllReturned));
    CHECK(WaitForSingleObject(event->Native,5000)==WAIT_OBJECT_0);
}
static VOID KeWaitForMultipleObjects(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{
    (void)type;(void)reason;(void)mode;(void)alertable;(void)timeout;(void)blocks;
    HANDLE native[3]; CHECK(n<=3); for(ULONG i=0;i<n;++i) native[i]=((PKEVENT)events[i])->Native;
    CHECK(WaitForMultipleObjects(n,native,FALSE,5000)<WAIT_OBJECT_0+n);
}
static volatile LONG UnexpectedWork=1;
static PB_RESULT PbV2RecycleLocked(PB_KERNEL_QUEUE *q) { (void)q; CHECK(!UnexpectedWork); return PB_OK; }
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q) { (void)q; CHECK(!UnexpectedWork); return FALSE; }
static VOID PsTerminateSystemThread(LONG status) { CHECK(status==STATUS_SUCCESS); }
#include "../../driver/v2/session_worker.h"
#include "../../driver/v2/session_query.h"
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{ (void)alertable;(void)timeout; CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); }
static VOID ZwClose(HANDLE thread) { CHECK(CloseHandle(thread)); ThreadHandles--; }
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{ if(AtRundown) SetEvent(AtRundown); AcquireSRWLockExclusive(&refs->Lock); CHECK(!refs->References); ReleaseSRWLockExclusive(&refs->Lock); }
static VOID ObDereferenceObject(PKEVENT event) { CHECK(ObjectRefs>0 && CloseHandle(event->Native)); ObjectRefs--; free(event); }
static VOID MmUnlockPages(MDL *mdl) { CHECK(mdl && LockedPages==1); LockedPages--; }
#include "../../driver/v2/session_cleanup.h"

typedef struct FIXTURE {
    TUN_CTX Ctx; PB_KERNEL_SESSION *S; UCHAR *Region;
    PB_QUEUE_VIEW Complete[PB_MAX_QUEUES];
} FIXTURE;
static PKEVENT Event(BOOLEAN manual,BOOLEAN initial)
{ PKEVENT e=calloc(1,sizeof(*e)); CHECK(e); e->Native=CreateEventW(NULL,manual,initial,NULL); CHECK(e->Native); ObjectRefs++; return e; }
static VOID EmbeddedEvent(PKEVENT e,BOOLEAN initial)
{ e->Native=CreateEventW(NULL,TRUE,initial,NULL); CHECK(e->Native); }
static FIXTURE *Create(ULONG queues,ULONG capacity)
{
    CHECK(!Pools && !Mdls && !Nbls && !ObjectRefs && !ThreadHandles && !LockedPages);
    FIXTURE *f=calloc(1,sizeof(*f)); CHECK(f); Failure=Acquisitions=0;
    PB_KERNEL_SESSION *s=ExAllocatePool2(0,sizeof(*s),0); f->S=s; s->Ctx=&f->Ctx; s->Epoch=42;
    CHECK(PbCalculateLayout(queues,4,capacity,&s->Layout)==PB_OK);
    f->Region=VirtualAlloc(NULL,s->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(f->Region);
    s->Region=f->Region; s->Lifecycle=(PB_SHARED_LIFECYCLE *)(f->Region+s->Layout.LifecycleOffset);
    s->Mdl=IoAllocateMdl(f->Region,s->Layout.RegionSize,FALSE,FALSE,NULL); CHECK(s->Mdl); s->PagesLocked=TRUE; LockedPages++;
    EmbeddedEvent(&s->StartEvent,FALSE); EmbeddedEvent(&s->StopEvent,FALSE); EmbeddedEvent(&s->AllReturned,TRUE);
    s->ProgressEvent=Event(FALSE,FALSE); f->Ctx.Running=1; f->Ctx.V2=s; f->Ctx.NblPool=f; f->Ctx.MiniportAdapterHandle=f;
    f->Ctx.Probe.V2SessionActive=1;
    for(ULONG i=0;i<queues;++i) {
        PB_KERNEL_QUEUE *q=s->Queues+i; q->Session=s; q->Index=i; q->KernelEvent=Event(FALSE,FALSE); q->UserEvent=Event(FALSE,FALSE);
        q->RxSlots=ExAllocatePool2(0,s->Layout.SlotCount*sizeof(*q->RxSlots),0);
        q->Pending=ExAllocatePool2(0,capacity*sizeof(*q->Pending),0);
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r) CHECK(PbAttachQueue(q->Rings+r,f->Region,&s->Layout,i,r,r==PB_QUEUE_TX_READY || r==PB_QUEUE_RX_COMPLETE)==PB_OK);
        CHECK(PbAttachQueue(f->Complete+i,f->Region,&s->Layout,i,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);
    }
    return f;
}
static VOID FreeEmbedded(VOID *pointer)
{
    if(pointer!=CleanupSession) return;
    CHECK(!Pools && !Mdls && !Nbls && !ObjectRefs && !ThreadHandles && !LockedPages);
    CHECK(CloseHandle(CleanupSession->StartEvent.Native) && CloseHandle(CleanupSession->StopEvent.Native) && CloseHandle(CleanupSession->AllReturned.Native));
    CleanupSession=NULL;
}
static VOID Destroy(FIXTURE *f)
{
    BeforePoolFree=FreeEmbedded;
    if(f->Ctx.V2) { CleanupSession=f->S; f->Ctx.V2=NULL; PbV2Free(f->S); }
    CHECK(!CleanupSession && !Pools && !Mdls && !Nbls && !ObjectRefs && !ThreadHandles && !LockedPages);
    BeforePoolFree=NULL; CHECK(VirtualFree(f->Region,0,MEM_RELEASE)); free(f);
}
static NET_BUFFER_LIST *ReceiveAt(FIXTURE *f,ULONG index,ULONG slot,PB_DESCRIPTOR *descriptor)
{
    PB_KERNEL_QUEUE *q=f->S->Queues+index;
    *descriptor=(PB_DESCRIPTOR){.Token={42,1,(USHORT)slot,(USHORT)index},.Length=20,.DataOffset=f->S->Layout.Queues[index].RxData+slot*PB_BLOCK_SIZE};
    f->Region[descriptor->DataOffset]=0x45;
    uint32_t complete; CHECK(PbAdmitReceive(&f->S->Layout,42,index,q->RxSlots,descriptor,0,&complete)==PB_OK); q->Credits++;
    NET_BUFFER_LIST *nbl=PbV2Receive(q,descriptor,1,MAXULONG,NULL); CHECK(nbl); PbV2RetainReceives(f->S,1,1); return nbl;
}
static NET_BUFFER_LIST *Receive(FIXTURE *f,ULONG index,PB_DESCRIPTOR *descriptor) { return ReceiveAt(f,index,0,descriptor); }
static DWORD WINAPI Worker(VOID *q) { PbV2Worker(q); return 0; }
static VOID Quiesce(FIXTURE *f,ULONG index)
{
    PB_KERNEL_QUEUE *q=f->S->Queues+index; CHECK(!q->Thread);
    q->Thread=CreateThread(NULL,0,Worker,q,0,NULL); CHECK(q->Thread); ThreadHandles++;
    CHECK(SetEvent(f->S->StartEvent.Native) && WaitForSingleObject(q->Thread,5000)==WAIT_OBJECT_0);
    CHECK(q->FaultQuiesced);
}
static ULONG State(FIXTURE *f,ULONG references)
{ PB_V2_STATUS s; CHECK(PbV2Query(&f->Ctx,&s)==STATUS_SUCCESS && s.ActiveReferences==references); return s.State; }
static VOID FullRing(VOID)
{
    FIXTURE *f=Create(1,4); PB_KERNEL_QUEUE *q=f->S->Queues;
    PB_DESCRIPTOR bad={.Token={41,1,0,0},.Length=20,.DataOffset=f->S->Layout.Queues[0].RxData};
    for(ULONG i=0;i<4;++i) { uint32_t complete; CHECK(PbAdmitReceive(&f->S->Layout,42,0,q->RxSlots,&bad,0,&complete)==PB_INVALID && complete); q->Credits++; PbV2CompleteLocked(q,&bad,PB_INVALID); }
    PbV2FlushLocked(q); CHECK(!q->Credits);
    PB_DESCRIPTOR good; NET_BUFFER_LIST *nbl=Receive(f,0,&good); CHECK(PbV2ReturnOne(&f->Ctx,nbl));
    CHECK(q->PendingCount==1 && q->Credits==1 && !f->S->Active);
    uint32_t complete; CHECK(PbAdmitReceive(&f->S->Layout,42,0,q->RxSlots,&good,0,&complete)==PB_STALE && !complete);
    PbV2Fault(f->S); CHECK(State(f,0)==PB_SESSION_QUARANTINED);
    Quiesce(f,0); CHECK(!q->PendingCount && !q->Credits && State(f,0)==PB_SESSION_STOPPED);
    CHECK(f->S->Completions==4); /* Retirement is not counted as completion delivery. */
    q->Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer=UINT64_MAX;
    CHECK(State(f,0)==PB_SESSION_STOPPED); /* Hostile writes cannot undo private quiescence. */
    Destroy(f);
}
static VOID CorruptRing(VOID)
{
    FIXTURE *f=Create(1,4); PB_DESCRIPTOR d; NET_BUFFER_LIST *nbl=Receive(f,0,&d); PB_KERNEL_QUEUE *q=f->S->Queues;
    q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Consumer=1;
    CHECK(PbV2ReturnOne(&f->Ctx,nbl) && f->S->Faulted && !q->PendingCount && !q->Credits);
    CHECK(State(f,0)==PB_SESSION_QUARANTINED); Quiesce(f,0); CHECK(State(f,0)==PB_SESSION_STOPPED); Destroy(f);
}
static VOID BlockTail(EX_RUNDOWN_REF *refs)
{ CHECK(refs->References==1 && SetEvent(TailEntered) && WaitForSingleObject(TailGate,5000)==WAIT_OBJECT_0); }
typedef struct RETURN { TUN_CTX *Ctx; NET_BUFFER_LIST *Nbl; BOOLEAN Batch; } RETURN;
static DWORD WINAPI Return(VOID *opaque)
{
    RETURN *r=opaque;
    if(r->Batch) {NET_BUFFER_LIST *head=r->Nbl; CHECK(PbV2ReturnNbls(r->Ctx,&head,PB_MAX_BATCH) && !head);}
    else CHECK(PbV2ReturnOne(r->Ctx,r->Nbl));
    return 0;
}
static DWORD WINAPI Cleanup(VOID *opaque)
{
    FIXTURE *f=opaque; AcquireSRWLockExclusive(&f->Ctx.TransitionLock); f->Ctx.V2=NULL; ReleaseSRWLockExclusive(&f->Ctx.TransitionLock);
    PbV2Free(f->S); CHECK(SetEvent(CleanupDone)); return 0;
}
static VOID DelayedReturn(BOOLEAN batch)
{
    FIXTURE *f=Create(2,4); PB_DESCRIPTOR d; NET_BUFFER_LIST *first=Receive(f,0,&d),*last=Receive(f,1,&d);
    if(batch) last->Next=ReceiveAt(f,1,1,&d);
    ULONG remaining=batch?2:1;
    PbV2Fault(f->S); Quiesce(f,0);
    CHECK(State(f,remaining+1)==PB_SESSION_QUARANTINED && !f->S->Queues[1].FaultQuiesced);
    CHECK(PbV2ReturnOne(&f->Ctx,first)); CHECK(State(f,remaining)==PB_SESSION_QUARANTINED);
    Quiesce(f,1); CHECK(State(f,remaining)==PB_SESSION_QUARANTINED && f->S->Queues[1].Credits==remaining);
    TailEntered=CreateEventW(NULL,TRUE,FALSE,NULL); TailGate=CreateEventW(NULL,TRUE,FALSE,NULL);
    AtRundown=CreateEventW(NULL,TRUE,FALSE,NULL); CleanupDone=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(TailEntered && TailGate && AtRundown && CleanupDone); BeforeRundownRelease=BlockTail;
    RETURN r={&f->Ctx,last,batch}; HANDLE returning=CreateThread(NULL,0,Return,&r,0,NULL); CHECK(returning && WaitForSingleObject(TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(State(f,0)==PB_SESSION_STOPPED && f->S->Returns.References==1 && LockedPages==1);
    BeforePoolFree=FreeEmbedded; CleanupSession=f->S;
    HANDLE cleanup=CreateThread(NULL,0,Cleanup,f,0,NULL); CHECK(cleanup && WaitForSingleObject(AtRundown,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(CleanupDone,30)==WAIT_TIMEOUT && LockedPages==1 && CleanupSession);
    CHECK(SetEvent(TailGate) && WaitForSingleObject(returning,5000)==WAIT_OBJECT_0 && WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0);
    BeforeRundownRelease=NULL; Destroy(f);
    CHECK(CloseHandle(returning) && CloseHandle(cleanup) && CloseHandle(TailEntered) && CloseHandle(TailGate) && CloseHandle(AtRundown) && CloseHandle(CleanupDone));
    TailEntered=TailGate=AtRundown=CleanupDone=NULL;
}
static VOID AdmittedSendReference(VOID)
{
    FIXTURE *f=Create(1,4); CHECK(ExAcquireRundownProtection(&f->S->Returns)); PbV2RetainReferences(f->S,1);
    PbV2Fault(f->S); Quiesce(f,0); CHECK(State(f,1)==PB_SESSION_QUARANTINED && !f->S->RxObjects);
    AtAllReturned=CreateEventW(NULL,TRUE,FALSE,NULL); AtRundown=CreateEventW(NULL,TRUE,FALSE,NULL);
    CleanupDone=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(AtAllReturned && AtRundown && CleanupDone);
    BeforePoolFree=FreeEmbedded; CleanupSession=f->S;
    HANDLE cleanup=CreateThread(NULL,0,Cleanup,f,0,NULL); CHECK(cleanup && WaitForSingleObject(AtAllReturned,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(CleanupDone,30)==WAIT_TIMEOUT && WaitForSingleObject(AtRundown,0)==WAIT_TIMEOUT && LockedPages==1);
    PbV2ReleaseReference(f->S); CHECK(WaitForSingleObject(AtRundown,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(CleanupDone,30)==WAIT_TIMEOUT && LockedPages==1);
    ExReleaseRundownProtection(&f->S->Returns); CHECK(WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0);
    Destroy(f); CHECK(CloseHandle(cleanup) && CloseHandle(AtAllReturned) && CloseHandle(AtRundown) && CloseHandle(CleanupDone));
    AtAllReturned=AtRundown=CleanupDone=NULL;
}
int main(VOID)
{
    DWORD before,after; CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    FullRing(); CorruptRing(); DelayedReturn(FALSE); DelayedReturn(TRUE); AdmittedSendReference();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("PASS: actual fault worker/NBL/completion/query/cleanup, full and corrupt completion rings, duplicate raw tokens, hostile producer, two queue witnesses, delayed NDIS return and callback tail, admitted send reference through both release barriers; no early unlock, zero resources and handle delta."); return 0;
}
