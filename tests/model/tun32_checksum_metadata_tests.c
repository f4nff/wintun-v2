/* SPDX-License-Identifier: GPL-2.0 */
/* NDIS checksum operations are selected by the family flags first. */
#include PB_CHECKSUM_OFFLOAD_HOST
static ULONG C32Cases,C32Violations,C32Rejected;
static VOID C32Case(ULONG protocol,ULONG kind,BOOLEAN enabled,ULONG runtime)
{
    CHECK((protocol==6 || protocol==17) && kind<5);
    ULONG ip=PB_TEST_IP_VERSION,length=1500,frameLength=length+14;
    O32Kind=protocol==6?0:1;O32Mode=0;O32Late=O32PolicyChanges=0;
    O32PushCalls=O32Published=O32CompleteCalls=O32LsoCompletions=0;
    Setup(1,64,128);RealEthernet=RealMdl=TRUE;HostedNdisVersion=runtime;FutureInfoReads=0;
    O32Allowed=enabled?PB_PROBE_SUPPORTED:0;O32Requested=PbPlatformOffloads(runtime);
    O32Lists=PbTestGuardAlloc(32*sizeof(*O32Lists));memset(O32Lists,0,32*sizeof(*O32Lists));++O32NblOwners;
    O32Buffers=PbTestGuardAlloc(32*sizeof(*O32Buffers));memset(O32Buffers,0,32*sizeof(*O32Buffers));++O32NbOwners;
    UCHAR *frames[32];MDL mdls[32][4];
    for(ULONG i=0;i<32;++i){
        frames[i]=PbTestGuardAlloc(frameLength);++O32Frames;memset(frames[i],0,14);
        frames[i][0]=frames[i][6]=2;O32W16(frames[i]+12,ip==4?0x0800:0x86dd);
        ULONG offset=O32Packet(frames[i]+14,length,O32Kind,i);
        CHECK(PbFinalizeChecksums(frames[i]+14,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
        O32Source(mdls[i],frames[i],frameLength);
        O32Buffers[i]=(NET_BUFFER){.Length=frameLength,.Frame=frames[i],.Mdl=mdls[i],.MdlOffset=7};
        O32Lists[i].Nb=O32Buffers+i;O32Lists[i].Next=i<31?O32Lists+i+1:NULL;
        NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum={0};
        /* No reserved bits are fabricated. With both family flags clear,
         * the documented control says there is no checksum operation. */
        if(kind!=4){checksum.Transmit.TcpChecksum=protocol==6 || kind==1;
            checksum.Transmit.UdpChecksum=protocol==17 || kind==1;
            checksum.Transmit.IpHeaderChecksum=ip==4 || kind==1;}
        checksum.Transmit.TcpHeaderOffset=kind>=2 && kind<4?offset+14:1023;
        if(kind==2){checksum.Transmit.IsIPv4=ip==4;checksum.Transmit.IsIPv6=ip==6;}
        if(kind==3){checksum.Transmit.IsIPv4=1;checksum.Transmit.IsIPv6=1;}
        O32Lso[i]=O32Lists[i].Info[0]=NULL;O32Uso[i]=O32Lists[i].Info[1]=NULL;
        O32Checksum[i]=O32Lists[i].Info[2]=checksum.Value;
    }
    CHECK(PbV2Send(Current,O32Lists) && !O32Lists && !O32Buffers && O32CompleteCalls==1 && Completed==32 &&
        AfterPinRelease==1 && !Session->Active && !Session->Returns.Value && !Faults);
    BOOLEAN accept=kind<2 || kind==4 || (kind==2 && enabled);
    ULONG expected=accept?32:0,reason=kind==3?PB_SEND_CHECKSUM_FAMILY:PB_SEND_POLICY;
    if(accept && !O32Published){
        CHECK(kind<2 && Current->Probe.SendDiagnostics.Reasons[PB_SEND_CHECKSUM_FAMILY]==32);
        ++C32Violations;expected=0;reason=PB_SEND_CHECKSUM_FAMILY;
    }
    CHECK(O32Published==expected && O32PushCalls==(ULONG)!!expected && Session->TxObjects==expected);
    for(ULONG i=0;i<32;++i)CHECK(O32Statuses[i]==(ULONG)(i<expected?NDIS_STATUS_SUCCESS:
        reason==PB_SEND_POLICY?NDIS_STATUS_NOT_SUPPORTED:NDIS_STATUS_INVALID_DATA));
    CHECK(Reads==32 && Describes==32 && Reserves==32 && PrefixBytes==1888 && PayloadBytes==48000 &&
        PrefixCalls==32 && PrefixMaps==32 && PayloadMaps==96 && !Controls && !Replies);
    CHECK(runtime>=PB_NDIS_VERSION(83) || !FutureInfoReads);
    CHECK(!Current->Probe.LsoNbls && !Current->Probe.UsoNbls && Current->Probe.ChecksumNbls==32 &&
        Current->Probe.OffloadRejected==32-expected && Current->Statistics.ifHCOutUcastPkts==expected &&
        Current->Statistics.ifHCOutOctets==(LONG64)expected*length);
    if(!expected){CHECK(Current->Probe.SendDiagnostics.Reasons[reason]==32);C32Rejected+=32;}
    PB_DESCRIPTOR items[32];uint32_t n,accepted,notify;
    CHECK(PbQueuePop(Consumers,items,32,&n)==(expected?PB_OK:PB_EMPTY) && n==expected);
    for(ULONG i=0;i<n;++i){PB_DESCRIPTOR *d=items+i;
        CHECK(d->Token.Slot==i && d->Token.Generation==1 && d->Length==length && !d->Type && !d->Flags &&
            PbValidateDescriptor(&Session->Layout,Session->Epoch,0,FALSE,d,7)==PB_OK &&
            !memcmp(Session->Region+d->DataOffset,frames[i]+14,length));}
    if(n){CHECK(PbQueuePush(Recyclers,items,n,&accepted,&notify)==PB_OK && accepted==n);
        KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&Session->Queues[0].TxLock,&lock);
        CHECK(PbV2RecycleLocked(Session->Queues)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);}
    for(ULONG i=0;i<64;++i)CHECK(Session->Queues[0].TxSlots[i].State==PB_SLOT_FREE);
    for(ULONG i=0;i<32;++i){O32SourceCheck(mdls[i],frames[i],frameLength);SourceFree(mdls[i]);PbTestGuardFree(frames[i]);--O32Frames;}
    Destroy();CHECK(!HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees && !O32Frames && !O32NblOwners && !O32NbOwners);
    ++C32Cases;printf("CHECKSUM32 ip%lu protocol%lu kind%lu enabled%u ndis%x published%lu pushes%lu prefix1888 payload48000 owners0\n",
        ip,protocol,kind,enabled,runtime,expected,O32PushCalls);
}
static VOID C32Matrix(VOID)
{const ULONG versions[]={20,89};for(ULONG v=0;v<2;++v)for(ULONG protocol=6;protocol<=17;protocol+=11)
    for(ULONG kind=0;kind<5;++kind)for(ULONG enabled=0;enabled<2;++enabled)C32Case(protocol,kind,(BOOLEAN)enabled,PB_NDIS_VERSION(versions[v]));}
int main(VOID)
{
    DWORD before,after;setvbuf(stdout,NULL,_IONBF,0);C32Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    C32Matrix();CHECK(C32Cases==80 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("CHECKSUM_METADATA ip%d cases80 warm40 measure40 violations%lu rejected%lu handles%lu/%lu mappings%lu/%lu owners0 repairs0 threads0\n",
        PB_TEST_IP_VERSION,C32Violations,C32Rejected,before,after,SourceAllocs,SourceFrees);
    if(C32Violations){CHECK(C32Violations==32);puts("CHECKSUM_FAMILY_ZERO_NOT_IGNORED: actual Send rejected defined no-checksum control; all packet/session owners reclaimed before141");return 141;}
    puts("PASS: NDIS checksum family selectors precede checksum task bits; plain bytes preserved and active-family errors/policy retained");return 0;
}
