/* SPDX-License-Identifier: GPL-2.0 */
/* Actual recycle producer versus the old staged writer: exact wire contents,
 * guarded inputs, publication/arm schedules and concurrent native event waits. */
#include "../safety/guard_memory.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK SAFETY_CHECK
static volatile __int64 *WatchedProducer;
static ULONG Publications;
static VOID (*PublishHook)(BOOLEAN after);
static __int64 Publish64(volatile __int64 *p,__int64 value)
{
    BOOLEAN watched=p==WatchedProducer;
    if(watched && PublishHook) PublishHook(FALSE);
    __int64 old=_InterlockedExchange64(p,value);
    if(watched) {Publications++;if(PublishHook) PublishHook(TRUE);}
    return old;
}
#define _InterlockedExchange64 Publish64
#ifndef PB_TOKEN_PUBLISH_CORE
#define PB_TOKEN_PUBLISH_CORE "../../common/v2_core.c"
#endif
#include PB_TOKEN_PUBLISH_CORE
#undef _InterlockedExchange64
static PB_RESULT StagedPush(PB_QUEUE_VIEW *v,const PB_TOKEN *tokens,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    if(accepted) *accepted=0;
    if(notify) *notify=0;
    if(!tokens || !count || count>PB_MAX_BATCH) return PB_INVALID;
    PB_DESCRIPTOR items[PB_MAX_BATCH]={0};
    for(ULONG i=0;i<count;++i) items[i].Token=tokens[i];
    return PbQueuePush(v,items,count,accepted,notify);
}
#ifdef PB_TOKEN_PUBLISH_BASELINE
#define WriteTokens StagedPush
#else
#define WriteTokens PbQueuePushTokens
#endif
typedef struct TOKEN_RING {PB_TEST_GUARD Memory;PB_QUEUE_VIEW P,C;} TOKEN_RING;
static TOKEN_RING MakeRing(ULONG capacity,ULONG64 base,ULONG used,ULONG armed)
{
    TOKEN_RING r={0};SIZE_T size=sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR);
    r.Memory=PbTestGuardCreate(size);r.P.Shared=(PB_QUEUE_SHARED *)PbTestGuardSpan(&r.Memory,size);
    memset(r.P.Shared,0,size);r.P.Entries=(PB_DESCRIPTOR *)(r.P.Shared+1);
    memset(r.P.Entries,0xa5,capacity*sizeof(PB_DESCRIPTOR));r.P.Capacity=capacity;r.P.Producer=1;
    r.C=r.P;r.C.Producer=0;r.C.Cursor=r.C.PeerCursor=base;
    r.P.Cursor=base+used;r.P.PeerCursor=base;
    r.P.Shared->Producer=r.P.Cursor;r.P.Shared->Consumer=base;r.P.Shared->Armed=armed;
    return r;
}
static PB_TOKEN Token(ULONG serial)
{PB_TOKEN t={0x123456789abcdef0ull,serial+1,(uint16_t)serial,(uint16_t)(serial>>16)};return t;}
static PB_DESCRIPTOR Item(ULONG serial)
{PB_DESCRIPTOR d={0};d.Token=Token(serial);return d;}
static VOID Matrix(VOID)
{
    const ULONG capacities[]={1,2,8,32,64,4096};ULONG cases=0;
    PB_TEST_GUARD input=PbTestGuardCreate(PB_MAX_BATCH*sizeof(PB_TOKEN));
    for(ULONG ci=0;ci<_countof(capacities);++ci) {
        ULONG capacity=capacities[ci],limit=min(capacity,PB_MAX_BATCH+1);
        for(ULONG space=0;space<=limit;++space) for(ULONG count=1;count<=PB_MAX_BATCH;++count)
            for(ULONG wrap=0;wrap<2;++wrap) {
                ULONG64 base=wrap?UINT64_MAX-15:0;
                TOKEN_RING actual=MakeRing(capacity,base,capacity-space,cases&1);
                TOKEN_RING expected=MakeRing(capacity,base,capacity-space,cases&1);
                PB_TOKEN *tokens=(PB_TOKEN *)PbTestGuardSpan(&input,count*sizeof(*tokens));
                for(ULONG i=0;i<count;++i) tokens[i]=Token(i);
                uint32_t n,notify,expectedN,expectedNotify;
                WatchedProducer=(volatile __int64 *)&actual.P.Shared->Producer;Publications=0;
                PB_RESULT r=WriteTokens(&actual.P,tokens,count,&n,&notify);WatchedProducer=NULL;
                PB_RESULT reference=StagedPush(&expected.P,tokens,count,&expectedN,&expectedNotify);
                CHECK(r==reference && n==min(space,count) && n==expectedN && notify==expectedNotify);
                CHECK(Publications==(ULONG)(n!=0));
                CHECK(actual.P.Cursor==expected.P.Cursor && actual.P.PeerCursor==expected.P.PeerCursor);
                CHECK(!memcmp(actual.P.Shared,expected.P.Shared,sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR)));
                PbTestGuardDestroy(&actual.Memory);PbTestGuardDestroy(&expected.Memory);cases++;
            }
    }
    PbTestGuardDestroy(&input);
    printf("PASS: recycle producer differential matrix: %lu cases; exact 64-byte wire descriptors, zero metadata, untouched suffix, guarded inputs/destinations, partial/full/wrapped rings and identical publication/notification.\n",cases);
}
static VOID Rejections(VOID)
{
    TOKEN_RING r=MakeRing(8,20,0,1);PB_TOKEN token=Token(1);uint32_t n=99,notify=99;
    PB_DESCRIPTOR before[8];memcpy(before,r.P.Entries,sizeof(before));
    CHECK(WriteTokens(NULL,&token,1,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(WriteTokens(&r.P,NULL,1,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(WriteTokens(&r.P,&token,0,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(WriteTokens(&r.P,&token,PB_MAX_BATCH+1,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(WriteTokens(&r.P,&token,1,NULL,&notify)==PB_INVALID && !notify);
    CHECK(WriteTokens(&r.P,&token,1,&n,NULL)==PB_INVALID && !n);
    CHECK(WriteTokens(&r.C,&token,1,&n,&notify)==PB_INVALID && !n && !notify);
    PB_QUEUE_VIEW invalid=r.P;invalid.Shared=NULL;
    CHECK(WriteTokens(&invalid,&token,1,&n,&notify)==PB_INVALID && !n && !notify);
    WatchedProducer=(volatile __int64 *)&r.P.Shared->Producer;Publications=0;
    for(ULONG i=0;i<3;++i) {
        r.P.PeerCursor=20;r.P.Shared->Consumer=i==0?21:i==1?11:20;
        if(i==2) r.P.PeerCursor=22;
        CHECK(WriteTokens(&r.P,&token,1,&n,&notify)==PB_CORRUPT && !n && !notify);
        CHECK(r.P.Cursor==20 && r.P.Shared->Producer==20 && r.P.Shared->Armed==1);
        CHECK(!memcmp(before,r.P.Entries,sizeof(before)));
    }
    CHECK(!Publications);WatchedProducer=NULL;PbTestGuardDestroy(&r.Memory);
    puts("PASS: invalid inputs, wrong direction and corrupt/regressed consumer cursors leave entries, producer publication and arm unchanged.");
}
static TOKEN_RING *Scheduled;
static ULONG ScheduledCount;
static PB_TOKEN *ScheduledInput;
static VOID ObservePublication(BOOLEAN after)
{
    TOKEN_RING *r=Scheduled;
    for(ULONG i=0;i<ScheduledCount;++i) {
        PB_DESCRIPTOR expected=Item(i);
        CHECK(!memcmp(&r->P.Entries[(ULONG)(r->P.Cursor-ScheduledCount+i)&(r->P.Capacity-1)],&expected,sizeof(expected)));
    }
    if(!after) CHECK(r->P.Shared->Producer==r->P.Cursor-ScheduledCount);
    else {
        CHECK(r->P.Shared->Producer==r->P.Cursor);
        PB_DESCRIPTOR output[PB_MAX_BATCH];uint32_t n;
        CHECK(PbQueuePop(&r->C,output,ScheduledCount,&n)==PB_OK && n==ScheduledCount);
        for(ULONG i=0;i<n;++i) {PB_DESCRIPTOR expected=Item(i);CHECK(!memcmp(output+i,&expected,sizeof(expected)));}
        /* Consumers may release every destination before the writer returns. */
        uint32_t ready;CHECK(PbQueueArm(&r->C,&ready)==PB_OK && !ready);
        memset(ScheduledInput,0xcc,ScheduledCount*sizeof(*ScheduledInput));PublishHook=NULL;
    }
}
static VOID Schedules(ULONG64 base)
{
    for(ULONG count=1;count<=PB_MAX_BATCH;count+=PB_MAX_BATCH-1) {
        TOKEN_RING r=MakeRing(PB_MAX_BATCH,base,0,0);PB_TOKEN tokens[PB_MAX_BATCH];
        for(ULONG i=0;i<count;++i) tokens[i]=Token(i);
        Scheduled=&r;ScheduledCount=count;ScheduledInput=tokens;PublishHook=ObservePublication;
        WatchedProducer=(volatile __int64 *)&r.P.Shared->Producer;Publications=0;
        uint32_t n,notify;CHECK(WriteTokens(&r.P,tokens,count,&n,&notify)==PB_OK && n==count && notify==1);
        CHECK(!PublishHook && Publications==1 && r.P.Shared->Consumer==r.P.Shared->Producer);
        WatchedProducer=NULL;PbTestGuardDestroy(&r.Memory);
    }
    for(ULONG schedule=0;schedule<3;++schedule) {
        TOKEN_RING r=MakeRing(1,base,0,0);PB_TOKEN token=Token(9);uint32_t n,notify,ready;
        if(schedule) CHECK(PbQueueArm(&r.C,&ready)==PB_OK && !ready);
        CHECK(WriteTokens(&r.P,&token,1,&n,&notify)==PB_OK && n==1 && notify==(ULONG)(schedule!=0));
        if(schedule!=2) CHECK(PbQueueArm(&r.C,&ready)==PB_OK && ready);
        else {HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(event && SetEvent(event));CHECK(WaitForSingleObject(event,1000)==WAIT_OBJECT_0 && CloseHandle(event));}
        PB_DESCRIPTOR out,expected=Item(9);CHECK(PbQueuePop(&r.C,&out,1,&n)==PB_OK && n==1 && !memcmp(&out,&expected,sizeof(out)));
        PbTestGuardDestroy(&r.Memory);
    }
    printf("PASS: recycle publish-before-cursor, immediate consumer reuse/arm and before/after-wait schedules, wrap=%u.\n",base!=0);
}
#define STRESS_COUNT 200000u
typedef struct STRESS {TOKEN_RING Ring;HANDLE Event;} STRESS;
static DWORD WINAPI Produce(VOID *opaque)
{
    STRESS *s=opaque;ULONG next=0;
    while(next<STRESS_COUNT) {
        PB_TOKEN tokens[PB_MAX_BATCH];ULONG count=min(PB_MAX_BATCH,STRESS_COUNT-next);
        for(ULONG i=0;i<count;++i) tokens[i]=Token(next+i);
        uint32_t n,notify;PB_RESULT r=WriteTokens(&s->Ring.P,tokens,count,&n,&notify);
        CHECK(r==PB_OK || r==PB_FULL);next+=n;if(notify) CHECK(SetEvent(s->Event));if(!n) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI Consume(VOID *opaque)
{
    STRESS *s=opaque;ULONG next=0;
    while(next<STRESS_COUNT) {
        PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t n;PB_RESULT r=PbQueuePop(&s->Ring.C,items,PB_MAX_BATCH,&n);
        CHECK(r==PB_OK || r==PB_EMPTY);
        for(ULONG i=0;i<n;++i) {PB_DESCRIPTOR expected=Item(next++);CHECK(!memcmp(items+i,&expected,sizeof(expected)));}
        if(!n) {uint32_t ready;CHECK(PbQueueArm(&s->Ring.C,&ready)==PB_OK);if(!ready) CHECK(WaitForSingleObject(s->Event,5000)==WAIT_OBJECT_0);}
    }
    return 0;
}
static VOID Concurrent(ULONG capacity)
{
    ULONG64 base=UINT64_MAX-31;STRESS s={MakeRing(capacity,base,0,0),CreateEventW(NULL,FALSE,FALSE,NULL)};CHECK(s.Event);
    HANDLE threads[]={CreateThread(NULL,0,Produce,&s,0,NULL),CreateThread(NULL,0,Consume,&s,0,NULL)};
    CHECK(threads[0] && threads[1] && WaitForMultipleObjects(2,threads,TRUE,20000)==WAIT_OBJECT_0);
    CHECK(s.Ring.P.Cursor==base+STRESS_COUNT && s.Ring.C.Cursor==s.Ring.P.Cursor);
    CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]) && CloseHandle(s.Event));PbTestGuardDestroy(&s.Ring.Memory);
    printf("PASS: concurrent recycle producer/full-descriptor consumer: capacity=%lu tokens=%u, native event waits, zero metadata and uint64 wrap.\n",capacity,STRESS_COUNT);
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Matrix();Rejections();Schedules(0);Schedules(UINT64_MAX);Concurrent(1);Concurrent(64);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
