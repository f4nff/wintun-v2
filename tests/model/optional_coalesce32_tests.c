/* SPDX-License-Identifier: GPL-2.0 */
/* Ready factory optional ExAllocate failure, followed by actual worker/Receive/
 * Return/Pause/Unregister/Free. Full Register is not executed here; the original
 * RegisterOwner group separately proves its optional-allocation acceptance.
 * SDK shims/gates are hosted scheduling models, not native IRQL permission. */
#ifdef PB_API_TESTS
#error Shipping optional coalesce paths required
#endif
#ifndef NDEBUG
#error Shipping optional coalesce configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define main UnusedOptionalWorkerMain
#include PB_OC_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"optional coalesce requires shipping fingerprint");
static HANDLE OcIndicated,OcRunningIdle,OcPausedIdle;
static ULONG OcShape,OcProtocol,OcNbls,OcCases,OcJoins,OcWorkerJoins;
static VOID *OcPointers[3];static ULONG OcFreedCount[3],OcAllocations,OcFailures;
static volatile LONG OcWaits;
static BOOLEAN OcMissing(ULONG index){return OcShape==0?index==0:OcShape==1?index==1:OcShape==2?index==2:OcShape==3?index>0:FALSE;}
static VOID *OcAllocate(ULONG index)
{
 CHECK(index<3&&index==OcAllocations);ULONG before=Acquisitions;BOOLEAN fail=OcMissing(index);Failure=fail?before+1:0;
 VOID *p=ExAllocatePool2(0,PB_BLOCK_SIZE,0);CHECK(Acquisitions==before+1&&((p==NULL)==fail));Failure=0;OcPointers[index]=p;++OcAllocations;if(fail)++OcFailures;return p;
}
static VOID OcFreed(VOID *p)
{CHECK(p);for(ULONG i=0;i<3;++i)if(p==OcPointers[i])CHECK(++OcFreedCount[i]==1);if(p==Session)Freed(p);}
static VOID OcIndicate(VOID *adapter,NET_BUFFER_LIST *head,ULONG count,ULONG flags)
{CHECK(adapter==Ctx&&head&&count==OcNbls&&!flags&&!Lists[0]&&!Indications);Lists[0]=head;++Indications;CHECK(Session->Active==(LONG)count&&Session->RxObjects==32&&SetEvent(OcIndicated));}
static VOID OcWorkerWait(ULONG count,VOID **events,int type,int reason,int mode,BOOLEAN alert,VOID *timeout,VOID *blocks)
{CHECK((count==2||count==3)&&!type&&!reason&&!mode&&!alert&&!timeout&&!blocks);HANDLE h[3];for(ULONG i=0;i<count;++i)h[i]=((KEVENT *)events[i])->Native;CHECK((count==2)==!!Ctx->Running);InterlockedIncrement(&OcWaits);CHECK(SetEvent(count==2?OcRunningIdle:OcPausedIdle)&&WaitForMultipleObjects(count,h,FALSE,10000)<WAIT_OBJECT_0+count);}
static VOID OcVerify(NET_BUFFER_LIST *head)
{
 ULONG seen=0,objects=0,coalesced=0,header=(PB_TEST_IP_VERSION==4?20:40)+(OcProtocol==6?20:8);BOOLEAN one=OcShape==1||OcShape==2;
 for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)n->Reserved[1]&~(ULONG_PTR)1);ULONG originals=one&&!objects?16:1;
  CHECK(r->Queue==Session->Queues&&r->Count==originals&&r->IpVersion==PB_TEST_IP_VERSION&&r->Protocol==OcProtocol&&r->Mdl->Partial==(originals==1)&&r->IndicationLength==(originals==16?header+16*(1500-header):1500)&&r->HeaderMdl->Next==r->Mdl&&r->Mdl->Length==r->IndicationLength);
  if(originals==16){ULONG buffer=OcShape==1?1:0;CHECK(r->CoalesceIndex==buffer&&r->Mdl->Address==Session->Queues[0].CoalesceBuffers[buffer]&&Session->Queues[0].CoalesceBusy[buffer]==1);++coalesced;}else CHECK(r->CoalesceIndex==MAXULONG);
  for(ULONG i=0;i<originals;++i){PB_DESCRIPTOR *d=r->Descriptors+i;CHECK(seen<32&&d->Token.Epoch==42&&d->Token.Queue==0&&d->Token.Slot==seen&&d->Token.Generation==1&&d->Length==1500);UCHAR expected[1500];Packet(expected,PB_TEST_IP_VERSION,OcProtocol,seen,1);CHECK(!memcmp(Session->Region+d->DataOffset,expected,1500));if(originals==16)CHECK(!memcmp((UCHAR *)r->Mdl->Address+header+i*(1500-header),expected+header,1500-header));else CHECK(r->Mdl->Address==Session->Region+d->DataOffset&&!memcmp(r->Mdl->Address,expected,1500));++seen;}
  VOID *future=(VOID *)(ULONG_PTR)~0u;if(RuntimeVersion<PB_NDIS_VERSION(30)){CHECK(n->Info[TcpRecvSegCoalesceInfo]==future&&n->Info[RscTcpTimestampDelta]==future);}else{CHECK(!n->Info[RscTcpTimestampDelta]);if(OcProtocol!=6||originals==1)CHECK(!n->Info[TcpRecvSegCoalesceInfo]);}
  if(RuntimeVersion<PB_NDIS_VERSION(89))CHECK(n->Info[UdpRecvSegCoalesceOffloadInfo]==future);else if(OcProtocol!=17||originals==1)CHECK(!n->Info[UdpRecvSegCoalesceOffloadInfo]);
  if(originals==16&&OcProtocol==6){NDIS_RSC_NBL_INFO info={.Value=n->Info[TcpRecvSegCoalesceInfo]};CHECK(info.Info.CoalescedSegCount==16&&r->ReceiveType==PB_DESC_TCP_GSO);}if(originals==16&&OcProtocol==17){NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=n->Info[UdpRecvSegCoalesceOffloadInfo]};CHECK(info.Receive.SegCount==16&&info.Receive.SegSize==1500-header&&r->ReceiveType==PB_DESC_UDP_GSO);}if(originals==1)CHECK(!n->Info[TcpIpChecksumNetBufferListInfo]&&r->ReceiveType==PB_DESC_PLAIN);++objects;
 }
 CHECK(seen==32&&objects==OcNbls&&coalesced==(ULONG)one);
}
static VOID OcCase(ULONG protocol,ULONG shape,BOOLEAN measured)
{
 CHECK(!Ctx&&!Session&&!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);OcProtocol=protocol;OcShape=shape;ULONG minor=shape==4?20:protocol==6?30:89;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=shape==4?0:protocol==6?(PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6):PB_RX_URO;OcNbls=shape==1||shape==2?17:32;
 Created=Joined=WorkerJoined=Indications=0;PauseId=0;memset(Lists,0,sizeof(Lists));memset(OcPointers,0,sizeof(OcPointers));memset(OcFreedCount,0,sizeof(OcFreedCount));OcAllocations=OcFailures=0;OcWaits=0;Failure=0;
 Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->Probe.V2SessionActive=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
 CHECK(PbCalculateLayout(1,32,32,&Session->Layout)==PB_OK);UCHAR *region;
#ifdef PB_GUARD_TESTS
 region=PbTestGuardAlloc(Session->Layout.RegionSize);
#else
 region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
 CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
 Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
 PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,32*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,32*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,32*sizeof(*q->Pending),0);q->Scratch=OcAllocate(0);q->CoalesceBuffers[0]=OcAllocate(1);q->CoalesceBuffers[1]=OcAllocate(2);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,32,42,0)==PB_OK&&OcAllocations==3&&OcFailures==(shape==4?0u:shape==3?2u:1u));BeforePoolFree=OcFreed;
 Base=0;for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);PB_QUEUE_VIEW *v=q->Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=Base;}
 PB_QUEUE_VIEW producer,consumer;CHECK(PbAttachQueue(&producer,region,&Session->Layout,0,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(&consumer,region,&Session->Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);PB_POOL pool;PB_SLOT slots[32];CHECK(PbPoolInitialize(&pool,slots,32,42,0)==PB_OK);
 HANDLE *gates[]={&OcIndicated,&OcRunningIdle,&OcPausedIdle,&PauseWaiting};for(ULONG i=0;i<_countof(gates);++i)*gates[i]=Event(TRUE,FALSE);Submit(&pool,&producer,PB_TEST_IP_VERSION,protocol,1);q->Thread=Start(Working,q);Await(OcIndicated);Await(OcRunningIdle);
 CHECK(Session->Active==(LONG)OcNbls&&q->Credits==32&&!Session->Returns.References&&!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]);OcVerify(Lists[0]);for(ULONG i=0;i<3;++i)CHECK(!OcFreedCount[i]);
 HANDLE pause=Start(Pausing,NULL);Await(PauseWaiting);Await(OcPausedIdle);CHECK(!Ctx->Running&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT&&Session->Active==(LONG)OcNbls&&Session->Returns.References==1);
 RETURN_JOB job={Lists[0],2};Lists[0]=NULL;Join(Start(Returning,&job));Join(pause);CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References&&!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]&&!Nbls&&Mdls==1&&Session->RxObjects==32&&Session->RxReturned==32&&Session->Completions==32&&!Session->Faulted);
 CHECK(Ctx->Statistics.ifHCInUcastPkts==32&&Ctx->Statistics.ifHCInOctets==48000&&!Ctx->Statistics.ifInErrors&&!Ctx->Statistics.ifInDiscards);Retire(&pool,&consumer,1);for(ULONG i=0;i<32;++i)CHECK(!q->RxSlots[i].Active&&q->RxSlots[i].Generation==1&&slots[i].State==PB_SLOT_FREE&&slots[i].Generation==1);for(ULONG i=0;i<3;++i)CHECK(!OcFreedCount[i]);
 Join(Start(Unregistering,NULL));CHECK(!Session&&!Ctx->V2&&Created==4&&Joined==4&&WorkerJoined==1&&!Objects&&!Locked&&!Mdls&&!Nbls&&Pools==1);ULONG frees=0;for(ULONG i=0;i<3;++i){CHECK(OcFreedCount[i]==(ULONG)(OcPointers[i]!=NULL));frees+=OcFreedCount[i];}CHECK(frees==3-OcFailures);
 Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;BeforePoolFree=NULL;
#ifdef PB_GUARD_TESTS
 PbTestGuardFree(region);
#else
 CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
 for(ULONG i=0;i<_countof(gates);++i)Close(*gates[i]);CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked&&OcWaits>0);++OcCases;OcJoins+=Joined;OcWorkerJoins+=WorkerJoined;
 printf("OPTIONAL32 ip%u proto%lu ndis%lu shape%lu measured%u packets32 nbls%lu coalesced%u alloc3 failed%lu freed%lu joins4 owners0 repairs0\n",PB_TEST_IP_VERSION,protocol,minor,shape,measured,OcNbls,shape==1||shape==2?1u:0u,OcFailures,frees);
}
static VOID OcMatrix(BOOLEAN measured)
{for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG shape=0;shape<5;++shape)OcCase(protocol,shape,measured);}
int main(VOID)
{
 setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;OcMatrix(FALSE);CHECK(OcCases==10&&GetProcessHandleCount(GetCurrentProcess(),&before));OcMatrix(TRUE);
 CHECK(OcCases==20&&OcJoins==80&&OcWorkerJoins==20&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
 printf("PASS: OPTIONAL32 family%u cases20 warm10 measured10 joins80 workerJoins20 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
