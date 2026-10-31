/* SPDX-License-Identifier: GPL-2.0 */
/* Actual shared-ring core: read side effects, deterministic arm/publish/reuse
 * schedules and concurrent publication through tiny/wrapping rings. Hosted only. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static ULONG ReadRmws;
static BOOLEAN CountReads;
static VOID (*ArmHook)(BOOLEAN after);
static VOID (*StoreHook)(volatile __int64 *address);
__int64 CountCompare64(volatile __int64 *p,__int64 exchange,__int64 compare)
{if(CountReads) ReadRmws++;return _InterlockedCompareExchange64(p,exchange,compare);}
static long HookExchange32(volatile long *p,long value)
{
    if(value==1 && ArmHook) ArmHook(FALSE);
    long old=_InterlockedExchange(p,value);
    if(value==1 && ArmHook) ArmHook(TRUE);
    return old;
}
static __int64 HookExchange64(volatile __int64 *p,__int64 value)
{__int64 old=_InterlockedExchange64(p,value);if(StoreHook) StoreHook(p);return old;}
#define _InterlockedCompareExchange64 CountCompare64
#define _InterlockedExchange HookExchange32
#define _InterlockedExchange64 HookExchange64
#ifndef PB_CORE_SOURCE
#define PB_CORE_SOURCE "../../common/v2_core.c"
#endif
#include PB_CORE_SOURCE
#undef _InterlockedCompareExchange64
#undef _InterlockedExchange
#undef _InterlockedExchange64

typedef struct FIXTURE {PB_LAYOUT Layout;PB_QUEUE_VIEW P,C;UCHAR *Region;HANDLE Event;} FIXTURE;
static FIXTURE *Current;
static ULONG Serial,Notifications;
static BOOLEAN PublishAfterArm;
static PB_DESCRIPTOR Item(ULONG serial)
{
    PB_DESCRIPTOR d;memset(&d,(int)(serial^0xa5),sizeof(d));
    d.Token.Epoch=0x123456789abcdef0ull;d.Token.Generation=serial+1;d.Length=20;
    d.DataOffset=~(uint64_t)serial;return d;
}
static FIXTURE *Create(ULONG capacity,ULONG64 base)
{
    FIXTURE *f=calloc(1,sizeof(*f));CHECK(f && PbCalculateLayout(1,1,capacity,&f->Layout)==PB_OK);
    f->Region=VirtualAlloc(NULL,f->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(f->Region);
    for(ULONG r=0;r<PB_QUEUE_COUNT;++r) {
        PB_QUEUE_VIEW view;CHECK(PbAttachQueue(&view,f->Region,&f->Layout,0,r,0)==PB_OK);
        CHECK(!((uintptr_t)&view.Shared->Producer&7) && !((uintptr_t)&view.Shared->Consumer&7));
    }
    CHECK(PbAttachQueue(&f->P,f->Region,&f->Layout,0,0,1)==PB_OK && PbAttachQueue(&f->C,f->Region,&f->Layout,0,0,0)==PB_OK);
    f->P.Cursor=f->P.PeerCursor=f->C.Cursor=f->C.PeerCursor=base;
    f->P.Shared->Producer=f->P.Shared->Consumer=base;
    f->Event=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(f->Event);return f;
}
static VOID Destroy(FIXTURE *f)
{CHECK(CloseHandle(f->Event) && VirtualFree(f->Region,0,MEM_RELEASE));free(f);}
static VOID Publish(FIXTURE *f,ULONG serial)
{
    PB_DESCRIPTOR d=Item(serial);uint32_t n,notify;
    CHECK(PbQueuePush(&f->P,&d,1,&n,&notify)==PB_OK && n==1);
    if(notify) {Notifications++;CHECK(SetEvent(f->Event));}
}
static VOID Pop(FIXTURE *f,ULONG serial)
{PB_DESCRIPTOR d,expected=Item(serial);uint32_t n;CHECK(PbQueuePop(&f->C,&d,1,&n)==PB_OK && n==1 && !memcmp(&d,&expected,sizeof(d)));}
static VOID InjectPublish(BOOLEAN after)
{if(after==PublishAfterArm) {ArmHook=NULL;Publish(Current,Serial);}}
static VOID InjectReuse(volatile __int64 *address)
{
    if(address==(volatile __int64 *)&Current->C.Shared->Consumer) {
        StoreHook=NULL;Publish(Current,Serial);
    }
}
static VOID Schedules(ULONG64 base)
{
    for(ULONG schedule=0;schedule<4;++schedule) {
        FIXTURE *f=Create(1,base);Current=f;Serial=100+schedule;Notifications=0;
        uint32_t ready;
        if(schedule==0) Publish(f,Serial);
        if(schedule==1 || schedule==2) {PublishAfterArm=schedule==2;ArmHook=InjectPublish;}
        CHECK(PbQueueArm(&f->C,&ready)==PB_OK && !ArmHook);
        if(schedule<3) CHECK(ready && Notifications==(ULONG)(schedule==2));
        else {
            CHECK(!ready);Publish(f,Serial);CHECK(Notifications==1);
            CHECK(WaitForSingleObject(f->Event,1000)==WAIT_OBJECT_0);
        }
        Pop(f,Serial);Destroy(f);Current=NULL;
    }
    /* Consumer storage is released after copying: a producer may immediately
     * overwrite the sole slot before Pop returns, without changing its output. */
    FIXTURE *f=Create(1,base);Current=f;Publish(f,200);Serial=201;StoreHook=InjectReuse;
    Pop(f,200);CHECK(!StoreHook);Pop(f,201);Destroy(f);Current=NULL;
    printf("PASS: real queue arm/publish/reuse schedules: wrap=%u, no lost wake or overwritten output.\n",base!=0);
}
static VOID ReadOnlyObservation(VOID)
{
    FIXTURE *f=Create(1,0);DWORD protection;uint32_t space,n;uint64_t producer;PB_DESCRIPTOR d;
    CHECK(VirtualProtect(f->P.Shared,sizeof(*f->P.Shared),PAGE_READONLY,&protection));
    CountReads=TRUE;ReadRmws=0;ULONG faults=0;
    __try {CHECK(PbQueueSpace(&f->P,&space)==PB_OK && space==1);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {faults++;}
    __try {CHECK(PbQueueSnapshotProducer(&f->C,&producer)==PB_OK && !producer);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {faults++;}
    __try {CHECK(PbQueuePop(&f->C,&d,1,&n)==PB_EMPTY && !n);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {faults++;}
    CountReads=FALSE;
#ifdef PB_CURSOR_RMW_BASELINE
    CHECK(faults==3 && ReadRmws==3);
#else
    CHECK(!faults && !ReadRmws);
#endif
    printf("PASS: three cursor observations: locked read-modify-writes=%lu read-only-page faults=%lu.\n",ReadRmws,faults);
    DWORD ignored;CHECK(VirtualProtect(f->P.Shared,sizeof(*f->P.Shared),protection,&ignored));Destroy(f);
}

#define STRESS_COUNT 200000u
typedef struct STRESS {FIXTURE *F;ULONG Batch;} STRESS;
static DWORD WINAPI Produce(VOID *opaque)
{
    STRESS *s=opaque;PB_DESCRIPTOR items[PB_MAX_BATCH];ULONG next=0;
    while(next<STRESS_COUNT) {
        ULONG count=min(s->Batch,STRESS_COUNT-next);for(ULONG i=0;i<count;++i) items[i]=Item(next+i);
        uint32_t n,notify;PB_RESULT r=PbQueuePush(&s->F->P,items,count,&n,&notify);CHECK(r==PB_OK || r==PB_FULL);
        next+=n;if(notify) CHECK(SetEvent(s->F->Event));if(!n) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI Consume(VOID *opaque)
{
    STRESS *s=opaque;PB_DESCRIPTOR items[PB_MAX_BATCH];ULONG next=0;
    while(next<STRESS_COUNT) {
        uint32_t n;PB_RESULT r=PbQueuePop(&s->F->C,items,s->Batch,&n);CHECK(r==PB_OK || r==PB_EMPTY);
        for(ULONG i=0;i<n;++i) {PB_DESCRIPTOR expected=Item(next++);CHECK(!memcmp(items+i,&expected,sizeof(expected)));}
        if(!n) {uint32_t ready;CHECK(PbQueueArm(&s->F->C,&ready)==PB_OK);if(!ready) CHECK(WaitForSingleObject(s->F->Event,5000)==WAIT_OBJECT_0);}
    }
    return 0;
}
static VOID Concurrent(ULONG capacity,ULONG batch,ULONG64 base)
{
    STRESS s={Create(capacity,base),batch};
    HANDLE threads[]={CreateThread(NULL,0,Produce,&s,0,NULL),CreateThread(NULL,0,Consume,&s,0,NULL)};
    CHECK(threads[0] && threads[1] && WaitForMultipleObjects(2,threads,TRUE,20000)==WAIT_OBJECT_0);
    CHECK(s.F->P.Cursor==base+STRESS_COUNT && s.F->C.Cursor==s.F->P.Cursor);
    CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]));Destroy(s.F);
    printf("PASS: concurrent actual ring: capacity=%lu batch=%lu wrap=%u descriptors=%u, exact full-descriptor contents.\n",capacity,batch,base!=0,STRESS_COUNT);
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    ReadOnlyObservation();Schedules(0);Schedules(UINT64_MAX);
    Concurrent(1,1,0);Concurrent(1,1,UINT64_MAX-31);Concurrent(64,32,UINT64_MAX-31);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
