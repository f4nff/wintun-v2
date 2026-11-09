/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Send/recycle/Receive/Return and hosted native lifetime. Bad recycle
 * generation is a defensive wire input. No private ownership repair, fake
 * FULL, actual NDIS worker loop, driver installation or throughput claim. */
#ifdef PB_API_TESTS
#error Shipping recycle send paths required
#endif
#ifndef NDEBUG
#error Shipping recycle send configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"recycle send requires shipping fingerprint");
#include PB_RCS_CONTROL
static BOOLEAN RcsLateMode;
static VOID LateComplete(NDIS_HANDLE,NET_BUFFER_LIST *,ULONG);
static VOID LateReject(TUN_CTX *,uint64_t,NET_BUFFER_LIST *,NET_BUFFER *,NDIS_STATUS,ULONG,ULONG,ULONG,const PB_SEND_FAILURE *,ULONG,ULONG,ULONG,PB_KERNEL_QUEUE *,ULONG);
static ULONG RcsStage,RcsCompleted,RcsReserves,RcsReasons[PB_SEND_REASON_COUNT],RcsWakes[3],RcsCases,RcsJoins,RcsWorkers;
static BOOLEAN RcsObserve;
static PB_RESULT RcsReserve(PB_POOL *pool,PB_TOKEN *token){CHECK(pool==&Session->Queues[0].TxPool);++RcsReserves;return PbPoolReserve(pool,token);}
static VOID RcsReject(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,ULONG prior,ULONG queue,ULONG offset,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{
 if(RcsLateMode){LateReject(ctx,epoch,nbl,nb,status,prior,queue,offset,failure,accepted,attempted,scope,q,pending);return;}
 CHECK(ctx==Ctx&&epoch==42&&nbl&&nb&&!prior&&!accepted&&!attempted&&scope==2&&!pending&&failure->Reason<PB_SEND_REASON_COUNT);++RcsReasons[failure->Reason];
 if(RcsStage==1)CHECK(status==NDIS_STATUS_RESOURCES&&failure->Reason==PB_SEND_POOL_FULL&&failure->CoreStatus==PB_FULL&&q==Session->Queues&&queue==0&&offset==14);
 else if(failure->Reason==PB_SEND_RECYCLE)CHECK(RcsStage==2&&status==NDIS_STATUS_PAUSED&&failure->CoreStatus==PB_STALE&&q==Session->Queues&&queue==0&&offset==14);
 else CHECK(RcsStage==2&&status==NDIS_STATUS_PAUSED&&failure->Reason==PB_SEND_ADMISSION&&!q&&queue==PB_SEND_QUEUE_UNKNOWN&&!offset);
}
static VOID RcsSdkComplete(NDIS_HANDLE adapter,NET_BUFFER_LIST *head,ULONG flags)
{
 if(RcsLateMode){LateComplete(adapter,head,flags);return;}
 AssertNoLocks();CHECK(adapter==Ctx&&flags==1&&Level==2&&Session->Active==33&&Session->Returns.References==1);ULONG count=0;
 while(head){NET_BUFFER_LIST *next=head->Next;B_INPUT *input=(B_INPUT *)head->Reserved[0];CHECK(input&&!input->Control&&!input->Nested&&input->Id==count&&head->Status==(RcsStage==0?NDIS_STATUS_SUCCESS:RcsStage==1?NDIS_STATUS_RESOURCES:NDIS_STATUS_PAUSED));UCHAR expected[1500];DataPacket(expected,input->Id,FALSE);CHECK(!memcmp(expected,input->Data+14,1500));IoFreeMdl(input->Nb.Mdl);NdisFreeNetBufferList(head);ExFreePoolWithTag(input,0);head=next;++count;}
 CHECK(count==32);RcsCompleted+=count;
}
static NET_BUFFER_LIST *RcsData(VOID)
{
 NET_BUFFER_LIST *head=NULL,*tail=NULL;for(ULONG i=0;i<32;++i){B_INPUT *input=ExAllocatePool2(0,sizeof(*input),0);CHECK(input);input->Id=i;input->Nb.Length=1514;input->Nb.Data=input->Data;input->Data[0]=input->Data[6]=2;W16(input->Data+12,Ip==4?0x0800:0x86dd);DataPacket(input->Data+14,i,FALSE);input->Nb.Mdl=IoAllocateMdl(input->Data,1514,FALSE,FALSE,NULL);CHECK(input->Nb.Mdl);MmBuildMdlForNonPagedPool(input->Nb.Mdl);NET_BUFFER_LIST *nbl=NdisAllocateNetBufferAndNetBufferList(Ctx,0,0,input->Nb.Mdl,0,1514);CHECK(nbl);memset(nbl->Info,0,sizeof(nbl->Info));nbl->Flags=0;nbl->Reserved[0]=input;if(tail)tail->Next=nbl;else head=nbl;tail=nbl;}return head;
}
static VOID RcsNotify(KEVENT *event){if(!RcsObserve)return;if(event==Session->ProgressEvent)++RcsWakes[0];else if(event==Session->Queues[0].UserEvent)++RcsWakes[1];else if(event==Session->Queues[0].KernelEvent)++RcsWakes[2];}
static DWORD WINAPI RcsIdle(VOID *opaque){(void)opaque;Await(Session->StopEvent.Native);return 0;}
static DWORD WINAPI RcsReturn(VOID *opaque){NET_BUFFER_LIST *list=opaque;TunReturnNetBufferLists(Ctx,list,0);return 0;}
static ULONG64 KeQueryInterruptTime(VOID){return 0;}
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#include "../../driver/v2/session_query.h"
static VOID RcsStatus(ULONG active,ULONG state){PB_V2_STATUS output;CHECK(PbV2Query(Ctx,&output)==STATUS_SUCCESS&&output.ActiveReferences==active&&output.State==state);}
static VOID RcsCase(ULONG protocol,ULONG minor,BOOLEAN wrap,BOOLEAN measured)
{
 Ip=PB_TEST_IP_VERSION;Protocol=protocol;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=0;Created=Joined=WorkerJoined=0;Indications=0;Lists[0]=Lists[1]=NULL;PayloadReads=PrefixReads=ControlReads=PayloadBytes=PrefixBytes=ControlBytes=PushCalls=0;RcsStage=RcsCompleted=RcsReserves=0;memset(PushCounts,0,sizeof(PushCounts));memset(RcsReasons,0,sizeof(RcsReasons));memset(RcsWakes,0,sizeof(RcsWakes));RcsObserve=FALSE;OnSetEvent=RcsNotify;Base=wrap?UINT64_MAX-15:0;
 Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.V2SessionActive=1;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
 CHECK(PbCalculateLayout(1,32,64,&Session->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
 UCHAR *region=PbTestGuardAlloc(Session->Layout.RegionSize);memset(region,0,Session->Layout.RegionSize);
#else
 UCHAR *region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
 CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);Session->PagesLocked=TRUE;Locked=1;
 Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);PauseWaiting=Event(TRUE,FALSE);
 PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,32*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,32*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,64*sizeof(*q->Pending),0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,32,42,0)==PB_OK);
 for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);q->Rings[r].Shared->Producer=q->Rings[r].Shared->Consumer=q->Rings[r].Cursor=q->Rings[r].PeerCursor=Base;}q->Thread=Start(RcsIdle,NULL);
 PB_QUEUE_VIEW submit,ready,recycle,completion;CHECK(PbAttachQueue(&submit,region,&Session->Layout,0,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(&ready,region,&Session->Layout,0,PB_QUEUE_TX_READY,FALSE)==PB_OK&&PbAttachQueue(&recycle,region,&Session->Layout,0,PB_QUEUE_TX_RECYCLE,TRUE)==PB_OK&&PbAttachQueue(&completion,region,&Session->Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);submit.Cursor=submit.PeerCursor=ready.Cursor=ready.PeerCursor=recycle.Cursor=recycle.PeerCursor=completion.Cursor=completion.PeerCursor=Base;
 PB_DESCRIPTOR rx[32]={0},tx[32],out[32];uint32_t count,notify;for(ULONG i=0;i<32;++i){rx[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=1500,.DataOffset=Session->Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};DataPacket(region+rx[i].DataOffset,i,TRUE);}CHECK(PbQueuePush(&submit,rx,32,&count,&notify)==PB_OK&&count==32&&PbV2ReceiveBatch(q)&&Indications==1&&Session->Active==32&&q->Credits==32);
 for(ULONG step=0;step<3;++step){RcsStage=step;if(step==2){PB_TOKEN tokens[32];for(ULONG i=0;i<32;++i)tokens[i]=tx[i].Token;tokens[16].Generation++;CHECK(PbQueuePushTokens(&recycle,tokens,32,&count,&notify)==PB_OK&&count==32);RcsObserve=TRUE;}
  CHECK(PbV2Send(Ctx,RcsData())&&Session->Active==32&&!Session->Returns.References&&RcsCompleted==32*(step+1));
  if(!step){CHECK(Session->TxObjects==32&&PayloadReads==32&&PrefixReads==32&&PushCalls==1&&PushCounts[0]==32&&RcsReserves==32&&q->TxPool.FreeBits[0]==0);CHECK(PbQueuePop(&ready,tx,32,&count)==PB_OK&&count==32);for(ULONG i=0;i<32;++i){UCHAR bytes[1500];DataPacket(bytes,i,FALSE);CHECK(tx[i].Token.Epoch==42&&!tx[i].Token.Queue&&tx[i].Token.Slot==i&&tx[i].Token.Generation==1&&!tx[i].Type&&!tx[i].Flags&&tx[i].Length==1500&&!memcmp(bytes,region+tx[i].DataOffset,1500));}}
  else if(step==1)CHECK(!Session->Faulted&&q->TxPoolExhausted&&q->TxPool.FreeBits[0]==0&&RcsReserves==33&&PrefixReads==64&&PayloadReads==32&&RcsReasons[PB_SEND_POOL_FULL]==32);
 }
 CHECK(Session->Faulted&&Session->Draining&&!Ctx->Probe.V2SessionActive&&!q->TxPoolExhausted&&q->TxPool.FreeBits[0]==0xffffu&&RcsReserves==33&&PayloadReads==32&&PayloadBytes==48000&&PrefixReads==65&&PrefixBytes==3835&&PushCalls==1&&!ControlReads&&RcsReasons[PB_SEND_POOL_FULL]==32&&RcsReasons[PB_SEND_RECYCLE]==1&&RcsReasons[PB_SEND_ADMISSION]==31&&q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==Base+32);
 for(ULONG i=0;i<32;++i)CHECK(q->TxSlots[i].Generation==1&&q->TxSlots[i].State==(i<16?PB_SLOT_FREE:PB_SLOT_PUBLISHED)&&q->RxSlots[i].Active&&q->RxSlots[i].Generation==1);
 CHECK(RcsWakes[0]==1&&RcsWakes[1]==1&&RcsWakes[2]==1&&Session->Lifecycle->Sequence==1&&Session->Lifecycle->Terminal==1);PbV2Fault(Session);PbV2Fault(Session);CHECK(RcsWakes[0]==1&&RcsWakes[1]==1&&RcsWakes[2]==1&&Session->Lifecycle->Sequence==1);RcsObserve=FALSE;
 RcsStatus(32,PB_SESSION_QUARANTINED);PbV2QuiesceFaultedQueue(q);CHECK(q->FaultQuiesced);RcsStatus(32,PB_SESSION_QUARANTINED);
 NET_BUFFER_LIST *first=Lists[0],*last=first;Lists[0]=NULL;for(ULONG i=0;i<15;++i){CHECK(last);last=last->Next;}CHECK(last);NET_BUFFER_LIST *second=last->Next;last->Next=NULL;CHECK(second);
 for(ULONG half=0;half<2;++half){NET_BUFFER_LIST *head=half?second:first;ULONG seen=0;for(NET_BUFFER_LIST *p=head;p;p=p->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)p->Reserved[1]&~(ULONG_PTR)1);UCHAR expected[1500];DataPacket(expected,half*16+seen,TRUE);CHECK(r->Count==1&&r->Descriptors[0].Token.Slot==half*16+seen&&r->Mdl->Partial&&!memcmp(r->Mdl->Address,expected,1500));VOID *future=(VOID *)(ULONG_PTR)~0u;CHECK(p->Info[TcpRecvSegCoalesceInfo]==(minor==20?future:NULL)&&p->Info[RscTcpTimestampDelta]==(minor==20?future:NULL)&&p->Info[UdpRecvSegCoalesceOffloadInfo]==(minor==20?future:NULL));++seen;}CHECK(seen==16);Join(Start(RcsReturn,head));CHECK(Session->Active==(half?0:16)&&q->Credits==(half?0u:16u)&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References);RcsStatus(half?0:16,half?PB_SESSION_STOPPED:PB_SESSION_QUARANTINED);}
 CHECK(PbQueuePop(&completion,out,32,&count)==PB_EMPTY&&!count);for(ULONG i=0;i<32;++i)CHECK(!q->RxSlots[i].Active);CHECK(Session->RxObjects==32&&Session->RxReturned==32&&!Session->Completions&&Ctx->Statistics.ifHCInUcastPkts==32&&Ctx->Statistics.ifHCInOctets==48000);
 Pausing(NULL);BeforePoolFree=Freed;Unregistering(NULL);CHECK(!Session&&!Ctx->V2&&!Locked&&!Objects&&!Mdls&&!Nbls&&Created==3&&Joined==3&&WorkerJoined==1);BeforePoolFree=NULL;OnSetEvent=NULL;Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;Close(PauseWaiting);
