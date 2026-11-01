/* SPDX-License-Identifier: GPL-2.0 */
/* Actual core with guarded source/destination rings and publication hooks.
 * Compare descriptor contents, partial acceptance and notification semantics
 * with the contiguous API; then exercise real concurrent arm/reuse schedules. */
#include "../safety/guard_memory.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK SAFETY_CHECK
static BOOLEAN Monitor;
static ULONG Publications,ArmExchanges;
static __int64 Publish64(volatile __int64 *p,__int64 value)
{ if(Monitor) Publications++; return _InterlockedExchange64(p,value); }
static long Arm32(volatile long *p,long value)
{ if(Monitor) ArmExchanges++; return _InterlockedExchange(p,value); }
#define _InterlockedExchange64 Publish64
#define _InterlockedExchange Arm32
#include "../../common/v2_core.c"
#undef _InterlockedExchange64
#undef _InterlockedExchange

typedef struct RING {PB_TEST_GUARD Memory;PB_QUEUE_VIEW P,C;} RING;
static RING MakeRing(ULONG capacity)
{
    RING r={0};r.Memory=PbTestGuardCreate(sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR));
    r.P.Shared=(PB_QUEUE_SHARED *)PbTestGuardSpan(&r.Memory,sizeof(PB_QUEUE_SHARED)+capacity*sizeof(PB_DESCRIPTOR));
    r.P.Entries=(PB_DESCRIPTOR *)(r.P.Shared+1);r.P.Capacity=capacity;r.P.Producer=1;
    r.C=r.P;r.C.Producer=0;return r;
}
static VOID ResetRing(RING *r,ULONG64 base,ULONG space,ULONG armed)
{
    memset(r->P.Shared,0x5a,sizeof(PB_QUEUE_SHARED)+r->P.Capacity*sizeof(PB_DESCRIPTOR));
    r->P.Cursor=base;r->P.PeerCursor=base-(r->P.Capacity-space);
    r->P.Shared->Producer=base;r->P.Shared->Consumer=r->P.PeerCursor;r->P.Shared->Armed=armed;
}
static PB_DESCRIPTOR Item(ULONG serial)
{
    PB_DESCRIPTOR d;memset(&d,(int)(serial^0xa5),sizeof(d));d.Token.Epoch=42;
    d.Token.Generation=serial+1;d.Length=20;d.DataOffset=~(uint64_t)serial;return d;
}
static VOID Matrix(VOID)
{
    const ULONG capacities[]={1,2,4,8,32,64,4096},destinations[]={1,2,8,32,64};ULONG cases=0;
    for(ULONG si=0;si<_countof(capacities);++si) {
        ULONG capacity=capacities[si];PB_TEST_GUARD memory=PbTestGuardCreate(capacity*sizeof(PB_DESCRIPTOR));
        PB_DESCRIPTOR *source=(PB_DESCRIPTOR *)PbTestGuardSpan(&memory,capacity*sizeof(*source));
        for(ULONG i=0;i<capacity;++i) source[i]=Item(i);
        DWORD old;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,PAGE_READONLY,&old));
        for(ULONG di=0;di<_countof(destinations);++di) {
            RING actual=MakeRing(destinations[di]),reference=MakeRing(destinations[di]);
            const ULONG heads[]={0,capacity/2,capacity-1};
            for(ULONG h=0;h<_countof(heads);++h) for(ULONG count=1;count<=min(capacity,PB_MAX_BATCH);++count)
                for(ULONG space=0;space<=actual.P.Capacity;++space) for(ULONG wrap=0;wrap<2;++wrap) {
                    ULONG64 base=wrap?UINT64_MAX-15:actual.P.Capacity-1;ULONG armed=cases&1;
                    ResetRing(&actual,base,space,armed);ResetRing(&reference,base,space,armed);
                    PB_DESCRIPTOR linear[PB_MAX_BATCH];for(ULONG i=0;i<count;++i) linear[i]=source[(heads[h]+i)&(capacity-1)];
                    uint32_t n,notify,expectedN,expectedNotify;Publications=ArmExchanges=0;Monitor=TRUE;
                    PB_RESULT result=PbQueuePushRing(&actual.P,source,capacity,heads[h],count,&n,&notify);Monitor=FALSE;
                    PB_RESULT expected=PbQueuePush(&reference.P,linear,count,&expectedN,&expectedNotify);
                    CHECK(result==expected && n==min(count,space) && n==expectedN && notify==expectedNotify);
                    CHECK(Publications==(ULONG)(n!=0) && ArmExchanges==Publications);
                    CHECK(actual.P.Cursor==base+n && actual.P.PeerCursor==reference.P.PeerCursor);
                    CHECK(!memcmp(actual.P.Shared,reference.P.Shared,sizeof(PB_QUEUE_SHARED)+actual.P.Capacity*sizeof(PB_DESCRIPTOR)));
                    for(ULONG i=0;i<n;++i) CHECK(!memcmp(actual.P.Entries+((ULONG)(base+i)&(actual.P.Capacity-1)),linear+i,sizeof(*linear)));
                    cases++;
                }
            PbTestGuardDestroy(&actual.Memory);PbTestGuardDestroy(&reference.Memory);
        }
        DWORD ignored;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,old,&ignored));PbTestGuardDestroy(&memory);
    }
    printf("PASS: circular source matrix: %lu guarded cases, read-only source, tiny/4096-entry source, independent wraps, partial/full ring, exact descriptor bytes and one publication/arm exchange.\n",cases);
}
static VOID Rejections(VOID)
{
    RING r=MakeRing(8);PB_DESCRIPTOR source[8]={0};uint32_t n,notify;
    const ULONG invalid[][3]={{0,0,1},{3,0,1},{8192,0,1},{UINT32_MAX,0,1},{8,8,1},{8,UINT32_MAX,1},{8,0,9},{8,0,0},{128,0,PB_MAX_BATCH+1}};
    ResetRing(&r,8,8,1);UCHAR before[sizeof(PB_QUEUE_SHARED)+8*sizeof(PB_DESCRIPTOR)];memcpy(before,r.P.Shared,sizeof(before));
    for(ULONG i=0;i<_countof(invalid);++i) {
        n=notify=99;Monitor=TRUE;Publications=ArmExchanges=0;
        CHECK(PbQueuePushRing(&r.P,source,invalid[i][0],invalid[i][1],invalid[i][2],&n,&notify)==PB_INVALID && !n && !notify);
        Monitor=FALSE;CHECK(!Publications && !ArmExchanges && !memcmp(before,r.P.Shared,sizeof(before)));
    }
    CHECK(PbQueuePushRing(NULL,source,8,0,1,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(PbQueuePushRing(&r.P,NULL,8,0,1,&n,&notify)==PB_INVALID && !n && !notify);
    CHECK(PbQueuePushRing(&r.P,source,8,0,1,NULL,&notify)==PB_INVALID && !notify);
    CHECK(PbQueuePushRing(&r.P,source,8,0,1,&n,NULL)==PB_INVALID && !n);
    r.P.Shared->Consumer=r.P.Cursor+1;
    CHECK(PbQueuePushRing(&r.P,source,8,7,2,&n,&notify)==PB_CORRUPT && !n && !notify && r.P.Shared->Armed==1 && r.P.Cursor==8);
    r.P.Shared->Consumer=UINT64_MAX;
    CHECK(PbQueuePushRing(&r.P,source,8,7,2,&n,&notify)==PB_CORRUPT && !n && !notify);
    PbTestGuardDestroy(&r.Memory);puts("PASS: invalid circular-source bounds, null outputs, oversized batches and corrupt peer cursors do not publish or consume the arm.");
}
#define STRESS_COUNT 200000u
typedef struct STRESS {RING R;HANDLE Event;} STRESS;
static DWORD WINAPI Produce(VOID *opaque)
{
    STRESS *s=opaque;PB_DESCRIPTOR source[64];ULONG head=63,next=0;
    while(next<STRESS_COUNT) {
        ULONG count=min(PB_MAX_BATCH,STRESS_COUNT-next);for(ULONG i=0;i<count;++i) source[(head+i)&63]=Item(next+i);
        uint32_t n,notify;PB_RESULT result=PbQueuePushRing(&s->R.P,source,64,head,count,&n,&notify);
        CHECK(result==PB_OK || result==PB_FULL);next+=n;head=(head+n)&63;
        if(notify) CHECK(SetEvent(s->Event));if(!n) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI Consume(VOID *opaque)
{
    STRESS *s=opaque;ULONG next=0;
    while(next<STRESS_COUNT) {
        PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t n;PB_RESULT result=PbQueuePop(&s->R.C,items,PB_MAX_BATCH,&n);CHECK(result==PB_OK || result==PB_EMPTY);
        for(ULONG i=0;i<n;++i) {PB_DESCRIPTOR expected=Item(next++);CHECK(!memcmp(items+i,&expected,sizeof(expected)));}
        if(!n) {uint32_t ready;CHECK(PbQueueArm(&s->R.C,&ready)==PB_OK);if(!ready) CHECK(WaitForSingleObject(s->Event,5000)==WAIT_OBJECT_0);}
    }
    return 0;
}
static VOID Concurrent(ULONG capacity)
{
    STRESS s={MakeRing(capacity),CreateEventW(NULL,FALSE,FALSE,NULL)};CHECK(s.Event);ULONG64 base=UINT64_MAX-31;
    ResetRing(&s.R,base,capacity,0);s.R.C.Cursor=s.R.C.PeerCursor=base;
    HANDLE threads[]={CreateThread(NULL,0,Produce,&s,0,NULL),CreateThread(NULL,0,Consume,&s,0,NULL)};
    CHECK(threads[0] && threads[1] && WaitForMultipleObjects(2,threads,TRUE,20000)==WAIT_OBJECT_0);
    CHECK(s.R.P.Cursor==base+STRESS_COUNT && s.R.C.Cursor==s.R.P.Cursor);
    CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]) && CloseHandle(s.Event));PbTestGuardDestroy(&s.R.Memory);
    printf("PASS: concurrent circular-source publication: capacity=%lu descriptors=%u, exact contents, real event waits and uint64 wrap.\n",capacity,STRESS_COUNT);
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Matrix();Rejections();Concurrent(1);Concurrent(64);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
