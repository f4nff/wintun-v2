/* SPDX-License-Identifier: GPL-2.0 */
/* Observe actual DLL counters without replacing Windows synchronization. */
#define PB_API_TESTS 1
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
static LONG CounterRead(volatile LONG *);
VOID CounterStore(volatile LONG *, LONG);
LONG CounterAdd(volatile LONG *, LONG);
static LONG CounterCompare(volatile LONG *, LONG, LONG);
static LONG CounterExchange(volatile LONG *, LONG);
static volatile LONG GuardAllocations;
#ifdef PB_GUARD_TESTS
static VOID *GuardCalloc(SIZE_T count, SIZE_T size)
{CHECK(!size || count <= SIZE_MAX / size); VOID *p=PbTestGuardAlloc(count*size); memset(p,0,count*size); InterlockedIncrement(&GuardAllocations); return p;}
static VOID GuardFree(VOID *p) {if(p){PbTestGuardFree(p); CHECK(InterlockedDecrement(&GuardAllocations)>=0);}}
static VOID *WINAPI GuardRegion(VOID *address, SIZE_T size, DWORD type, DWORD protection)
{CHECK(!address && type==(MEM_RESERVE|MEM_COMMIT) && protection==PAGE_READWRITE); return GuardCalloc(1,size);}
static BOOL WINAPI GuardUnmap(VOID *p, SIZE_T size, DWORD type)
{CHECK(!size && type==MEM_RELEASE); GuardFree(p); return TRUE;}
#define calloc GuardCalloc
#define free GuardFree
#define VirtualAlloc GuardRegion
#define VirtualFree GuardUnmap
#endif
#pragma push_macro("ReadAcquire")
#pragma push_macro("WriteRelease")
#pragma push_macro("InterlockedAdd")
#pragma push_macro("InterlockedCompareExchange")
#pragma push_macro("InterlockedExchange")
#undef ReadAcquire
#undef WriteRelease
#undef InterlockedAdd
#undef InterlockedCompareExchange
#undef InterlockedExchange
#define ReadAcquire CounterRead
#define WriteRelease CounterStore
#define InterlockedAdd CounterAdd
#define InterlockedCompareExchange CounterCompare
#define InterlockedExchange CounterExchange
#ifndef PB_OWNERSHIP_SESSION_SOURCE
#define PB_OWNERSHIP_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_OWNERSHIP_SESSION_SOURCE
#pragma pop_macro("InterlockedExchange")
#pragma pop_macro("InterlockedCompareExchange")
#pragma pop_macro("InterlockedAdd")
#pragma pop_macro("WriteRelease")
#pragma pop_macro("ReadAcquire")
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
static PB_SESSION *Observed;
static volatile LONG Updates, Stores, WriterRmws, ReaderRmws, SnapshotReads, LifecycleUpdates;
static volatile LONG *PauseCounter;
static HANDLE Paused, Continue;
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_USER_QUEUE *CounterQueue(volatile LONG *p)
{
    if(Observed) for(ULONG i=0;i<Observed->Layout.QueueCount;++i) {
        PB_USER_QUEUE *q=Observed->Queues+i;
        if(p==&q->Loans || p==&q->Published) return q;
    }
    return NULL;
}
static VOID ObserveUpdate(volatile LONG *p, LONG value)
{
    PB_USER_QUEUE *q=CounterQueue(p); if(!q) return;
    CHECK(value>=0 && value<=(LONG)((p==&q->Loans?2:1)*q->Session->Layout.SlotCount));
    BOOL exclusive=TryAcquireSRWLockExclusive(&q->Session->DataGate);
    if(exclusive) ReleaseSRWLockExclusive(&q->Session->DataGate);
    CHECK(!exclusive);
    if(ReadAcquire(&q->Owner)!=(LONG)GetCurrentThreadId()) {
        CHECK((LONG)GetCurrentThreadId()==ReadAcquire(q->Session->TestBoundaryThreads+PB_TEST_WORKER_START));
        BOOL shared=TryAcquireSRWLockShared(&q->Session->DataGate);
        if(shared) ReleaseSRWLockShared(&q->Session->DataGate);
        CHECK(!shared && p==&q->Published); InterlockedIncrement(&LifecycleUpdates);
    }
    InterlockedIncrement(&Updates);
    if(p==PauseCounter && !value) {
        PauseCounter=NULL; CHECK(SetEvent(Paused) && WaitForSingleObject(Continue,5000)==WAIT_OBJECT_0);
    }
}
static LONG CounterRead(volatile LONG *p)
{if(CounterQueue(p)) InterlockedIncrement(&SnapshotReads); return ReadAcquire(p);}
VOID CounterStore(volatile LONG *p,LONG value)
{WriteRelease(p,value); if(CounterQueue(p)) {InterlockedIncrement(&Stores); ObserveUpdate(p,value);}}
LONG CounterAdd(volatile LONG *p,LONG delta)
{LONG value=InterlockedAdd(p,delta); if(CounterQueue(p)) {InterlockedIncrement(&WriterRmws); ObserveUpdate(p,value);} return value;}
static LONG CounterCompare(volatile LONG *p,LONG exchange,LONG compare)
{if(CounterQueue(p)) {CHECK(!exchange && !compare); InterlockedIncrement(&ReaderRmws);} return InterlockedCompareExchange(p,exchange,compare);}
static LONG CounterExchange(volatile LONG *p,LONG value)
{LONG old=InterlockedExchange(p,value); if(CounterQueue(p)) {InterlockedIncrement(&WriterRmws); ObserveUpdate(p,value);} return old;}
static PB_SESSION *Create(ULONG queues)
{
    PB_CONFIG config={sizeof(config),queues,32,32,0,0}; PB_SESSION *s; VOID *region; PB_LAYOUT layout; uint64_t epoch;
    CHECK(PbTestOpen(&config,&s,&region,&layout,&epoch)==PB_OK); Observed=s; return s;
}
static VOID CloseSession(PB_SESSION *s)
{
    /* Keep only the test shell until the actual public close callback returns. */
    Retain(s); CHECK(Api.Close(s,2000)==PB_OK && s->ResourcesClosed);
    WaitForThreadpoolWorkCallbacks(s->Work,FALSE); Observed=NULL; DropReference(s);
    PB_RUNTIME_STATUS status; CHECK(Life.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !GuardAllocations);
}
static VOID CheckCounters(PB_USER_QUEUE *q,LONG loans,LONG published)
{CHECK(ReadAcquire(&q->Loans)==loans && ReadAcquire(&q->Published)==published);}
static VOID Cycle(PB_SESSION *s,ULONG index,ULONG count,ULONG generation)
{
    PB_USER_QUEUE *q=s->Queues+index; PB_DESCRIPTOR input[PB_MAX_BATCH],output[PB_MAX_BATCH];
    PB_TOKEN tokens[PB_MAX_BATCH]; PB_LEASE leases[PB_MAX_BATCH]; uint32_t n,notify;
    for(ULONG i=0;i<count;++i) {
        memset(input+i,0,sizeof(*input)); input[i].Token=(PB_TOKEN){s->Epoch,generation,(uint16_t)i,(uint16_t)index};
        input[i].Length=20; input[i].DataOffset=s->Layout.Queues[index].TxData+i*PB_BLOCK_SIZE;
    }
    CHECK(PbQueuePush(&s->TestRings[index][PB_QUEUE_TX_READY],input,count,&n,&notify)==PB_OK && n==count);
    CHECK(Api.ReceiveBatch(q,leases,count,&n)==PB_OK && n==count); CheckCounters(q,count,0);
    for(ULONG i=0;i<count;++i) tokens[i]=leases[i].Descriptor.Token;
    CHECK(Api.ReleaseBatch(q,tokens,count,&n)==PB_OK && n==count); CheckCounters(q,0,0);
    CHECK(PbQueuePopTokens(&s->TestRings[index][PB_QUEUE_TX_RECYCLE],tokens,count,&n)==PB_OK && n==count);
    CHECK(Api.ReserveBatch(q,leases,count,&n)==PB_OK && n==count); CheckCounters(q,count,0);
    for(ULONG i=0;i<count;++i) tokens[i]=leases[i].Descriptor.Token;
    CHECK(Api.CancelBatch(q,tokens,count)==PB_OK); CheckCounters(q,0,0);
    CHECK(Api.ReserveBatch(q,leases,count,&n)==PB_OK && n==count);
    for(ULONG i=0;i<count;++i) leases[i].Descriptor.Length=20;
    CHECK(Api.CommitBatch(q,leases,count,&n)==PB_OK && n==count); CheckCounters(q,0,count);
    CHECK(PbQueuePopBudget(&s->TestRings[index][PB_QUEUE_RX_SUBMIT],input,count,128u*1024u,&n)==PB_OK && n==count);
    CHECK(PbQueuePush(&s->TestRings[index][PB_QUEUE_RX_COMPLETE],input,count,&n,&notify)==PB_OK && n==count);
    CHECK(Api.PollCompletions(q,output,count,&n)==PB_OK && n==count && !memcmp(input,output,count*sizeof(*input))); CheckCounters(q,0,0);
}
static VOID Operations(VOID)
{
    PB_SESSION *s=Create(1); Updates=Stores=WriterRmws=ReaderRmws=SnapshotReads=LifecycleUpdates=0;
    Cycle(s,0,32,1); PB_CLOSE_PROGRESS p; CHECK(Life.GetCloseProgress(s,&p)==PB_OK && !p.UserLeases && !p.Published);
    CHECK(Updates==8 && !LifecycleUpdates);
#ifdef PB_OWNERSHIP_RMW_BASELINE
    CHECK(WriterRmws==8 && !Stores && ReaderRmws==2);
#else
    CHECK(!WriterRmws && Stores==8 && !ReaderRmws && SnapshotReads>=10);
#endif
    printf("PASS: actual receive/release, reserve/cancel, commit/poll: updates=%ld counter writer RMWs=%ld stores=%ld snapshot RMWs=%ld; every writer owns its DataGate interval.\n",Updates,WriterRmws,Stores,ReaderRmws);
    CloseSession(s);
}
typedef struct JOB {PB_SESSION *S; ULONG Index,Count; HANDLE Start;} JOB;
static volatile LONG Observing;
static DWORD WINAPI OwnQueue(VOID *opaque)
{
    JOB *job=opaque; CHECK(WaitForSingleObject(job->Start,5000)==WAIT_OBJECT_0);
    for(ULONG i=1;i<=10000;++i) Cycle(job->S,job->Index,job->Count,i);
    return 0;
}
static DWORD WINAPI ObserveProgress(VOID *opaque)
{
    PB_SESSION *s=opaque;
    do {PB_CLOSE_PROGRESS p; CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.Phase==PB_LIFE_RUNNING && p.UserLeases<=2*s->Layout.QueueCount*s->Layout.SlotCount && p.Published<=s->Layout.QueueCount*s->Layout.SlotCount);}
    while(ReadAcquire(&Observing)); return 0;
}
static VOID Concurrent(VOID)
{
    PB_SESSION *s=Create(4); HANDLE start=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(start);
    const ULONG counts[]={1,3,8,32}; JOB jobs[4]; HANDLE threads[4];
    for(ULONG i=0;i<4;++i) {jobs[i]=(JOB){s,i,counts[i],start}; threads[i]=CreateThread(NULL,0,OwnQueue,jobs+i,0,NULL); CHECK(threads[i]);}
    Observing=1; HANDLE observer=CreateThread(NULL,0,ObserveProgress,s,0,NULL); CHECK(observer && SetEvent(start));
    CHECK(WaitForMultipleObjects(4,threads,TRUE,30000)==WAIT_OBJECT_0); InterlockedExchange(&Observing,0);
    CHECK(WaitForSingleObject(observer,5000)==WAIT_OBJECT_0 && CloseHandle(observer) && CloseHandle(start));
    for(ULONG i=0;i<4;++i) {CHECK(CloseHandle(threads[i])); CheckCounters(s->Queues+i,0,0);}
    PB_CLOSE_PROGRESS p; CHECK(Life.GetCloseProgress(s,&p)==PB_OK && !p.UserLeases && !p.Published); CloseSession(s);
    puts("PASS: four independent queue owners complete 40000 actual TX/return, reserve/cancel and commit/poll cycles beside a real progress reader; no lost counters or loans.");
}
static DWORD WINAPI CommitDuringStop(VOID *opaque)
{
    PB_SESSION *s=opaque; PB_USER_QUEUE *q=s->Queues; PB_LEASE lease; uint32_t n,notify;
    CHECK(Api.ReserveBatch(q,&lease,1,&n)==PB_OK && n==1); lease.Descriptor.Length=20;
    PauseCounter=&q->Loans; CHECK(Api.CommitBatch(q,&lease,1,&n)==PB_OK && n==1);
    PB_DESCRIPTOR d; CHECK(PbQueuePopBudget(&s->TestRings[0][PB_QUEUE_RX_SUBMIT],&d,1,128u*1024u,&n)==PB_OK && n==1);
    CHECK(PbQueuePush(&s->TestRings[0][PB_QUEUE_RX_COMPLETE],&d,1,&n,&notify)==PB_OK && n==1); SetEvent(s->ProgressEvent); return 0;
}
static DWORD WINAPI WrongOwner(VOID *opaque)
{
    PB_SESSION *s=opaque; PB_LEASE lease; uint32_t n;
    CHECK(Api.ReserveBatch(s->Queues,&lease,1,&n)==PB_BUSY && !n); return 0;
}
static VOID GatedCommit(VOID)
{
    PB_SESSION *s=Create(1); PbTestActive(s,1);
    Paused=CreateEventW(NULL,TRUE,FALSE,NULL); Continue=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(Paused && Continue);
    HANDLE owner=CreateThread(NULL,0,CommitDuringStop,s,0,NULL); CHECK(owner && WaitForSingleObject(Paused,5000)==WAIT_OBJECT_0);
    PB_CLOSE_PROGRESS p; CHECK(Life.GetCloseProgress(s,&p)==PB_OK && !p.UserLeases && !p.Published);
    CHECK(Api.PrepareClose(s,30)==PB_QUARANTINED && Life.GetCloseProgress(s,&p)==PB_OK && p.Phase<PB_LIFE_PREPARED && !s->ResourcesClosed);
    /* QueueEnter may wait behind lifecycle's pending exclusive DataGate before
     * checking Owner. Do not block the thread responsible for releasing it. */
    HANDLE wrong=CreateThread(NULL,0,WrongOwner,s,0,NULL); CHECK(wrong);
    CHECK(SetEvent(Continue) && WaitForSingleObject(owner,5000)==WAIT_OBJECT_0 && CloseHandle(owner));
    CHECK(WaitForSingleObject(wrong,5000)==WAIT_OBJECT_0 && CloseHandle(wrong));
    CHECK(Api.PrepareClose(s,30)==PB_QUARANTINED && !s->ResourcesClosed);
    PbTestActive(s,0); CHECK(Api.PrepareClose(s,2000)==PB_OK && !s->Queues[0].Published && !s->ResourcesClosed);
    CHECK(CloseHandle(Paused) && CloseHandle(Continue)); Paused=Continue=NULL; CloseSession(s);
    puts("PASS: PrepareClose cannot trust transient zero counters between commit stores; DataGate and native references both block preparation, and wrong-owner admission stays rejected.");
}
static VOID FaultRetirement(VOID)
{
    PB_SESSION *s=Create(1); PB_USER_QUEUE *q=s->Queues; PB_LEASE lease; PB_DESCRIPTOR d; uint32_t n,notify;
    CHECK(Api.ReserveBatch(q,&lease,1,&n)==PB_OK && n==1); lease.Descriptor.Length=20;
    CHECK(Api.CommitBatch(q,&lease,1,&n)==PB_OK && n==1); PbTestActive(s,1);
    CHECK(PbQueuePopBudget(&s->TestRings[0][PB_QUEUE_RX_SUBMIT],&d,1,128u*1024u,&n)==PB_OK && n==1); d.Token.Epoch++;
    CHECK(PbQueuePush(&s->TestRings[0][PB_QUEUE_RX_COMPLETE],&d,1,&n,&notify)==PB_OK && n==1);
    CHECK(Api.PollCompletions(q,&d,1,&n)==PB_CORRUPT && !n && q->Published==1);
    CHECK(Api.PrepareClose(s,30)==PB_QUARANTINED && !s->ResourcesClosed); LONG before=LifecycleUpdates;
    PbTestActive(s,0); CHECK(Api.PrepareClose(s,2000)==PB_OK && !q->Published && LifecycleUpdates==before+1 && !s->ResourcesClosed);
    CloseSession(s); puts("PASS: damaged completion retains Published until trusted native STOPPED; lifecycle resets it under exclusive DataGate before actual public close.");
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0); CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    Operations(); Concurrent(); GatedCommit(); FaultRetirement(); return 0;
}
