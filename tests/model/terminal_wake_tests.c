/* SPDX-License-Identifier: GPL-2.0 */
/* Actual SYS and DLL queue bodies, shared mapping, real Windows locks/events.
 * Ready construction/destruction are hosted; no NDIS/IRQL/system worker or
 * full DLL Open/Close/lifecycle execution is claimed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../include/packetbridge_version.h"
#ifdef PB_API_TESTS
#error Terminal bridge requires shipping DLL state
#endif
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping source fingerprint required");
#define CHECK SAFETY_CHECK
#define NT_ASSERT CHECK
#define IO_NO_INCREMENT 0
typedef struct {HANDLE Native;} KEVENT,*PKEVENT;
typedef struct {VOID *Address;} MDL;
typedef struct {LONG References;} EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct {SRWLOCK *Lock;} KLOCK_QUEUE_HANDLE;
typedef struct _TUN_CTX {SRWLOCK TransitionLock;struct {volatile LONG V2SessionActive;} Probe;} TUN_CTX;
static VOID NativeSet(KEVENT *);
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait) {CHECK(!priority&&!wait);NativeSet(event);}
static VOID KeClearEvent(KEVENT *event) {CHECK(ResetEvent(event->Native));}
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *h)
{h->Lock=(SRWLOCK *)lock;AcquireSRWLockExclusive(h->Lock);}
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *h) {ReleaseSRWLockExclusive(h->Lock);}
#ifndef PB_TERMINAL_KERNEL_SOURCE
#include "../../driver/v2/session_fault.h"
#include "../../driver/v2/session_references.h"
#include "../../driver/v2/session_drain.h"
#include "../../driver/v2/session_recycle.h"
#else
#include PB_TERMINAL_KERNEL_SOURCE
#endif
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/session_state.h"
static BOOLEAN WINAPI ObserveTry(PSRWLOCK);
static PB_RESULT ObserveArm(PB_QUEUE_VIEW *,uint32_t *);
static DWORD WINAPI ObserveWait(DWORD,const HANDLE *,BOOL,DWORD);
static BOOL WINAPI ObserveSet(HANDLE);
#define TryAcquireSRWLockShared ObserveTry
#define PbQueueArm ObserveArm
#define WaitForMultipleObjects ObserveWait
#define SetEvent ObserveSet
#include "../../api/v2/session_queue.h"
#undef TryAcquireSRWLockShared
#undef PbQueueArm
#undef WaitForMultipleObjects
#undef SetEvent
typedef struct FIXTURE FIXTURE;
typedef struct JOB {
    FIXTURE *F;ULONG Index,Stage;BOOL Receive,InsideWait;
    HANDLE Thread,Ready,Go,Reached,Continue,AfterWait,Retire,Done,External;
    PB_RESULT Result;PB_TOKEN Tx,Rx;PB_LEASE Reserved;
    volatile LONG NativeWaits,Arms;
} JOB;
struct FIXTURE {
    PB_KERNEL_SESSION *K;PB_SESSION *U;TUN_CTX *Ctx;
    KEVENT Progress,Kernel[PB_MAX_QUEUES],User[PB_MAX_QUEUES];
    JOB Jobs[PB_MAX_QUEUES];ULONG Queues;BOOL Fault,Cascade,Wrap;
    HANDLE FirstBroadcast,ContinueBroadcast;
    volatile LONG KernelUsers,KernelProgress,KernelWakes,LocalProgress,LocalClosing;
};
static __declspec(thread) JOB *ThisJob;
static FIXTURE *Current;
static volatile LONG Allocations,Handles;
static ULONG Scenarios;
static BOOL MissingBroadcast;
static VOID *Alloc(SIZE_T size)
{
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(size);memset(p,0,size);
#else
    VOID *p=calloc(1,size);
#endif
    CHECK(p);InterlockedIncrement(&Allocations);return p;
}
static VOID Free(VOID *p)
{
    CHECK(p&&InterlockedDecrement(&Allocations)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID *Region(SIZE_T size)
{
#ifdef PB_GUARD_TESTS
    return Alloc(size);
#else
    VOID *p=VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(p);InterlockedIncrement(&Allocations);return p;
#endif
}
static VOID FreeRegion(VOID *p)
{
#ifdef PB_GUARD_TESTS
    Free(p);
#else
    CHECK(p&&VirtualFree(p,0,MEM_RELEASE)&&InterlockedDecrement(&Allocations)>=0);
#endif
}
static HANDLE Event(BOOL manual,BOOL initial)
{HANDLE h=CreateEventW(NULL,manual,initial,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static HANDLE Thread(LPTHREAD_START_ROUTINE entry,VOID *argument)
{HANDLE h=CreateThread(NULL,0,entry,argument,0,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static VOID Close(HANDLE h) {CHECK(h&&CloseHandle(h)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE h) {CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static VOID Point(ULONG stage)
{
    JOB *j=ThisJob;if(!j||!j->InsideWait||j->Stage!=stage)return;
    CHECK(SetEvent(j->Reached));if(stage!=7)Await(j->Continue);
}
static BOOLEAN WINAPI ObserveTry(PSRWLOCK lock)
{if(ThisJob&&lock==&ThisJob->F->U->Lifetime)Point(1);return TryAcquireSRWLockShared(lock);}
static PB_RESULT ObserveArm(PB_QUEUE_VIEW *view,uint32_t *ready)
{
    JOB *j=ThisJob;ULONG before=0;
    if(j&&j->InsideWait){before=view==&j->F->U->Queues[j->Index].Rings[PB_QUEUE_TX_READY]?2:4;Point(before);InterlockedIncrement(&j->Arms);}
    PB_RESULT result=PbQueueArm(view,ready);if(before)Point(before+1);return result;
}
static DWORD WINAPI ObserveWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    JOB *j=ThisJob;CHECK(j&&j->InsideWait&&!all&&timeout==INFINITE&&count==(j->Receive?2u:3u));
    CHECK(events[0]==j->F->U->ClosingEvent&&events[1]==j->F->U->Queues[j->Index].UserEvent);
    if(!j->Receive)CHECK(events[2]==j->External);
    InterlockedIncrement(&j->NativeWaits);Point(6);Point(7);
    return WaitForMultipleObjects(count,events,all,timeout);
}
static BOOL WINAPI ObserveSet(HANDLE event)
{
    FIXTURE *f=Current;CHECK(f);
    if(event==f->U->ProgressEvent)InterlockedIncrement(&f->LocalProgress);
    if(event==f->U->ClosingEvent)InterlockedIncrement(&f->LocalClosing);
    return SetEvent(event);
}
static VOID NativeSet(KEVENT *event)
{
    FIXTURE *f=Current;CHECK(f);LONG user=-1;
    if(event==&f->Progress)InterlockedIncrement(&f->KernelProgress);
    for(ULONG i=0;i<f->Queues;++i){if(event==f->K->Queues[i].UserEvent){user=(LONG)i;InterlockedIncrement(&f->KernelUsers);}
        if(event==f->K->Queues[i].KernelEvent)InterlockedIncrement(&f->KernelWakes);}
    CHECK(SetEvent(event->Native));
    if(f->Cascade&&user==0){CHECK(SetEvent(f->FirstBroadcast));Await(f->ContinueBroadcast);}
}
static DWORD WINAPI Owner(VOID *argument)
{
    JOB *j=argument;ThisJob=j;FIXTURE *f=j->F;PB_USER_QUEUE *q=NULL;
    CHECK(OpenQueue(f->U,j->Index,&q)==PB_OK&&q==f->U->Queues+j->Index);
    PB_LEASE tx;uint32_t count;CHECK(ReceiveBatch(q,&tx,1,&count)==PB_OK&&count==1);j->Tx=tx.Descriptor.Token;
    CHECK(ReserveBatch(q,&j->Reserved,1,&count)==PB_OK&&count==1);j->Rx=j->Reserved.Descriptor.Token;
    CHECK(q->Loans==2&&SetEvent(j->Ready));Await(j->Go);
    j->InsideWait=TRUE;j->Result=j->Receive?Wait(q,INFINITE):PbCompatWait(q,j->External,FALSE,INFINITE);j->InsideWait=FALSE;
    CHECK(SetEvent(j->AfterWait));Await(j->Retire);
    CHECK(j->Result==PB_STOPPED&&!f->U->Fatal&&f->U->CloseRequested&&q->Loans==2&&!q->Published);
    CHECK(!f->U->ResourcesClosed&&f->U->Region&&f->K->Active==1);
    CHECK(Wait(q,INFINITE)==PB_STOPPED);
    PB_LEASE unused;count=99;CHECK(ReserveBatch(q,&unused,1,&count)==PB_STOPPED&&!count);
    count=99;CHECK(CommitBatch(q,&j->Reserved,1,&count)==PB_STOPPED&&!count);
    CHECK(CancelBatch(q,&j->Rx,1)==PB_OK&&q->Loans==1);
    count=99;CHECK(ReleaseBatch(q,&j->Tx,1,&count)==PB_OK&&count==1&&!q->Loans&&!f->U->Fatal);
    CHECK(SetEvent(j->Done));ThisJob=NULL;return 0;
}
static FIXTURE *Create(ULONG queues,ULONG stage,BOOL receive,BOOL manual,BOOL fault,BOOL wrap,BOOL cascade)
{
    CHECK(!Current&&!Allocations&&!Handles);FIXTURE *f=Alloc(sizeof(*f));Current=f;f->Queues=queues;f->Fault=fault;f->Cascade=cascade;f->Wrap=wrap;
    f->K=Alloc(sizeof(*f->K));f->U=Alloc(sizeof(*f->U));f->Ctx=Alloc(sizeof(*f->Ctx));
    PB_KERNEL_SESSION *k=f->K;PB_SESSION *u=f->U;CHECK(PbCalculateLayout(queues,4,8,&k->Layout)==PB_OK);
    k->Epoch=u->Epoch=42;u->Layout=k->Layout;k->Ctx=f->Ctx;k->Region=u->Region=Region(k->Layout.RegionSize);
    memcpy(k->Region,&k->Layout,sizeof(k->Layout));k->Lifecycle=u->SharedLifecycle=(PB_SHARED_LIFECYCLE *)(k->Region+k->Layout.LifecycleOffset);
    f->Progress.Native=u->ProgressEvent=Event(FALSE,FALSE);k->ProgressEvent=&f->Progress;
    u->ClosingEvent=Event(TRUE,FALSE);k->AllReturned.Native=Event(TRUE,TRUE);
    f->FirstBroadcast=Event(TRUE,FALSE);f->ContinueBroadcast=Event(TRUE,FALSE);f->Ctx->Probe.V2SessionActive=1;
    PbV2RetainReferences(k,1);CHECK(k->Active==1&&WaitForSingleObject(k->AllReturned.Native,0)==WAIT_TIMEOUT);
    for(ULONG i=0;i<queues;++i){
        PB_KERNEL_QUEUE *n=k->Queues+i;PB_USER_QUEUE *q=u->Queues+i;n->Session=k;n->Index=i;q->Session=u;q->Index=i;
        n->TxSlots=Alloc(4*sizeof(*n->TxSlots));q->TxSlots=Alloc(4*sizeof(*q->TxSlots));q->RxSlots=Alloc(4*sizeof(*q->RxSlots));
        CHECK(PbPoolInitialize(&n->TxPool,n->TxSlots,4,42,(USHORT)i)==PB_OK&&PbPoolInitialize(&q->Tx,q->TxSlots,4,42,(USHORT)i)==PB_OK&&PbPoolInitialize(&q->Rx,q->RxSlots,4,42,(USHORT)i)==PB_OK);
        f->User[i].Native=q->UserEvent=Event(manual,FALSE);f->Kernel[i].Native=q->KernelEvent=Event(manual,FALSE);n->UserEvent=&f->User[i];n->KernelEvent=&f->Kernel[i];
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r){BOOL userProducer=r==PB_QUEUE_TX_RECYCLE||r==PB_QUEUE_RX_SUBMIT;
            CHECK(PbAttachQueue(q->Rings+r,u->Region,&u->Layout,i,r,userProducer)==PB_OK&&PbAttachQueue(n->Rings+r,k->Region,&k->Layout,i,r,!userProducer)==PB_OK);
            uint64_t base=wrap?UINT64_MAX:0;q->Rings[r].Cursor=q->Rings[r].PeerCursor=n->Rings[r].Cursor=n->Rings[r].PeerCursor=base;
            q->Rings[r].Shared->Producer=q->Rings[r].Shared->Consumer=base;
        }
        PB_TOKEN token;CHECK(PbPoolReserve(&n->TxPool,&token)==PB_OK&&PbPoolTransition(&n->TxPool,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        PB_DESCRIPTOR d={.Token=token,.Length=20,.DataOffset=u->Layout.Queues[i].TxData+token.Slot*PB_BLOCK_SIZE};uint32_t count,notify;
        CHECK(PbQueuePush(n->Rings+PB_QUEUE_TX_READY,&d,1,&count,&notify)==PB_OK&&count==1&&!notify);
        JOB *j=f->Jobs+i;j->F=f;j->Index=i;j->Stage=stage;j->Receive=receive;
        j->Ready=Event(TRUE,FALSE);j->Go=Event(TRUE,FALSE);j->Reached=Event(TRUE,FALSE);j->Continue=Event(TRUE,FALSE);
        j->AfterWait=Event(TRUE,FALSE);j->Retire=Event(TRUE,FALSE);j->Done=Event(TRUE,FALSE);j->External=Event(FALSE,FALSE);
        j->Thread=Thread(Owner,j);
    }
    for(ULONG i=0;i<queues;++i)Await(f->Jobs[i].Ready);return f;
}
static VOID Publish(FIXTURE *f)
{
    if(f->Fault)PbV2Fault(f->K);
    else{AcquireSRWLockExclusive(&f->Ctx->TransitionLock);PbV2BeginDrain(f->K,GetTickCount64()+30000);ReleaseSRWLockExclusive(&f->Ctx->TransitionLock);PbV2Progress(f->K,TRUE);}
}
static DWORD WINAPI Publisher(VOID *argument) {Publish(argument);return 0;}
static VOID Destroy(FIXTURE *f)
{
    CHECK(f==Current);PB_KERNEL_SESSION *k=f->K;PB_SESSION *u=f->U;
    for(ULONG i=0;i<f->Queues;++i){JOB *j=f->Jobs+i;Await(j->Done);Await(j->Thread);Close(j->Thread);
        CHECK(!u->Queues[i].Loans&&!u->Queues[i].Published&&k->Queues[i].TxSlots[j->Tx.Slot].State==PB_SLOT_PUBLISHED);
        uint64_t frontier=f->Wrap?0:1;
        PB_QUEUE_VIEW *ready=u->Queues[i].Rings+PB_QUEUE_TX_READY,*recycle=u->Queues[i].Rings+PB_QUEUE_TX_RECYCLE;
        CHECK(ready->Cursor==frontier&&k->Queues[i].Rings[PB_QUEUE_TX_READY].Cursor==frontier&&
            ready->Shared->Producer==frontier&&ready->Shared->Consumer==frontier);
        CHECK(recycle->Cursor==frontier&&recycle->Shared->Producer==frontier);
        if(!f->Fault){KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&k->Queues[i].TxLock,&lock);CHECK(PbV2RecycleLocked(k->Queues+i)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);
            CHECK(k->Queues[i].TxSlots[j->Tx.Slot].State==PB_SLOT_FREE&&k->Queues[i].Rings[PB_QUEUE_TX_RECYCLE].Cursor==frontier&&recycle->Shared->Consumer==frontier);}
        else CHECK(k->Queues[i].Rings[PB_QUEUE_TX_RECYCLE].Cursor==(f->Wrap?UINT64_MAX:0)&&recycle->Shared->Consumer==(f->Wrap?UINT64_MAX:0));
        Close(j->Ready);Close(j->Go);Close(j->Reached);Close(j->Continue);Close(j->AfterWait);Close(j->Retire);Close(j->Done);Close(j->External);
    }
    CHECK(k->Active==1&&!u->ResourcesClosed&&u->Region);PbV2ReleaseReference(k);CHECK(!k->Active);Await(k->AllReturned.Native);
    CHECK(TryAcquireSRWLockExclusive(&u->Lifetime));CHECK(TryAcquireSRWLockExclusive(&u->DataGate));
    for(ULONG i=0;i<f->Queues;++i){Close(u->Queues[i].KernelEvent);Close(u->Queues[i].UserEvent);Free(u->Queues[i].TxSlots);Free(u->Queues[i].RxSlots);Free(k->Queues[i].TxSlots);}
    FreeRegion(u->Region);u->Region=NULL;u->ResourcesClosed=1;ReleaseSRWLockExclusive(&u->DataGate);ReleaseSRWLockExclusive(&u->Lifetime);
    Close(u->ClosingEvent);Close(u->ProgressEvent);Close(k->AllReturned.Native);Close(f->FirstBroadcast);Close(f->ContinueBroadcast);
    Free(k);Free(u);Free(f->Ctx);Free(f);Current=NULL;CHECK(!Allocations&&!Handles);
}
static VOID Run(ULONG queues,ULONG stage,BOOL receive,BOOL manual,BOOL fault,BOOL wrap,BOOL cascade,BOOL control)
{
    FIXTURE *f=Create(queues,stage,receive,manual,fault,wrap,cascade);HANDLE publisher=NULL;
    if(!stage)Publish(f);
    for(ULONG i=0;i<queues;++i)CHECK(SetEvent(f->Jobs[i].Go));
    if(stage){for(ULONG i=0;i<queues;++i)Await(f->Jobs[i].Reached);
        if(stage==7){PbV2Progress(f->K,FALSE);PbV2Progress(f->K,FALSE);CHECK(!f->K->Lifecycle->Terminal&&f->K->Lifecycle->Sequence==2&&!f->KernelUsers&&!f->KernelWakes&&!f->U->CloseRequested);
            for(ULONG i=0;i<queues;++i)CHECK(WaitForSingleObject(f->Jobs[i].AfterWait,0)==WAIT_TIMEOUT);}
        if(cascade){publisher=Thread(Publisher,f);Await(f->FirstBroadcast);
            for(ULONG i=0;i<queues;++i)Await(f->Jobs[i].AfterWait);
            CHECK(f->KernelUsers==1&&f->U->CloseRequested&&SetEvent(f->ContinueBroadcast));Await(publisher);Close(publisher);
        }else Publish(f);
        if(stage!=7)for(ULONG i=queues;i;--i)CHECK(SetEvent(f->Jobs[i-1].Continue));
    }
    /* Observe the missing native broadcast, rescue the infinite wait, and only
     * report the negative control after every owner/publisher and resource. */
    if(control&&f->KernelUsers==0){CHECK(stage==7&&WaitForSingleObject(f->Jobs[0].AfterWait,50)==WAIT_TIMEOUT);MissingBroadcast=TRUE;
        for(ULONG i=0;i<queues;++i)CHECK(SetEvent(f->U->Queues[i].UserEvent));}
    for(ULONG i=0;i<queues;++i)Await(f->Jobs[i].AfterWait);
    CHECK(f->K->Lifecycle->Terminal==1&&f->K->Lifecycle->Sequence==(stage==7?3u:1u));
    CHECK(f->KernelProgress==(stage==7?3:1)&&f->KernelUsers==(MissingBroadcast?0:(LONG)queues)&&f->KernelWakes==(fault?(LONG)queues:0));
    CHECK(f->U->CloseRequested&&!f->U->Fatal&&!f->U->ResourcesClosed&&f->LocalProgress==1&&f->LocalClosing>=1);
    for(ULONG i=0;i<queues;++i){JOB *j=f->Jobs+i;CHECK(j->Result==PB_STOPPED&&f->U->Queues[i].Loans==2&&!f->U->Queues[i].Published);
        CHECK(j->NativeWaits==((stage>=2)?1:0));uint32_t ready;CHECK(PbQueueArm(f->K->Queues[i].Rings+PB_QUEUE_TX_RECYCLE,&ready)==PB_OK&&!ready);}
    LONG users=f->KernelUsers,progress=f->KernelProgress,wakes=f->KernelWakes;
    if(fault){PbV2Fault(f->K);CHECK(f->KernelUsers==users&&f->KernelProgress==progress&&f->KernelWakes==wakes);}
    for(ULONG i=0;i<queues;++i)CHECK(SetEvent(f->Jobs[i].Retire));
    Destroy(f);Scenarios++;
}
int main(int argc,char **argv)
{
    if(argc==2){CHECK(!strcmp(argv[1],"1"));Run(8,7,TRUE,FALSE,TRUE,TRUE,FALSE,TRUE);
        if(MissingBroadcast){puts("TERMINAL_USER_EVENT_BROADCAST_MISSING: infinite wait rescued; all owners/publisher joined and resources freed");return 107;}
        puts("PASS: actual terminal broadcast control, eight infinite waiters and balanced resources.");return 0;}
    CHECK(argc==1);Run(1,7,TRUE,FALSE,TRUE,FALSE,FALSE,FALSE);
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Scenarios=0;
    for(ULONG pass=0;pass<2;++pass)for(ULONG qi=0;qi<3;++qi)for(ULONG reset=0;reset<2;++reset)for(ULONG fault=0;fault<2;++fault)for(ULONG receive=0;receive<2;++receive){
        ULONG queues=qi==0?1:qi==1?2:8;
        for(ULONG stage=0;stage<8;++stage){if(!receive&&(stage==2||stage==3))continue;Run(queues,stage,(BOOL)receive,(BOOL)reset,(BOOL)fault,(BOOL)pass,FALSE,FALSE);}
        Run(queues,7,(BOOL)receive,(BOOL)reset,(BOOL)fault,(BOOL)pass,TRUE,FALSE);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&!Handles&&!Allocations);
    printf("PASS: %lu measured actual SYS-to-DLL terminal schedules, shipping state, 1/2/8 queues, auto/manual events, cursor wrap, native wait/compat path, progress-only keeps waiting; all joins/handles balanced (%lu).\n",Scenarios,after);return 0;
}
