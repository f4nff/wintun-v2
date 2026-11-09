/* SPDX-License-Identifier: GPL-2.0 */
/* A real registered hosted worker builds32 packets while Pause waits for its
 * Transition shared transaction. Gates select another CPU interleaving, not
 * permission for native spinlock/IRQL code to block. Original DDIs/locks and
 * all production function bodies remain intact; full host NT_ASSERT=CHECK. */
#ifdef PB_API_TESTS
#error Shipping receive rollback paths required
#endif
#ifndef NDEBUG
#error Shipping receive rollback configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define main UnusedRollbackWorkerMain
#include PB_RB_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"receive rollback requires shipping fingerprint");
static HANDLE RbFailed,RbRollbackGo,RbPauseContended,RbIndicated,RbIndicateGo,RbPauseBank,RbWorkerWaiting;
static volatile LONG RbFailureVisits,RbPauseConflicts,RbBankVisits,RbWaits;
static LONG RbBasePools,RbBaseMdls;
static ULONG RbFailedObjects,RbProtocol,RbCases,RbJoins,RbWorkerJoins;
static NET_BUFFER_LIST *RbNbl(VOID *pool,ULONG context,ULONG backfill,MDL *mdl,ULONG offset,ULONG length)
{
 NET_BUFFER_LIST *n=NdisAllocateNetBufferAndNetBufferList(pool,context,backfill,mdl,offset,length);
 if(!n){CHECK(Failure==4&&Acquisitions==4&&InterlockedIncrement(&RbFailureVisits)==1&&Pools==RbBasePools+1&&Mdls==RbBaseMdls+2&&!Nbls&&!Session->Active&&!Session->Returns.References);
  CHECK(mdl&&mdl->Next&&mdl->Next->Partial==!Enabled&&Session->Queues[0].Credits==32&&!Session->Queues[0].PendingCount&&Session->Queues[0].CoalesceBusy[0]==!!Enabled&&!Session->Queues[0].CoalesceBusy[1]);
  for(ULONG i=0;i<32;++i)CHECK(Session->Queues[0].RxSlots[i].Active&&Session->Queues[0].RxSlots[i].Generation==1);
  CHECK(WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0&&SetEvent(RbFailed));Await(RbRollbackGo);
 }
 return n;
}
static KIRQL RbPauseAcquire(SRWLOCK *lock)
{CHECK(lock==&Ctx->TransitionLock&&GetCurrentThreadId()==PauseId);BOOLEAN got=TryAcquireSRWLockExclusive(lock);if(got)ReleaseSRWLockExclusive(lock);CHECK(!got&&Ctx->Running&&!Session->Active&&InterlockedIncrement(&RbPauseConflicts)==1&&SetEvent(RbPauseContended));return ExAcquireSpinLockExclusive(lock);}
static VOID RbBankWait(TUN_CTX *ctx,ULONG bank)
{CHECK(ctx==Ctx&&bank<2);if(!bank){CHECK(GetCurrentThreadId()==PauseId&&!Ctx->Running&&Ctx->Probe.IndicationCount[0]==1&&WaitForSingleObject(Ctx->Probe.IndicationIdle[0].Native,0)==WAIT_TIMEOUT&&InterlockedIncrement(&RbBankVisits)==1&&SetEvent(RbPauseBank));}PbWaitIndications(ctx,bank);}
static VOID RbIndicate(VOID *adapter,NET_BUFFER_LIST *head,ULONG count,ULONG flags)
{CHECK(adapter==Ctx&&head&&count==(Enabled?1u:31u)&&!flags&&!Lists[0]&&!Indications);Lists[0]=head;++Indications;CHECK(Session->Active==(LONG)count&&Session->Queues[0].PendingCount==RbFailedObjects&&SetEvent(RbIndicated));Await(RbIndicateGo);}
static VOID RbWorkerWait(ULONG count,VOID **events,int type,int reason,int mode,BOOLEAN alert,VOID *timeout,VOID *blocks)
{CHECK(count==3&&!type&&!reason&&!mode&&!alert&&!timeout&&!blocks&&!Ctx->Running&&!Session->Active);HANDLE handles[3];for(ULONG i=0;i<count;++i)handles[i]=((KEVENT *)events[i])->Native;InterlockedIncrement(&RbWaits);CHECK(SetEvent(RbWorkerWaiting)&&WaitForMultipleObjects(count,handles,FALSE,10000)<WAIT_OBJECT_0+count);}
static VOID RbVerify(NET_BUFFER_LIST *head)
{
 ULONG seen=RbFailedObjects,nbls=0,header=(PB_TEST_IP_VERSION==4?20:40)+(RbProtocol==6?20:8);
 for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)n->Reserved[1]&~(ULONG_PTR)1);CHECK(r->Queue==Session->Queues&&r->Count==(Enabled?16u:1u)&&r->IpVersion==PB_TEST_IP_VERSION&&r->Protocol==RbProtocol&&r->Mdl->Partial==!Enabled&&r->IndicationLength==(Enabled?header+16*(1500-header):1500));
  CHECK(r->CoalesceIndex==(Enabled?0u:MAXULONG)&&r->HeaderMdl->Next==r->Mdl&&r->Mdl->Length==r->IndicationLength);
  for(ULONG i=0;i<r->Count;++i){PB_DESCRIPTOR *d=r->Descriptors+i;CHECK(seen<32&&d->Token.Epoch==42&&d->Token.Queue==0&&d->Token.Slot==seen&&d->Token.Generation==1&&d->Length==1500);UCHAR expected[1500];Packet(expected,PB_TEST_IP_VERSION,RbProtocol,seen,1);CHECK(!memcmp(Session->Region+d->DataOffset,expected,1500));if(Enabled)CHECK(!memcmp((UCHAR *)r->Mdl->Address+header+i*(1500-header),expected+header,1500-header));else CHECK(!memcmp(r->Mdl->Address,expected,1500));++seen;}
  VOID *future=(VOID *)(ULONG_PTR)~0u;if(RuntimeVersion<PB_NDIS_VERSION(30))CHECK(n->Info[TcpRecvSegCoalesceInfo]==future);else if(!Enabled||RbProtocol!=6)CHECK(!n->Info[TcpRecvSegCoalesceInfo]);CHECK(n->Info[RscTcpTimestampDelta]==(RuntimeVersion<PB_NDIS_VERSION(30)?future:NULL));if(RuntimeVersion<PB_NDIS_VERSION(89))CHECK(n->Info[UdpRecvSegCoalesceOffloadInfo]==future);else if(!Enabled||RbProtocol!=17)CHECK(!n->Info[UdpRecvSegCoalesceOffloadInfo]);
  if(Enabled&&RbProtocol==6){NDIS_RSC_NBL_INFO info={.Value=n->Info[TcpRecvSegCoalesceInfo]};CHECK(info.Info.CoalescedSegCount==16&&r->ReceiveType==PB_DESC_TCP_GSO);}if(Enabled&&RbProtocol==17){NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=n->Info[UdpRecvSegCoalesceOffloadInfo]};CHECK(info.Receive.SegCount==16&&info.Receive.SegSize==1500-header&&r->ReceiveType==PB_DESC_UDP_GSO);}
  ++nbls;
 }
 CHECK(seen==32&&nbls==(Enabled?1u:31u));
}
static VOID RbCase(ULONG protocol,ULONG minor,BOOLEAN wrap,BOOLEAN measured)
{
 CHECK(!Ctx&&!Session&&!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);RbProtocol=protocol;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=minor==20?0:protocol==6?(PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6):PB_RX_URO;RbFailedObjects=Enabled?16:1;
 Created=Joined=WorkerJoined=Indications=0;PauseId=0;memset(Lists,0,sizeof(Lists));RbFailureVisits=RbPauseConflicts=RbBankVisits=RbWaits=0;Failure=0;
 Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->Probe.V2SessionActive=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
 CHECK(PbCalculateLayout(1,32,32,&Session->Layout)==PB_OK);UCHAR *region;
#ifdef PB_GUARD_TESTS
 region=PbTestGuardAlloc(Session->Layout.RegionSize);
#else
 region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
 CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
 Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
 PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,32*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,32*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,32*sizeof(*q->Pending),0);q->Scratch=ExAllocatePool2(0,PB_BLOCK_SIZE,0);for(ULONG i=0;i<2;++i)q->CoalesceBuffers[i]=ExAllocatePool2(0,PB_BLOCK_SIZE,0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,32,42,0)==PB_OK);
 Base=wrap?UINT64_MAX-15:0;for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);PB_QUEUE_VIEW *v=q->Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=Base;}
 PB_QUEUE_VIEW producer,consumer;CHECK(PbAttachQueue(&producer,region,&Session->Layout,0,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(&consumer,region,&Session->Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);producer.Cursor=producer.PeerCursor=consumer.Cursor=consumer.PeerCursor=Base;PB_POOL pool;PB_SLOT slots[32];CHECK(PbPoolInitialize(&pool,slots,32,42,0)==PB_OK);
 HANDLE *gates[]={&RbFailed,&RbRollbackGo,&RbPauseContended,&RbIndicated,&RbIndicateGo,&RbPauseBank,&RbWorkerWaiting,&PauseWaiting};for(ULONG i=0;i<_countof(gates);++i)*gates[i]=Event(TRUE,FALSE);
 Submit(&pool,&producer,PB_TEST_IP_VERSION,protocol,1);RbBasePools=Pools;RbBaseMdls=Mdls;Acquisitions=0;Failure=4;q->Thread=Start(Working,q);Await(RbFailed);
 CHECK(!Session->Active&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0);HANDLE pause=Start(Pausing,NULL);Await(RbPauseContended);CHECK(Ctx->Running&&!Session->Active&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT&&SetEvent(RbRollbackGo));
 Await(RbIndicated);Await(RbPauseBank);CHECK(!Ctx->Running&&Session->Active==(Enabled?1:31)&&Session->Returns.References==1&&Pools==RbBasePools+(Enabled?1:31)&&Mdls==RbBaseMdls+2*(Enabled?1:31)&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);RbVerify(Lists[0]);
 RETURN_JOB job={Lists[0],2};Lists[0]=NULL;Join(Start(Returning,&job));CHECK(!Session->Active&&Session->Returns.References==1);CHECK(Session->Returns.References==1&&!Ctx->Probe.ReturnRundown.References&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0&&Ctx->Probe.IndicationCount[0]==1&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);
 CHECK(Pools==RbBasePools&&Mdls==RbBaseMdls&&!Nbls&&!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]);CHECK(SetEvent(RbIndicateGo));Join(pause);Await(RbWorkerWaiting);CHECK(!Session->Returns.References&&!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]&&RbFailureVisits==1&&RbPauseConflicts==1&&RbBankVisits==1&&!Session->Faulted);
 PB_DESCRIPTOR completed[32];uint32_t count;CHECK(PbQueuePop(&consumer,completed,32,&count)==PB_OK&&count==32);uint32_t seen=0;for(ULONG i=0;i<count;++i){ULONG slot=completed[i].Token.Slot;CHECK(slot<32&&!(seen&(1u<<slot))&&completed[i].Token.Generation==1&&completed[i].Status==(ULONG)(slot<RbFailedObjects?PB_FULL:PB_OK));seen|=1u<<slot;CHECK(PbPoolTransition(&pool,&completed[i].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);}CHECK(seen==UINT32_MAX&&Session->RxObjects==32-RbFailedObjects&&Session->RxReturned==32-RbFailedObjects&&Session->Completions==32&&Ctx->Statistics.ifHCInUcastPkts==32-RbFailedObjects&&Ctx->Statistics.ifHCInOctets==(32-RbFailedObjects)*1500);
 for(ULONG i=0;i<32;++i)CHECK(!q->RxSlots[i].Active&&q->RxSlots[i].Generation==1&&slots[i].State==PB_SLOT_FREE&&slots[i].Generation==1);Failure=0;BeforePoolFree=Freed;Join(Start(Unregistering,NULL));CHECK(!Session&&!Ctx->V2&&Created==4&&Joined==4&&WorkerJoined==1&&!Objects&&!Locked&&!Mdls&&!Nbls&&Pools==1);BeforePoolFree=NULL;
 Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;
#ifdef PB_GUARD_TESTS
 PbTestGuardFree(region);
#else
 CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
 for(ULONG i=0;i<_countof(gates);++i)Close(*gates[i]);CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);++RbCases;RbJoins+=Joined;RbWorkerJoins+=WorkerJoined;
 printf("ROLLBACK32 ip%u proto%lu ndis%lu wrap%u measured%u packets32 rejected%lu returned%lu nbls%u ordinal4 pauseConflict1 bankWait1 joins4 owners0 repairs0\n",PB_TEST_IP_VERSION,protocol,minor,wrap,measured,RbFailedObjects,32-RbFailedObjects,minor==20?31u:1u);
}
static VOID RbMatrix(BOOLEAN measured)
{for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG modern=0;modern<2;++modern)for(ULONG wrap=0;wrap<2;++wrap)RbCase(protocol,modern?(protocol==6?30:89):20,(BOOLEAN)wrap,measured);}
int main(VOID)
{
 setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;RbMatrix(FALSE);CHECK(RbCases==8&&GetProcessHandleCount(GetCurrentProcess(),&before));RbMatrix(TRUE);
 CHECK(RbCases==16&&RbJoins==64&&RbWorkerJoins==16&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
 printf("PASS: ROLLBACK32 family%u cases16 warm8 measured8 joins64 workerJoins16 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
