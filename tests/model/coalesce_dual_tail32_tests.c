/* SPDX-License-Identifier: GPL-2.0 */
/* Two real coalesced NBL buffers are released by one return window before
 * diagnostic capture. Hosted failure statuses are defensive inputs only. */
#ifdef PB_API_TESTS
#error Shipping dual coalesce paths required
#endif
#ifndef NDEBUG
#error Shipping dual coalesce configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define main UnusedDualWorkerPauseMain
#include PB_DUAL_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"dual coalesce requires shipping fingerprint");
static HANDLE DualOldReady,DualNewStart,DualNewReady,DualDiagnostic,DualTail,DualTailGo,DualRundown;
static ULONG DualBeforeFree,DualDiagnosticVisits;
static LONG DualHoldRole;
static ULONG DualIp,DualOldProtocol,DualNewProtocol;
static ULONG DualCases,DualJoins,DualWorkerJoins;
static VOID DualBeforeDiagnostic(KSPIN_LOCK *lock)
{
    CHECK(lock==&Ctx->Probe.ReturnDiagnosticsLock&&ReturnRole==0&&++DualDiagnosticVisits==1);
    CHECK(!Nbls&&Mdls==1&&!Session->Queues[0].CoalesceBusy[0]&&!Session->Queues[0].CoalesceBusy[1]);
    CHECK(DualBeforeFree==2&&Session->Active==2&&Session->Queues[0].Credits==32&&Session->Returns.References==1);
    CHECK(SetEvent(DualDiagnostic));
}
static VOID DualBeforeNblFree(NET_BUFFER_LIST *nbl)
{
    if(ReturnRole!=0)return;
    PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl)&~(ULONG_PTR)1);
    CHECK(record->Count==16&&record->CoalesceIndex==DualBeforeFree&&Session->Queues[0].CoalesceBusy[record->CoalesceIndex]==1);++DualBeforeFree;
}
static VOID DualReturningTail(EX_RUNDOWN_REF *refs)
{
    if(refs==&Ctx->Probe.ReturnRundown)return;
    CHECK(Session&&refs==&Session->Returns);
    if(ReturnRole==DualHoldRole){CHECK(!Session->Active&&SetEvent(DualTail));Await(DualTailGo);}
}
static VOID ObservedDualRundown(EX_RUNDOWN_REF *refs)
{
    CHECK(refs==&Session->Returns&&!Session->Active&&refs->References==1&&WorkerJoined==1);
    BOOLEAN acquired=TryAcquireSRWLockExclusive(&refs->Lock);if(acquired)ReleaseSRWLockExclusive(&refs->Lock);
    CHECK(!acquired&&SetEvent(DualRundown));ExWaitForRundownProtectionRelease(refs);
}
static VOID DualPacket(UCHAR *p,ULONG protocol,ULONG slot,ULONG length)
{
    ULONG ip=DualIp==4?20u:40u,header=ip+(protocol==6?20u:8u);memset(p,0,length);
    if(DualIp==4){p[0]=0x45;W16(p+2,length);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[15]=1;p[16]=10;p[19]=2;}
    else{p[0]=0x60;W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=1;p[39]=2;}
    W16(p+ip,4000+slot/16);W16(p+ip+2,5000);
    if(protocol==6){W32(p+ip+4,1000+(slot%16)*(length-header));p[ip+12]=0x50;p[ip+13]=0x10;W16(p+ip+14,8192);}else W16(p+ip+4,length-ip);
    memset(p+header,(int)(0x30+slot),length-header);W32(p+header,slot);
    CHECK(PbFinalizeChecksums(p,length,DualIp==4,protocol==6,protocol==17)==PB_PACKET_OK);
}
static VOID DualSubmit(PB_POOL *pool,PB_QUEUE_VIEW *producer,ULONG first,ULONG protocol,ULONG length)
{
    PB_DESCRIPTOR items[32]={0};for(ULONG i=0;i<32;++i){ULONG slot=first+i;CHECK(PbPoolReserve(pool,&items[i].Token)==PB_OK&&items[i].Token.Slot==slot&&items[i].Token.Generation==1);items[i].Length=length;items[i].DataOffset=Session->Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE;DualPacket(Session->Region+items[i].DataOffset,protocol,slot,length);CHECK(PbPoolTransition(pool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);}
    uint32_t count,notify;CHECK(PbQueuePush(producer,items,32,&count,&notify)==PB_OK&&count==32);
}
static VOID DualInspect(NET_BUFFER_LIST *head,ULONG first,ULONG protocol,ULONG length)
{
    ULONG count=0,header=(DualIp==4?20u:40u)+(protocol==6?20u:8u);BYTE expected[1500];
    for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);
        CHECK(count<2&&r->Queue==Session->Queues&&r->Count==16&&r->CoalesceIndex==count&&r->IpVersion==DualIp&&r->Protocol==protocol&&r->ReceiveType==(protocol==6?PB_DESC_TCP_GSO:PB_DESC_UDP_GSO));
        CHECK(r->IndicationLength==header+16*(length-header)&&r->Mdl->Address==Session->Queues[0].CoalesceBuffers[count]&&!r->Mdl->Partial&&r->Mdl->Length==r->IndicationLength&&n->Length==14+r->IndicationLength);
        for(ULONG j=0;j<16;++j){ULONG slot=first+count*16+j;CHECK(r->Descriptors[j].Token.Slot==slot&&r->Descriptors[j].Token.Generation==1&&r->Descriptors[j].Length==length);DualPacket(expected,protocol,slot,length);CHECK(!memcmp((BYTE *)r->Mdl->Address+header+j*(length-header),expected+header,length-header));}
        if(protocol==6){NDIS_RSC_NBL_INFO info={.Value=n->Info[TcpRecvSegCoalesceInfo]};CHECK(info.Info.CoalescedSegCount==16);}
        else{NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=n->Info[UdpRecvSegCoalesceOffloadInfo]};CHECK(info.Receive.SegCount==16&&info.Receive.SegSize==length-header);}
        if(RuntimeVersion<PB_NDIS_VERSION(89))CHECK(n->Info[UdpRecvSegCoalesceOffloadInfo]==(VOID *)(ULONG_PTR)~0u);++count;
    }
    CHECK(count==2);
}
static DWORD WINAPI DualReceiver(VOID *opaque)
{
    PB_KERNEL_QUEUE *q=opaque;CHECK(PbV2ReceiveBatch(q)&&SetEvent(DualOldReady));Await(DualNewStart);
    CHECK(PbV2ReceiveBatch(q)&&SetEvent(DualNewReady));Await(Session->StopEvent.Native);return 0;
}
static VOID DualDiagnostics(VOID)
{
    PB_RETURN_SNAPSHOT snapshot={0};PbSnapshotReturnEvents(&Ctx->Probe.ReturnDiagnostics,&snapshot);CHECK(snapshot.RecordCount==2);
    ULONG header=(DualIp==4?20u:40u)+(DualOldProtocol==6?20u:8u),length=header+16*(1500-header);
    for(ULONG i=0;i<2;++i){PB_RETURN_EVENT *e=snapshot.Records+i;CHECK(e->FirstToken.Slot==16*i&&e->OriginalCount==16&&e->OriginalBytes==16*1500&&e->IndicationLength==length&&e->IpDeclaredLength==length&&e->IpVersion==DualIp&&e->Protocol==DualOldProtocol&&e->NblStatus==0xc0000001u&&e->CompletionStatus==PB_RX_REJECTED&&e->Origin==PB_RETURN_ORIGIN_NDIS);}
}
static VOID DualCase(ULONG ip,ULONG protocol,ULONG minor,BOOLEAN freshFirst,BOOLEAN wrap,BOOLEAN measured)
{
    CHECK(!Ctx&&!Session&&!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);DualIp=ip;DualOldProtocol=protocol;DualNewProtocol=minor>=89?(protocol==6?17:6):6;RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=(ip==4?PB_RX_RSC4:PB_RX_RSC6)|(minor>=89?PB_RX_URO:0);
    Indications=SelectedPaused=RestartIndex=FirstIndex=Created=Joined=WorkerJoined=0;PauseId=DualBeforeFree=DualDiagnosticVisits=0;DualHoldRole=freshFirst?0:2;memset(Lists,0,sizeof(Lists));
    Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->Probe.V2SessionActive=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
    CHECK(PbCalculateLayout(1,64,128,&Session->Layout)==PB_OK);UCHAR *region;
#ifdef PB_GUARD_TESTS
    region=PbTestGuardAlloc(Session->Layout.RegionSize);
#else
    region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
    CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
    Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
    PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(FALSE);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,64*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,64*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,128*sizeof(*q->Pending),0);q->Scratch=ExAllocatePool2(0,PB_BLOCK_SIZE,0);for(ULONG i=0;i<2;++i)q->CoalesceBuffers[i]=ExAllocatePool2(0,PB_BLOCK_SIZE,0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,64,42,0)==PB_OK);
    Base=wrap?UINT64_MAX-15:0;for(ULONG r=0;r<PB_QUEUE_COUNT;++r){CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,0,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);PB_QUEUE_VIEW *v=q->Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=Base;}
    PB_QUEUE_VIEW producer,consumer;CHECK(PbAttachQueue(&producer,region,&Session->Layout,0,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(&consumer,region,&Session->Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);producer.Cursor=producer.PeerCursor=consumer.Cursor=consumer.PeerCursor=Base;PB_SLOT slots[64];PB_POOL pool;CHECK(PbPoolInitialize(&pool,slots,64,42,0)==PB_OK);
    DualOldReady=Event(TRUE,FALSE);DualNewStart=Event(TRUE,FALSE);DualNewReady=Event(TRUE,FALSE);DualDiagnostic=Event(TRUE,FALSE);DualTail=Event(TRUE,FALSE);DualTailGo=Event(TRUE,FALSE);DualRundown=Event(TRUE,FALSE);PauseWaiting=Event(TRUE,FALSE);
    DualSubmit(&pool,&producer,0,DualOldProtocol,1500);q->Thread=Start(DualReceiver,q);Await(DualOldReady);CHECK(Indications==1&&Session->Active==2&&q->Credits==32);DualInspect(Lists[0],0,DualOldProtocol,1500);
    for(NET_BUFFER_LIST *n=Lists[0];n;n=n->Next)n->Status=0xc0000001u;
    AcquireSRWLockExclusive((SRWLOCK *)&Ctx->Probe.ReturnDiagnosticsLock);BeforeDiagnosticLock=DualBeforeDiagnostic;BeforeNblFree=DualBeforeNblFree;BeforeRundownRelease=DualReturningTail;
    RETURN_JOB old={Lists[0],0};Lists[0]=NULL;HANDLE oldThread=Start(Returning,&old);Await(DualDiagnostic);
    CHECK(Session->Active==2&&q->Credits==32&&!q->PendingCount&&!Session->Completions);DualSubmit(&pool,&producer,32,DualNewProtocol,1400);CHECK(SetEvent(DualNewStart));Await(DualNewReady);
    CHECK(Indications==2&&Session->Active==4&&q->Credits==64&&q->CoalesceBusy[0]&&q->CoalesceBusy[1]&&Session->Returns.References==1);DualInspect(Lists[1],32,DualNewProtocol,1400);
    HANDLE pause=Start(Pausing,NULL);Await(PauseWaiting);CHECK(!Ctx->Running&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);RETURN_JOB fresh={Lists[1],2};Lists[1]=NULL;HANDLE freshThread=NULL,*heldThread;
    if(freshFirst){freshThread=Start(Returning,&fresh);Join(freshThread);CHECK(Session->Active==2&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);ReleaseSRWLockExclusive((SRWLOCK *)&Ctx->Probe.ReturnDiagnosticsLock);Await(DualTail);heldThread=&oldThread;}
    else{ReleaseSRWLockExclusive((SRWLOCK *)&Ctx->Probe.ReturnDiagnosticsLock);Join(oldThread);CHECK(Session->Active==2&&q->CoalesceBusy[0]&&q->CoalesceBusy[1]&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);DualInspect(fresh.Head,32,DualNewProtocol,1400);freshThread=Start(Returning,&fresh);Await(DualTail);heldThread=&freshThread;}
    Join(pause);CHECK(!Session->Active&&Session->Returns.References==1&&Ctx->Probe.ReturnRundown.References==1&&!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]);DualDiagnostics();
    PB_DESCRIPTOR completed[64];uint32_t count;CHECK(PbQueuePop(&consumer,completed,64,&count)==PB_OK&&count==64);uint64_t seen=0;for(ULONG i=0;i<count;++i){ULONG slot=completed[i].Token.Slot;CHECK(slot<64&&!(seen&(1ull<<slot))&&completed[i].Token.Generation==1&&completed[i].Status==(ULONG)(slot<32?PB_RX_REJECTED:PB_OK));seen|=1ull<<slot;CHECK(PbPoolTransition(&pool,&completed[i].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);}CHECK(seen==UINT64_MAX&&Session->RxObjects==64&&Session->RxReturned==64&&Session->Completions==64&&Ctx->Statistics.ifInErrors==2&&Ctx->Statistics.ifHCInUcastPkts==32&&Ctx->Statistics.ifHCInOctets==32*1400);
    CHECK(!Session->Faulted&&!Session->Draining&&!Session->Stopping&&Ctx->Statistics.ifInDiscards==0);for(ULONG i=0;i<64;++i)CHECK(slots[i].State==PB_SLOT_FREE&&slots[i].Generation==1&&!q->RxSlots[i].Active);
    BeforePoolFree=Freed;HANDLE cleanup=Start(Unregistering,NULL);Await(DualRundown);CHECK(Session&&Locked==1&&Mdls==1&&Objects==3&&WaitForSingleObject(cleanup,0)==WAIT_TIMEOUT);CHECK(SetEvent(DualTailGo));Join(*heldThread);Join(cleanup);
    CHECK(!Session&&!Ctx->V2&&!Ctx->Probe.ReturnRundown.References&&WorkerJoined==1&&Created==5&&Joined==5&&!Objects&&!Locked&&!Mdls&&!Nbls);BeforePoolFree=NULL;BeforeRundownRelease=NULL;BeforeDiagnosticLock=NULL;BeforeNblFree=NULL;
    Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(region);
#else
    CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif
    Close(DualOldReady);Close(DualNewStart);Close(DualNewReady);Close(DualDiagnostic);Close(DualTail);Close(DualTailGo);Close(DualRundown);Close(PauseWaiting);CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);++DualCases;DualJoins+=Joined;DualWorkerJoins+=WorkerJoined;
    printf("DUALTAIL32 ip%lu old%lu new%lu ndis%lu freshFirst%u wrap%u measured%u batches32/32 unique64 nbl2/2 joins5 owners0 repairs0\n",ip,DualOldProtocol,DualNewProtocol,minor,freshFirst,wrap,measured);
}
int main(VOID)
{
    DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(ULONG shape=0;shape<3;++shape)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG first=0;first<2;++first)DualCase(PB_TEST_IP_VERSION,shape==2?17:6,shape==0?30:89,(BOOLEAN)first,(BOOLEAN)wrap,(BOOLEAN)pass);
    }
    CHECK(DualCases==24&&DualJoins==120&&DualWorkerJoins==24&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: DUALTAIL32 family%u cases%lu warm12 measured12 joins%lu workerJoins%lu handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,DualCases,DualJoins,DualWorkerJoins,before,after);return 0;
}
