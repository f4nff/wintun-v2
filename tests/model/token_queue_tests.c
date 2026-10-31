/* SPDX-License-Identifier: GPL-2.0 */
/* Actual core: token snapshots versus full descriptors, guarded copy footprint,
 * publication/reuse ordering, arm races and concurrent native event waits. */
#include "../safety/guard_memory.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK SAFETY_CHECK
static volatile __int64 *WatchedConsumer;
static ULONG Publications;
static VOID (*ArmHook)(BOOLEAN after);
static VOID (*StoreHook)(VOID);
static long TokenArm32(volatile long *p,long value)
{
    if(value==1 && ArmHook) ArmHook(FALSE);
    long old=_InterlockedExchange(p,value);
    if(value==1 && ArmHook) ArmHook(TRUE);
    return old;
}
static __int64 TokenPublish64(volatile __int64 *p,__int64 value)
{
    __int64 old=_InterlockedExchange64(p,value);
    if(p==WatchedConsumer) {Publications++;if(StoreHook) StoreHook();}
    return old;
}
#define _InterlockedExchange64 TokenPublish64
#define _InterlockedExchange TokenArm32
#ifndef PB_TOKEN_CORE_SOURCE
#define PB_TOKEN_CORE_SOURCE "../../common/v2_core.c"
#endif
#include PB_TOKEN_CORE_SOURCE
#undef _InterlockedExchange64
#undef _InterlockedExchange
#ifdef PB_TOKEN_QUEUE_BASELINE
/* Frozen full-descriptor reader followed by the old caller's token extraction. */
static PB_RESULT ReadTokens(PB_QUEUE_VIEW *v,PB_TOKEN *tokens,uint32_t count,uint32_t *received)
{
    if(received) *received=0;
    if(!received || !tokens || !count || count>PB_MAX_BATCH) return PB_INVALID;
    PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t n;
    PB_RESULT r=PbQueuePop(v,items,count,&n);
    if(r==PB_OK) {for(uint32_t i=0;i<n;++i) tokens[i]=items[i].Token;*received=n;}
    return r;
}
#else
#define ReadTokens PbQueuePopTokens
#endif
typedef struct TOKEN_RING {PB_TEST_GUARD Memory;PB_QUEUE_VIEW P,C;} TOKEN_RING;
static TOKEN_RING MakeTokenRing(ULONG capacity)
{
    TOKEN_RING r={0};r.Memory=PbTestGuardCreate(sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR));
    r.P.Shared=(PB_QUEUE_SHARED *)PbTestGuardSpan(&r.Memory,sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR));
    r.P.Entries=(PB_DESCRIPTOR *)(r.P.Shared+1);r.P.Capacity=capacity;r.P.Producer=1;
    r.C=r.P;r.C.Producer=0;return r;
}
static PB_DESCRIPTOR TokenItem(ULONG serial)
{
    PB_DESCRIPTOR d;memset(&d,(int)(serial^0xa5),sizeof(d));d.Token.Epoch=0x123456789abcdef0ull;
    d.Token.Generation=serial+1;d.Token.Slot=(uint16_t)serial;d.Token.Queue=(uint16_t)(serial>>16);
    d.Length=UINT32_MAX;d.DataOffset=~(uint64_t)serial;return d;
}
static VOID ResetTokens(TOKEN_RING *r,ULONG64 base,ULONG available,ULONG armed)
{
    memset(r->P.Shared,0,sizeof(*r->P.Shared));r->P.Shared->Producer=base+available;r->P.Shared->Consumer=base;r->P.Shared->Armed=armed;
    r->P.Cursor=base+available;r->P.PeerCursor=base;r->C.Cursor=r->C.PeerCursor=base;
    for(ULONG i=0;i<available;++i) r->P.Entries[(ULONG)(base+i)&(r->P.Capacity-1)]=TokenItem(i);
}
static VOID TokenMatrix(VOID)
{
    const ULONG capacities[]={1,2,8,32,64,4096};ULONG cases=0;
    PB_TEST_GUARD memory=PbTestGuardCreate(PB_MAX_BATCH*sizeof(PB_TOKEN));
    for(ULONG ci=0;ci<_countof(capacities);++ci) {
        TOKEN_RING r=MakeTokenRing(capacities[ci]),reference=MakeTokenRing(capacities[ci]);
        ULONG limit=min(capacities[ci],33);
        for(ULONG ai=0;ai<=limit+1;++ai) for(ULONG count=1;count<=PB_MAX_BATCH;++count) for(ULONG wrap=0;wrap<2;++wrap) {
            ULONG available=ai>limit?capacities[ci]:ai;ULONG64 base=wrap?UINT64_MAX-15:0;
            ResetTokens(&r,base,available,cases&1);ResetTokens(&reference,base,available,cases&1);
            PB_TOKEN *tokens=(PB_TOKEN *)PbTestGuardSpan(&memory,count*sizeof(*tokens));memset(tokens,0xcc,count*sizeof(*tokens));
            PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t n,expectedN;
            WatchedConsumer=(volatile __int64 *)&r.C.Shared->Consumer;Publications=0;
            PB_RESULT result=ReadTokens(&r.C,tokens,count,&n);WatchedConsumer=NULL;
            PB_RESULT expected=PbQueuePop(&reference.C,items,count,&expectedN);
            CHECK(result==expected && n==min(count,available) && n==expectedN && Publications==(ULONG)(n!=0));
            CHECK(r.C.Cursor==reference.C.Cursor && r.C.PeerCursor==reference.C.PeerCursor);
            CHECK(!memcmp(r.P.Shared,reference.P.Shared,sizeof(*r.P.Shared)));
            for(ULONG i=0;i<n;++i) CHECK(!memcmp(tokens+i,&items[i].Token,sizeof(*tokens)));
            for(ULONG i=n*sizeof(*tokens);i<count*sizeof(*tokens);++i) CHECK(((UCHAR *)tokens)[i]==0xcc);
            cases++;
        }
        PbTestGuardDestroy(&r.Memory);PbTestGuardDestroy(&reference.Memory);
    }
    PbTestGuardDestroy(&memory);
    printf("PASS: token/full-descriptor differential matrix: %lu cases, guarded output, arbitrary non-token bytes, capacities 1..4096, partial batches and uint64 wrap.\n",cases);
}
static VOID TokenRejections(VOID)
{
    TOKEN_RING r=MakeTokenRing(8);PB_TOKEN tokens[PB_MAX_BATCH],before[PB_MAX_BATCH];uint32_t n=99;
    memset(tokens,0xcc,sizeof(tokens));memcpy(before,tokens,sizeof(tokens));ResetTokens(&r,20,1,1);
    WatchedConsumer=(volatile __int64 *)&r.C.Shared->Consumer;Publications=0;
    CHECK(ReadTokens(NULL,tokens,1,&n)==PB_INVALID && !n);
    CHECK(ReadTokens(&r.C,NULL,1,&n)==PB_INVALID && !n);
    CHECK(ReadTokens(&r.C,tokens,1,NULL)==PB_INVALID);
    CHECK(ReadTokens(&r.C,tokens,0,&n)==PB_INVALID && !n);
    CHECK(ReadTokens(&r.C,tokens,PB_MAX_BATCH+1,&n)==PB_INVALID && !n);
    CHECK(ReadTokens(&r.P,tokens,1,&n)==PB_INVALID && !n);
    PB_QUEUE_VIEW invalid=r.C;invalid.Shared=NULL;CHECK(ReadTokens(&invalid,tokens,1,&n)==PB_INVALID && !n);
    for(ULONG i=0;i<3;++i) {
        ResetTokens(&r,20,1,1);
        if(i==0) r.P.Shared->Producer=29;
        if(i==1) r.P.Shared->Producer=19;
        if(i==2) r.C.PeerCursor=22;
        CHECK(ReadTokens(&r.C,tokens,1,&n)==PB_CORRUPT && !n);
        CHECK(r.C.Cursor==20 && r.P.Shared->Consumer==20 && r.P.Shared->Armed==1);
    }
    CHECK(!Publications && !memcmp(tokens,before,sizeof(tokens)));WatchedConsumer=NULL;PbTestGuardDestroy(&r.Memory);
    puts("PASS: null/invalid requests and corrupt or regressed producer cursors leave output, consumption and arm unchanged.");
}
static VOID TokenFootprint(VOID)
{
    /* Deliberately partial synthetic entry: measure reads only. Real rings map
     * full 64-byte descriptors. No claim of a production access violation. */
    _Static_assert(offsetof(PB_DESCRIPTOR,Token)==0,"token prefix");
    PB_TEST_GUARD memory=PbTestGuardCreate(sizeof(PB_TOKEN));PB_DESCRIPTOR d=TokenItem(9);
    memcpy(PbTestGuardSpan(&memory,sizeof(PB_TOKEN)),&d.Token,sizeof(PB_TOKEN));
    PB_QUEUE_SHARED shared={0};shared.Producer=1;PB_QUEUE_VIEW v={0};
    v.Shared=&shared;v.Entries=(PB_DESCRIPTOR *)PbTestGuardSpan(&memory,sizeof(PB_TOKEN));v.Capacity=1;
    PB_TOKEN token;uint32_t n;ULONG faults=0;
    __try {CHECK(ReadTokens(&v,&token,1,&n)==PB_OK && n==1 && !memcmp(&token,&d.Token,sizeof(token)));}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {faults++;}
#ifdef PB_TOKEN_QUEUE_BASELINE
    CHECK(faults==1 && !shared.Consumer);
#else
    CHECK(!faults && shared.Consumer==1);
#endif
    printf("PASS: synthetic guarded 16-byte read footprint: trailing-descriptor access faults=%lu.\n",faults);
    PbTestGuardDestroy(&memory);
}
static TOKEN_RING *Scheduled;
static HANDLE ScheduledEvent;
static ULONG ScheduledSerial,Notifications;
static BOOLEAN AfterArm;
static VOID PublishToken(TOKEN_RING *r,ULONG serial)
{
    PB_DESCRIPTOR d=TokenItem(serial);uint32_t n,notify;CHECK(PbQueuePush(&r->P,&d,1,&n,&notify)==PB_OK && n==1);
    if(notify) {Notifications++;CHECK(SetEvent(ScheduledEvent));}
}
static VOID InjectTokenArm(BOOLEAN after)
{if(after==AfterArm) {ArmHook=NULL;PublishToken(Scheduled,ScheduledSerial);}}
static VOID InjectTokenReuse(VOID)
{StoreHook=NULL;PublishToken(Scheduled,ScheduledSerial);}
static VOID TakeToken(TOKEN_RING *r,ULONG serial)
{
    PB_TOKEN token;PB_DESCRIPTOR expected=TokenItem(serial);uint32_t n;
    CHECK(ReadTokens(&r->C,&token,1,&n)==PB_OK && n==1 && !memcmp(&token,&expected.Token,sizeof(token)));
}
static VOID TokenSchedules(ULONG64 base)
{
    ScheduledEvent=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(ScheduledEvent);
    for(ULONG schedule=0;schedule<4;++schedule) {
        TOKEN_RING r=MakeTokenRing(1);ResetTokens(&r,base,0,0);Scheduled=&r;ScheduledSerial=100+schedule;Notifications=0;
        uint32_t ready;if(schedule==0) PublishToken(&r,ScheduledSerial);
        if(schedule==1 || schedule==2) {AfterArm=schedule==2;ArmHook=InjectTokenArm;}
        CHECK(PbQueueArm(&r.C,&ready)==PB_OK && !ArmHook);
        if(schedule<3) CHECK(ready && Notifications==(ULONG)(schedule==2));
        else {CHECK(!ready);PublishToken(&r,ScheduledSerial);CHECK(Notifications==1 && WaitForSingleObject(ScheduledEvent,1000)==WAIT_OBJECT_0);}
        TakeToken(&r,ScheduledSerial);PbTestGuardDestroy(&r.Memory);
    }
    TOKEN_RING r=MakeTokenRing(1);ResetTokens(&r,base,0,0);Scheduled=&r;PublishToken(&r,200);ScheduledSerial=201;
    WatchedConsumer=(volatile __int64 *)&r.C.Shared->Consumer;StoreHook=InjectTokenReuse;
    TakeToken(&r,200);CHECK(!StoreHook);TakeToken(&r,201);WatchedConsumer=NULL;
    PbTestGuardDestroy(&r.Memory);Scheduled=NULL;CHECK(CloseHandle(ScheduledEvent));
    printf("PASS: token arm/publish and immediate slot reuse schedules: wrap=%u, exact snapshot and no lost wake.\n",base!=0);
}
#define TOKEN_STRESS_COUNT 200000u
typedef struct TOKEN_STRESS {TOKEN_RING R;HANDLE Event;} TOKEN_STRESS;
static DWORD WINAPI TokenProduce(VOID *opaque)
{
    TOKEN_STRESS *s=opaque;PB_DESCRIPTOR items[PB_MAX_BATCH];ULONG next=0;
    while(next<TOKEN_STRESS_COUNT) {
        ULONG count=min(PB_MAX_BATCH,TOKEN_STRESS_COUNT-next);for(ULONG i=0;i<count;++i) items[i]=TokenItem(next+i);
        uint32_t n,notify;PB_RESULT r=PbQueuePush(&s->R.P,items,count,&n,&notify);CHECK(r==PB_OK || r==PB_FULL);
        next+=n;if(notify) CHECK(SetEvent(s->Event));if(!n) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI TokenConsume(VOID *opaque)
{
    TOKEN_STRESS *s=opaque;ULONG next=0;
    while(next<TOKEN_STRESS_COUNT) {
        PB_TOKEN tokens[PB_MAX_BATCH];uint32_t n;PB_RESULT r=ReadTokens(&s->R.C,tokens,PB_MAX_BATCH,&n);CHECK(r==PB_OK || r==PB_EMPTY);
        for(ULONG i=0;i<n;++i) {PB_DESCRIPTOR expected=TokenItem(next++);CHECK(!memcmp(tokens+i,&expected.Token,sizeof(*tokens)));}
        if(!n) {uint32_t ready;CHECK(PbQueueArm(&s->R.C,&ready)==PB_OK);if(!ready) CHECK(WaitForSingleObject(s->Event,5000)==WAIT_OBJECT_0);}
    }
    return 0;
}
static VOID TokenConcurrent(ULONG capacity)
{
    TOKEN_STRESS s={MakeTokenRing(capacity),CreateEventW(NULL,FALSE,FALSE,NULL)};CHECK(s.Event);ULONG64 base=UINT64_MAX-31;
    ResetTokens(&s.R,base,0,0);
    HANDLE threads[]={CreateThread(NULL,0,TokenProduce,&s,0,NULL),CreateThread(NULL,0,TokenConsume,&s,0,NULL)};
    CHECK(threads[0] && threads[1] && WaitForMultipleObjects(2,threads,TRUE,20000)==WAIT_OBJECT_0);
    CHECK(s.R.P.Cursor==base+TOKEN_STRESS_COUNT && s.R.C.Cursor==s.R.P.Cursor);
    CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]) && CloseHandle(s.Event));PbTestGuardDestroy(&s.R.Memory);
    printf("PASS: concurrent full-descriptor producer/token consumer: capacity=%lu tokens=%u, real event waits and uint64 wrap.\n",capacity,TOKEN_STRESS_COUNT);
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    TokenMatrix();TokenRejections();TokenFootprint();TokenSchedules(0);TokenSchedules(UINT64_MAX);
    TokenConcurrent(1);TokenConcurrent(64);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
