/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Describe with DDI metadata/read stand-ins and actual packet core.
 * Counter is driver-side reparses; Finalize itself parses TCP once. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../common/packet.h"
#include "../../include/packetbridge_v2_abi.h"
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_send_diagnostics.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
typedef LONG NDIS_STATUS;
enum {NDIS_STATUS_SUCCESS,NDIS_STATUS_INVALID_LENGTH,NDIS_STATUS_RESOURCES,NDIS_STATUS_NOT_SUPPORTED,NDIS_STATUS_INVALID_DATA};
#define NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE 1u
enum {TcpLargeSendNetBufferListInfo,UdpSegmentationOffloadInfo,TcpIpChecksumNetBufferListInfo};
typedef union {struct {ULONG IsIPv4:1,IsIPv6:1,IpHeaderChecksum:1,TcpChecksum:1,UdpChecksum:1,TcpHeaderOffset:10;}Transmit;VOID *Value;} NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO;
/* Match the overlapping LSO/USO fields used by the WDK, without importing DDI types. */
typedef union {struct {ULONG Unused:30,Type:1,Reserved:1;}Transmit;struct {ULONG MSS:20,TcpHeaderOffset:10,Type:1,IPVersion:1;}LsoV2Transmit;VOID *Value;} NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO;
typedef union {struct {ULONG MSS:20,UdpHeaderOffset:10,Reserved:1,IPVersion:1;}Transmit;VOID *Value;} NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO;
typedef struct NET_BUFFER {struct NET_BUFFER *Next;UCHAR *Data;ULONG Length;} NET_BUFFER;
typedef struct {VOID *Info[3];NET_BUFFER *First;} NET_BUFFER_LIST;
typedef struct {ULONG Mask;} TUN_CTX;
#define NET_BUFFER_DATA_LENGTH(n) ((n)->Length)
#define NET_BUFFER_NEXT_NB(n) ((n)->Next)
#define NET_BUFFER_LIST_FIRST_NB(n) ((n)->First)
#define NET_BUFFER_LIST_INFO(n,i) ((n)->Info[i])
static ULONG Finalizes,Reparses,PolicyReads;
static TUN_CTX *DescribingCtx;
static BOOLEAN DisableOnChecksum,DisableOnPlan;
static BOOLEAN PbV2Read(NET_BUFFER *nb,ULONG offset,UCHAR *out,ULONG length)
{CHECK(offset<=nb->Length && length<=nb->Length-offset);memcpy(out,nb->Data+offset,length);return TRUE;}
static ULONG PbEffectiveMask(TUN_CTX *ctx) {PolicyReads++;return ctx->Mask;}
static PB_PACKET_STATUS ChecksumResult(PB_PACKET_STATUS status)
{if(status==PB_PACKET_OK && DisableOnChecksum) {CHECK(DescribingCtx);DescribingCtx->Mask=0;}return status;}
static PB_PACKET_STATUS Finalize(UCHAR *packet,uint32_t length,int ip,int tcp,int udp)
{Finalizes++;return ChecksumResult(PbFinalizeChecksums(packet,length,ip,tcp,udp));}
#ifndef PB_DESCRIBE_BASELINE
static PB_PACKET_STATUS FinalizeOffset(UCHAR *packet,uint32_t length,int ip,int tcp,int udp,uint32_t *offset)
{Finalizes++;return ChecksumResult(PbFinalizeChecksumsWithOffset(packet,length,ip,tcp,udp,offset));}
#endif
static PB_PACKET_STATUS Reparse(const UCHAR *packet,uint32_t length,PB_SEGMENT_TYPE type,uint32_t mss,PB_SEGMENT_PLAN *plan)
{Reparses++;PB_PACKET_STATUS status=PbPlanSegments(packet,length,type,mss,plan);if(status==PB_PACKET_OK && DisableOnPlan) {CHECK(DescribingCtx);DescribingCtx->Mask=0;}return status;}
#define PbPlanSegments Reparse
#define PbFinalizeChecksums Finalize
#ifndef PB_DESCRIBE_BASELINE
#define PbFinalizeChecksumsWithOffset FinalizeOffset
#endif
#ifndef PB_DESCRIBE_SOURCE
#define PB_DESCRIBE_SOURCE "../../driver/v2/session_describe.h"
#endif
#include PB_DESCRIBE_SOURCE
#undef PbPlanSegments
#undef PbFinalizeChecksums
#undef PbFinalizeChecksumsWithOffset
/* Historical A/B bodies predate the diagnostic output. Keep them byte-for-byte
 * intact and compare their original status/payload/cost contract; only current
 * production code can be required to produce the later failure reasons. */