#ifdef PB_GUARD_TESTS
 PbTestGuardFree(region);
#else
 CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
 CHECK(!Pools&&!Handles&&!Objects&&!Mdls&&!Nbls&&FixtureRundownAcquires==FixtureRundownReleases);++RcsCases;RcsJoins+=Joined;RcsWorkers+=WorkerJoined;printf("RECYCLE_SEND32 ip%lu proto%lu ndis%lu wrap%u measured%u sends32/32/32 rx32 recycle32 bad17 free16 published16 reserve33 payload48000 prefix3835 wake1/1/1 returned16/16 rxRetired32 rxComplete0 joins3 owners0 repairs0\n",Ip,protocol,minor,wrap,measured);
}
static int RcsDefaultMain(VOID){DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)RcsCase(protocol,minor,(BOOLEAN)wrap,(BOOLEAN)pass);}CHECK(RcsCases==16&&RcsJoins==48&&RcsWorkers==16&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: RECYCLE_SEND32 family%u cases16 warm8 measured8 joins48 workerJoins16 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;}

/* Valid late pressure: the scheduling gate follows the real shared-lock release.
 * SDK waits here model scheduling only, not permissible native spinlock blocking.
 * The sole peer owns both ring views until it joins; only then may main take over.
 * The native worker is idle-only; final recycling runs the actual locked helper. */
