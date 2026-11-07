/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL session, pools, gates and lifecycle. A deterministic native peer
 * consumes published tokens and legally overwrites only reacquired packets. */
#define PB_API_TESTS 1
#include "../../api/v2/packetbridge.h"
#include <stdio.h>
static PB_RESULT PeerReturn(PB_QUEUE_VIEW *,const PB_TOKEN *,uint32_t,uint32_t *,uint32_t *);
static PB_RESULT GuardTransition(PB_POOL *,const PB_TOKEN *,uint32_t,uint32_t);
static HANDLE WINAPI BoEvent(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCWSTR);
static HANDLE WINAPI BoThread(LPSECURITY_ATTRIBUTES,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,LPDWORD);
static BOOL WINAPI BoClose(HANDLE);
static PTP_POOL WINAPI BoPool(PVOID);
static VOID WINAPI BoClosePool(PTP_POOL);
static PTP_WORK WINAPI BoWork(PTP_WORK_CALLBACK,PVOID,PTP_CALLBACK_ENVIRON);
static VOID WINAPI BoCloseWork(PTP_WORK);
#define CreateEventW BoEvent
#define CreateThread BoThread
#define CloseHandle BoClose
#define CreateThreadpool BoPool
#define CloseThreadpool BoClosePool
#define CreateThreadpoolWork BoWork
#define CloseThreadpoolWork BoCloseWork
#define PbQueuePushTokens PeerReturn
#define PbPoolTransition GuardTransition
#define main ExistingOwnershipMain
#include "ownership_counter_tests.c"
#undef main
#undef PbQueuePushTokens
#undef PbPoolTransition
#undef CreateEventW
#undef CreateThread
#undef CloseHandle
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
static volatile LONG BoHandles,BoPools,BoWorks;
static SRWLOCK BoWitnessLock=SRWLOCK_INIT;
static HANDLE BoWorkers[64];
static DWORD BoWorkerIds[64],BoWorkerCount;
static PTP_WORK_CALLBACK BoCallback;
static PVOID BoContext;
static VOID CALLBACK BoDispatch(PTP_CALLBACK_INSTANCE instance,PVOID context,PTP_WORK work)
{
    (void)context;DWORD id=GetCurrentThreadId();
    AcquireSRWLockExclusive(&BoWitnessLock);DWORD index=0;
    while(index<BoWorkerCount && BoWorkerIds[index]!=id)++index;
    if(index==BoWorkerCount) {
        CHECK(index<ARRAYSIZE(BoWorkers) && DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),BoWorkers+index,SYNCHRONIZE,FALSE,0));
        BoWorkerIds[index]=id;++BoWorkerCount;
    }
    ReleaseSRWLockExclusive(&BoWitnessLock);BoCallback(instance,BoContext,work);
}
static HANDLE WINAPI BoEvent(LPSECURITY_ATTRIBUTES a,BOOL m,BOOL s,LPCWSTR n)
{HANDLE h=CreateEventW(a,m,s,n);CHECK(h);InterlockedIncrement(&BoHandles);return h;}
static HANDLE WINAPI BoThread(LPSECURITY_ATTRIBUTES a,SIZE_T s,LPTHREAD_START_ROUTINE f,LPVOID p,DWORD flags,LPDWORD id)
{HANDLE h=CreateThread(a,s,f,p,flags,id);CHECK(h);InterlockedIncrement(&BoHandles);return h;}
static BOOL WINAPI BoClose(HANDLE h)
{BOOL okay=CloseHandle(h);CHECK(okay && InterlockedDecrement(&BoHandles)>=0);return okay;}
static PTP_POOL WINAPI BoPool(PVOID p)
{PTP_POOL pool=CreateThreadpool(p);CHECK(pool);InterlockedIncrement(&BoPools);return pool;}
static VOID WINAPI BoClosePool(PTP_POOL pool)
{CloseThreadpool(pool);CHECK(InterlockedDecrement(&BoPools)>=0);}
static PTP_WORK WINAPI BoWork(PTP_WORK_CALLBACK f,PVOID p,PTP_CALLBACK_ENVIRON e)
{CHECK(!BoWorks && !BoWorkerCount);BoCallback=f;BoContext=p;PTP_WORK work=CreateThreadpoolWork(BoDispatch,NULL,e);CHECK(work);InterlockedIncrement(&BoWorks);return work;}
static VOID WINAPI BoCloseWork(PTP_WORK work)
{CloseThreadpoolWork(work);CHECK(InterlockedDecrement(&BoWorks)>=0);}
static VOID BoFinish(PB_SESSION *s)
{
    Retain(s);CHECK(Api.Close(s,2000)==PB_OK && s->ResourcesClosed);WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    Observed=NULL;DropReference(s);
    /* A completed callback and a closed pool do not make a terminated-thread
     * sample synchronous. Join every captured native thread object explicitly
     * before the OS handle accounting; no thread-ID lookup/reuse race. */
    for(DWORD i=0;i<BoWorkerCount;++i) {
        DWORD waited=WaitForSingleObject(BoWorkers[i],5000);CHECK(CloseHandle(BoWorkers[i]));
        if(waited!=WAIT_OBJECT_0){fprintf(stderr,"private pool worker %lu not terminated: %lu\n",BoWorkerIds[i],waited);ExitProcess(1);}
    }
    BoWorkerCount=0;BoCallback=NULL;BoContext=NULL;
    PB_RUNTIME_STATUS status;CHECK(Life.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !GuardAllocations);
    CHECK(!BoHandles && !BoPools && !BoWorks);
}
#define CloseSession BoFinish
#define CreateEventW BoEvent
#define CreateThread BoThread
#define CloseHandle BoClose
static BOOL PeerArmed,LimitTransitions;
static ULONG ExpectedTransitions,Transitions,Reused,Scenarios;
static uint32_t *AliasedCount,Poison;
static HANDLE PeerEntered,PeerGate;
static PB_RESULT GuardTransition(PB_POOL *pool,const PB_TOKEN *token,uint32_t before,uint32_t after)
{
    if(LimitTransitions && ++Transitions>ExpectedTransitions) {
        /* Stop before an uninitialised/out-of-bounds private-token read. */
        fputs("PUBLIC_COUNT_ESCAPED_BATCH_BOUND\n",stderr);ExitProcess(52);
    }
    return PbPoolTransition(pool,token,before,after);
}
static PB_RESULT PeerReturn(PB_QUEUE_VIEW *ring,const PB_TOKEN *tokens,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    uint64_t before=ring->Cursor;PB_RESULT result=PbQueuePushTokens(ring,tokens,count,accepted,notify);
    if(result==PB_OK && PeerArmed) {
        PB_SESSION *s=Observed;PB_TOKEN returned[PB_MAX_BATCH];PB_DESCRIPTOR next[PB_MAX_BATCH]={0};uint32_t n,k,wake;
        CHECK(ring==&s->Queues[0].Rings[PB_QUEUE_TX_RECYCLE]);
        CHECK(PbQueuePopTokens(&s->TestRings[0][PB_QUEUE_TX_RECYCLE],returned,PB_MAX_BATCH,&n)==PB_OK && n==ring->Cursor-before);
        for(uint32_t i=0;i<n;++i) {
            PB_POOL *pool=s->TestTxPools;
            CHECK(PbPoolTransition(pool,returned+i,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);
            pool->Next=returned[i].Slot;CHECK(PbPoolReserve(pool,&next[i].Token)==PB_OK);
            CHECK(next[i].Token.Slot==returned[i].Slot && next[i].Token.Generation==returned[i].Generation+1);
            next[i].DataOffset=s->Layout.Queues[0].TxData+returned[i].Slot*PB_BLOCK_SIZE;next[i].Length=8192;
            uint8_t *bytes=s->Region+next[i].DataOffset;memset(bytes,0xcc,next[i].Length);
            if(AliasedCount && (uintptr_t)AliasedCount>=(uintptr_t)bytes &&
                (uintptr_t)AliasedCount-(uintptr_t)bytes<next[i].Length)memcpy(AliasedCount,&Poison,sizeof(Poison));
            CHECK(PbPoolTransition(pool,&next[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        }
        CHECK(PbQueuePush(&s->TestRings[0][PB_QUEUE_TX_READY],next,n,&k,&wake)==PB_OK && k==n);Reused+=n;
        if(PeerGate)CHECK(SetEvent(PeerEntered) && WaitForSingleObject(PeerGate,5000)==WAIT_OBJECT_0);
    }
    return result;
}
static PB_USER_QUEUE *Borrow(PB_SESSION *s,PB_LEASE *leases,ULONG n)
{
    static const uint8_t frame[8192]={0x45};PB_USER_QUEUE *q;uint32_t received;
    CHECK(Api.OpenQueue(s,0,&q)==PB_OK);
    for(ULONG i=0;i<n;++i)CHECK(PbTestSend(s,0,frame,sizeof(frame),0,0)==PB_OK);
    CHECK(Api.ReceiveBatch(q,leases,min(n,PB_MAX_BATCH),&received)==PB_OK && received==min(n,PB_MAX_BATCH));
    if(n>PB_MAX_BATCH)CHECK(Api.ReceiveBatch(q,leases+PB_MAX_BATCH,n-PB_MAX_BATCH,&received)==PB_OK && received==n-PB_MAX_BATCH);
    return q;
}
static VOID ReleaseAll(PB_USER_QUEUE *q,PB_LEASE *leases,ULONG n)
{
    for(ULONG first=0;first<n;first+=PB_MAX_BATCH) {
        PB_TOKEN tokens[PB_MAX_BATCH];ULONG count=min(PB_MAX_BATCH,n-first);uint32_t accepted;
        for(ULONG i=0;i<count;++i)tokens[i]=leases[first+i].Descriptor.Token;
        CHECK(Api.ReleaseBatch(q,tokens,count,&accepted)==PB_OK && accepted==count);
    }
}
static VOID ReturnOutput(ULONG n,BOOL held,BOOL inputAlias,ULONG position,uint32_t poison,BOOL baseline)
{
    PB_SESSION *s=CreateSized(1,128);PB_LEASE leases[PB_MAX_BATCH+1],again[PB_MAX_BATCH];
    PB_USER_QUEUE *q=Borrow(s,leases,n+held);PB_TOKEN stable[PB_MAX_BATCH];
    for(ULONG i=0;i<n;++i)stable[i]=leases[i].Descriptor.Token;
    PB_TOKEN *tokens=inputAlias?(PB_TOKEN *)leases[0].Data:stable;
    if(inputAlias)memcpy(tokens,stable,n*sizeof(*tokens));
    AliasedCount=(uint32_t *)(leases[held?n:position].Data+4096);Poison=poison;Reused=Transitions=0;
    PeerArmed=LimitTransitions=TRUE;ExpectedTransitions=n;uint64_t producer=q->Rings[PB_QUEUE_TX_RECYCLE].Cursor;
    PB_RESULT result=Api.ReleaseBatch(q,tokens,n,AliasedCount);LimitTransitions=FALSE;
    if(!held) {
        if(baseline && result==PB_OK) {
            CHECK(!*AliasedCount && !Transitions && q->Loans==(LONG)n && Reused==n);
            fputs("REUSED_OUTPUT_COUNT_LOST_RETIREMENT\n",stderr);ExitProcess(51);
        }
        CHECK(result==PB_INVALID && !*AliasedCount && !Transitions && !Reused && q->Loans==(LONG)n && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==producer);
        uint32_t accepted;LimitTransitions=TRUE;Transitions=0;
        CHECK(Api.ReleaseBatch(q,tokens,n,&accepted)==PB_OK && accepted==n && Transitions==n);LimitTransitions=FALSE;
    } else CHECK(result==PB_OK && *AliasedCount==n && Transitions==n && q->Loans==1);
    PeerArmed=FALSE;AliasedCount=NULL;CHECK(Reused==n);
    for(ULONG i=0;i<n;++i)CHECK(q->TxSlots[stable[i].Slot].State==PB_SLOT_FREE);
    uint32_t received;CHECK(Api.ReceiveBatch(q,again,n,&received)==PB_OK && received==n);
    for(ULONG i=0;i<n;++i)CHECK(again[i].Descriptor.Token.Generation==stable[i].Generation+1);
    ReleaseAll(q,again,n);if(held)ReleaseAll(q,leases+n,1);
    CheckCounters(q,0,0);CloseSession(s);++Scenarios;
}
static VOID CountStorage(ULONG n,BOOL poll)
{
    PB_SESSION *s=CreateSized(1,128);PB_USER_QUEUE *q;PB_LEASE leases[PB_MAX_BATCH];uint32_t count;
    CHECK(Api.OpenQueue(s,0,&q)==PB_OK);
    if(!poll) {
        uint32_t *output=&leases[n-1].Capacity;
        CHECK(Api.ReserveBatch(q,leases,n,output)==PB_OK && *output==n);CheckCounters(q,n,0);
        PB_TOKEN tokens[PB_MAX_BATCH];for(ULONG i=0;i<n;++i)tokens[i]=leases[i].Descriptor.Token;
        CHECK(Api.CancelBatch(q,tokens,n)==PB_OK);CheckCounters(q,0,0);
    } else {
        CHECK(Api.ReserveBatch(q,leases,n,&count)==PB_OK && count==n);
        for(ULONG i=0;i<n;++i)leases[i].Descriptor.Length=20;
        CHECK(Api.CommitBatch(q,leases,n,&count)==PB_OK && count==n);CheckCounters(q,0,n);
        PB_DESCRIPTOR native[PB_MAX_BATCH],output[PB_MAX_BATCH];uint32_t wake;
        CHECK(PbQueuePop(&s->TestRings[0][PB_QUEUE_RX_SUBMIT],native,n,&count)==PB_OK && count==n);
        CHECK(PbQueuePush(&s->TestRings[0][PB_QUEUE_RX_COMPLETE],native,n,&count,&wake)==PB_OK && count==n);
        uint32_t *received=&output[n-1].Token.Generation;
        CHECK(Api.PollCompletions(q,output,n,received)==PB_OK && *received==n && !s->Fatal);CheckCounters(q,0,0);
    }
    CloseSession(s);++Scenarios;
}
static VOID CompleteRx(PB_SESSION *s,PB_USER_QUEUE *q,ULONG n)
{
    PB_DESCRIPTOR native[PB_MAX_BATCH],output[PB_MAX_BATCH];uint32_t count,wake;
    CHECK(PbQueuePop(&s->TestRings[0][PB_QUEUE_RX_SUBMIT],native,n,&count)==PB_OK && count==n);
    CHECK(PbQueuePush(&s->TestRings[0][PB_QUEUE_RX_COMPLETE],native,n,&count,&wake)==PB_OK && count==n);
    CHECK(Api.PollCompletions(q,output,n,&count)==PB_OK && count==n);
}
static VOID CommitOutput(ULONG n,BOOL held,ULONG position)
{
    PB_SESSION *s=CreateSized(1,128);PB_USER_QUEUE *q;PB_LEASE leases[PB_MAX_BATCH+1];uint32_t count;
    CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,leases,n,&count)==PB_OK && count==n);
    if(held)CHECK(Api.ReserveBatch(q,leases+n,1,&count)==PB_OK && count==1);
    for(ULONG i=0;i<n;++i)leases[i].Descriptor.Length=20;
    uint32_t *output=(uint32_t *)(leases[held?n:position].Data+4096);
    uint64_t producer=q->Rings[PB_QUEUE_RX_SUBMIT].Cursor;PB_RESULT r=Api.CommitBatch(q,leases,n,output);
    if(!held) {
        CHECK(r==PB_INVALID && !*output && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==producer);CheckCounters(q,n,0);
        for(ULONG i=0;i<n;++i)CHECK(q->RxSlots[leases[i].Descriptor.Token.Slot].State==PB_SLOT_RESERVED);
        CHECK(Api.CommitBatch(q,leases,n,&count)==PB_OK && count==n);
    } else CHECK(r==PB_OK && *output==n);
    CheckCounters(q,held,n);CompleteRx(s,q,n);CheckCounters(q,held,0);
    if(held)CHECK(Api.CancelBatch(q,&leases[n].Descriptor.Token,1)==PB_OK);
    CloseSession(s);++Scenarios;
}
static VOID PartialPrefix(ULONG n,BOOL commit)
{
    PB_SESSION *s=CreateSized(1,128);PB_USER_QUEUE *q;PB_LEASE leases[PB_MAX_BATCH];PB_TOKEN tokens[PB_MAX_BATCH];uint32_t count,wake;
    if(commit) {
        CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,leases,n,&count)==PB_OK && count==n);
        for(ULONG i=0;i<n;++i)leases[i].Descriptor.Length=20;
    } else q=Borrow(s,leases,n);
    for(ULONG i=0;i<n;++i)tokens[i]=leases[i].Descriptor.Token;
    PB_QUEUE_VIEW *producer=&q->Rings[commit?PB_QUEUE_RX_SUBMIT:PB_QUEUE_TX_RECYCLE];
    PB_QUEUE_VIEW *consumer=&s->TestRings[0][commit?PB_QUEUE_RX_SUBMIT:PB_QUEUE_TX_RECYCLE];
    ULONG prefix=n/2,fill=producer->Capacity-prefix;PB_DESCRIPTOR dummy[PB_MAX_BATCH]={0};
    /* Deliberate ring pressure injection, independent of the valid private
     * slot ledger. It checks prefix accounting, not an ordinary traffic model. */
    for(ULONG i=0;i<fill;i+=PB_MAX_BATCH) {
        ULONG k=min(PB_MAX_BATCH,fill-i);CHECK(PbQueuePush(producer,dummy,k,&count,&wake)==PB_OK && count==k);
    }
    PB_RESULT r=commit?Api.CommitBatch(q,leases,n,&count):Api.ReleaseBatch(q,tokens,n,&count);
    CHECK(r==PB_OK && count==prefix);CheckCounters(q,n-prefix,commit?prefix:0);
    for(ULONG i=0;i<fill;i+=PB_MAX_BATCH) {
        ULONG k=min(PB_MAX_BATCH,fill-i);CHECK(PbQueuePop(consumer,dummy,k,&count)==PB_OK && count==k);
    }
    if(commit)CompleteRx(s,q,prefix);
    else CHECK(PbQueuePop(consumer,dummy,prefix,&count)==PB_OK && count==prefix);
    CheckCounters(q,n-prefix,0);
    r=commit?Api.CommitBatch(q,leases+prefix,n-prefix,&count):Api.ReleaseBatch(q,tokens+prefix,n-prefix,&count);
    CHECK(r==PB_OK && count==n-prefix);CheckCounters(q,0,commit?n-prefix:0);
    if(commit)CompleteRx(s,q,n-prefix);else CHECK(PbQueuePop(consumer,dummy,n-prefix,&count)==PB_OK && count==n-prefix);
    CheckCounters(q,0,0);CloseSession(s);++Scenarios;
}
#ifndef PB_BATCH_OUTPUT_BASELINE
static VOID Geometry(VOID)
{
    PB_SESSION *s=CreateSized(1,128);PB_LEASE tx[128],rx[128];PB_USER_QUEUE *q=Borrow(s,tx,128);uint32_t n;
    for(ULONG i=0;i<128;i+=PB_MAX_BATCH)CHECK(Api.ReserveBatch(q,rx+i,PB_MAX_BATCH,&n)==PB_OK && n==PB_MAX_BATCH);
    const intptr_t offsets[]={-8,-4,-3,-2,-1,0,1,3,PB_BLOCK_SIZE-4,PB_BLOCK_SIZE-3,PB_BLOCK_SIZE-2,PB_BLOCK_SIZE-1,PB_BLOCK_SIZE,PB_BLOCK_SIZE+1,PB_BLOCK_SIZE+4};
    for(ULONG direction=0;direction<2;++direction) {
        PB_LEASE *leases=direction?rx:tx;PB_TOKEN tokens[]={leases[0].Descriptor.Token,leases[127].Descriptor.Token};
        for(ULONG item=0;item<2;++item)for(ULONG i=0;i<ARRAYSIZE(offsets);++i) {
            uintptr_t start=(uintptr_t)leases[item?127:0].Data;intptr_t offset=offsets[i];
            BOOL overlaps=offset<(intptr_t)PB_BLOCK_SIZE && offset+(intptr_t)sizeof(uint32_t)>0;
            const uint32_t *output=(const uint32_t *)(uintptr_t)((intptr_t)start+offset);
            CHECK(CountInTransferredPacket(q,tokens,2,output,direction)==overlaps);++Scenarios;
        }
        CHECK(!CountInTransferredPacket(q,tokens,2,NULL,direction) && !CountInTransferredPacket(q,tokens,2,(const uint32_t *)UINTPTR_MAX,direction));
    }
    PB_TEST_GUARD inaccessible=PbTestGuardCreate(PB_MAX_BATCH*sizeof(PB_TOKEN));uint32_t external=0;
    CHECK(!CountInTransferredPacket(q,(const PB_TOKEN *)inaccessible.Base,PB_MAX_BATCH,&external,FALSE));
    PbTestGuardDestroy(&inaccessible); /* Ordinary external outputs never walk tokens here. */
    ReleaseAll(q,tx,128);
    for(ULONG i=0;i<128;i+=PB_MAX_BATCH) {
        PB_TOKEN tokens[PB_MAX_BATCH];for(ULONG j=0;j<PB_MAX_BATCH;++j)tokens[j]=rx[i+j].Descriptor.Token;
        CHECK(Api.CancelBatch(q,tokens,PB_MAX_BATCH)==PB_OK);
    }
    CheckCounters(q,0,0);CloseSession(s);
}
#endif
static VOID StoppedReturn(ULONG n,BOOL fatal)
{
    PB_SESSION *s=CreateSized(1,128);PB_LEASE leases[PB_MAX_BATCH];PB_USER_QUEUE *q=Borrow(s,leases,n);uint32_t count,wake;
    PB_TOKEN tokens[PB_MAX_BATCH];for(ULONG i=0;i<n;++i)tokens[i]=leases[i].Descriptor.Token;
    if(fatal)FaultSession(s);
    else {
        InterlockedExchange(&s->CloseRequested,1);SetEvent(s->ClosingEvent);
        PB_DESCRIPTOR dummy[PB_MAX_BATCH]={0};
        for(ULONG i=0;i<128;i+=PB_MAX_BATCH)CHECK(PbQueuePush(&q->Rings[PB_QUEUE_TX_RECYCLE],dummy,PB_MAX_BATCH,&count,&wake)==PB_OK && count==PB_MAX_BATCH);
    }
    uint32_t *bad=(uint32_t *)(leases[n-1].Data+4096);uint64_t producer=q->Rings[PB_QUEUE_TX_RECYCLE].Cursor;
    CHECK(Api.ReleaseBatch(q,tokens,n,bad)==PB_INVALID && !*bad && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==producer);CheckCounters(q,n,0);
    CHECK(Api.ReleaseBatch(q,tokens,n,&count)==PB_OK && count==n && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==producer);CheckCounters(q,0,0);
    PbTestActive(s,1);CHECK(Api.PrepareClose(s,0)==PB_QUARANTINED && !s->ResourcesClosed && s->Region);
    PbTestActive(s,0);CHECK(Api.PrepareClose(s,2000)==PB_OK);CloseSession(s);++Scenarios;
}
typedef struct RETURN_OWNER {PB_SESSION *Session;ULONG Count;HANDLE Invalid,Retry; } RETURN_OWNER;
static DWORD WINAPI ReturnOwner(VOID *opaque)
{
    RETURN_OWNER *owner=opaque;PB_LEASE leases[PB_MAX_BATCH];PB_USER_QUEUE *q=Borrow(owner->Session,leases,owner->Count);
    PB_TOKEN *tokens=(PB_TOKEN *)leases[0].Data;for(ULONG i=0;i<owner->Count;++i)tokens[i]=leases[i].Descriptor.Token;
    uint32_t *bad=(uint32_t *)(leases[0].Data+4096),accepted;
    CHECK(Api.ReleaseBatch(q,tokens,owner->Count,bad)==PB_INVALID && !*bad);
    CHECK(SetEvent(owner->Invalid) && WaitForSingleObject(owner->Retry,5000)==WAIT_OBJECT_0);
    AliasedCount=bad;Poison=UINT32_MAX;ExpectedTransitions=owner->Count;Transitions=Reused=0;PeerArmed=LimitTransitions=TRUE;
    CHECK(Api.ReleaseBatch(q,tokens,owner->Count,&accepted)==PB_OK && accepted==owner->Count && Transitions==owner->Count && Reused==owner->Count);
    LimitTransitions=PeerArmed=FALSE;AliasedCount=NULL;return 0;
}
static VOID ConcurrentReturn(ULONG n,BOOL earlyNative)
{
    PB_SESSION *s=CreateSized(1,128);PbTestActive(s,1);
    RETURN_OWNER owner={s,n,CreateEventW(NULL,FALSE,FALSE,NULL),CreateEventW(NULL,FALSE,FALSE,NULL)};
    PeerEntered=CreateEventW(NULL,FALSE,FALSE,NULL);PeerGate=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(owner.Invalid && owner.Retry && PeerEntered && PeerGate);
    HANDLE thread=CreateThread(NULL,0,ReturnOwner,&owner,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(owner.Invalid,5000)==WAIT_OBJECT_0 && Life.BeginStop(s)==PB_OK && Api.PrepareClose(s,0)==PB_BUSY);
    CHECK(SetEvent(owner.Retry) && WaitForSingleObject(PeerEntered,5000)==WAIT_OBJECT_0);
    CHECK(!TryAcquireSRWLockExclusive(&s->DataGate));CheckCounters(s->Queues,n,0);
    CHECK(Api.PrepareClose(s,0)==PB_BUSY && !s->ResourcesClosed && s->Region);
    if(earlyNative)PbTestActive(s,0);
    CHECK(SetEvent(PeerGate) && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));
    CheckCounters(s->Queues,0,0);PbTestActive(s,0);CHECK(Api.PrepareClose(s,2000)==PB_OK && !s->ResourcesClosed);
    CHECK(CloseHandle(owner.Invalid) && CloseHandle(owner.Retry) && CloseHandle(PeerEntered) && CloseHandle(PeerGate));PeerEntered=PeerGate=NULL;
    CloseSession(s);++Scenarios;
}
static VOID Matrix(VOID)
{
    for(ULONG n=1;n<=PB_MAX_BATCH;++n)ReturnOutput(n,FALSE,FALSE,n-1,0,FALSE);
    const ULONG counts[]={1,2,3,7,8,31,63,64};
    for(ULONG c=0;c<ARRAYSIZE(counts);++c) {
        ULONG n=counts[c];ReturnOutput(n,FALSE,TRUE,0,UINT32_MAX,FALSE);
        ReturnOutput(n,TRUE,TRUE,0,UINT32_MAX,FALSE);CountStorage(n,FALSE);CountStorage(n,TRUE);
        CommitOutput(n,FALSE,0);CommitOutput(n,FALSE,n-1);CommitOutput(n,TRUE,0);
        if(n>1){PartialPrefix(n,FALSE);PartialPrefix(n,TRUE);}
        StoppedReturn(n,FALSE);StoppedReturn(n,TRUE);
        ConcurrentReturn(n,FALSE);ConcurrentReturn(n,TRUE);
    }
#ifndef PB_BATCH_OUTPUT_BASELINE
    Geometry();
#endif
}
int main(int argc,char **argv)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    if(argc>1)ReturnOutput(2,FALSE,FALSE,0,!strcmp(argv[1],"--baseline-overflow")?UINT32_MAX:0,TRUE);
    else {
        DWORD handles[3];
        for(ULONG pass=0;pass<3;++pass) {
            Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),handles+pass));
            CHECK(!BoHandles && !BoPools && !BoWorks && !GuardAllocations);
            if(pass && handles[pass]>handles[0]){fprintf(stderr,"batch handles: warm=%lu pass=%lu current=%lu scenarios=%lu\n",handles[0],pass,handles[pass],Scenarios);ExitProcess(1);}
        }
        printf("PASS: full-matrix warmup/measured process handles=%lu/%lu/%lu; all application handles/pools/work/allocations returned after each session.\n",handles[0],handles[1],handles[2]);
    }
    printf("PASS: %lu actual batch-output schedules; transferred output storage rejected before publication, held-loan output retained, input alias still valid, private counters independent of overlapping output metadata and zero retained resources.\n",Scenarios);return 0;
}
