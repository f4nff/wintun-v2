/* SPDX-License-Identifier: GPL-2.0 */
/* Included by the actual send fixture. Source MDLs are independent, read-only
 * guarded mappings. Describe's stand-in performs a complete actual MDL read. */
#pragma once
#ifndef PB_SEND_FAULT_BASELINE
static VOID MakeReadChain(MDL mdls[4],const UCHAR *frame,ULONG length)
{
    memset(mdls,0,4*sizeof(*mdls));ULONG first=min(length,40u),second=min(length-first,24u);
    ULONG sizes[]={first+7,0,second,length-first-second},at=0;
    for(ULONG i=0;i<4;++i) {
        mdls[i].Next=i<3?mdls+i+1:NULL;mdls[i].Length=sizes[i];if(!sizes[i]) continue;
        mdls[i].Address=PbTestGuardAlloc(sizes[i]);ULONG padding=i?0:7;
        memset(mdls[i].Address,0xa5,padding);memcpy(mdls[i].Address+padding,frame+at,sizes[i]-padding);at+=sizes[i]-padding;
        DWORD old;CHECK(VirtualProtect(mdls[i].Address,sizes[i],PAGE_READONLY,&old));
    }
    CHECK(at==length);
}
static VOID FreeReadChain(MDL mdls[4])
{for(ULONG i=0;i<4;++i) if(mdls[i].Address) PbTestGuardFree(mdls[i].Address);}
static VOID OrdinaryFrame(UCHAR *frame,ULONG length,ULONG ip,ULONG protocol,BOOLEAN vlan)
{
    memset(frame,0x2a,length);ULONG h=vlan?18:14;put16(frame+12,vlan?0x8100:ip==4?0x0800:0x86dd);
    if(vlan) put16(frame+16,ip==4?0x0800:0x86dd);UCHAR *p=frame+h;
    if(ip==4) {p[0]=0x45;put16(p+2,(USHORT)(length-h));p[9]=(UCHAR)protocol;}
    else {p[0]=0x60;put16(p+4,(USHORT)(length-h-40));p[6]=(UCHAR)protocol;}
}
static VOID OrdinaryPrefixes(VOID)
{
    const ULONG queues[]={1,2,3,8};ULONG cases=0;
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG protocol=6;protocol<=17;protocol+=11)
        for(ULONG vlan=0;vlan<2;++vlan) for(ULONG qi=0;qi<_countof(queues);++qi) {
            Setup(queues[qi],4,4);RealEthernet=RealMdl=TRUE;
            UCHAR *frame=PbTestGuardAlloc(1500);OrdinaryFrame(frame,1500,ip,protocol,(BOOLEAN)vlan);
            ULONG h=vlan?18:14,index=ActualSelectQueue(frame+h,1500-h,queues[qi]);
            MDL mdls[4];MakeReadChain(mdls,frame,1500);
            NET_BUFFER nb={.Length=1500,.Queue=index,.Frame=frame,.Mdl=mdls,.MdlOffset=7};NET_BUFFER_LIST nbl={.Nb=&nb};
            CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && Completed==1 && AfterPinRelease==1);
            CHECK(Describes==1 && !Controls && !Replies && Session->TxObjects==1 && PrefixCalls==1 && PayloadBytes==1500-h && PayloadMaps==3);
#ifdef PB_SEND_PREFIX_BASELINE
            CHECK(PrefixBytes==128 && PrefixMaps==3);
#else
            CHECK(PrefixBytes==PB_ETH_CLASSIFY_MAX && PrefixMaps==2);
