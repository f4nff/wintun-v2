/* SPDX-License-Identifier: GPL-2.0 */
/* Actual normal drain/worker/query/return paths with the existing hosted DDIs.
 * Two workers join before the original fixture destroys its storage; this does
 * not replace or claim to execute the separate native Free barrier. */
#ifdef PB_API_TESTS
#error Shipping multiqueue drain paths required
#endif
#ifndef NDEBUG
#error Shipping multiqueue drain configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"multiqueue drain requires shipping fingerprint");
static ULONG NdrNdisVersion;
#define PB_NBL_VERSION NdrNdisVersion
#define main UnusedMultiqueueDrainHostMain
#include PB_NDR_HOST
#undef main

static HANDLE NdrWaitEntered[2];
static volatile LONG NdrCalls[2],NdrWork[2],NdrWaits[2],NdrExits[2],NdrAllocations;
static ULONG NdrCases,NdrJoins,NdrHandles;
static __declspec(thread) ULONG NdrRole;
static VOID NdrAwait(HANDLE h){CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static HANDLE NdrEvent(BOOL initial){HANDLE h=CreateEventW(NULL,TRUE,initial,NULL);CHECK(h);++NdrHandles;return h;}
static VOID NdrClose(HANDLE h){CHECK(NdrHandles && CloseHandle(h));--NdrHandles;}
static VOID NdrAllocated(VOID){InterlockedIncrement(&NdrAllocations);}
static BOOLEAN NdrReceive(PB_KERNEL_QUEUE *q)
{CHECK(q->Index<2 && InterlockedIncrement(NdrCalls+q->Index)<=3);BOOLEAN work=ActualReceiveBatch(q);if(work)InterlockedIncrement(NdrWork+q->Index);return work;}
static VOID NdrWait(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alert,VOID *timeout,VOID *blocks)
{
    CHECK(n==2 && !type && !reason && !mode && !alert && !timeout && !blocks && NdrRole<2);
    CHECK(events[0]==&Current->StopEvent && events[1]==Current->Queues[NdrRole].KernelEvent);
    HANDLE native[2]={((KEVENT *)events[0])->Native,((KEVENT *)events[1])->Native};
    CHECK(InterlockedIncrement(NdrWaits+NdrRole)==1 && SetEvent(NdrWaitEntered[NdrRole]));
    /* This is SDK wait entry, not proof that the OS has parked the thread. */
    CHECK(WaitForMultipleObjects(2,native,FALSE,5000)==WAIT_OBJECT_0);
}
static VOID NdrExit(LONG status){CHECK(status==STATUS_SUCCESS && NdrRole<2 && InterlockedIncrement(NdrExits+NdrRole)==1);}
typedef struct NDR_JOB {PB_KERNEL_QUEUE *Queue;HANDLE Entered,Gate;} NDR_JOB;
static DWORD WINAPI NdrWorker(VOID *opaque)
{NDR_JOB *job=opaque;NdrRole=job->Queue->Index;CHECK(SetEvent(job->Entered));if(job->Gate)NdrAwait(job->Gate);PbV2Worker(job->Queue);return 0;}
static HANDLE NdrStart(NDR_JOB *job){HANDLE h=CreateThread(NULL,0,NdrWorker,job,0,NULL);CHECK(h);++NdrHandles;return h;}
static VOID NdrJoin(HANDLE h){DWORD code;NdrAwait(h);CHECK(GetExitCodeThread(h,&code) && !code);NdrClose(h);++NdrJoins;}
static VOID NdrW16(UCHAR *p,ULONG v){p[0]=(UCHAR)(v>>8);p[1]=(UCHAR)v;}
static VOID NdrW32(UCHAR *p,ULONG v){NdrW16(p,v>>16);NdrW16(p+2,v);}
static VOID NdrPacket(UCHAR *p,ULONG protocol,ULONG sequence)
{
    ULONG ip=PB_TEST_IP_VERSION==4?20u:40u,header=ip+(protocol==6?20u:8u);memset(p,0,1500);
#if PB_TEST_IP_VERSION == 4
    p[0]=0x45;NdrW16(p+2,1500);NdrW16(p+4,sequence);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[15]=1;p[16]=10;p[19]=2;
#else
    p[0]=0x60;NdrW16(p+4,1460);p[6]=(UCHAR)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=1;p[39]=2;
#endif
    NdrW16(p+ip,10001);NdrW16(p+ip+2,10002);
    if(protocol==6){NdrW32(p+ip+4,0xfffffff0u+sequence*(1500-header));p[ip+12]=0x50;p[ip+13]=0x10;NdrW16(p+ip+14,65535);}else NdrW16(p+ip+4,1500-ip);
    memset(p+header,(int)sequence,1500-header);NdrW32(p+header,sequence);
    /* These payload bytes are opaque markers, never scheduling clocks. */
    for(ULONG i=0;i<16;++i)p[header+4+i]=(UCHAR)(0xf0u+i+sequence);
    CHECK(PbFinalizeChecksums(p,1500,PB_TEST_IP_VERSION==4,protocol==6,protocol==17)==PB_PACKET_OK);
}
static VOID NdrStatus(FIXTURE *f,ULONG expected,ULONG active)
{PB_V2_STATUS status;CHECK(PbV2Query(&f->Ctx,&status)==STATUS_SUCCESS && status.State==expected && status.ActiveReferences==active && status.QueueCount==2);}
static VOID NdrReturnLast(FIXTURE *f,NET_BUFFER_LIST **last)
{CHECK(*last && PbV2ReturnNbls(&f->Ctx,last,PB_MAX_BATCH) && !*last && !f->S.Active && !f->S.Returns.References && !Nbls && !Mdls);}
static VOID NdrCase(ULONG protocol,ULONG minor,BOOL wrap,BOOL returnFirst,BOOL measured)
{
    CHECK(!NdrHandles && !Pools && !Mdls && !Nbls);NdrNdisVersion=PB_NDIS_VERSION(minor);
    FIXTURE *f=Create(2,32,32,0);f->Ctx.MiniportAdapterHandle=f;UnexpectedIndication=FALSE;AutoConsume=FALSE;
    memset((VOID *)NdrCalls,0,sizeof(NdrCalls));memset((VOID *)NdrWork,0,sizeof(NdrWork));memset((VOID *)NdrWaits,0,sizeof(NdrWaits));memset((VOID *)NdrExits,0,sizeof(NdrExits));NdrAllocations=0;AfterNblAllocate=NdrAllocated;
    PB_QUEUE_VIEW producer[2],consumer[2];uint64_t base=wrap?UINT64_MAX-7u:0;
    for(ULONG q=0;q<2;++q){
        PB_KERNEL_QUEUE *n=f->S.Queues+q;n->TxSlots=ExAllocatePool2(0,32*sizeof(*n->TxSlots),0);CHECK(PbPoolInitialize(&n->TxPool,n->TxSlots,32,42,(USHORT)q)==PB_OK);
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r){PB_QUEUE_VIEW *v=n->Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=base;}
        CHECK(PbAttachQueue(producer+q,f->S.Region,&f->S.Layout,q,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK && PbAttachQueue(consumer+q,f->S.Region,&f->S.Layout,q,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);
        producer[q].Cursor=producer[q].PeerCursor=consumer[q].Cursor=consumer[q].PeerCursor=base;
        PB_DESCRIPTOR items[16];for(ULONG i=0;i<16;++i){items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,(USHORT)q},.Length=1500,.DataOffset=f->S.Layout.Queues[q].RxData+i*PB_BLOCK_SIZE};NdrPacket(f->S.Region+items[i].DataOffset,protocol,q*16+i);}
        uint32_t count,notify;CHECK(PbQueuePush(producer+q,items,16,&count,&notify)==PB_OK && count==16 && !notify);
    }
    CHECK(ActualReceiveBatch(f->S.Queues) && Indicated && f->S.Active==16 && NdrAllocations==16);
    NET_BUFFER_LIST *first=Indicated,*last=NULL,*previous=NULL;Indicated=NULL;ULONG index=0;
    for(NET_BUFFER_LIST *n=first;n;n=n->Next){
        PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);
        CHECK(index<16 && record->Count==1 && record->Descriptors[0].Token.Slot==index && record->Descriptors[0].Token.Queue==0 && record->IpVersion==PB_TEST_IP_VERSION && record->Protocol==protocol && record->IndicationLength==1500);
        BYTE expected[1500];NdrPacket(expected,protocol,index);CHECK(record->Mdl->Partial && !memcmp(record->Mdl->Address,expected,1500));
        if(minor==20)CHECK(n->Info[TcpRecvSegCoalesceInfo]==(VOID *)(ULONG_PTR)~0u && n->Info[RscTcpTimestampDelta]==(VOID *)(ULONG_PTR)~0u && n->Info[UdpRecvSegCoalesceOffloadInfo]==(VOID *)(ULONG_PTR)~0u);
        if(index==14)previous=n;if(index==15)last=n;++index;
    }
    CHECK(index==16 && previous && last && !last->Next);previous->Next=NULL;
    CHECK(PbV2ReturnNbls(&f->Ctx,&first,PB_MAX_BATCH) && !first && f->S.Active==1 && f->S.Queues[0].Credits==1 && Nbls==1);
    AcquireSRWLockExclusive(&f->Ctx.TransitionLock);PbV2BeginDrain(&f->S,10000);ReleaseSRWLockExclusive(&f->Ctx.TransitionLock);
    CHECK(f->S.DrainSnapshotReady && f->S.Draining && !f->S.Faulted);for(ULONG q=0;q<2;++q)CHECK(f->S.Queues[q].DrainProducer==base+16);
    NdrStatus(f,PB_SESSION_DRAINING,1);
    HANDLE q1Gate=NdrEvent(FALSE);NDR_JOB jobs[2];HANDLE workers[2];
    for(ULONG q=0;q<2;++q){NdrWaitEntered[q]=NdrEvent(FALSE);jobs[q]=(NDR_JOB){f->S.Queues+q,NdrEvent(FALSE),q?q1Gate:NULL};workers[q]=NdrStart(jobs+q);NdrAwait(jobs[q].Entered);}
    NdrAwait(NdrWaitEntered[0]);CHECK(NdrCalls[0]==1 && !NdrWork[0] && !NdrCalls[1]);
    if(returnFirst){NdrReturnLast(f,&last);NdrStatus(f,PB_SESSION_DRAINING,0);CHECK(f->S.Queues[1].Rings[PB_QUEUE_RX_SUBMIT].Cursor==base);}
    CHECK(SetEvent(q1Gate));NdrAwait(NdrWaitEntered[1]);CHECK(NdrCalls[1]==2 && NdrWork[1]==1 && NdrAllocations==16 && !Indicated);
    for(ULONG q=0;q<2;++q)CHECK(f->S.Queues[q].Rings[PB_QUEUE_RX_SUBMIT].Cursor==base+16);
    if(!returnFirst){NdrStatus(f,PB_SESSION_DRAINING,1);NdrReturnLast(f,&last);}
    /* STOPPED acknowledges native drain, independently of DLL consumption. */
    NdrStatus(f,PB_SESSION_STOPPED,0);CHECK(f->S.Completions==32 && f->S.RxObjects==16 && f->S.RxReturned==16 && NdrAllocations==16 && !f->S.Returns.References);
    for(ULONG q=0;q<2;++q){
        CHECK(!f->S.Queues[q].Credits && !f->S.Queues[q].PendingCount && consumer[q].Shared->Producer-consumer[q].Shared->Consumer==16);
        PB_DESCRIPTOR items[16];uint32_t count;CHECK(PbQueuePop(consumer+q,items,16,&count)==PB_OK && count==16);
        for(ULONG i=0;i<16;++i){CHECK(items[i].Status==(ULONG)(q?PB_STOPPED:PB_OK) && items[i].Token.Queue==q && items[i].Token.Slot==i && items[i].Token.Generation==1 && items[i].Length==1500);BYTE expected[1500];NdrPacket(expected,protocol,q*16+i);CHECK(!memcmp(f->S.Region+items[i].DataOffset,expected,1500));}
    }
    InterlockedExchange(&f->S.Stopping,1);KeSetEvent(&f->S.StopEvent,0,FALSE);
    /* Both successful returns and STOPPED completions end generation 1. */
    for(ULONG q=0;q<2;++q){NdrJoin(workers[q]);CHECK(NdrExits[q]==1 && NdrWaits[q]==1 && NdrCalls[q]==(q?2:1));NdrClose(jobs[q].Entered);NdrClose(NdrWaitEntered[q]);ExFreePoolWithTag(f->S.Queues[q].TxSlots,0);f->S.Queues[q].TxSlots=NULL;for(ULONG i=0;i<16;++i)CHECK(!f->S.Queues[q].RxSlots[i].Active && f->S.Queues[q].RxSlots[i].Generation==1);}
    NdrClose(q1Gate);AfterNblAllocate=NULL;CHECK(!NdrHandles && !Pools && !Mdls && !Nbls && FixtureRundownAcquires==FixtureRundownReleases);Destroy(f);
    ++NdrCases;printf("MULTIQUEUE_DRAIN32 ip%u protocol%lu ndis%lu wrap%u returnFirst%u measured%u originals32 nativeNbl16 stopped16 complete32 joins2 waitEntries2 owners0\n",PB_TEST_IP_VERSION,protocol,minor,wrap,returnFirst,measured);
}
int main(VOID)
{
    DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG udp=0;udp<2;++udp)for(ULONG latest=0;latest<2;++latest)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG first=0;first<2;++first)NdrCase(udp?17:6,latest?89:20,wrap,first,pass);}
    CHECK(NdrCases==32 && NdrJoins==64 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("PASS: family%u multiqueue normal drain32 cases%lu warm16 measured16 joins%lu handles%lu/%lu\n",PB_TEST_IP_VERSION,NdrCases,NdrJoins,before,after);return 0;
}
