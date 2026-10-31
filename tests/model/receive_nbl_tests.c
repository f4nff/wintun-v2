/* SPDX-License-Identifier: GPL-2.0 */
#include "kernel_nbl_fixture.h"
_Static_assert(PB_RX_REJECTED==4 && PB_CORRUPT==4, "completion/API wire compatibility");
static VOID ScrambleNbl(NET_BUFFER_LIST *nbl) {nbl->Status=0xdeadbeefu;}
static VOID IpShape(UCHAR *bytes,ULONG ip,ULONG protocol,ULONG length)
{
    bytes[0]=(UCHAR)((ip<<4)|(ip==4?5:0));bytes[ip==4?9:6]=(UCHAR)protocol;
    ULONG declared=ip==4?length:length-40;ULONG at=ip==4?2:4;
    bytes[at]=(UCHAR)(declared>>8);bytes[at+1]=(UCHAR)declared;
}
static VOID RunCase(ULONG failure,ULONG ip,ULONG protocol,BOOLEAN copied,ULONG returnStatus,BOOLEAN maximum,BOOLEAN local)
{
    PB_KERNEL_SESSION s={0}; TUN_CTX ctx={0}; PB_LAYOUT layout;
    CHECK(PbCalculateLayout(1,maximum?32:2,maximum?32:4,&layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    UCHAR *region=PbTestGuardAlloc(layout.RegionSize);
#else
    UCHAR *region=_aligned_malloc(layout.RegionSize,64);
#endif
    CHECK(region); memset(region,0,layout.RegionSize);
    MDL parent={0}; parent.Address=region; parent.Length=layout.RegionSize;
    s.Ctx=&ctx; s.Layout=layout; s.Epoch=42; s.Region=region; s.Mdl=&parent;
    s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(s.AllReturned.Native);
    KEVENT kernel={CreateEventW(NULL,FALSE,FALSE,NULL)},user={CreateEventW(NULL,FALSE,FALSE,NULL)}; CHECK(kernel.Native && user.Native);
    PB_KERNEL_QUEUE *q=s.Queues; PB_RX_SLOT slots[32]={0}; PB_DESCRIPTOR pending[32]={0},descriptors[32]={0};
    q->Session=&s; q->RxSlots=slots; q->Pending=pending; q->KernelEvent=&kernel; q->UserEvent=&user;
#ifdef PB_GUARD_TESTS
    q->CoalesceBuffers[0]=PbTestGuardAlloc(65535); memset(q->CoalesceBuffers[0],0,65535);
#else
    q->CoalesceBuffers[0]=calloc(1,PB_BLOCK_SIZE);
#endif
    CHECK(q->CoalesceBuffers[0]); q->CoalesceBusy[0]=copied;
    ULONG objects=copied?(maximum?32u:2u):1u,ipLength=ip==4?20u:40u;
    PB_COALESCE coal={.Length=ipLength+8+32,.Protocol=(UCHAR)protocol,.IpVersion=(UCHAR)ip,.SegmentSize=16,.PayloadLength=32};
    if(maximum) { coal.Length=65535; coal.PayloadLength=65535-ipLength-(protocol==6?20:8); coal.SegmentSize=(coal.PayloadLength+objects-1)/objects; }
    for(ULONG i=0;i<objects;++i) {
        descriptors[i].Token=(PB_TOKEN){42,1,(USHORT)i,0};
        /* Reproduce the observed plain ICMP shapes as well as TCP/UDP fixtures. */
        descriptors[i].Length=protocol==1?576u:protocol==58?1280u:ipLength+8+16;
        if(maximum) descriptors[i].Length=copied?ipLength+(protocol==6?20:8)+(i+1==objects?coal.PayloadLength-i*coal.SegmentSize:coal.SegmentSize):65535;
        descriptors[i].DataOffset=layout.Queues[0].RxData+i*PB_BLOCK_SIZE;
        IpShape(region+descriptors[i].DataOffset,ip,protocol,descriptors[i].Length);
        slots[i].Active=1;
    }
    IpShape(q->CoalesceBuffers[0],ip,protocol,coal.Length); q->Credits=objects;
    PB_QUEUE_VIEW completion; CHECK(PbAttachQueue(&q->Rings[PB_QUEUE_RX_COMPLETE],region,&layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
    CHECK(PbAttachQueue(&completion,region,&layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    Acquisitions=0; Failure=failure;
    NET_BUFFER_LIST *nbl=PbV2Receive(q,descriptors,objects,copied?0:MAXULONG,copied?&coal:NULL);
    if(failure) { CHECK(!nbl && !Pools && !Mdls && !Nbls && !s.Active && !s.RxObjects); }
    else {
        CHECK(nbl && Pools==1 && Mdls==2 && Nbls==1 && !s.Active && !s.RxObjects);
        CHECK(nbl->FirstMdl->Next && nbl->FirstMdl->Next->Partial==!copied && nbl->SourceHandle==ctx.MiniportAdapterHandle);
        CHECK(nbl->Flags==(ip==4?NDIS_NBL_FLAGS_IS_IPV4:NDIS_NBL_FLAGS_IS_IPV6)+(copied?(protocol==6?NDIS_NBL_FLAGS_IS_TCP:NDIS_NBL_FLAGS_IS_UDP):0));
        CHECK(!NET_BUFFER_LIST_INFO(nbl,RscTcpTimestampDelta) && !NET_BUFFER_LIST_INFO(nbl,Ieee8021QNetBufferListInfo));
        if(copied) {
            NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum={.Value=NET_BUFFER_LIST_INFO(nbl,TcpIpChecksumNetBufferListInfo)};
            CHECK(checksum.Receive.IpChecksumSucceeded==(ULONG)(ip==4));
            CHECK(checksum.Receive.TcpChecksumSucceeded==(ULONG)(protocol==6) && checksum.Receive.UdpChecksumSucceeded==(ULONG)(protocol==17));
            CHECK(checksum.Receive.IpChecksumValueInvalid==(ULONG)(ip==4 && protocol==17));
        } else CHECK(!NET_BUFFER_LIST_INFO(nbl,TcpIpChecksumNetBufferListInfo));
        PbV2RetainReceives(&s,1,objects); NET_BUFFER_LIST_STATUS(nbl)=returnStatus;
#ifdef PB_RETURN_DIAGNOSTICS_ENABLED
        PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl)&~(ULONG_PTR)1);
        record->LocallyCancelled=local;
        /* Returned metadata must come from the creation snapshot, not mutable payload. */
        region[descriptors[0].DataOffset]=0; q->CoalesceBuffers[0][0]=0;
        ULONG at=ip==4?2:4;
        region[descriptors[0].DataOffset+at]=region[descriptors[0].DataOffset+at+1]=0;
        q->CoalesceBuffers[0][at]=q->CoalesceBuffers[0][at+1]=0;
#endif
        FixtureDiagnosticLocks=0; BeforeNblFree=ScrambleNbl;
        CHECK(PbV2ReturnOne(&ctx,nbl));
        BeforeNblFree=NULL;
        PB_DESCRIPTOR returned[32]; uint32_t count;
        CHECK(PbQueuePop(&completion,returned,32,&count)==PB_OK && count==objects);
        for(ULONG i=0;i<objects;++i) CHECK(returned[i].Token.Slot==i && returned[i].Status==(ULONG)(local?PB_STOPPED:returnStatus?PB_RX_REJECTED:PB_OK) && !slots[i].Active);
        CHECK(!Pools && !Mdls && !Nbls && !s.Active && !s.Returns.References && !q->Credits && !q->PendingCount && !q->CoalesceBusy[0]);
        CHECK(s.RxObjects==objects && s.RxReturned==objects && s.Completions==objects && WaitForSingleObject(s.AllReturned.Native,0)==WAIT_OBJECT_0);
        CHECK(ctx.Statistics.ifHCInUcastPkts==((returnStatus || local)?0:objects) && ctx.Statistics.ifInErrors==(!local && returnStatus?1:0));
#ifdef PB_RETURN_DIAGNOSTICS_ENABLED
        CHECK(ctx.Statistics.ifInDiscards==(local?objects:0));
        CHECK(FixtureDiagnosticLocks==((returnStatus || local)?1:0));
        if(returnStatus || local) {
            PB_RETURN_SNAPSHOT diagnostic={0}; PbSnapshotReturnEvents(&ctx.Probe.ReturnDiagnostics,&diagnostic);
            CHECK(diagnostic.RecordCount==1 && diagnostic.NdisNonzeroNbls==(local?0:1) && diagnostic.LocalCancelledPackets==(local?objects:0));
            PB_RETURN_EVENT *e=diagnostic.Records;
            CHECK(e->NblStatus==returnStatus && e->CompletionStatus==(ULONG)(local?PB_STOPPED:PB_RX_REJECTED));
            CHECK(e->Origin==(ULONG)(local?PB_RETURN_ORIGIN_LOCAL_CANCEL:PB_RETURN_ORIGIN_NDIS));
            CHECK(e->FirstToken.Epoch==42 && e->FirstToken.Slot==0 && e->FirstToken.Generation==1 && e->OriginalCount==objects);
            CHECK(e->IndicationLength==(copied?coal.Length:descriptors[0].Length) && e->IpVersion==ip && e->Protocol==protocol);
            CHECK(e->IpDeclaredLength==e->IndicationLength);
        }
#endif
    }
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(q->CoalesceBuffers[0]);
#else
    free(q->CoalesceBuffers[0]);
#endif
    CloseHandle(kernel.Native); CloseHandle(user.Native); CloseHandle(s.AllReturned.Native);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(region);
#else
    _aligned_free(region);
#endif
}
static VOID Run(ULONG failure,ULONG ip,ULONG protocol,BOOLEAN copied,ULONG status,BOOLEAN maximum)
{RunCase(failure,ip,protocol,copied,status,maximum,FALSE);}
int main(VOID)
{
    DWORD initial,final; CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial)); ULONG cases=0;
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG copied=0;copied<2;++copied) for(ULONG protocol=6;protocol<=17;protocol+=11) {
        for(ULONG fail=1;fail<=4;++fail) { Run(fail,ip,protocol,(BOOLEAN)copied,0,FALSE); cases++; }
        Run(0,ip,protocol,(BOOLEAN)copied,0,FALSE); Run(0,ip,protocol,(BOOLEAN)copied,1,FALSE); cases+=2;
    }
#ifdef PB_GUARD_TESTS
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG copied=0;copied<2;++copied) for(ULONG protocol=6;protocol<=17;protocol+=11)
        Run(0,ip,protocol,(BOOLEAN)copied,0,TRUE);
    PB_KERNEL_SESSION empty={0}; PB_KERNEL_QUEUE q={0}; q.Session=&empty; PB_DESCRIPTOR d={.Length=20};
    CHECK(!PbV2Receive(&q,NULL,1,MAXULONG,NULL) && !PbV2Receive(&q,&d,0,MAXULONG,NULL) && !PbV2Receive(&q,&d,33,MAXULONG,NULL));
    CHECK(!PbV2Receive(&q,&d,1,0,NULL) && !PbV2Receive(&q,&d,1,MAXULONG,NULL));
    d.Length=65536; CHECK(!PbV2Receive(&q,&d,1,MAXULONG,NULL));
    puts("PASS: eight maximum-length/32-descriptor guarded cases and six internal-shape/range rejects.");
#endif
    CHECK(cases==48 && GetProcessHandleCount(GetCurrentProcess(),&final) && initial==final);
#ifdef PB_RETURN_DIAGNOSTICS_ENABLED
    RunCase(0,4,6,FALSE,0x00010003u,FALSE,FALSE);
    RunCase(0,6,17,TRUE,0xc0000001u,FALSE,FALSE);
    RunCase(0,4,17,FALSE,0xc023002au,FALSE,TRUE);
    RunCase(0,6,6,TRUE,0,FALSE,TRUE);
    /* Target capture: plain IPv4 ICMP/576 and IPv6 ICMP/1280 rejected by the
     * protocol stack. Preserve status, token retirement and all ownership counts. */
    RunCase(0,4,1,FALSE,0xc000023eu,FALSE,FALSE);
    RunCase(0,6,58,FALSE,0xc000023eu,FALSE,FALSE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&final) && initial==final);
    puts("PASS: raw positive/negative/protocol-unreachable NBL statuses survive free/reuse; packet rejection keeps wire value 4, cancellation is STOPPED/discard; metadata snapshot, zero resource/handle delta and exceptional-only locks.");
#endif
    puts("PASS: actual NBL allocation/return/completion, 48 plain/coalesced IPv4/IPv6 cases, all four acquisition failures, OOB flags, success/error returns, zero tracked resources and handle delta.");
    return 0;
}
