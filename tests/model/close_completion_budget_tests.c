/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL/core, real Windows thread pool/events. Native QUERY/ownership are
 * hosted; no SYS is installed or loaded. Frozen/current lifecycle A/B fixture. */
#define PB_API_TESTS 1
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
#define CLOSE_BUDGET (4*PB_MAX_BATCH)
static PB_RESULT ObservedPop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t *);
static DWORD WINAPI ObservedWait(HANDLE,DWORD);
static volatile LONG GuardAllocations;
#ifdef PB_GUARD_TESTS
static VOID *GuardCalloc(SIZE_T count,SIZE_T size)
{CHECK(!size || count<=SIZE_MAX/size);VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);InterlockedIncrement(&GuardAllocations);return p;}
static VOID GuardFree(VOID *p)
{if(p){PbTestGuardFree(p);CHECK(InterlockedDecrement(&GuardAllocations)>=0);}}
static VOID *WINAPI GuardRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{CHECK(!address && size<=PB_MAX_REGION && type==(MEM_RESERVE|MEM_COMMIT) && protection==PAGE_READWRITE);return GuardCalloc(1,size);}
static BOOL WINAPI GuardUnmap(VOID *p,SIZE_T size,DWORD type)
{CHECK(!size && type==MEM_RELEASE);GuardFree(p);return TRUE;}
#define calloc GuardCalloc
#define free GuardFree
#define VirtualAlloc GuardRegion
#define VirtualFree GuardUnmap
#endif
#define PbQueuePop ObservedPop
#define WaitForSingleObject ObservedWait
#ifndef PB_CLOSE_COMPLETION_SESSION_SOURCE
#define PB_CLOSE_COMPLETION_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_CLOSE_COMPLETION_SESSION_SOURCE
#undef PbQueuePop
#undef WaitForSingleObject
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_SESSION *Current;
static uint64_t Base;
static ULONG Calls[PB_MAX_QUEUES],Popped[PB_MAX_QUEUES],Turns,Timeouts;
static HANDLE WaitEntered,WaitGate;
static BOOL Gated;
static PB_RESULT ObservedPop(PB_QUEUE_VIEW *view,PB_DESCRIPTOR *items,uint32_t capacity,uint32_t *count)
{
    PB_RESULT result=PbQueuePop(view,items,capacity,count);
    if(Current && GetCurrentThreadId()==PbTestBoundaryThread(Current,PB_TEST_WORKER_START)) for(ULONG q=0;q<Current->Layout.QueueCount;++q)
        if(view==&Current->Queues[q].Rings[PB_QUEUE_RX_COMPLETE]) {
            CHECK(capacity==PB_MAX_BATCH && ++Calls[q]<=4);
            Popped[q]+=*count;CHECK(Popped[q]<=4*PB_MAX_BATCH);
        }
    return result;
}
static DWORD WINAPI ObservedWait(HANDLE event,DWORD timeout)
{
    if(Current && event==Current->ProgressEvent && timeout==1000) {
        ++Turns;
        for(ULONG q=0;q<Current->Layout.QueueCount;++q) {CHECK(Calls[q]<=4 && Popped[q]<=CLOSE_BUDGET);Calls[q]=Popped[q]=0;}
        if(WaitEntered && !Gated) {Gated=TRUE;CHECK(SetEvent(WaitEntered) && WaitForSingleObject(WaitGate,5000)==WAIT_OBJECT_0);}
        DWORD result=WaitForSingleObject(event,timeout);if(result==WAIT_TIMEOUT) ++Timeouts;
        CHECK(result==WAIT_OBJECT_0 || result==WAIT_TIMEOUT);return result;
    }
    return WaitForSingleObject(event,timeout);
}
static HANDLE Event(VOID)
{HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(event);return event;}
static VOID Create(ULONG queues,ULONG slots,BOOL wrap)
{
    CHECK(!Current && !GuardAllocations);memset(Calls,0,sizeof(Calls));memset(Popped,0,sizeof(Popped));Turns=Timeouts=0;Gated=FALSE;
    ULONG capacity=1;while(capacity<slots) capacity*=2;
    PB_CONFIG config={sizeof(config),queues,slots,capacity,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&Current,&region,&layout,&epoch)==PB_OK);Retain(Current);Base=wrap?UINT64_MAX-63:0;
    for(ULONG q=0;q<queues;++q) {
        PB_USER_QUEUE *queue;CHECK(Api.OpenQueue(Current,q,&queue)==PB_OK);
        for(ULONG r=PB_QUEUE_RX_SUBMIT;r<=PB_QUEUE_RX_COMPLETE;++r) {
            PB_QUEUE_VIEW *user=&queue->Rings[r],*native=&Current->TestRings[q][r];
            user->Cursor=user->PeerCursor=native->Cursor=native->PeerCursor=Base;
            user->Shared->Producer=user->Shared->Consumer=Base;
        }
    }
}
static VOID SubmitPackets(ULONG q,ULONG submissions,ULONG completions,BOOL damage)
{
    ULONG done=0;
    while(done<submissions) {
        PB_LEASE leases[PB_MAX_BATCH];PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t count;
        ULONG n=submissions-done;if(n>PB_MAX_BATCH) n=PB_MAX_BATCH;
        CHECK(Api.ReserveBatch(Current->Queues+q,leases,n,&count)==PB_OK && count==n);
        for(ULONG i=0;i<n;++i) {leases[i].Descriptor.Length=20;leases[i].Data[0]=0x45;leases[i].Data[3]=20;}
        CHECK(Api.CommitBatch(Current->Queues+q,leases,n,&count)==PB_OK && count==n);
        CHECK(PbTestTake(Current,q,items,n,&count)==PB_OK && count==n);
        ULONG complete=done>=completions?0:completions-done;if(complete>n) complete=n;
        if(damage && !done) ++items[0].Token.Epoch;
        if(complete) CHECK(PbTestComplete(Current,q,items,complete)==PB_OK);
        done+=n;
    }
}
static VOID WaitPrepared(VOID)
{
    ULONGLONG deadline=GetTickCount64()+5000;AcquireSRWLockExclusive(&Current->State);
    while(Current->Progress.Phase<PB_LIFE_PREPARED) {
        CHECK(GetTickCount64()<deadline);
        CHECK(SleepConditionVariableSRW(&Current->Changed,&Current->State,100,0) || GetLastError()==ERROR_TIMEOUT);
    }
    ReleaseSRWLockExclusive(&Current->State);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->ResourcesClosed && Current->Region && !Timeouts);
    for(ULONG q=0;q<Current->Layout.QueueCount;++q) {
        CHECK(!Current->Queues[q].Published && !Current->Queues[q].Loans && Calls[q]<=4 && Popped[q]<=CLOSE_BUDGET);
        for(ULONG i=0;i<Current->Layout.SlotCount;++i) CHECK(Current->Queues[q].RxSlots[i].State==PB_SLOT_FREE);
    }
}
static VOID Prepare(VOID)
{CHECK(Api.PrepareClose(Current,5000)==PB_OK);WaitPrepared();}
static VOID CheckConsumed(ULONG q,ULONG completed,ULONG oldConsumed,BOOL structural)
{
    PB_USER_QUEUE *queue=Current->Queues+q;
#ifdef PB_CLOSE_COMPLETION_BASELINE
    ULONG expected=oldConsumed;
#else
    (void)oldConsumed;ULONG expected=completed;
#endif
    if(queue->Rings[PB_QUEUE_RX_COMPLETE].Cursor!=Base+expected)
        fprintf(stderr,"completion cursor: q=%lu completed=%lu observed=%llu expected=%lu structural=%u fatal=%ld\n",
            q,completed,(unsigned long long)(queue->Rings[PB_QUEUE_RX_COMPLETE].Cursor-Base),expected,structural,Current->Fatal);
    CHECK(queue->Rings[PB_QUEUE_RX_COMPLETE].Cursor==Base+expected);
    if(structural) return;
    PB_DESCRIPTOR item;uint32_t count;
    PB_RESULT result=Api.PollCompletions(queue,&item,1,&count);
    CHECK(!count && result==(expected<completed?PB_CORRUPT:PB_EMPTY));
    if(expected<completed) CHECK(Current->Fatal);
}
static VOID Finish(VOID)
{
    /* Prepare joined the previous callback; final Close starts a new turn. */
    memset(Calls,0,sizeof(Calls));memset(Popped,0,sizeof(Popped));
    CHECK(Api.Close(Current,5000)==PB_OK);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->ResourcesClosed && !Current->Region);DropReference(Current);Current=NULL;
    PB_RUNTIME_STATUS runtime;CHECK(Life.GetRuntimeStatus(&runtime)==PB_OK && !runtime.Sessions && !runtime.RetainedBytes && !runtime.Adapters && !GuardAllocations);
    WaitEntered=WaitGate=NULL;
}
static VOID Backlog(ULONG count,BOOL wrap)
{
    Create(1,count,wrap);SubmitPackets(0,count,count,FALSE);Prepare();CHECK(!Current->Fatal);
#ifndef PB_CLOSE_COMPLETION_BASELINE
    CHECK(Turns==(count-1)/CLOSE_BUDGET);
#else
    CHECK(!Turns);
#endif
    CheckConsumed(0,count,count>CLOSE_BUDGET?CLOSE_BUDGET:count,FALSE);Finish();
}
static VOID TwoQueues(BOOL wrap,ULONG damage)
{
    const ULONG first=CLOSE_BUDGET+1,second=2*CLOSE_BUDGET-1;
    Create(2,second,wrap);SubmitPackets(0,first,damage==2?0:first,damage==1);SubmitPackets(1,second,second,FALSE);
    if(damage==2) Current->Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Shared->Producer=Base+Current->Layout.RingCapacity+1;
    Prepare();CHECK(!!Current->Fatal==!!damage);
    CheckConsumed(0,damage==2?0:first,damage==2?0:damage==1?PB_MAX_BATCH:CLOSE_BUDGET,damage==2);
    CheckConsumed(1,second,CLOSE_BUDGET,FALSE);Finish();
}
static VOID LatePublication(BOOL wrap)
{
    Create(1,129,wrap);SubmitPackets(0,129,0,FALSE);
    HANDLE entered=Event(),gate=Event();PbTestBlock(Current,PB_TEST_QUERY,entered,gate);
    CHECK(Life.BeginStop(Current)==PB_OK && WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    PB_QUEUE_VIEW *submit=&Current->TestRings[0][PB_QUEUE_RX_SUBMIT];
    /* The host retained the submitted descriptors. Publish them while QUERY is
     * gated, after lifecycle's initial empty poll and before its STOPPED reply. */
    for(ULONG done=0;done<129;) {
        PB_DESCRIPTOR items[PB_MAX_BATCH];ULONG n=129-done;if(n>PB_MAX_BATCH) n=PB_MAX_BATCH;
        for(ULONG i=0;i<n;++i) items[i]=submit->Entries[(ULONG)(Base+done+i)&(submit->Capacity-1)];
        CHECK(PbTestComplete(Current,0,items,n)==PB_OK);done+=n;
    }
    CHECK(SetEvent(gate));WaitPrepared();CHECK(!Current->Fatal);CheckConsumed(0,129,0,FALSE);Finish();CHECK(CloseHandle(gate) && CloseHandle(entered));
}
static VOID Abandoned(BOOL wrap,ULONG completed)
{Create(1,256,wrap);SubmitPackets(0,256,completed,FALSE);Prepare();CHECK(!Current->Fatal);CheckConsumed(0,completed,completed>128?128:completed,FALSE);Finish();}
static VOID Barrier(BOOL native)
{
    const ULONG total=2*CLOSE_BUDGET-1;
    Create(1,total+1,FALSE);SubmitPackets(0,total,total,FALSE);PB_LEASE loan;uint32_t count;
    if(native) PbTestActive(Current,1);
    else CHECK(Api.ReserveBatch(Current->Queues,&loan,1,&count)==PB_OK && count==1);
    HANDLE entered=Event(),gate=Event();WaitEntered=entered;WaitGate=gate;
    CHECK(Life.BeginStop(Current)==PB_OK && WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    PB_CLOSE_PROGRESS progress;CHECK(Life.GetCloseProgress(Current,&progress)==PB_OK && progress.Phase<PB_LIFE_PREPARED && !Current->ResourcesClosed && Current->Region);
    CHECK(Current->Queues[0].Published==(LONG)(total-CLOSE_BUDGET) && (native?Current->TestActive==1:Current->Queues[0].Loans==1));
    if(native) PbTestActive(Current,0);
    else CHECK(Api.CancelBatch(Current->Queues,&loan.Descriptor.Token,1)==PB_OK);
    CHECK(SetEvent(gate));WaitPrepared();CHECK(!Current->Fatal);CheckConsumed(0,total,total,FALSE);Finish();CHECK(CloseHandle(entered) && CloseHandle(gate));
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    ULONG counts[]={CLOSE_BUDGET-1,CLOSE_BUDGET,CLOSE_BUDGET+1,2*CLOSE_BUDGET,510};
    for(ULONG wrap=0;wrap<2;++wrap) {
        for(ULONG i=0;i<sizeof(counts)/sizeof(counts[0]);++i) Backlog(counts[i],wrap);
        TwoQueues(wrap,0);TwoQueues(wrap,1);TwoQueues(wrap,2);LatePublication(wrap);Abandoned(wrap,0);Abandoned(wrap,129);
    }
    Barrier(FALSE);Barrier(TRUE);
    puts("PASS: 24 actual close/completion scenarios: bounded backlog, cursor wrap, multi-queue, late publication, abandoned/damaged delivery and user/native barriers; frozen false corruption reproduced, current valid rings drained before reconciliation.");return 0;
}
