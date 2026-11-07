/* SPDX-License-Identifier: GPL-2.0 */
/* Deterministic peer reuse at the publication boundary, without data races in
 * the test: the peer overwrites only memory whose ownership was just returned. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/session_state.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
static PB_SESSION Session;
static PB_QUEUE_VIEW Ready,Recycle;
static BOOL Reuse;
static uint32_t Reused;
static PB_RESULT PeerPush(PB_QUEUE_VIEW *ring,const PB_TOKEN *tokens,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    PB_RESULT r=PbQueuePushTokens(ring,tokens,count,accepted,notify);
    if(r==PB_OK&&Reuse) {
        PB_TOKEN returned[PB_MAX_BATCH];PB_DESCRIPTOR next[PB_MAX_BATCH]={0};uint32_t n,published,wake;
        CHECK(PbQueuePopTokens(&Recycle,returned,PB_MAX_BATCH,&n)==PB_OK&&n==*accepted);
        for(uint32_t i=0;i<n;++i) {
            uint32_t offset=Session.Layout.Queues[0].TxData+returned[i].Slot*PB_BLOCK_SIZE;
            memset(Session.Region+offset,0xcc,1024);
            next[i].Token=returned[i];next[i].Token.Generation++;
            next[i].Length=1024;next[i].DataOffset=offset;
            CHECK(PbValidateDescriptor(&Session.Layout,Session.Epoch,0,0,next+i,7)==PB_OK);
        }
        CHECK(PbQueuePush(&Ready,next,n,&published,&wake)==PB_OK&&published==n);Reused+=n;
    }
    return r;
}
#define PbQueuePushTokens PeerPush
#ifndef PB_RETURN_ALIAS_QUEUE_SOURCE
#define PB_RETURN_ALIAS_QUEUE_SOURCE "../../api/v2/session_queue.h"
#endif
#include PB_RETURN_ALIAS_QUEUE_SOURCE
#undef PbQueuePushTokens
static void Test(uint32_t count,BOOL historical)
{
    memset(&Session,0,sizeof(Session));CHECK(PbCalculateLayout(1,64,64,&Session.Layout)==PB_OK);
    Session.Region=VirtualAlloc(NULL,Session.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(Session.Region);
    Session.SharedLifecycle=(PB_SHARED_LIFECYCLE *)(Session.Region+Session.Layout.LifecycleOffset);Session.Epoch=42;
    Session.ClosingEvent=CreateEventW(NULL,TRUE,FALSE,NULL);Session.ProgressEvent=CreateEventW(NULL,FALSE,FALSE,NULL);
    PB_USER_QUEUE *q=Session.Queues;q->Session=&Session;
    q->KernelEvent=CreateEventW(NULL,FALSE,FALSE,NULL);q->UserEvent=CreateEventW(NULL,FALSE,FALSE,NULL);
    q->TxSlots=calloc(64,sizeof(PB_SLOT));q->RxSlots=calloc(64,sizeof(PB_SLOT));CHECK(q->TxSlots&&q->RxSlots);
    CHECK(PbPoolInitialize(&q->Tx,q->TxSlots,64,42,0)==PB_OK&&PbPoolInitialize(&q->Rx,q->RxSlots,64,42,0)==PB_OK);
    for(uint32_t i=0;i<4;++i)CHECK(PbAttachQueue(q->Rings+i,Session.Region,&Session.Layout,0,i,i==1||i==2)==PB_OK);
    CHECK(PbAttachQueue(&Ready,Session.Region,&Session.Layout,0,0,1)==PB_OK&&PbAttachQueue(&Recycle,Session.Region,&Session.Layout,0,1,0)==PB_OK);
    PB_DESCRIPTOR items[64]={0};for(uint32_t i=0;i<count;++i){items[i].Token=(PB_TOKEN){42,1,(uint16_t)i,0};items[i].Length=1024;items[i].DataOffset=Session.Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;}
    uint32_t n,wake;CHECK(PbQueuePush(&Ready,items,count,&n,&wake)==PB_OK&&n==count);
    PB_LEASE leases[64];CHECK(ReceiveBatch(q,leases,count,&n)==PB_OK&&n==count);
    PB_TOKEN *alias=(PB_TOKEN *)leases[0].Data;for(uint32_t i=0;i<count;++i)alias[i]=leases[i].Descriptor.Token;
    Reuse=TRUE;Reused=0;CHECK(ReleaseBatch(q,alias,count,&n)==PB_OK&&n==count&&Reused==count&&q->Loans==0);Reuse=FALSE;
    if(historical) {
        CHECK(q->TxSlots[0].State==PB_SLOT_PUBLISHED);
        CHECK(ReceiveBatch(q,leases,count,&n)==PB_CORRUPT&&!n&&Session.Fatal);
    } else {
        for(uint32_t i=0;i<count;++i)CHECK(q->TxSlots[i].State==PB_SLOT_FREE);
        CHECK(ReceiveBatch(q,leases,count,&n)==PB_OK&&n==count&&q->Loans==(LONG)count);
        PB_TOKEN stable[64];for(uint32_t i=0;i<count;++i)stable[i]=leases[i].Descriptor.Token;
        CHECK(ReleaseBatch(q,stable,count,&n)==PB_OK&&n==count&&!q->Loans);
        CHECK(ReleaseBatch(q,stable,count,&n)==PB_BUSY&&!n);
    }
    CHECK(CloseHandle(q->KernelEvent)&&CloseHandle(q->UserEvent)&&CloseHandle(Session.ClosingEvent)&&CloseHandle(Session.ProgressEvent));
    free(q->TxSlots);free(q->RxSlots);CHECK(VirtualFree(Session.Region,0,MEM_RELEASE));
}
int main(int argc,char **argv)
{
    BOOL historical=argc==2&&!strcmp(argv[1],"--expect-baseline");DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(uint32_t n=1;n<=PB_MAX_BATCH;++n)Test(n,historical);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
    puts(historical?"PASS: reproduced stale DLL slot bookkeeping after legal immediate peer reuse for batches 1..64.":
        "PASS: return input in released payload, immediate peer overwrite/republication, batches 1..64, exact local retirement and next-generation import; zero handle delta.");return 0;
}

