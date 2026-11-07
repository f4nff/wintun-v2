/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Read/Describe/Send and RESOURCES control reply, with hosted DDIs.
 * An independent nested Send runs synchronously; Pause/Drain use other SDK
 * threads. The registered session and native idle worker are fixture-owned. */
#ifdef PB_API_TESTS
#error Shipping control batch paths required
#endif
#ifndef NDEBUG
#error Shipping control batch configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"control batch requires shipping fingerprint");
#define NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO PB_HOST_CHECKSUM_INFO
#define main UnusedControlBatchWorkerMain
#include PB_BATCH_HOST
#undef main
#undef NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO
#include PB_BATCH_TYPES
typedef struct NET_BUFFER {struct NET_BUFFER *Next;ULONG Length;MDL *Mdl;UCHAR *Data;} NET_BUFFER;
typedef struct B_INPUT {NET_BUFFER Nb;ULONG Id,Nested,Control;UCHAR Data[1514];} B_INPUT;
#define NET_BUFFER_LIST_FIRST_NB(n) ((NET_BUFFER *)((n)->Reserved[0]))
#define NET_BUFFER_NEXT_NB(n) ((n)->Next)
#define NET_BUFFER_DATA_LENGTH(n) ((n)->Length)
#define NET_BUFFER_CURRENT_MDL(n) ((n)->Mdl)
#define NET_BUFFER_CURRENT_MDL_OFFSET(n) 0u
#define MmGetMdlByteCount(m) ((m)->Length)
#define TcpLargeSendNetBufferListInfo NetBufferListFrameType
#define UdpSegmentationOffloadInfo TcpRecvSegCoalesceInfo
#define NDIS_STATUS_RESOURCES 2u
#define NDIS_STATUS_MEDIA_DISCONNECTED 3u
#define NDIS_STATUS_INVALID_LENGTH 4u
#define NDIS_STATUS_INVALID_DATA 5u
#define NDIS_STATUS_NOT_SUPPORTED 6u
#define NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE 1u
#define NormalPagePriority 0u
#define POOL_NX_OPTIN 1
#define POOL_NX_OPTOUT 0
#define POOL_NX_OPTIN_AUTO 0
#ifndef DISPATCH_LEVEL
#define DISPATCH_LEVEL 2u
#endif
#define NDIS_DEFAULT_PORT_NUMBER 0u
#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL 1u
#define NDIS_RECEIVE_FLAGS_RESOURCES 2u
#define NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL 1u
static VOID *MmGetSystemAddressForMdlSafe(MDL *mdl,ULONG priority){CHECK(!priority&&mdl);return mdl->Address;}
#define PbV2Read ActualBatchRead
#include "../../driver/v2/session_read.h"
#undef PbV2Read
static ULONG PayloadReads,PrefixReads,ControlReads,Replies,CompletedOuter,CompletedNested,Rejects;
static ULONG PayloadBytes,PrefixBytes,ControlBytes,ReplyCopies,ReplyBytes,PushCalls,PushCounts[3];
static ULONG BatchCases,BatchJoins,BatchWorkers;
static BOOLEAN PbV2Read(NET_BUFFER *nb,ULONG skip,UCHAR *data,ULONG length)
{B_INPUT *input=(B_INPUT *)nb;if(input->Control){++ControlReads;ControlBytes+=length;}else if(skip){++PayloadReads;PayloadBytes+=length;}else{++PrefixReads;PrefixBytes+=length;}return ActualBatchRead(nb,skip,data,length);}
#define PbV2Describe ActualBatchDescribe
#include "../../driver/v2/session_describe.h"
#undef PbV2Describe
static BOOLEAN PbV2Send(TUN_CTX *,NET_BUFFER_LIST *);
static NET_BUFFER_LIST *MakeChain(BOOLEAN);
static VOID DataPacket(UCHAR *,ULONG,BOOLEAN);
static HANDLE ReplyEntered,ReplyGo,CompleteEntered,CompleteGo,BatchTail,BatchTailGo,BatchPauseReady,DrainReady,BatchRundown;
static ULONG Ip,Protocol,Mode;
static LONG SenderId;
static BOOLEAN NestedCall;
static __declspec(thread) KIRQL Level;
static UCHAR ExpectedReply[128];static uint32_t ExpectedReplyLength;
static VOID AssertNoLocks(VOID)
{
    BOOLEAN okay=TryAcquireSRWLockExclusive(&Ctx->TransitionLock);if(okay)ReleaseSRWLockExclusive(&Ctx->TransitionLock);CHECK(okay);
    okay=TryAcquireSRWLockExclusive((SRWLOCK *)&Session->Queues[0].TxLock);if(okay)ReleaseSRWLockExclusive((SRWLOCK *)&Session->Queues[0].TxLock);CHECK(okay);
}
static VOID KeRaiseIrql(KIRQL next,KIRQL *saved){CHECK(next==2&&Level<=next);*saved=Level;Level=next;}
static VOID KeLowerIrql(KIRQL saved){CHECK(Level==2);Level=saved;}
static VOID NdisMIndicateReceiveNetBufferLists(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbl,ULONG port,ULONG count,ULONG flags)
{
    AssertNoLocks();CHECK(adapter==Ctx&&!port&&count==1&&flags==3&&Level==2&&Session->Active==1&&Session->Returns.References==1&&Ctx->Probe.IndicationCount[0]==1);
    CHECK(Session->TxObjects==16&&nbl->Length==ExpectedReplyLength&&!memcmp(nbl->FirstMdl->Address,ExpectedReply,ExpectedReplyLength)&&!nbl->Next&&!nbl->Reserved[1]);
    VOID *future=(VOID *)(ULONG_PTR)~0u;CHECK(nbl->Info[TcpRecvSegCoalesceInfo]==(RuntimeVersion>=PB_NDIS_VERSION(30)?NULL:future)&&nbl->Info[RscTcpTimestampDelta]==(RuntimeVersion>=PB_NDIS_VERSION(30)?NULL:future)&&nbl->Info[UdpRecvSegCoalesceOffloadInfo]==(RuntimeVersion>=PB_NDIS_VERSION(89)?NULL:future));
    CHECK(++Replies==1);NestedCall=TRUE;CHECK(PbV2Send(Ctx,MakeChain(TRUE)));NestedCall=FALSE;
    CHECK(CompletedNested==32&&Session->Active==1&&Session->Returns.References==1&&Session->TxObjects==48&&SetEvent(ReplyEntered));Await(ReplyGo);
}
static VOID NdisMSendNetBufferListsComplete(NDIS_HANDLE adapter,NET_BUFFER_LIST *head,ULONG flags)
{
    AssertNoLocks();CHECK(adapter==Ctx&&flags==1&&Level==2&&Session->Active==(NestedCall?2:1));ULONG count=0;
    while(head){NET_BUFFER_LIST *next=head->Next;B_INPUT *input=(B_INPUT *)head->Reserved[0];CHECK(input&&input->Nb.Mdl==head->FirstMdl);
        ULONG expected=NestedCall||!Mode||input->Control||input->Id<16?NDIS_STATUS_SUCCESS:NDIS_STATUS_PAUSED;CHECK(head->Status==expected&&input->Nested==(ULONG)NestedCall);
        if(!input->Control){UCHAR original[1500];DataPacket(original,input->Id,(BOOLEAN)input->Nested);CHECK(!memcmp(input->Data+14,original,1500));}
        IoFreeMdl(input->Nb.Mdl);NdisFreeNetBufferList(head);ExFreePoolWithTag(input,0);++count;head=next;
    }
    if(NestedCall){CHECK(count==32);CompletedNested+=count;return;}
    CHECK(count==33&&!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]);CompletedOuter+=count;CHECK(SetEvent(CompleteEntered));Await(CompleteGo);
}
#define PbIndicateReceive BatchIndicate
#define PbCompleteSend BatchComplete
#include "../../driver/v2/ndis_callouts.h"
#undef PbIndicateReceive
#undef PbCompleteSend
static VOID BatchReplyCopy(VOID *destination,const VOID *source,SIZE_T length)
{CHECK(source&&destination&&length==ExpectedReplyLength&&!ReplyCopies);++ReplyCopies;ReplyBytes+=(ULONG)length;memcpy(destination,source,length);}
#pragma push_macro("RtlCopyMemory")
#undef RtlCopyMemory
#define RtlCopyMemory BatchReplyCopy
#define PbV2ControlReply ActualBatchReply
#define PbIndicateReceive BatchIndicate
#include PB_BATCH_REPLY
#undef PbV2ControlReply
#undef PbIndicateReceive
#pragma pop_macro("RtlCopyMemory")
static VOID BatchReject(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,ULONG prior,ULONG queue,ULONG offset,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{(void)prior;CHECK(ctx==Ctx&&epoch==42&&nbl&&nb&&Mode&&status==NDIS_STATUS_PAUSED&&queue==PB_SEND_QUEUE_UNKNOWN&&!offset&&failure->Reason==PB_SEND_ADMISSION&&!accepted&&!attempted&&scope==2&&!q&&!pending);++Rejects;}
static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbl){CHECK(nbl);}
static PB_RESULT BatchPush(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{CHECK(view==&Session->Queues[0].Rings[PB_QUEUE_TX_READY]&&PushCalls<3);PushCounts[PushCalls++]=count;return PbQueuePush(view,items,count,accepted,notify);}
#define PbV2Describe ActualBatchDescribe
#define PbV2ControlReply ActualBatchReply
#define PbCompleteSend BatchComplete
#define PbV2RejectSend BatchReject
#define PbQueuePush BatchPush
#include "../../driver/v2/session_send.h"
#undef PbQueuePush
#undef PbV2Describe
#undef PbV2ControlReply
#undef PbCompleteSend
#undef PbV2RejectSend
#include PB_BATCH_FRAME
static VOID DataPacket(UCHAR *p,ULONG id,BOOLEAN nested)
{
    ULONG ip=Ip==4?20u:40u,header=ip+(Protocol==6?20u:8u);memset(p,0,1500);
    if(Ip==4){p[0]=0x45;W16(p+2,1500);p[8]=64;p[9]=(UCHAR)Protocol;p[12]=10;p[15]=1;p[16]=10;p[19]=2;}else{p[0]=0x60;W16(p+4,1460);p[6]=(UCHAR)Protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=1;p[39]=2;}
    W16(p+ip,(uint16_t)(4000+nested));W16(p+ip+2,5000);if(Protocol==6){W32(p+ip+4,1000+id*(1500-header));p[ip+12]=0x50;p[ip+13]=0x10;W16(p+ip+14,8192);}else W16(p+ip+4,1500-ip);
    memset(p+header,(int)(id+(nested?0x80:0x20)),1500-header);W32(p+header,id);W32(p+header+4,nested);CHECK(PbFinalizeChecksums(p,1500,Ip==4,Protocol==6,Protocol==17)==PB_PACKET_OK);
}
static NET_BUFFER_LIST *MakeChain(BOOLEAN nested)
{
    NET_BUFFER_LIST *head=NULL,*tail=NULL;ULONG count=nested?32:33;
    for(ULONG i=0;i<count;++i){B_INPUT *input=ExAllocatePool2(0,sizeof(*input),0);CHECK(input);input->Nested=nested;input->Control=!nested&&i==16;input->Id=nested?i:i<16?i:i-1;
        if(input->Control){input->Nb.Length=Frame(input->Data,(BOOLEAN)(Ip==6));CHECK(PbEthernetControl(input->Data,input->Nb.Length,Ctx->Probe.PeerMac,ExpectedReply,&ExpectedReplyLength)==PB_ETH_REPLY);}
        else{input->Nb.Length=1514;memset(input->Data,0,14);input->Data[0]=input->Data[6]=2;W16(input->Data+12,Ip==4?0x0800:0x86dd);DataPacket(input->Data+14,input->Id,nested);}
        input->Nb.Data=input->Data;input->Nb.Mdl=IoAllocateMdl(input->Data,input->Nb.Length,FALSE,FALSE,NULL);CHECK(input->Nb.Mdl);MmBuildMdlForNonPagedPool(input->Nb.Mdl);
        NET_BUFFER_LIST *nbl=NdisAllocateNetBufferAndNetBufferList(Ctx,0,0,input->Nb.Mdl,0,input->Nb.Length);CHECK(nbl);memset(nbl->Info,0,sizeof(nbl->Info));nbl->Flags=0;nbl->Reserved[0]=input;if(tail)tail->Next=nbl;else head=nbl;tail=nbl;
    }return head;
}
static VOID BatchSignals(KEVENT *event){if(GetCurrentThreadId()==PauseId&&event==Session->Queues[0].KernelEvent)CHECK(SetEvent(BatchPauseReady));}
static VOID BatchReturning(EX_RUNDOWN_REF *refs){if((LONG)GetCurrentThreadId()==SenderId&&refs==&Session->Returns&&!NestedCall){CHECK(!Session->Active&&SetEvent(BatchTail));Await(BatchTailGo);}}
static VOID ObservedBatchRundown(EX_RUNDOWN_REF *refs){CHECK(refs==&Session->Returns&&!Session->Active&&refs->References==1);BOOLEAN acquired=TryAcquireSRWLockExclusive(&refs->Lock);if(acquired)ReleaseSRWLockExclusive(&refs->Lock);CHECK(!acquired&&SetEvent(BatchRundown));ExWaitForRundownProtectionRelease(refs);}
static DWORD WINAPI SendOriginal(VOID *head){SenderId=(LONG)GetCurrentThreadId();CHECK(PbV2Send(Ctx,head));return 0;}
static DWORD WINAPI DrainOriginal(VOID *unused)
{(void)unused;KIRQL irql=ExAcquireSpinLockExclusive(&Ctx->TransitionLock);PbV2BeginDrain(Session,10000);ULONG bank=PbRotateIndications(Ctx);ExReleaseSpinLockExclusive(&Ctx->TransitionLock,irql);PbV2Progress(Session,TRUE);CHECK(SetEvent(DrainReady));PbWaitIndications(Ctx,bank);PbV2Wake(Session);return 0;}
static DWORD WINAPI IdleBatch(VOID *unused){(void)unused;Await(Session->StopEvent.Native);return 0;}
static VOID BatchCase(ULONG protocol,ULONG minor,ULONG mode,BOOLEAN wrap,BOOLEAN measured)
{
    Ip=PB_TEST_IP_VERSION;Protocol=protocol;Mode=mode;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=0;Replies=PayloadReads=PrefixReads=ControlReads=CompletedOuter=CompletedNested=Rejects=0;Created=Joined=WorkerJoined=0;PauseId=0;NestedCall=FALSE;
    PayloadBytes=PrefixBytes=ControlBytes=ReplyCopies=ReplyBytes=PushCalls=0;memset(PushCounts,0,sizeof(PushCounts));Base=wrap?UINT64_MAX-31:0;
    Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
    CHECK(PbCalculateLayout(1,64,128,&Session->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    UCHAR *region=PbTestGuardAlloc(Session->Layout.RegionSize);memset(region,0,Session->Layout.RegionSize);
#else
    UCHAR *region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
    CHECK(region);Session->Region=region;Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);Session->PagesLocked=TRUE;Locked=1;
    Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
    PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,64*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,64*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,128*sizeof(*q->Pending),0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,64,42,0)==PB_OK);
    for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);q->Rings[r].Shared->Producer=q->Rings[r].Shared->Consumer=q->Rings[r].Cursor=q->Rings[r].PeerCursor=Base;}
    HANDLE *events[]={&ReplyEntered,&ReplyGo,&CompleteEntered,&CompleteGo,&BatchTail,&BatchTailGo,&BatchPauseReady,&DrainReady,&BatchRundown,&PauseWaiting};for(ULONG i=0;i<_countof(events);++i)*events[i]=Event(TRUE,FALSE);
    q->Thread=Start(IdleBatch,NULL);OnSetEvent=BatchSignals;BeforeRundownRelease=BatchReturning;HANDLE sender=Start(SendOriginal,MakeChain(FALSE));Await(ReplyEntered);HANDLE pause=NULL,drain=NULL;
    if(mode==1){pause=Start(Pausing,NULL);Await(BatchPauseReady);CHECK(!Ctx->Running&&Session->Active==1&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);}
    if(mode==2){drain=Start(DrainOriginal,NULL);Await(DrainReady);CHECK(Session->Draining&&Session->Active==1&&WaitForSingleObject(drain,0)==WAIT_TIMEOUT);}
    CHECK(SetEvent(ReplyGo));Await(CompleteEntered);if(drain)Join(drain);if(!pause){pause=Start(Pausing,NULL);Await(BatchPauseReady);}ULONG expected=mode?48:64;CHECK(Session->Active==1&&Session->TxObjects==expected&&PayloadReads==expected&&PrefixReads==expected&&ControlReads==2&&Rejects==(mode?16u:0u));
    CHECK(PayloadBytes==1500*expected&&PrefixBytes==59*expected&&ControlBytes==(Ip==4?60u:86u)&&PushCalls==(mode?2u:3u)&&PushCounts[0]==16&&PushCounts[1]==32&&PushCounts[2]==(mode?0u:16u));
    CHECK(ReplyCopies==1&&ReplyBytes==ExpectedReplyLength&&ReplyBytes==(Ip==4?60u:86u));
    CHECK(CompletedNested==32&&CompletedOuter==33&&Ctx->Statistics.ifHCOutUcastPkts==expected&&Ctx->Statistics.ifHCOutOctets==1500*expected&&!Session->Faulted&&!q->TxPoolExhausted);
    PB_QUEUE_VIEW consumer,recycler;CHECK(PbAttachQueue(&consumer,region,&Session->Layout,0,PB_QUEUE_TX_READY,FALSE)==PB_OK&&PbAttachQueue(&recycler,region,&Session->Layout,0,PB_QUEUE_TX_RECYCLE,TRUE)==PB_OK);consumer.Cursor=consumer.PeerCursor=recycler.Cursor=recycler.PeerCursor=Base;
    PB_DESCRIPTOR items[64];uint32_t count,accepted,notify;CHECK(PbQueuePop(&consumer,items,64,&count)==PB_OK&&count==expected);BYTE data[1500];for(ULONG i=0;i<count;++i){BOOLEAN nested=i>=16&&i<48;ULONG id=nested?i-16:i<16?i:i-32;DataPacket(data,id,nested);CHECK(!items[i].Type&&!items[i].Flags&&items[i].Length==1500&&items[i].Token.Slot==i&&items[i].Token.Generation==1&&!memcmp(region+items[i].DataOffset,data,1500));}
    CHECK(PbQueuePush(&recycler,items,count,&accepted,&notify)==PB_OK&&accepted==count);KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->TxLock,&lock);CHECK(PbV2RecycleLocked(q)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);
    CHECK(q->Rings[PB_QUEUE_TX_READY].Cursor==Base+expected&&consumer.Cursor==Base+expected&&q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==Base+expected&&recycler.Cursor==Base+expected);for(ULONG i=0;i<64;++i)CHECK(q->TxSlots[i].State==PB_SLOT_FREE&&q->TxSlots[i].Generation==(i<expected?1u:0u));
    CHECK(SetEvent(CompleteGo));Await(BatchTail);Join(pause);CHECK(Session->Returns.References==1&&!Session->Active);BeforePoolFree=Freed;HANDLE cleanup=Start(Unregistering,NULL);Await(BatchRundown);CHECK(Session&&Locked==1&&Mdls==1&&!Nbls&&SetEvent(BatchTailGo));Join(sender);Join(cleanup);
    CHECK(!Session&&!Ctx->V2&&!Locked&&!Objects&&!Mdls&&!Nbls&&WorkerJoined==1&&Created==Joined&&Joined==(mode==2?5u:4u));BeforeRundownRelease=NULL;BeforePoolFree=NULL;OnSetEvent=NULL;Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(region);
#else
    CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
    for(ULONG i=0;i<_countof(events);++i)Close(*events[i]);CHECK(!Pools&&!Handles&&!Objects&&!Locked&&!Mdls&&!Nbls&&FixtureRundownAcquires==FixtureRundownReleases);++BatchCases;BatchJoins+=Joined;BatchWorkers+=WorkerJoined;
    printf("CONTROL_BATCH32 ip%lu proto%lu ndis%lu mode%lu wrap%u measured%u outer32 control1 nested32 published%lu push%lu payload%lu prefix%lu controlBytes%lu replyCopies%lu replyBytes%lu joins%lu owners0 repairs0\n",Ip,protocol,minor,mode,wrap,measured,expected,PushCalls,PayloadBytes,PrefixBytes,ControlBytes,ReplyCopies,ReplyBytes,Joined);
}
int main(VOID)
{
    DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG mode=0;mode<3;++mode)BatchCase(protocol,minor,mode,(BOOLEAN)wrap,(BOOLEAN)pass);}
    CHECK(BatchCases==48&&BatchJoins==208&&BatchWorkers==48&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: CONTROL_BATCH32 family%u cases48 warm24 measured24 joins%lu workerJoins%lu handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,BatchJoins,BatchWorkers,before,after);return 0;
}
