/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL queue/lifecycle paths. Observe validated pool transitions and real
 * kernel-event calls without replacing their behavior. No driver is loaded. */
#define PB_API_TESTS 1
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
typedef struct TRANSITION {PB_TOKEN Token;ULONG Before,After;} TRANSITION;
static TRANSITION Trace[2*PB_MAX_BATCH];
static PB_POOL *WatchedPool;
static HANDLE WatchedKernel,TransitionEntered,TransitionContinue;
static volatile LONG Recording,Calls,Notifications,GuardPools,GuardRegions;
static volatile LONG OwnedHandles,OwnedPools,OwnedWorks;
static PB_RESULT TrackedTransition(PB_POOL *,const PB_TOKEN *,uint32_t,uint32_t);
static BOOL WINAPI TrackedEvent(HANDLE);
static HANDLE WINAPI TrackedCreateEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(attributes,manual,initial,name);CHECK(event);InterlockedIncrement(&OwnedHandles);return event;}
static BOOL WINAPI TrackedClose(HANDLE event)
{BOOL result=CloseHandle(event);CHECK(result && InterlockedDecrement(&OwnedHandles)>=0);return result;}
static PTP_POOL WINAPI TrackedPool(PVOID reserved)
{PTP_POOL pool=CreateThreadpool(reserved);CHECK(pool);InterlockedIncrement(&OwnedPools);return pool;}
static VOID WINAPI TrackedPoolClose(PTP_POOL pool)
{CloseThreadpool(pool);CHECK(InterlockedDecrement(&OwnedPools)>=0);}
static PTP_WORK WINAPI TrackedWork(PTP_WORK_CALLBACK callback,PVOID context,PTP_CALLBACK_ENVIRON environment)
{PTP_WORK work=CreateThreadpoolWork(callback,context,environment);CHECK(work);InterlockedIncrement(&OwnedWorks);return work;}
static VOID WINAPI TrackedWorkClose(PTP_WORK work)
{CloseThreadpoolWork(work);CHECK(InterlockedDecrement(&OwnedWorks)>=0);}
#ifdef PB_GUARD_TESTS
static VOID *GuardCalloc(SIZE_T count,SIZE_T size)
{CHECK(!size || count<=SIZE_MAX/size);VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);InterlockedIncrement(&GuardPools);return p;}
static VOID GuardFree(VOID *p) {if(p){PbTestGuardFree(p);CHECK(InterlockedDecrement(&GuardPools)>=0);}}
static VOID *WINAPI GuardRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{CHECK(!address && type==(MEM_RESERVE|MEM_COMMIT) && protection==PAGE_READWRITE);VOID *p=PbTestGuardAlloc(size);memset(p,0,size);InterlockedIncrement(&GuardRegions);return p;}
static BOOL WINAPI GuardUnmap(VOID *address,SIZE_T size,DWORD type)
{CHECK(!size && type==MEM_RELEASE);PbTestGuardFree(address);CHECK(InterlockedDecrement(&GuardRegions)>=0);return TRUE;}
#define calloc GuardCalloc
#define free GuardFree
#define VirtualAlloc GuardRegion
#define VirtualFree GuardUnmap
#endif
#define PbPoolTransition TrackedTransition
#define SetEvent TrackedEvent
#define CreateEventW TrackedCreateEvent
#define CloseHandle TrackedClose
#define CreateThreadpool TrackedPool
#define CloseThreadpool TrackedPoolClose
#define CreateThreadpoolWork TrackedWork
#define CloseThreadpoolWork TrackedWorkClose
#ifndef PB_COMPLETION_SESSION_SOURCE
#define PB_COMPLETION_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_COMPLETION_SESSION_SOURCE
#undef PbPoolTransition
#undef SetEvent
#undef CreateEventW
#undef CloseHandle
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
static PB_RESULT TrackedTransition(PB_POOL *p,const PB_TOKEN *t,uint32_t before,uint32_t after)
{
    BOOLEAN record=ReadAcquire(&Recording) && p==WatchedPool;
    LONG index=record?InterlockedIncrement(&Calls)-1:0;
    if(record) {CHECK(index>=0 && index<2*PB_MAX_BATCH && p->Slots[t->Slot].State==before);Trace[index]=(TRANSITION){*t,before,after};}
    PB_RESULT result=PbPoolTransition(p,t,before,after);
    if(record) {
        CHECK(result==PB_OK && p->Slots[t->Slot].State==after);
        if(!index && TransitionEntered) {CHECK(SetEvent(TransitionEntered) && WaitForSingleObject(TransitionContinue,5000)==WAIT_OBJECT_0);}
    }
    return result;
}
static BOOL WINAPI TrackedEvent(HANDLE event)
{if(ReadAcquire(&Recording) && event==WatchedKernel) InterlockedIncrement(&Notifications);return SetEvent(event);}
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
typedef struct FIXTURE {PB_SESSION *S;PB_USER_QUEUE *Q;PB_QUEUE_VIEW Submit,Complete;PB_DESCRIPTOR Items[PB_MAX_BATCH];VOID *Region;PB_LAYOUT Layout;uint64_t Epoch;ULONG Count;} FIXTURE;
static VOID WaitCleanup(VOID)
{
    ULONGLONG deadline=GetTickCount64()+5000;
    for(;;) {PB_RUNTIME_STATUS status;CHECK(Life.GetRuntimeStatus(&status)==PB_OK);
        if(!status.Sessions && !ReadAcquire(&GuardPools) && !ReadAcquire(&GuardRegions) &&
            !ReadAcquire(&OwnedHandles) && !ReadAcquire(&OwnedPools) && !ReadAcquire(&OwnedWorks)) break;
        CHECK(GetTickCount64()<deadline);SwitchToThread();}
}
static VOID CloseFixture(FIXTURE *f)
{
    /* Retain only the test shell so its work object can be joined after public
     * Close. Resources still pass through the actual close/lifetime barriers. */
    Retain(f->S);CHECK(Api.Close(f->S,2000)==PB_OK && f->S->ResourcesClosed);
    WaitForThreadpoolWorkCallbacks(f->S->Work,FALSE);DropReference(f->S);WaitCleanup();
}
static FIXTURE CreateFixture(ULONG count,BOOLEAN wrap,BOOLEAN extra)
{
    FIXTURE f={0};f.Count=count;PB_CONFIG config={sizeof(config),1,count+extra,64,0,0};
    CHECK(PbTestOpen(&config,&f.S,&f.Region,&f.Layout,&f.Epoch)==PB_OK && Api.OpenQueue(f.S,0,&f.Q)==PB_OK);
    CHECK(PbAttachQueue(&f.Submit,f.Region,&f.Layout,0,PB_QUEUE_RX_SUBMIT,0)==PB_OK);
    CHECK(PbAttachQueue(&f.Complete,f.Region,&f.Layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
    if(wrap) {
        uint64_t base=UINT64_MAX-15;
        PB_QUEUE_VIEW *views[]={&f.Submit,&f.Complete,f.Q->Rings+PB_QUEUE_RX_SUBMIT,f.Q->Rings+PB_QUEUE_RX_COMPLETE};
        for(ULONG i=0;i<4;++i) {views[i]->Cursor=views[i]->PeerCursor=base;views[i]->Shared->Producer=views[i]->Shared->Consumer=base;}
    }
    PB_LEASE leases[PB_MAX_BATCH];uint32_t n;CHECK(Api.ReserveBatch(f.Q,leases,count,&n)==PB_OK && n==count);
    for(ULONG i=0;i<count;++i) leases[i].Descriptor.Length=20;
    CHECK(Api.CommitBatch(f.Q,leases,count,&n)==PB_OK && n==count);
    CHECK(PbQueuePop(&f.Submit,f.Items,count,&n)==PB_OK && n==count);PbTestActive(f.S,count);
    return f;
}
static VOID PublishCompletion(FIXTURE *f,const PB_DESCRIPTOR *items)
{uint32_t n,notify;CHECK(PbQueuePush(&f->Complete,items,f->Count,&n,&notify)==PB_OK && n==f->Count);}
static VOID StartRecording(FIXTURE *f)
{WatchedPool=&f->Q->Rx;WatchedKernel=f->Q->KernelEvent;Calls=Notifications=0;memset(Trace,0,sizeof(Trace));InterlockedExchange(&Recording,1);}
static VOID CheckTrace(ULONG count)
{
#ifdef PB_COMPLETION_RETIRE_BASELINE
    CHECK(Calls==(LONG)(2*count));
    for(ULONG i=0;i<count;++i) CHECK(!memcmp(&Trace[2*i].Token,&Trace[2*i+1].Token,sizeof(PB_TOKEN)) &&
        Trace[2*i].Before==PB_SLOT_PUBLISHED && Trace[2*i].After==PB_SLOT_COMPLETE &&
        Trace[2*i+1].Before==PB_SLOT_COMPLETE && Trace[2*i+1].After==PB_SLOT_FREE);
#else
    CHECK(Calls==(LONG)count);
    for(ULONG i=0;i<count;++i) CHECK(Trace[i].Before==PB_SLOT_PUBLISHED && Trace[i].After==PB_SLOT_FREE);
#endif
    CHECK(Notifications==1);
}
static VOID Success(ULONG count,ULONG capacity,BOOLEAN wrap)
{
    FIXTURE f=CreateFixture(count,wrap,FALSE);PB_DESCRIPTOR complete[PB_MAX_BATCH];const ULONG status[]={PB_OK,PB_INVALID,PB_RX_REJECTED,PB_STOPPED};
    for(ULONG i=0;i<count;++i) {complete[i]=f.Items[count-1-i];complete[i].Status=status[i%4];}
    PB_LEASE leases[PB_MAX_BATCH];uint32_t n;CHECK(Api.ReserveBatch(f.Q,leases,1,&n)==PB_FULL && !n);
    PublishCompletion(&f,complete);PbTestActive(f.S,0);
    PB_TEST_GUARD memory=PbTestGuardCreate(capacity*sizeof(PB_DESCRIPTOR));PB_DESCRIPTOR *output=(PB_DESCRIPTOR *)PbTestGuardSpan(&memory,capacity*sizeof(*output));
    ULONG done=0,totalCalls=0;ULONG64 seen=0;
    while(done<count) {
        StartRecording(&f);CHECK(Api.PollCompletions(f.Q,output,capacity,&n)==PB_OK && n==min(capacity,count-done));
        InterlockedExchange(&Recording,0);CheckTrace(n);totalCalls+=Calls;
        CHECK(!memcmp(output,complete+done,n*sizeof(*output)) && !f.S->Fatal);
        for(ULONG i=0;i<n;++i) {ULONG slot=output[i].Token.Slot;CHECK(!(seen&(1ull<<slot)));seen|=1ull<<slot;}
        done+=n;CHECK(f.Q->Published==(LONG)(count-done) && !f.Q->Loans);
        for(ULONG i=0;i<count;++i) {
            BOOLEAN retired=(seen&(1ull<<i))!=0;
            CHECK(f.Q->RxSlots[i].State==(retired?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
            CHECK(!!(f.Q->Rx.FreeBits[i/32]&(1u<<(i&31)))==retired);
        }
        CHECK(Api.ReserveBatch(f.Q,leases,count,&n)==PB_OK && n==done);
        PB_TOKEN tokens[PB_MAX_BATCH];for(ULONG i=0;i<n;++i) {tokens[i]=leases[i].Descriptor.Token;CHECK((seen&(1ull<<tokens[i].Slot)) && tokens[i].Generation>1);}
        CHECK(Api.CancelBatch(f.Q,tokens,n)==PB_OK);
    }
    StartRecording(&f);CHECK(Api.PollCompletions(f.Q,output,capacity,&n)==PB_EMPTY && !n);InterlockedExchange(&Recording,0);
    CHECK(!Calls && !Notifications && !f.Q->Published && !f.Q->Loans);
    PbTestGuardDestroy(&memory);CloseFixture(&f);
    printf("PASS: actual completion retirement count=%lu capacity=%lu wrap=%u transition_calls=%lu; exact bytes/status/order, prefix reuse, generations/index and no empty-poll wake.\n",count,capacity,wrap,totalCalls);
}
static VOID InvalidCompletion(ULONG count,ULONG position,ULONG fault)
{
    FIXTURE f=CreateFixture(count,FALSE,(BOOLEAN)(fault==4));PB_DESCRIPTOR complete[PB_MAX_BATCH];memcpy(complete,f.Items,count*sizeof(*complete));
    PB_LEASE extra;uint32_t n;
    if(fault==0) complete[position].Token.Epoch++;
    if(fault==1) complete[position].Token.Generation++;
    if(fault==2) complete[position?position:1]=complete[0];
    if(fault==3) complete[position].Token.Slot=(USHORT)f.Layout.SlotCount;
    if(fault==4) {CHECK(Api.ReserveBatch(f.Q,&extra,1,&n)==PB_OK && n==1);complete[position]=extra.Descriptor;}
    PB_POOL saved=f.Q->Rx;PB_SLOT slots[PB_MAX_BATCH+1];memcpy(slots,f.Q->RxSlots,f.Layout.SlotCount*sizeof(*slots));
    PublishCompletion(&f,complete);PB_DESCRIPTOR output[PB_MAX_BATCH];StartRecording(&f);
    CHECK(Api.PollCompletions(f.Q,output,count,&n)==PB_CORRUPT && !n);InterlockedExchange(&Recording,0);
#ifdef PB_COMPLETION_RETIRE_BASELINE
    CHECK(!Notifications);
#else
    CHECK(Notifications==1); /* The failed batch still released completion-ring space. */
#endif
    CHECK(!Calls && !memcmp(&saved,&f.Q->Rx,sizeof(saved)) && !memcmp(slots,f.Q->RxSlots,f.Layout.SlotCount*sizeof(*slots)));
    CHECK(f.Q->Published==(LONG)count && f.S->Fatal && !f.S->ResourcesClosed);
    if(fault==4) {CHECK(Api.PrepareClose(f.S,0)==PB_BUSY && Api.CancelBatch(f.Q,&extra.Descriptor.Token,1)==PB_OK);}
    CHECK(Api.PrepareClose(f.S,10)==PB_QUARANTINED && !f.S->ResourcesClosed && f.S->Region==f.Region);
    PbTestActive(f.S,0);CHECK(Api.PrepareClose(f.S,2000)==PB_OK && !f.Q->Published && !f.S->ResourcesClosed);
    for(ULONG i=0;i<f.Layout.SlotCount;++i) CHECK(f.Q->RxSlots[i].State==PB_SLOT_FREE);
    CloseFixture(&f);
    printf("PASS: invalid completion count=%lu position=%lu fault=%lu: zero partial transitions, space wakes=%ld, exact pool rollback and native/user barriers retained.\n",count,position,fault,Notifications);
}
typedef struct CLOSE_JOB {FIXTURE *F;PB_RESULT Result;} CLOSE_JOB;
static DWORD WINAPI PrepareDuringPoll(VOID *opaque)
{
    CLOSE_JOB *job=opaque;CHECK(WaitForSingleObject(TransitionEntered,5000)==WAIT_OBJECT_0);
    job->Result=Api.PrepareClose(job->F->S,0);PB_CLOSE_PROGRESS progress;CHECK(Life.GetCloseProgress(job->F->S,&progress)==PB_OK);
    CHECK(job->Result==PB_QUARANTINED && progress.Phase<PB_LIFE_PREPARED && !job->F->S->ResourcesClosed && job->F->Q->Published==32);
    CHECK(SetEvent(TransitionContinue));return 0;
}
static VOID ConcurrentPrepare(VOID)
{
    FIXTURE f=CreateFixture(32,TRUE,FALSE);PublishCompletion(&f,f.Items);PbTestActive(f.S,0);
    TransitionEntered=CreateEventW(NULL,TRUE,FALSE,NULL);TransitionContinue=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(TransitionEntered && TransitionContinue);
    CLOSE_JOB job={&f,PB_OK};HANDLE thread=CreateThread(NULL,0,PrepareDuringPoll,&job,0,NULL);CHECK(thread);
    PB_DESCRIPTOR output[PB_MAX_BATCH];uint32_t n;StartRecording(&f);CHECK(Api.PollCompletions(f.Q,output,32,&n)==PB_OK && n==32);
    InterlockedExchange(&Recording,0);CheckTrace(32);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));
    CHECK(CloseHandle(TransitionEntered) && CloseHandle(TransitionContinue));TransitionEntered=TransitionContinue=NULL;
    CHECK(Api.PrepareClose(f.S,2000)==PB_OK && !f.Q->Published && !f.S->ResourcesClosed);CloseFixture(&f);
    puts("PASS: actual PrepareClose at a partial retirement cannot observe/retire the gated pool or release its mapping; poll finishes before native preparation.");
}
typedef struct OWNER_JOB {PB_USER_QUEUE *Q;PB_RESULT Result;uint32_t Count;} OWNER_JOB;
static DWORD WINAPI WrongOwnerPoll(VOID *opaque)
{OWNER_JOB *job=opaque;PB_DESCRIPTOR output[PB_MAX_BATCH];job->Result=Api.PollCompletions(job->Q,output,PB_MAX_BATCH,&job->Count);return 0;}
static VOID OwnerAndGeneration(VOID)
{
    FIXTURE f=CreateFixture(2,FALSE,FALSE);f.Q->RxSlots[0].Generation=UINT32_MAX;f.Items[0].Token.Generation=UINT32_MAX;
    PublishCompletion(&f,f.Items);PbTestActive(f.S,0);uint64_t cursor=f.Q->Rings[PB_QUEUE_RX_COMPLETE].Cursor;
    OWNER_JOB job={f.Q,PB_OK,99};StartRecording(&f);HANDLE thread=CreateThread(NULL,0,WrongOwnerPoll,&job,0,NULL);
    CHECK(thread && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));InterlockedExchange(&Recording,0);
    CHECK(job.Result==PB_BUSY && !job.Count && !Calls && !Notifications && f.Q->Rings[PB_QUEUE_RX_COMPLETE].Cursor==cursor && f.Q->Published==2);
    PB_DESCRIPTOR output[2];uint32_t n;StartRecording(&f);CHECK(Api.PollCompletions(f.Q,output,2,&n)==PB_OK && n==2);InterlockedExchange(&Recording,0);CheckTrace(2);
    CHECK(output[0].Token.Generation==UINT32_MAX && f.Q->RxSlots[0].State==PB_SLOT_FREE && f.Q->RxSlots[0].Generation==UINT32_MAX);
    PB_LEASE leases[2];CHECK(Api.ReserveBatch(f.Q,leases,2,&n)==PB_OK && n==1 && leases[0].Descriptor.Token.Slot==1 && leases[0].Descriptor.Token.Generation==2);
    CHECK(f.Q->RxSlots[0].State==PB_SLOT_RETIRED && !(f.Q->Rx.FreeBits[0]&1) && Api.CancelBatch(f.Q,&leases[0].Descriptor.Token,1)==PB_OK);
    CloseFixture(&f);puts("PASS: wrong-owner poll cannot consume or mutate; a completed UINT32_MAX token retires permanently on reserve without generation wrap.");
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    Success(1,1,FALSE);
    const ULONG counts[]={1,2,4,8,16,32};
    for(ULONG i=0;i<_countof(counts);++i) for(ULONG wrap=0;wrap<2;++wrap) {Success(counts[i],1,(BOOLEAN)wrap);Success(counts[i],counts[i],(BOOLEAN)wrap);}
    const ULONG invalidCounts[]={2,8,32};
    for(ULONG i=0;i<_countof(invalidCounts);++i) for(ULONG place=0;place<3;++place) for(ULONG fault=0;fault<5;++fault)
        InvalidCompletion(invalidCounts[i],place==0?0:place==1?invalidCounts[i]/2:invalidCounts[i]-1,fault);
    ConcurrentPrepare();OwnerAndGeneration();WaitCleanup();
    puts("PASS: all session-owned device/events, work/pool objects, admission and guarded allocations reclaimed exactly; native Windows pool housekeeping is not a session-owned handle.");
    return 0;
}