static HANDLE LateHandoff,LateContinue,LatePeerDone;
static volatile LONG LateArmed;
static BOOLEAN LatePaused;
static PB_QUEUE_VIEW LateConsumer,LateRecycler;
static ULONG LateCases,LateJoins,LateWorkers,LatePoolDrops,LateAdmissionDrops,LateCompleted;
static LONG LateSenderId;
static VOID LateReject(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,ULONG prior,ULONG queue,ULONG offset,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{
 CHECK(ctx==Ctx&&epoch==42&&nbl&&nb&&!prior&&!accepted&&!attempted&&scope==2);B_INPUT *input=(B_INPUT *)nbl->Reserved[0];
 if(failure->Reason==PB_SEND_POOL_FULL){CHECK(input->Id==16&&status==NDIS_STATUS_RESOURCES&&failure->CoreStatus==PB_FULL&&q==Session->Queues&&queue==0&&offset==14&&pending==16&&++LatePoolDrops==1);InterlockedExchange(&LateArmed,1);}
 else{CHECK(LatePaused&&input->Id>=17&&status==NDIS_STATUS_PAUSED&&failure->Reason==PB_SEND_ADMISSION&&!q&&queue==PB_SEND_QUEUE_UNKNOWN&&!offset&&!pending);++LateAdmissionDrops;}
}
static VOID RcsSharedRelease(SRWLOCK *lock,KIRQL saved)
{
 ExReleaseSpinLockShared(lock,saved);
 if(RcsLateMode&&(LONG)GetCurrentThreadId()==LateSenderId&&InterlockedCompareExchange(&LateArmed,0,1)==1){
  BOOLEAN got=TryAcquireSRWLockExclusive(&Ctx->TransitionLock);if(got)ReleaseSRWLockExclusive(&Ctx->TransitionLock);CHECK(got);got=TryAcquireSRWLockExclusive((SRWLOCK *)&Session->Queues[0].TxLock);if(got)ReleaseSRWLockExclusive((SRWLOCK *)&Session->Queues[0].TxLock);CHECK(got);
  CHECK(Session->TxObjects==16&&PushCalls==1&&PushCounts[0]==16&&Session->Active==1&&SetEvent(LateHandoff));Await(LateContinue);
 }
}
static VOID LateComplete(NDIS_HANDLE adapter,NET_BUFFER_LIST *head,ULONG flags)
{
 AssertNoLocks();CHECK(adapter==Ctx&&flags==1&&Level==2&&Session->Active==1&&Session->Returns.References==(LatePaused?2:1));ULONG count=0;
 while(head){NET_BUFFER_LIST *next=head->Next;B_INPUT *input=(B_INPUT *)head->Reserved[0];CHECK(input&&input->Id==count&&!input->Nested&&!input->Control);ULONG expected=count==16?NDIS_STATUS_RESOURCES:LatePaused&&count>16?NDIS_STATUS_PAUSED:NDIS_STATUS_SUCCESS;CHECK(head->Status==expected);UCHAR data[1500];DataPacket(data,count,FALSE);CHECK(!memcmp(data,input->Data+14,1500));IoFreeMdl(input->Nb.Mdl);NdisFreeNetBufferList(head);ExFreePoolWithTag(input,0);head=next;++count;}
 CHECK(count==32);LateCompleted+=count;
}
static DWORD WINAPI LateSending(VOID *head){LateSenderId=(LONG)GetCurrentThreadId();CHECK(PbV2Send(Ctx,head));return 0;}
static DWORD WINAPI LatePeer(VOID *unused)
{
 (void)unused;uint32_t ready,count,accepted,notify;CHECK(PbQueueArm(&LateConsumer,&ready)==PB_OK&&!ready);CHECK(SetEvent(LatePeerDone));Await(Session->Queues[0].UserEvent->Native);
 PB_DESCRIPTOR items[16];CHECK(PbQueuePop(&LateConsumer,items,16,&count)==PB_OK&&count==16);PB_TOKEN tokens[16];
 for(ULONG i=0;i<16;++i){UCHAR expected[1500];DataPacket(expected,i,FALSE);CHECK(items[i].Token.Epoch==42&&!items[i].Token.Queue&&items[i].Token.Slot==i&&items[i].Token.Generation==1&&items[i].Length==1500&&!items[i].Type&&!items[i].Flags&&!memcmp(expected,Session->Region+items[i].DataOffset,1500));tokens[i]=items[i].Token;}
 CHECK(PbQueuePushTokens(&LateRecycler,tokens,16,&accepted,&notify)==PB_OK&&accepted==16);if(notify)KeSetEvent(Session->Queues[0].KernelEvent,0,FALSE);return 0;
}
static VOID LateCase(ULONG protocol,ULONG minor,BOOLEAN wrap,BOOLEAN paused,BOOLEAN measured)
{
 CHECK(!Ctx&&!Session&&!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);RcsLateMode=TRUE;LatePaused=paused;Ip=PB_TEST_IP_VERSION;Protocol=protocol;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=0;Created=Joined=WorkerJoined=Indications=0;PauseId=0;LateSenderId=0;LateArmed=0;LatePoolDrops=LateAdmissionDrops=LateCompleted=0;RcsReserves=0;PayloadReads=PrefixReads=ControlReads=PayloadBytes=PrefixBytes=ControlBytes=PushCalls=0;memset(PushCounts,0,sizeof(PushCounts));Failure=0;Base=wrap?UINT64_MAX-15:0;
 Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.V2SessionActive=1;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;CHECK(PbCalculateLayout(1,16,32,&Session->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
 UCHAR *region=PbTestGuardAlloc(Session->Layout.RegionSize);memset(region,0,Session->Layout.RegionSize);
#else
 UCHAR *region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
 CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);Session->PagesLocked=TRUE;Locked=1;
 Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
 PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,16*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,16*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,32*sizeof(*q->Pending),0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,16,42,0)==PB_OK);
 for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);PB_QUEUE_VIEW *v=q->Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=Base;}
 CHECK(PbAttachQueue(&LateConsumer,region,&Session->Layout,0,PB_QUEUE_TX_READY,FALSE)==PB_OK&&PbAttachQueue(&LateRecycler,region,&Session->Layout,0,PB_QUEUE_TX_RECYCLE,TRUE)==PB_OK);LateConsumer.Cursor=LateConsumer.PeerCursor=LateRecycler.Cursor=LateRecycler.PeerCursor=Base;
 LateHandoff=Event(TRUE,FALSE);LateContinue=Event(TRUE,FALSE);LatePeerDone=Event(TRUE,FALSE);PauseWaiting=Event(TRUE,FALSE);q->Thread=Start(RcsIdle,NULL);HANDLE peer=Start(LatePeer,NULL);Await(LatePeerDone);HANDLE sender=Start(LateSending,RcsData());Await(LateHandoff);Join(peer);CHECK(LateConsumer.Cursor==Base+16&&LateRecycler.Cursor==Base+16&&q->TxPoolExhausted);HANDLE pause=NULL;
 if(paused){pause=Start(Pausing,NULL);Await(PauseWaiting);CHECK(!Ctx->Running&&Session->Active==1&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);}
 CHECK(SetEvent(LateContinue));Join(sender);if(pause)Join(pause);else Pausing(NULL);ULONG accepted=paused?16:31;
 CHECK(LatePoolDrops==1&&LateAdmissionDrops==(paused?15u:0u)&&LateCompleted==32&&RcsReserves==(paused?17u:32u)&&PayloadReads==accepted&&PayloadBytes==1500*accepted&&PrefixReads==(paused?17u:32u)&&PrefixBytes==59*PrefixReads&&!ControlReads&&PushCalls==(paused?1u:2u)&&PushCounts[0]==16&&PushCounts[1]==(paused?0u:15u)&&Session->TxObjects==accepted&&!Session->Faulted&&!Session->Active&&!Session->Returns.References&&!Nbls&&Mdls==1);
 CHECK(Ctx->Statistics.ifHCOutUcastPkts==accepted&&Ctx->Statistics.ifHCOutOctets==1500*accepted);PB_DESCRIPTOR tail[16];uint32_t count,n,notify;PB_RESULT popped=PbQueuePop(&LateConsumer,tail,16,&count);CHECK(popped==(paused?PB_EMPTY:PB_OK)&&count==(paused?0u:15u));
 if(!paused){PB_TOKEN tokens[15];for(ULONG i=0;i<15;++i){UCHAR expected[1500];DataPacket(expected,17+i,FALSE);CHECK(tail[i].Token.Epoch==42&&!tail[i].Token.Queue&&tail[i].Token.Slot==i&&tail[i].Token.Generation==2&&!tail[i].Type&&!tail[i].Flags&&tail[i].Length==1500&&!memcmp(expected,region+tail[i].DataOffset,1500));tokens[i]=tail[i].Token;}CHECK(PbQueuePushTokens(&LateRecycler,tokens,15,&n,&notify)==PB_OK&&n==15);}
 KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->TxLock,&lock);CHECK(PbV2RecycleLocked(q)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);for(ULONG i=0;i<16;++i)CHECK(q->TxSlots[i].State==PB_SLOT_FREE&&q->TxSlots[i].Generation==(!paused&&i<15?2u:1u));CHECK(!q->TxPoolExhausted&&q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==Base+accepted&&LateRecycler.Cursor==Base+accepted&&LateConsumer.Cursor==Base+accepted);
 BeforePoolFree=Freed;Unregistering(NULL);CHECK(!Session&&!Ctx->V2&&!Locked&&!Objects&&!Mdls&&!Nbls&&Created==Joined&&Joined==(paused?4u:3u)&&WorkerJoined==1);BeforePoolFree=NULL;Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;Close(LateHandoff);Close(LateContinue);Close(LatePeerDone);Close(PauseWaiting);