#endif
            PB_DESCRIPTOR item;uint32_t n;CHECK(PbQueuePop(Consumers+index,&item,1,&n)==PB_OK && n==1);
            CHECK(item.Token.Queue==index && item.Length==1500-h && !memcmp(Session->Region+item.DataOffset,frame+h,item.Length));
            printf("PASS: actual MDL send ip=%lu protocol=%lu vlan=%lu queues=%lu: prefix_bytes=%llu prefix_maps=%lu payload_bytes=%llu payload_maps=%lu, exact full payload and queue.\n",ip,protocol,vlan,queues[qi],PrefixBytes,PrefixMaps,PayloadBytes,PayloadMaps);
            FreeReadChain(mdls);PbTestGuardFree(frame);Destroy();cases++;
        }
    CHECK(cases==32);
}
static VOID ControlPrefixes(VOID)
{
    const ULONG kinds[]={3,4,5,6,8,9};
    for(ULONG ki=0;ki<_countof(kinds);++ki) {
        Setup(1,4,4);RealEthernet=RealMdl=TRUE;UCHAR frame[128],expected[128];ULONG length=EthernetFrame(frame,kinds[ki]);
        uint32_t expectedLength;PB_ETH_RESULT result=ActualEthernetControl(frame,length,Current->Probe.PeerMac,expected,&expectedLength);
        MDL mdls[4];MakeReadChain(mdls,frame,length);
        NET_BUFFER nb={.Length=length,.Frame=frame,.Mdl=mdls,.MdlOffset=7};NET_BUFFER_LIST nbl={.Nb=&nb};
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && Completed==1 && AfterPinRelease==1);
        CHECK(PrefixBytes==length && Controls==1 && !Describes && !Reserves && !Session->TxObjects && Replies==(ULONG)(result==PB_ETH_REPLY));
        if(Replies) CHECK(LastReplyLength==expectedLength && !memcmp(LastReply,expected,expectedLength));
#ifdef PB_SEND_PREFIX_BASELINE
        CHECK(PrefixCalls==1);
#else
        CHECK(PrefixCalls==2);
#endif
        printf("PASS: actual control MDLs kind=%lu bytes=%llu read_calls=%lu maps=%lu: exact reply/drop, no publication and pin through unlocked callback.\n",kinds[ki],PrefixBytes,PrefixCalls,PrefixMaps);
        FreeReadChain(mdls);Destroy();
    }
}
static VOID ShortPrefixes(VOID)
{
    const ULONG sizes[]={34,38,54,58,59,60,64,127,128,129};ULONG cases=0;
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG vlan=0;vlan<2;++vlan) for(ULONG si=0;si<_countof(sizes);++si) {
        ULONG h=vlan?18:14,length=sizes[si];if(length<h+(ip==4?20u:40u)) continue;
        Setup(3,4,4);RealEthernet=RealMdl=TRUE;UCHAR *frame=PbTestGuardAlloc(length);OrdinaryFrame(frame,length,ip,6,(BOOLEAN)vlan);
        ULONG index=ActualSelectQueue(frame+h,length-h,3);MDL mdls[4];MakeReadChain(mdls,frame,length);
        NET_BUFFER nb={.Length=length,.Queue=index,.Frame=frame,.Mdl=mdls,.MdlOffset=7};NET_BUFFER_LIST nbl={.Nb=&nb};
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && Completed==1 && AfterPinRelease==1);
#ifdef PB_SEND_PREFIX_BASELINE
        ULONG prefix=min(length,PB_ETH_CONTROL_MAX);
#else
        ULONG prefix=min(length,PB_ETH_CLASSIFY_MAX);
