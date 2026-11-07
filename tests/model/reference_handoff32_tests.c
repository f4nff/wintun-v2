/* SPDX-License-Identifier: GPL-2.0 */
/* Actual receive/return/reference/Pause paths with a controlled hosted schedule.
 * A gate freezes the hosted DDI before SetEvent; it does not claim that native
 * spinlock/IRQL code may block. No production algorithm or ownership repair. */
#ifdef PB_API_TESTS
#error Shipping reference handoff paths required
#endif
#ifndef NDEBUG
#error Shipping reference handoff configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"reference handoff requires shipping fingerprint");
#define main UnusedHandoffWorkerMain
#include PB_HO_HOST
#undef main
static HANDLE HoNewReady,HoAllowRetain,HoRetainEntered,HoOldZero,HoOldGo,HoPauseEntered;
static volatile LONG HoReceiveId,HoOldId;
static volatile LONG HoOldArmed;
static volatile LONG HoRetainSelected;
static ULONG HoProtocol; static volatile LONG HoClears,HoSets;
static ULONG HoCases,HoJoins,HoWorkerJoins;
static VOID HandoffRetain(PB_KERNEL_SESSION *s,ULONG nbls,ULONG objects)
{
 if((LONG)GetCurrentThreadId()==ReadAcquire(&HoReceiveId)){CHECK(s==Session&&nbls==32&&objects==32&&Session->Active==32&&SetEvent(HoNewReady));Await(HoAllowRetain);}
 PbV2RetainReceives(s,nbls,objects);
}
static VOID HandoffReferenceAcquire(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{
 CHECK(lock==&Session->ReferenceLock);
 if((LONG)GetCurrentThreadId()==ReadAcquire(&HoReceiveId)&&!InterlockedCompareExchange(&HoRetainSelected,1,0)){BOOLEAN got=TryAcquireSRWLockExclusive((SRWLOCK *)lock);if(got)ReleaseSRWLockExclusive((SRWLOCK *)lock);CHECK(!got&&!Session->Active&&SetEvent(HoRetainEntered));}
 KeAcquireInStackQueuedSpinLock(lock,handle);
}
static VOID HandoffClear(KEVENT *event)
{CHECK(event==&Session->AllReturned&&Session->Active==32&&WaitForSingleObject(event->Native,0)==WAIT_OBJECT_0);if((LONG)GetCurrentThreadId()==ReadAcquire(&HoReceiveId))CHECK(HoSets==1&&HoClears==1);KeClearEvent(event);InterlockedIncrement(&HoClears);}
static KIRQL HandoffPauseAcquire(SRWLOCK *lock)
{CHECK(lock==&Ctx->TransitionLock);BOOLEAN got=TryAcquireSRWLockExclusive(lock);if(got)ReleaseSRWLockExclusive(lock);CHECK(!got&&SetEvent(HoPauseEntered));return ExAcquireSpinLockExclusive(lock);}
static VOID HoSet(KEVENT *event)
{
 if(Session&&event==&Session->AllReturned){CHECK(!Session->Active);InterlockedIncrement(&HoSets);if((LONG)GetCurrentThreadId()==ReadAcquire(&HoOldId)&&InterlockedCompareExchange(&HoOldArmed,0,1)==1){CHECK(SetEvent(HoOldZero));Await(HoOldGo);}}
}
static DWORD WINAPI HoReceive(VOID *unused){(void)unused;InterlockedExchange(&HoReceiveId,(LONG)GetCurrentThreadId());CHECK(PbV2ReceiveBatch(Session->Queues+1));return 0;}
static DWORD WINAPI HoReturn(VOID *lists){InterlockedExchange(&HoOldId,(LONG)GetCurrentThreadId());TunReturnNetBufferLists(Ctx,lists,0);return 0;}
static DWORD WINAPI HoLastReturn(VOID *lists){TunReturnNetBufferLists(Ctx,lists,0);return 0;}
static DWORD WINAPI HoIdle(VOID *unused){(void)unused;Await(Session->StopEvent.Native);return 0;}
static VOID HoFill(PB_POOL *pool,PB_QUEUE_VIEW *producer,ULONG queue)
{PB_DESCRIPTOR d[32];for(ULONG i=0;i<32;++i){CHECK(PbPoolReserve(pool,&d[i].Token)==PB_OK&&d[i].Token.Slot==i&&d[i].Token.Generation==1);PB_TOKEN token=d[i].Token;d[i]=(PB_DESCRIPTOR){.Token=token,.Length=1500,.DataOffset=Session->Layout.Queues[queue].RxData+i*PB_BLOCK_SIZE};Packet(Session->Region+d[i].DataOffset,PB_TEST_IP_VERSION,HoProtocol,queue*32+i,1);CHECK(PbPoolTransition(pool,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);}uint32_t count,notify;CHECK(PbQueuePush(producer,d,32,&count,&notify)==PB_OK&&count==32&&!notify);}
static VOID HoVerify(NET_BUFFER_LIST *head,ULONG queue,ULONG minor)
{ULONG seen=0;for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)n->Reserved[1]&~(ULONG_PTR)1);CHECK(seen<32&&r->Queue==Session->Queues+queue&&r->Count==1&&r->Descriptors[0].Token.Queue==queue&&r->Descriptors[0].Token.Slot==seen&&r->Descriptors[0].Token.Generation==1&&r->Mdl->Partial&&r->IndicationLength==1500&&r->IpVersion==PB_TEST_IP_VERSION&&r->Protocol==HoProtocol);BYTE expected[1500];Packet(expected,PB_TEST_IP_VERSION,HoProtocol,queue*32+seen,1);CHECK(!memcmp(r->Mdl->Address,expected,1500));VOID *future=(VOID *)(ULONG_PTR)~0u;CHECK(n->Info[TcpRecvSegCoalesceInfo]==(minor==20?future:NULL)&&n->Info[RscTcpTimestampDelta]==(minor==20?future:NULL)&&n->Info[UdpRecvSegCoalesceOffloadInfo]==(minor==20?future:NULL));++seen;}CHECK(seen==32);}
static VOID HoRetire(PB_POOL *pool,PB_QUEUE_VIEW *consumer,ULONG queue)
{PB_DESCRIPTOR out[32];uint32_t count;CHECK(PbQueuePop(consumer,out,32,&count)==PB_OK&&count==32);uint32_t seen=0;for(ULONG i=0;i<32;++i){CHECK(out[i].Token.Queue==queue&&out[i].Token.Slot<32&&out[i].Token.Generation==1&&out[i].Status==PB_OK);uint32_t bit=1u<<out[i].Token.Slot;CHECK(!(seen&bit));seen|=bit;CHECK(PbPoolTransition(pool,&out[i].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);}CHECK(seen==UINT32_MAX);}
static VOID HoCase(ULONG protocol,ULONG minor,BOOLEAN wrap,BOOLEAN measured)
{
 HoProtocol=protocol;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=0;Created=Joined=WorkerJoined=Indications=0;memset(Lists,0,sizeof(Lists));HoReceiveId=HoOldId=PauseId=0;HoOldArmed=HoRetainSelected=FALSE;HoSets=HoClears=0;
 Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
 CHECK(PbCalculateLayout(2,32,32,&Session->Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
 BYTE *region=PbTestGuardAlloc(Session->Layout.RegionSize);memset(region,0,Session->Layout.RegionSize);
#else
 BYTE *region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
 CHECK(region);Session->Region=region;Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);Session->PagesLocked=TRUE;Locked=1;Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
 PB_QUEUE_VIEW producer[2],consumer[2];PB_POOL pools[2];PB_SLOT slots[2][32];Base=wrap?UINT64_MAX-15:0;
 for(ULONG qi=0;qi<2;++qi){PB_KERNEL_QUEUE *q=Session->Queues+qi;q->Session=Session;q->Index=qi;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,32*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,32*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,32*sizeof(*q->Pending),0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,32,42,(USHORT)qi)==PB_OK&&PbPoolInitialize(pools+qi,slots[qi],32,42,(USHORT)qi)==PB_OK);for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,qi,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);q->Rings[r].Shared->Producer=q->Rings[r].Shared->Consumer=q->Rings[r].Cursor=q->Rings[r].PeerCursor=Base;}CHECK(PbAttachQueue(producer+qi,region,&Session->Layout,qi,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(consumer+qi,region,&Session->Layout,qi,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);producer[qi].Cursor=producer[qi].PeerCursor=consumer[qi].Cursor=consumer[qi].PeerCursor=Base;q->Thread=Start(HoIdle,NULL);HoFill(pools+qi,producer+qi,qi);}
 HANDLE *events[]={&HoNewReady,&HoAllowRetain,&HoRetainEntered,&HoOldZero,&HoOldGo,&HoPauseEntered,&PauseWaiting};for(ULONG i=0;i<_countof(events);++i)*events[i]=Event(TRUE,FALSE);
 OnSetEvent=HoSet;CHECK(PbV2ReceiveBatch(Session->Queues)&&Indications==1&&Session->Active==32);HoVerify(Lists[0],0,minor);HANDLE receive=Start(HoReceive,NULL);Await(HoNewReady);CHECK(Session->Active==32&&Indications==1&&Nbls==64&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_TIMEOUT);
 NET_BUFFER_LIST *old=Lists[0];Lists[0]=NULL;HoOldArmed=TRUE;HANDLE returning=Start(HoReturn,old);Await(HoOldZero);CHECK(!Session->Active&&Nbls==32&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_TIMEOUT);CHECK(SetEvent(HoAllowRetain));Await(HoRetainEntered);HANDLE pause=Start(Pausing,NULL);Await(HoPauseEntered);CHECK(WaitForSingleObject(pause,0)==WAIT_TIMEOUT);CHECK(SetEvent(HoOldGo));Join(returning);Join(receive);Await(PauseWaiting);
 CHECK(Indications==2&&Session->Active==32&&Session->Returns.References==1&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_TIMEOUT&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT&&!Ctx->Running);HoVerify(Lists[1],1,minor);HoRetire(pools,consumer,0);
 NET_BUFFER_LIST *fresh=Lists[1];Lists[1]=NULL;Join(Start(HoLastReturn,fresh));Join(pause);CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References&&HoSets==2&&HoClears==2&&Session->RxObjects==64&&Session->RxReturned==64&&Session->Completions==64&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0);HoRetire(pools+1,consumer+1,1);
 for(ULONG q=0;q<2;++q)for(ULONG i=0;i<32;++i)CHECK(slots[q][i].State==PB_SLOT_FREE&&slots[q][i].Generation==1&&!Session->Queues[q].RxSlots[i].Active&&Session->Queues[q].RxSlots[i].Generation==1);CHECK(!Session->Faulted&&!Session->Draining&&Ctx->Statistics.ifHCInUcastPkts==64&&Ctx->Statistics.ifHCInOctets==96000);
 BeforePoolFree=Freed;Unregistering(NULL);CHECK(!Session&&!Ctx->V2&&Created==6&&Joined==6&&WorkerJoined==2&&!Locked&&!Objects&&!Mdls&&!Nbls);BeforePoolFree=NULL;OnSetEvent=NULL;Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;
#ifdef PB_GUARD_TESTS
 PbTestGuardFree(region);
#else
 CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
 for(ULONG i=0;i<_countof(events);++i)Close(*events[i]);CHECK(!Pools&&!Handles&&!Objects&&!Mdls&&!Nbls&&FixtureRundownAcquires==FixtureRundownReleases);++HoCases;HoJoins+=Joined;HoWorkerJoins+=WorkerJoined;printf("HANDOFF32 ip%u proto%lu ndis%lu wrap%u measured%u q0_32 q1_32 distinct64 sets2 clears2 pauseBlocked1 joins6 owners0 repairs0\n",PB_TEST_IP_VERSION,protocol,minor,wrap,measured);
}
int main(VOID)
{
 DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)HoCase(protocol,minor,(BOOLEAN)wrap,(BOOLEAN)pass);}
 CHECK(HoCases==16&&HoJoins==96&&HoWorkerJoins==32&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: HANDOFF32 family%u cases16 warm8 measured8 joins96 workerJoins32 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