#ifdef PB_GUARD_TESTS
 PbTestGuardFree(region);
#else
 CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
 CHECK(!Pools&&!Handles&&!Objects&&!Mdls&&!Nbls&&FixtureRundownAcquires==FixtureRundownReleases);RcsLateMode=FALSE;++LateCases;LateJoins+=Joined;LateWorkers+=WorkerJoined;
 printf("LATE_PRESSURE32 ip%lu proto%lu ndis%lu wrap%u paused%u measured%u accepted%lu resources1 pausedDrops%lu payload%lu prefix%lu push%lu joins%lu owners0 repairs0\n",Ip,protocol,minor,wrap,paused,measured,accepted,LateAdmissionDrops,PayloadBytes,PrefixBytes,PushCalls,Joined);
}
static int LateMain(VOID)
{
 DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG paused=0;paused<2;++paused)LateCase(protocol,minor,(BOOLEAN)wrap,(BOOLEAN)paused,(BOOLEAN)pass);}
 CHECK(LateCases==32&&LateJoins==112&&LateWorkers==32&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: LATE_PRESSURE32 family%u cases32 warm16 measured16 joins112 workerJoins32 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
int main(int argc,char **argv){if(argc==1)return RcsDefaultMain();CHECK(argc==2&&!strcmp(argv[1],"--late-pressure32"));return LateMain();}
