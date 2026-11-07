/* SPDX-License-Identifier: GPL-2.0 */
/* Actual unregister/free/pause and receive/return/reference helpers. Native
 * Windows locks, threads and events host the DDIs; no SYS or device is loaded. */
#include "kernel_nbl_fixture.h"
#define Executive 0
#define KernelMode 0
static PB_KERNEL_SESSION *volatile Current;
static volatile LONG Handles,Objects,Mappings,WorkerHandles,Workers,Scenarios;
static volatile LONG PauseId,UnregisterId,ReturnId,IndicationWaits,PinGateUsed;
static volatile LONG QueueWakes;
static __declspec(thread) LONG CriticalDepth,ResourceMode;
static HANDLE PauseLookup,PausePinned,PauseGate,PauseDone,PauseAtReturn;
static HANDLE UnregisterAtReturn,UnregisterAtRundown,UnregisterDone;
static HANDLE FirstReturned,NextReturn,ReturnTail,ReleaseTail;
static BOOL PauseFirst,GateTail;
static HANDLE Event(BOOL manual,BOOL signaled)
{HANDLE event=CreateEventW(NULL,manual,signaled,NULL);CHECK(event);InterlockedIncrement(&Handles);return event;}
static VOID CloseEvent(HANDLE event)
{CHECK(event&&CloseHandle(event));CHECK(InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE event) {CHECK(WaitForSingleObject(event,5000)==WAIT_OBJECT_0);}
static VOID KeEnterCriticalRegion(VOID) {CHECK(!CriticalDepth++);}
static VOID KeLeaveCriticalRegion(VOID) {CHECK(CriticalDepth--==1&&!ResourceMode);}
static VOID ExAcquireResourceSharedLite(SRWLOCK *lock,BOOLEAN wait)
{
    CHECK(CriticalDepth==1&&wait&&!ResourceMode);
    if(PauseLookup&&(LONG)GetCurrentThreadId()==PauseId) CHECK(SetEvent(PauseLookup));
    AcquireSRWLockShared(lock);ResourceMode=1;
}
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait)
{CHECK(CriticalDepth==1&&wait&&!ResourceMode);AcquireSRWLockExclusive(lock);ResourceMode=2;}
static VOID ExReleaseResourceLite(SRWLOCK *lock)
{
    CHECK(CriticalDepth==1&&ResourceMode);
    if(ResourceMode==1) ReleaseSRWLockShared(lock);else ReleaseSRWLockExclusive(lock);
    ResourceMode=0;
}
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockShared(lock);}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock) {AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL irql) {CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    CHECK(!reason&&!mode&&!alertable&&!timeout);
    if(Current&&event==&Current->AllReturned) {
        if((LONG)GetCurrentThreadId()==UnregisterId) CHECK(SetEvent(UnregisterAtReturn));
        if((LONG)GetCurrentThreadId()==PauseId) CHECK(SetEvent(PauseAtReturn));
    }
    Await(event->Native);
}
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{CHECK(!alertable&&!timeout);Await(thread);}
static VOID ZwClose(HANDLE thread)
{CloseEvent(thread);CHECK(InterlockedDecrement(&WorkerHandles)>=0);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    CHECK(Current&&refs==&Current->Returns&&SetEvent(UnregisterAtRundown));
    AcquireSRWLockExclusive(&refs->Lock);CHECK(!refs->References);ReleaseSRWLockExclusive(&refs->Lock);
}
static VOID ObDereferenceObject(KEVENT *event)
{CHECK(event);CloseEvent(event->Native);free(event);CHECK(InterlockedDecrement(&Objects)>=0);}
static VOID MmUnlockPages(MDL *mdl)
{
    CHECK(Current&&Current->Mdl==mdl&&Current->Region==mdl->Address&&Mappings==1);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(mdl->Address);
#else
    CHECK(VirtualFree(mdl->Address,0,MEM_RELEASE));
#endif
    InterlockedDecrement(&Mappings);
}
static VOID PbReportOffload(TUN_CTX *ctx)
{
    CHECK(!ctx->V2&&!ctx->Probe.V2SessionActive&&!ctx->Probe.AllowedMask);
    CHECK(TryAcquireSRWLockExclusive(&ctx->TransitionLock));ReleaseSRWLockExclusive(&ctx->TransitionLock);
}
static VOID PbWaitIndications(TUN_CTX *ctx,ULONG bank)
{
    CHECK(bank<2);InterlockedIncrement(&IndicationWaits);
    CHECK(TryAcquireSRWLockExclusive(&ctx->TransitionLock));ReleaseSRWLockExclusive(&ctx->TransitionLock);
    if(PauseFirst&&!bank&&!InterlockedExchange(&PinGateUsed,1)) {
        CHECK(Current&&Current->Returns.References==1&&SetEvent(PausePinned));Await(PauseGate);
    }
}
#include "../../driver/v2/session_cleanup.h"
#ifndef PB_UNREGISTER_SOURCE
#define PB_UNREGISTER_SOURCE "../../driver/v2/session_unregister.h"
#endif
#include PB_UNREGISTER_SOURCE
#ifndef PB_PAUSE_SOURCE
#define PB_PAUSE_SOURCE "../../driver/v2/session_pause.h"
#endif
#include PB_PAUSE_SOURCE
static VOID BeforeFree(VOID *memory)
{
    if(memory!=Current) return;
    CHECK(!Pools&&!Mdls&&!Nbls&&!Objects&&!Mappings&&!WorkerHandles&&!Workers);
    CloseEvent(Current->StartEvent.Native);CloseEvent(Current->StopEvent.Native);CloseEvent(Current->AllReturned.Native);
    Current=NULL;
}
static VOID ReturnRelease(EX_RUNDOWN_REF *refs)
{
    if(GateTail&&(LONG)GetCurrentThreadId()==ReturnId&&Current&&refs==&Current->Returns&&!Current->Active) {
        CHECK(SetEvent(ReturnTail));Await(ReleaseTail);
    }
}
static VOID CountWake(KEVENT *event)
{
    if(Current) for(ULONG i=0;i<Current->Layout.QueueCount;++i)
        if(event==Current->Queues[i].KernelEvent) InterlockedIncrement(&QueueWakes);
}
static KEVENT *ObjectEvent(BOOL manual)
{
    KEVENT *event=calloc(1,sizeof(*event));CHECK(event);event->Native=Event(manual,FALSE);
    InterlockedIncrement(&Objects);return event;
}
static DWORD WINAPI Worker(VOID *opaque)
{
    PB_KERNEL_SESSION *s=opaque;Await(s->StopEvent.Native);CHECK(s->Stopping);
    CHECK(InterlockedDecrement(&Workers)>=0);return 0;
}
static PB_KERNEL_SESSION *Create(TUN_CTX *ctx,ULONG queues,BOOL manual,NET_BUFFER_LIST **lists)
{
    CHECK(!Current&&!Pools&&!Mdls&&!Nbls&&!Objects&&!Mappings&&!WorkerHandles&&!Workers);
    PB_KERNEL_SESSION *s=ExAllocatePoolZero(NonPagedPool,sizeof(*s),TUN_MEMORY_TAG);CHECK(s);Current=s;
    s->Ctx=ctx;s->Epoch=42;ctx->V2=s;ctx->Running=1;ctx->NblPool=ctx->MiniportAdapterHandle=ctx;
    ctx->Probe.V2SessionActive=1;ctx->Probe.AllowedMask=1;
    ctx->Device.Disconnected.Native=Event(TRUE,FALSE);
    CHECK(PbCalculateLayout(queues,4,8,&s->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    s->Region=PbTestGuardAlloc(s->Layout.RegionSize);memset(s->Region,0,s->Layout.RegionSize);
#else
    s->Region=VirtualAlloc(NULL,s->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
    CHECK(s->Region);InterlockedIncrement(&Mappings);
    s->Lifecycle=(PB_SHARED_LIFECYCLE *)(s->Region+s->Layout.LifecycleOffset);
    s->Mdl=IoAllocateMdl(s->Region,s->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(s->Mdl);s->PagesLocked=TRUE;
    s->StartEvent.Native=Event(TRUE,TRUE);s->StopEvent.Native=Event(TRUE,FALSE);s->AllReturned.Native=Event(TRUE,TRUE);
    s->ProgressEvent=ObjectEvent(manual);NET_BUFFER_LIST **tail=lists;
    for(ULONG i=0;i<queues;++i) {
        PB_KERNEL_QUEUE *q=s->Queues+i;q->Session=s;q->Index=i;
        q->KernelEvent=ObjectEvent(manual);q->UserEvent=ObjectEvent(manual);
        q->RxSlots=ExAllocatePoolZero(NonPagedPool,4*sizeof(*q->RxSlots),TUN_MEMORY_TAG);
        q->Pending=ExAllocatePoolZero(NonPagedPool,8*sizeof(*q->Pending),TUN_MEMORY_TAG);CHECK(q->RxSlots&&q->Pending);
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_RX_COMPLETE,s->Region,&s->Layout,i,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
        PB_DESCRIPTOR d={.Token={42,1,0,(USHORT)i},.Length=20,.DataOffset=s->Layout.Queues[i].RxData};uint32_t complete;
        CHECK(PbAdmitReceive(&s->Layout,s->Epoch,i,q->RxSlots,&d,0,&complete)==PB_OK);q->Credits=1;s->Region[d.DataOffset]=0x45;
        NET_BUFFER_LIST *nbl=PbV2Receive(q,&d,1,MAXULONG,NULL);CHECK(nbl);PbV2RetainReceives(s,1,1);
        *tail=nbl;tail=&nbl->Next;InterlockedIncrement(&Workers);
        q->Thread=CreateThread(NULL,0,Worker,s,0,NULL);CHECK(q->Thread);InterlockedIncrement(&Handles);InterlockedIncrement(&WorkerHandles);
    }
    CHECK(s->Active==(LONG)queues&&WaitForSingleObject(s->AllReturned.Native,0)==WAIT_TIMEOUT);return s;
}
static DWORD WINAPI PauseThread(VOID *opaque)
{
    TUN_CTX *ctx=opaque;PauseId=(LONG)GetCurrentThreadId();
    KIRQL irql=ExAcquireSpinLockExclusive(&ctx->TransitionLock);WriteRelease(&ctx->Running,0);ExReleaseSpinLockExclusive(&ctx->TransitionLock,irql);
    PbV2Pause(ctx);CHECK(!CriticalDepth&&!ResourceMode&&SetEvent(PauseDone));return 0;
}
static DWORD WINAPI UnregisterThread(VOID *opaque)
{
    TUN_CTX *ctx=opaque;UnregisterId=(LONG)GetCurrentThreadId();KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&ctx->Device.RegistrationLock,TRUE);PbV2Unregister(ctx);
    ExReleaseResourceLite(&ctx->Device.RegistrationLock);KeLeaveCriticalRegion();CHECK(SetEvent(UnregisterDone));return 0;
}
typedef struct RETURN_JOB {TUN_CTX *Ctx;NET_BUFFER_LIST *Lists;} RETURN_JOB;
static DWORD WINAPI ReturnThread(VOID *opaque)
{
    RETURN_JOB *job=opaque;ReturnId=(LONG)GetCurrentThreadId();NET_BUFFER_LIST *head=job->Lists;
    while(head) {
        CHECK(PbV2ReturnNbls(job->Ctx,&head,PB_MAX_BATCH));
        if(head) {CHECK(Current&&Current->Active==1&&SetEvent(FirstReturned));Await(NextReturn);}
    }
    return 0;
}
static HANDLE Thread(LPTHREAD_START_ROUTINE run,VOID *opaque)
{HANDLE thread=CreateThread(NULL,0,run,opaque,0,NULL);CHECK(thread);InterlockedIncrement(&Handles);return thread;}
static VOID Join(HANDLE thread) {Await(thread);CloseEvent(thread);}
static BOOL Run(BOOL pauseFirst,BOOL tail,ULONG queues,BOOL manual)
{
    PauseFirst=pauseFirst;GateTail=tail;PauseId=UnregisterId=ReturnId=IndicationWaits=PinGateUsed=QueueWakes=0;
    PauseLookup=Event(TRUE,FALSE);PausePinned=Event(TRUE,FALSE);PauseGate=Event(TRUE,FALSE);PauseDone=Event(TRUE,FALSE);PauseAtReturn=Event(TRUE,FALSE);
    UnregisterAtReturn=Event(TRUE,FALSE);UnregisterAtRundown=Event(TRUE,FALSE);UnregisterDone=Event(TRUE,FALSE);
    FirstReturned=Event(TRUE,FALSE);NextReturn=Event(TRUE,FALSE);ReturnTail=Event(TRUE,FALSE);ReleaseTail=Event(TRUE,FALSE);
    TUN_CTX ctx={0};NET_BUFFER_LIST *lists=NULL;PB_KERNEL_SESSION *s=Create(&ctx,queues,manual,&lists);
    BeforePoolFree=BeforeFree;BeforeRundownRelease=ReturnRelease;OnSetEvent=CountWake;
    HANDLE pause,unregister;BOOL premature=FALSE;
    if(pauseFirst) {
        pause=Thread(PauseThread,&ctx);Await(PausePinned);CHECK(s->Returns.References==1);
        CHECK(QueueWakes==(LONG)queues);
        for(ULONG i=0;i<queues;++i) {
            HANDLE event=s->Queues[i].KernelEvent->Native;CHECK(WaitForSingleObject(event,0)==WAIT_OBJECT_0);
            CHECK(WaitForSingleObject(event,0)==(manual?WAIT_OBJECT_0:WAIT_TIMEOUT));
        }
        unregister=Thread(UnregisterThread,&ctx);Await(UnregisterAtReturn);
        /* A long-held shared registration lock would deadlock at this point.
         * The existing Returns pin alone keeps the detached snapshot alive. */
        CHECK(!ctx.V2&&s->Returns.References==1&&s->Active==(LONG)queues&&WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(PauseGate));Await(PauseAtReturn);
    } else {
        unregister=Thread(UnregisterThread,&ctx);Await(UnregisterAtReturn);CHECK(!ctx.V2&&s->Active==(LONG)queues);
        pause=Thread(PauseThread,&ctx);HANDLE observed[]={PauseLookup,PauseDone};
        DWORD waited=WaitForMultipleObjects(2,observed,FALSE,5000);CHECK(waited==WAIT_OBJECT_0||waited==WAIT_OBJECT_0+1);
        premature=waited==WAIT_OBJECT_0+1;
        if(!premature) CHECK(WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
    }
    CHECK(Current==s&&Mappings==1&&Nbls==(LONG)queues&&WaitForSingleObject(UnregisterDone,0)==WAIT_TIMEOUT);
    RETURN_JOB job={&ctx,lists};HANDLE returned=Thread(ReturnThread,&job);
    if(queues==2) {
        Await(FirstReturned);CHECK(Current==s&&s->Active==1&&Mappings==1&&Nbls==1&&WaitForSingleObject(UnregisterDone,0)==WAIT_TIMEOUT);
        if(!premature) CHECK(WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(NextReturn));
    }
    if(tail) {
        Await(ReturnTail);Await(UnregisterAtRundown);
        CHECK(Current==s&&!s->Active&&!Nbls&&Mappings==1&&s->Returns.References>=1&&WaitForSingleObject(UnregisterDone,0)==WAIT_TIMEOUT);
        if(pauseFirst) Await(PauseDone);else if(!premature) CHECK(WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(ReleaseTail));
    }
    Join(returned);Join(unregister);Join(pause);
    CHECK(!Current&&!ctx.V2&&!Pools&&!Mdls&&!Nbls&&!Objects&&!Mappings&&!WorkerHandles&&!Workers&&IndicationWaits==2);
    CHECK(WaitForSingleObject(ctx.Device.Disconnected.Native,0)==WAIT_OBJECT_0);CloseEvent(ctx.Device.Disconnected.Native);
    HANDLE events[]={PauseLookup,PausePinned,PauseGate,PauseDone,PauseAtReturn,UnregisterAtReturn,UnregisterAtRundown,UnregisterDone,FirstReturned,NextReturn,ReturnTail,ReleaseTail};
    for(ULONG i=0;i<_countof(events);++i) CloseEvent(events[i]);
    PauseLookup=PausePinned=PauseGate=PauseDone=PauseAtReturn=NULL;
    UnregisterAtReturn=UnregisterAtRundown=UnregisterDone=FirstReturned=NextReturn=ReturnTail=ReleaseTail=NULL;
    BeforePoolFree=NULL;BeforeRundownRelease=NULL;OnSetEvent=NULL;
    CHECK(QueueWakes==(pauseFirst?(LONG)queues:0));CHECK(!Handles);InterlockedIncrement(&Scenarios);return premature;
}
int main(int argc,char **argv)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(argc==2) {
        CHECK(!strcmp(argv[1],"1"));BOOL premature=Run(FALSE,TRUE,2,FALSE);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
        if(premature) {puts("PAUSE_COMPLETED_BEFORE_RETIRED_RECEIVE_RETURN");return 86;}
        puts("PASS: detached session pause waits for real NBL return, callback tail and unregister exit");return 0;
    }
    CHECK(argc==1);
    for(ULONG repeat=0;repeat<3;++repeat) for(ULONG first=0;first<2;++first) for(ULONG tail=0;tail<2;++tail)
        for(ULONG queues=1;queues<=2;++queues) for(ULONG manual=0;manual<2;++manual)
            CHECK(!Run((BOOL)first,(BOOL)tail,queues,(BOOL)manual));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
    printf("PASS: %ld actual pause/unregister schedules; short registration snapshot, retained NBLs, two queues, manual/auto queue events, return rundown tails, joined workers/callers and zero resource/handle residue\n",Scenarios);return 0;
}