#endif
        CHECK(PrefixCalls==1 && PrefixBytes==prefix && PrefixMaps==(prefix<=40?1u:prefix<=64?2u:3u));
        CHECK(PayloadBytes==length-h && !Controls && !Replies && Session->TxObjects==1);
        PB_DESCRIPTOR item;uint32_t n;CHECK(PbQueuePop(Consumers+index,&item,1,&n)==PB_OK && n==1 && item.Token.Queue==index && item.Length==length-h);
        CHECK(!memcmp(Session->Region+item.DataOffset,frame+h,item.Length));
        FreeReadChain(mdls);PbTestGuardFree(frame);Destroy();cases++;
    }
    printf("PASS: actual send %lu short/header/min-59/64/128 boundary cases, exact complete bytes and unchanged queue selection.\n",cases);
}
static VOID FailedPrefixReads(VOID)
{
    for(ULONG control=0;control<2;++control) for(ULONG fail=0;fail<4;++fail) {
        Setup(1,4,4);RealEthernet=RealMdl=TRUE;UCHAR frame[1500];ULONG length;
        if(control) length=EthernetFrame(frame,9);else {length=1500;OrdinaryFrame(frame,length,6,6,FALSE);}
        MDL mdls[4];MakeReadChain(mdls,frame,length);
        if(fail<3) mdls[fail==0?0:fail==1?2:3].Failed=TRUE;
        NET_BUFFER nb={.Length=length,.Frame=frame,.Mdl=mdls,.MdlOffset=fail==3?mdls[0].Length+1:7};NET_BUFFER_LIST nbl={.Nb=&nb};
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_RESOURCES && Completed==1 && AfterPinRelease==1);
        CHECK(!Controls && !Replies && !Session->TxObjects && !Faults && !Session->Active && !Session->Returns.Value);
        for(ULONG i=0;i<4;++i) CHECK(Session->Queues[0].TxSlots[i].State==PB_SLOT_FREE);
        if(!control && fail==2) {
#ifdef PB_SEND_PREFIX_BASELINE
            CHECK(!Reserves && !Describes);
#else
            CHECK(Reserves==1 && Describes==1 && PrefixBytes==PB_ETH_CLASSIFY_MAX);
#endif
        }
        FreeReadChain(mdls);Destroy();
    }
    puts("PASS: actual MDL mapping failures in classification/control/payload and invalid current offset publish nothing; reserved slot rolls back, all references and callbacks complete.");
}
static VOID ReaderBounds(VOID)
{
    UCHAR frame[200];for(ULONG i=0;i<sizeof(frame);++i) frame[i]=(UCHAR)(i*37u);
    MDL mdls[4];MakeReadChain(mdls,frame,sizeof(frame));NET_BUFFER nb={.Length=sizeof(frame),.Mdl=mdls,.MdlOffset=7};
    PB_TEST_GUARD output=PbTestGuardCreate(sizeof(frame));ULONG cases=0;ReadingPayload=FALSE;
    const ULONG sizes[]={0,1,2,7,8,14,18,40,59,64,128,200};
    for(ULONG skip=0;skip<=sizeof(frame);++skip) for(ULONG si=0;si<_countof(sizes);++si) {
        ULONG length=min(sizes[si],sizeof(frame)-skip);UCHAR *bytes=PbTestGuardSpan(&output,length);
        CHECK(ActualRead(&nb,skip,bytes,length) && !memcmp(bytes,frame+skip,length));cases++;
    }
    ULONG visits=MdlVisits;UCHAR *bytes=PbTestGuardSpan(&output,sizeof(frame));memset(bytes,0xcc,sizeof(frame));
    CHECK(!ActualRead(&nb,201,bytes,0) && !ActualRead(&nb,0,bytes,201) && !ActualRead(&nb,UINT32_MAX,bytes,1) && !ActualRead(&nb,1,bytes,UINT32_MAX));
    CHECK(MdlVisits==visits);for(ULONG i=0;i<sizeof(frame);++i) CHECK(bytes[i]==0xcc);
    mdls[2].Next=NULL;CHECK(!ActualRead(&nb,0,bytes,sizeof(frame)));nb.Mdl=NULL;
    CHECK(!ActualRead(&nb,0,bytes,1) && ActualRead(&nb,200,bytes,0));
    FreeReadChain(mdls);PbTestGuardDestroy(&output);
    printf("PASS: actual MDL reader %lu guarded span/skip cases, nonzero current offset, empty MDL, short/null chain and overflow bounds.\n",cases);
}
static VOID SendPrefixes(VOID)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    OrdinaryPrefixes();ShortPrefixes();ControlPrefixes();FailedPrefixReads();ReaderBounds();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
}
#endif