#if defined(PB_DESCRIBE_BASELINE) || defined(PB_EVENT_POLICY_BASELINE)
#define PB_TEST_SEND_DIAGNOSTICS 0
#else
#define PB_TEST_SEND_DIAGNOSTICS 1
#endif
static NDIS_STATUS Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG ethernet,PB_DESCRIPTOR *d,UCHAR *output,PB_SEND_FAILURE *failure)
{
#if PB_TEST_SEND_DIAGNOSTICS
    return PbV2Describe(ctx,nbl,nb,ethernet,d,output,failure);
#else
    (void)failure;return PbV2Describe(ctx,nbl,nb,ethernet,d,output);
#endif
}
static VOID W16(UCHAR *p,ULONG n) {p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID Packet(UCHAR *p,ULONG length,ULONG ip,ULONG protocol,BOOLEAN options)
{
    memset(p,0,length);ULONG offset=ip==4?20:40;
    if(ip==4) {
        p[0]=0x45;W16(p+2,length);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;
        if(options) {p[0]=0x46;memset(p+20,1,4);offset=24;}
    } else {
        p[0]=0x60;W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=0x20;p[24]=0x30;
        if(options) {p[6]=0;p[40]=(UCHAR)protocol;p[41]=0;offset=48;}
    }
    if(protocol==6) {W16(p+offset,4000);W16(p+offset+2,5000);p[offset+12]=0x50;p[offset+13]=0x10;W16(p+offset+14,8192);}
    else if(protocol==17) {W16(p+offset,4000);W16(p+offset+2,5000);W16(p+offset+4,length-offset);}
    memset(p+offset+(protocol==6?20:8),0x5a,length-offset-(protocol==6?20:8));
}
static VOID Run(ULONG ip,ULONG protocol,ULONG length,BOOLEAN options,ULONG ethernet,ULONG padding,BOOLEAN enabled,BOOLEAN badOffset,BOOLEAN badFamily,BOOLEAN both)
{
    UCHAR *frame=PbTestGuardAlloc(length+ethernet+padding),*output=PbTestGuardAlloc(PB_BLOCK_SIZE),*oracle=PbTestGuardAlloc(length);
    memset(frame,0,length+ethernet+padding);Packet(frame+ethernet,length,ip,protocol,options);memcpy(oracle,frame+ethernet,length);
    ULONG offset=(ip==4?20:40)+(options?(ip==4?4:8):0);
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO c={0};c.Transmit.IsIPv4=ip==4;c.Transmit.IsIPv6=ip==6;
    c.Transmit.IpHeaderChecksum=ip==4;c.Transmit.TcpChecksum=protocol==6;c.Transmit.UdpChecksum=protocol==17;
    c.Transmit.TcpHeaderOffset=offset+ethernet+(badOffset?1:0);
    if(badFamily) {c.Transmit.IsIPv4=ip!=4;c.Transmit.IsIPv6=ip!=6;}
    if(both) c.Transmit.TcpChecksum=c.Transmit.UdpChecksum=1;
    PB_PACKET_STATUS oracleStatus=PbFinalizeChecksums(oracle,length,ip==4,protocol==6 || both,protocol==17 || both);
    NET_BUFFER nb={NULL,frame,length+ethernet+padding};NET_BUFFER_LIST nbl={{0},&nb};nbl.Info[TcpIpChecksumNetBufferListInfo]=c.Value;
    TUN_CTX ctx={enabled?PB_PROBE_SUPPORTED:0};PB_DESCRIPTOR d={0};Finalizes=Reparses=PolicyReads=0;DescribingCtx=&ctx;
    PB_SEND_FAILURE failure={0};NDIS_STATUS r=Describe(&ctx,&nbl,&nb,ethernet,&d,output,&failure);
#if PB_TEST_SEND_DIAGNOSTICS
    if(r!=NDIS_STATUS_SUCCESS) CHECK(failure.Reason==(ULONG)(badFamily?PB_SEND_CHECKSUM_FAMILY:both?PB_SEND_CHECKSUM_DATA:badOffset?PB_SEND_CHECKSUM_OFFSET:PB_SEND_POLICY));
#endif
    CHECK(r==((badOffset || badFamily || both)?NDIS_STATUS_INVALID_DATA:enabled && !DisableOnChecksum?NDIS_STATUS_SUCCESS:NDIS_STATUS_NOT_SUPPORTED));
    CHECK(PolicyReads==(ULONG)(r==NDIS_STATUS_SUCCESS || r==NDIS_STATUS_NOT_SUPPORTED));
    CHECK(d.Length==length && !d.Type);
    if(!badFamily && !both) CHECK(oracleStatus==PB_PACKET_OK && !memcmp(oracle,output,length));
    if(protocol==6 && !badFamily && !both) {
        CHECK(Finalizes==1);
#ifdef PB_DESCRIBE_BASELINE
        CHECK(Reparses==1);
#else
        CHECK(!Reparses);
#endif
    } else CHECK(!Reparses);
    printf("PASS: Describe ip=%lu protocol=%lu length=%lu options=%u eth=%lu pad=%lu enabled=%u badOffset=%u badFamily=%u both=%u status=%ld finalizes=%lu driver_reparses=%lu policy_reads=%lu disable_after_checksum=%u.\n",ip,protocol,length,options,ethernet,padding,enabled,badOffset,badFamily,both,r,Finalizes,Reparses,PolicyReads,DisableOnChecksum);
    DescribingCtx=NULL;
    PbTestGuardFree(frame);PbTestGuardFree(output);PbTestGuardFree(oracle);
}
static VOID Plain(ULONG ip,ULONG protocol,ULONG length,ULONG ethernet,ULONG padding,BOOLEAN options,BOOLEAN fragment,ULONG bad)
{
    UCHAR *frame=PbTestGuardAlloc(ethernet+length+padding),*output=PbTestGuardAlloc(PB_BLOCK_SIZE);
    memset(frame,0,ethernet+length+padding);Packet(frame+ethernet,length,ip,protocol,options);
    if(fragment) frame[ethernet+6]|=0x20;
    if(bad==1) W16(frame+ethernet+(ip==4?2:4),ip==4?length+1:length-39);
    if(bad==2) frame[ethernet]=0x70;
    NET_BUFFER nb={NULL,frame,ethernet+length+padding};NET_BUFFER_LIST nbl={{0},&nb};TUN_CTX ctx={0};PB_DESCRIPTOR d={0};
    Finalizes=Reparses=PolicyReads=0;
    CHECK(Describe(&ctx,&nbl,&nb,ethernet,&d,output,NULL)==(bad?NDIS_STATUS_INVALID_DATA:NDIS_STATUS_SUCCESS));
    if(!bad) {
        CHECK(d.Length==length && !d.Type && !memcmp(frame+ethernet,output,length));
#if defined(PB_EVENT_POLICY_BASELINE) || defined(PB_DESCRIBE_BASELINE)
        CHECK(PolicyReads==1);
#else
        CHECK(!PolicyReads);
#endif
    } else CHECK(!PolicyReads);
    CHECK(!Finalizes && !Reparses);
    printf("PASS: plain Describe ip=%lu protocol=%lu length=%lu ethernet=%lu padding=%lu options=%u fragment=%u bad=%lu policy_reads=%lu, unchanged bytes/validation.\n",ip,protocol,length,ethernet,padding,options,fragment,bad,PolicyReads);
    PbTestGuardFree(frame);PbTestGuardFree(output);
}
static VOID Gso(ULONG ip,ULONG protocol,BOOLEAN enabled,BOOLEAN badOffset,BOOLEAN badFamily)
{
    ULONG length=(ip==4?20:40)+(protocol==6?20:8)+2800,ethernet=18,offset=(ip==4?20:40)+ethernet;
    UCHAR *frame=PbTestGuardAlloc(length+ethernet),*output=PbTestGuardAlloc(PB_BLOCK_SIZE);
    memset(frame,0,length+ethernet);Packet(frame+ethernet,length,ip,protocol,FALSE);
    if(protocol==17) W16(frame+ethernet+(ip==4?20:40)+6,1);
    NET_BUFFER nb={NULL,frame,length+ethernet};NET_BUFFER_LIST nbl={{0},&nb};
    NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso={0};NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO uso={0};
    if(protocol==6) {
        lso.LsoV2Transmit.MSS=1400;lso.LsoV2Transmit.Type=NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE;
        lso.LsoV2Transmit.TcpHeaderOffset=offset+badOffset;lso.LsoV2Transmit.IPVersion=badFamily?ip!=6:ip==6;
        nbl.Info[TcpLargeSendNetBufferListInfo]=lso.Value;
    } else {
        uso.Transmit.MSS=1400;uso.Transmit.UdpHeaderOffset=offset+badOffset;uso.Transmit.IPVersion=badFamily?ip!=6:ip==6;
        nbl.Info[UdpSegmentationOffloadInfo]=uso.Value;
    }
    TUN_CTX ctx={enabled?PB_PROBE_SUPPORTED:0};PB_DESCRIPTOR d={0};Finalizes=Reparses=PolicyReads=0;DescribingCtx=&ctx;
    PB_SEND_FAILURE failure={0};NDIS_STATUS status=Describe(&ctx,&nbl,&nb,ethernet,&d,output,&failure);
#if PB_TEST_SEND_DIAGNOSTICS
    if(status!=NDIS_STATUS_SUCCESS) CHECK(failure.Reason==(ULONG)(badOffset?PB_SEND_GSO_OFFSET:badFamily?PB_SEND_GSO_FAMILY:PB_SEND_POLICY));
#endif
    CHECK(status==((badOffset || badFamily)?NDIS_STATUS_INVALID_DATA:enabled && !DisableOnPlan?NDIS_STATUS_SUCCESS:NDIS_STATUS_NOT_SUPPORTED));
    CHECK(Reparses==1 && !Finalizes && PolicyReads==(ULONG)(!badOffset && !badFamily));
    CHECK(!memcmp(output,frame+ethernet,length));
    if(!badOffset && !badFamily) CHECK(d.Type==(ULONG)(protocol==6?PB_DESC_TCP_GSO:PB_DESC_UDP_GSO) && d.Mss==1400 && d.L4Offset+ethernet==offset);
    printf("PASS: GSO Describe ip=%lu protocol=%lu enabled=%u bad_offset=%u bad_family=%u disable_after_plan=%u status=%ld policy_reads=%lu.\n",ip,protocol,enabled,badOffset,badFamily,DisableOnPlan,status,PolicyReads);
    DescribingCtx=NULL;PbTestGuardFree(frame);PbTestGuardFree(output);
}
#ifndef PB_DESCRIBE_BASELINE
static VOID OffsetContract(VOID)
{
    UCHAR *p=PbTestGuardAlloc(64);uint32_t *offset=PbTestGuardAlloc(sizeof(*offset));
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG protocol=6;protocol<=17;protocol+=11) {
        Packet(p,64,ip,protocol,FALSE);*offset=UINT32_MAX;
        CHECK(PbFinalizeChecksumsWithOffset(p,64,ip==4,protocol==6,protocol==17,offset)==PB_PACKET_OK);
        CHECK(*offset==(ip==4?20u:40u));
    }
    Packet(p,64,4,1,FALSE);p[6]=0x20; /* Fragment/ICMP still supports IPv4 header-only checksum. */
    *offset=UINT32_MAX;CHECK(PbFinalizeChecksumsWithOffset(p,64,1,0,0,offset)==PB_PACKET_OK && !*offset);
    Packet(p,64,4,6,FALSE);*offset=UINT32_MAX;
    CHECK(PbFinalizeChecksumsWithOffset(p,64,0,0,1,offset)==PB_PACKET_INVALID && *offset==UINT32_MAX);
    CHECK(PbFinalizeChecksumsWithOffset(NULL,64,0,1,0,offset)==PB_PACKET_INVALID && *offset==UINT32_MAX);
    const ULONG edges[]={0,1,19,20,39,40,63,65536,UINT32_MAX};
    for(ULONG i=0;i<(sizeof(edges)/sizeof(edges[0]));++i) {
        CHECK(PbFinalizeChecksumsWithOffset(p,edges[i],0,1,0,offset)!=PB_PACKET_OK && *offset==UINT32_MAX);
    }
    PbTestGuardFree(p);PbTestGuardFree(offset);
    puts("PASS: guarded checksum offset output: IPv4/6 TCP/UDP, header-only ICMP/fragment, failure preserves output and invalid length bounds.");
}
#endif
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Run(4,6,1500,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);Run(6,6,1500,FALSE,18,20,TRUE,FALSE,FALSE,FALSE);
    Run(4,6,1500,TRUE,18,0,TRUE,FALSE,FALSE,FALSE);Run(6,6,1500,TRUE,14,0,TRUE,FALSE,FALSE,FALSE);
    Run(4,6,64,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);Run(6,6,65000,FALSE,18,0,TRUE,FALSE,FALSE,FALSE);
    Run(4,6,1500,FALSE,14,0,FALSE,FALSE,FALSE,FALSE);Run(4,6,1500,FALSE,14,0,TRUE,TRUE,FALSE,FALSE);
    Run(4,6,1500,FALSE,14,0,TRUE,FALSE,TRUE,FALSE);Run(4,6,1500,FALSE,14,0,TRUE,FALSE,FALSE,TRUE);
    Run(4,17,1500,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);Run(6,17,1500,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);
    Run(4,1,64,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG protocol=6;protocol<=17;protocol+=11) {
        Plain(ip,protocol,64,14,20,FALSE,FALSE,0);Plain(ip,protocol,1500,18,0,FALSE,FALSE,0);
    }
    Plain(4,1,64,14,0,FALSE,TRUE,0);Plain(6,6,1500,18,0,TRUE,FALSE,0);
    Plain(4,6,64,14,0,FALSE,FALSE,1);Plain(6,17,64,18,0,FALSE,FALSE,1);Plain(6,6,64,14,0,FALSE,FALSE,2);
    DisableOnChecksum=TRUE;Run(4,6,64,FALSE,14,0,TRUE,FALSE,FALSE,FALSE);DisableOnChecksum=FALSE;
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG protocol=6;protocol<=17;protocol+=11) {Gso(ip,protocol,TRUE,FALSE,FALSE);Gso(ip,protocol,FALSE,FALSE,FALSE);}
    Gso(4,6,TRUE,TRUE,FALSE);Gso(6,17,TRUE,FALSE,TRUE);
    DisableOnPlan=TRUE;Gso(4,6,TRUE,FALSE,FALSE);Gso(6,17,TRUE,FALSE,FALSE);DisableOnPlan=FALSE;
#ifndef PB_DESCRIBE_BASELINE
    OffsetContract();
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
