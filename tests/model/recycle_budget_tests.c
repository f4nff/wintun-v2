/* SPDX-License-Identifier: GPL-2.0 */
/* Actual kernel recycle and shared core: bounded work and private ownership are
 * checked independently of the event schedules in worker_poll_tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
typedef struct {ULONG Calls;} KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct _TUN_CTX {struct {volatile LONG V2SessionActive;} Probe;} TUN_CTX;
#define PbQueuePopTokens CorePopTokens
#include "../../common/v2_core.c"
#undef PbQueuePopTokens
static ULONG Pops;
PB_RESULT PbQueuePopTokens(PB_QUEUE_VIEW *v,PB_TOKEN *tokens,uint32_t count,uint32_t *done)
{Pops++;return CorePopTokens(v,tokens,count,done);}
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait)
{(void)priority;(void)wait;event->Calls++;}
#define IO_NO_INCREMENT 0
#include "../../driver/v2/session_fault.h"
#include "../../driver/v2/session_recycle.h"
typedef struct FIXTURE {
    PB_KERNEL_SESSION Session;TUN_CTX Context;PB_SLOT Slots[PB_MAX_SLOTS];PB_TOKEN Tokens[PB_MAX_SLOTS];
    PB_QUEUE_VIEW Peer;KEVENT Kernel,User,Progress;
} FIXTURE;
static VOID Create(FIXTURE *f,ULONG slots,BOOLEAN wrap,BOOLEAN terminal)
{
    memset(f,0,sizeof(*f));PB_KERNEL_SESSION *s=&f->Session;
    CHECK(PbCalculateLayout(1,slots,512,&s->Layout)==PB_OK);
    s->Region=VirtualAlloc(NULL,s->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(s->Region);
    s->Epoch=42;s->Ctx=&f->Context;f->Context.Probe.V2SessionActive=1;
    s->Lifecycle=(PB_SHARED_LIFECYCLE *)(s->Region+s->Layout.LifecycleOffset);s->ProgressEvent=&f->Progress;
    PB_KERNEL_QUEUE *q=s->Queues;q->Session=s;q->TxSlots=f->Slots;q->KernelEvent=&f->Kernel;q->UserEvent=&f->User;q->TxPoolExhausted=TRUE;
    CHECK(PbPoolInitialize(&q->TxPool,f->Slots,slots,42,0)==PB_OK);
    for(ULONG i=0;i<slots;++i) {
        if(terminal)f->Slots[i].Generation=UINT32_MAX-1;
        CHECK(PbPoolReserve(&q->TxPool,f->Tokens+i)==PB_OK&&f->Tokens[i].Slot==i);
        CHECK(PbPoolTransition(&q->TxPool,f->Tokens+i,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    CHECK(PbAttachQueue(q->Rings+PB_QUEUE_TX_RECYCLE,s->Region,&s->Layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
    CHECK(PbAttachQueue(&f->Peer,s->Region,&s->Layout,0,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    uint64_t base=wrap?UINT64_MAX-127:0;PB_QUEUE_VIEW *v=q->Rings+PB_QUEUE_TX_RECYCLE;
    v->Cursor=v->PeerCursor=f->Peer.Cursor=f->Peer.PeerCursor=base;v->Shared->Producer=v->Shared->Consumer=base;
    Pops=0;CHECK(PbV2RecycleLocked(q)==PB_OK&&Pops==1&&q->TxPoolExhausted);
}
static VOID Publish(FIXTURE *f,ULONG slots)
{
    for(ULONG i=0;i<slots;) {
        uint32_t n,notify,count=min(slots-i,PB_MAX_BATCH);
        CHECK(PbQueuePushTokens(&f->Peer,f->Tokens+i,count,&n,&notify)==PB_OK&&n==count&&!notify);i+=n;
    }
}
static VOID Destroy(FIXTURE *f)
{CHECK(VirtualFree(f->Session.Region,0,MEM_RELEASE));}
static VOID Valid(ULONG slots,BOOLEAN wrap,BOOLEAN terminal)
{
    FIXTURE f;Create(&f,slots,wrap,terminal);Publish(&f,slots);PB_KERNEL_QUEUE *q=f.Session.Queues;
    ULONG retired=0;
    while(retired<slots) {
        Pops=0;CHECK(PbV2RecycleLocked(q)==PB_OK&&Pops<=4&&!q->TxPoolExhausted);
        retired=min(slots,retired+4*PB_MAX_BATCH);
        for(ULONG i=0;i<slots;++i)CHECK(f.Slots[i].State==(i<retired?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
        uint32_t ready;CHECK(PbQueueArm(q->Rings+PB_QUEUE_TX_RECYCLE,&ready)==PB_OK&&ready==(uint32_t)(retired<slots));
    }
    CHECK(!f.Session.Faulted&&!f.Kernel.Calls&&!f.User.Calls&&!f.Progress.Calls);
    PB_TOKEN token={1,2,3,4},sentinel=token;
    if(!terminal)for(ULONG i=0;i<slots;++i)CHECK(PbPoolReserve(&q->TxPool,&token)==PB_OK&&token.Generation==2);
    token=sentinel;CHECK(PbPoolReserve(&q->TxPool,&token)==PB_FULL&&!memcmp(&token,&sentinel,sizeof(token)));
    Pops=0;CHECK(PbV2RecycleLocked(q)==PB_OK&&Pops==1);Destroy(&f);
}
static VOID Invalid(ULONG index,ULONG kind,BOOLEAN wrap)
{
    FIXTURE f;Create(&f,512,wrap,FALSE);PB_KERNEL_QUEUE *q=f.Session.Queues;
    switch(kind) {
    case 0:f.Tokens[index].Epoch++;break;
    case 1:f.Tokens[index].Generation++;break;
    case 2:f.Tokens[index].Queue++;break;
    case 3:f.Tokens[index].Slot=512;break;
    default:CHECK(index);f.Tokens[index]=f.Tokens[index-1];break;
    }
    Publish(&f,512);ULONG beforeFault=(index/(4*PB_MAX_BATCH))*(4*PB_MAX_BATCH);
    for(ULONG i=0;i<beforeFault;i+=4*PB_MAX_BATCH){Pops=0;CHECK(PbV2RecycleLocked(q)==PB_OK&&Pops==4);}
    Pops=0;PB_RESULT result=PbV2RecycleLocked(q);CHECK((result==PB_STALE||result==PB_BUSY)&&Pops<=4);
    for(ULONG i=0;i<512;++i)CHECK(f.Slots[i].State==(i<index?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
    CHECK(q->TxPoolExhausted==(index==0)&&f.Session.Faulted&&f.Session.Draining&&!f.Context.Probe.V2SessionActive);
    CHECK(f.Kernel.Calls==1&&f.User.Calls==1&&f.Progress.Calls==1&&f.Session.Lifecycle->Terminal==1&&f.Session.Lifecycle->Sequence==1);
    PbV2Fault(&f.Session);CHECK(f.Kernel.Calls==1&&f.User.Calls==1&&f.Progress.Calls==1);Destroy(&f);
}
int main(void)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));ULONG cases=0;
    const ULONG counts[]={1,63,64,65,255,256,257,511,512};
    for(ULONG wrap=0;wrap<2;++wrap) {
        for(ULONG terminal=0;terminal<2;++terminal)for(ULONG i=0;i<sizeof(counts)/sizeof(counts[0]);++i){Valid(counts[i],(BOOLEAN)wrap,(BOOLEAN)terminal);cases++;}
        for(ULONG index=0;index<PB_MAX_SLOTS;++index)for(ULONG kind=0;kind<5;++kind) {
            if(kind==4&&!index)continue;Invalid(index,kind,(BOOLEAN)wrap);cases++;
        }
        for(ULONG backwards=0;backwards<2;++backwards) {
            FIXTURE f;Create(&f,1,(BOOLEAN)wrap,FALSE);PB_KERNEL_QUEUE *q=f.Session.Queues;PB_QUEUE_VIEW *v=q->Rings+PB_QUEUE_TX_RECYCLE;
            v->Shared->Producer=v->Cursor+(backwards?UINT64_MAX:513);Pops=0;
            CHECK(PbV2RecycleLocked(q)==PB_CORRUPT&&Pops==1&&q->TxPoolExhausted&&f.Slots[0].State==PB_SLOT_PUBLISHED&&f.Session.Faulted);Destroy(&f);cases++;
        }
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual kernel recycle cases, slots 1..512, four-batch work bound and arm/recheck continuation, terminal generations, bad token at all 512 positions, valid-prefix retirement, corrupt cursors, single fault notification, cursor wrap and zero handle delta.\n",cases);return 0;
}
