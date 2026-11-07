/* SPDX-License-Identifier: GPL-2.0 */
/* Actual OID parameter handlers plus independent mask-gate expectations. */
#ifdef PB_API_TESTS
#error LSO checksum policy requires actual production paths
#endif
#ifndef NDEBUG
#error LSO checksum policy requires NDEBUG configuration
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select independent IPv4 or IPv6
#endif
#define main OriginalOffloadHandlerMain
#include "offload_handler_tests.c"
#undef main
static ULONG PolicyCases,PolicyViolations;
static NDIS_OFFLOAD ObservedReport;
static ULONG ObservedReports;
static VOID ObserveLsoReport(VOID *adapter,NDIS_STATUS_INDICATION *indication)
{
    CHECK(indication && indication->StatusBuffer && indication->StatusBufferSize<=sizeof(ObservedReport));
    memset(&ObservedReport,0,sizeof(ObservedReport));
    memcpy(&ObservedReport,indication->StatusBuffer,indication->StatusBufferSize);++ObservedReports;
    NdisMIndicateStatusEx(adapter,indication);
}
static ULONG LsoBit(VOID){return PB_TEST_IP_VERSION==4?PB_TX_LSO4:PB_TX_LSO6;}
static VOID Expected(BOOLEAN okay){++PolicyCases;if(!okay)++PolicyViolations;}
static BOOLEAN EncodedLso(BOOLEAN enabled)
{
#if PB_TEST_IP_VERSION == 4
    return ObservedReport.LsoV2.IPv4.MaxOffLoadSize==(enabled?32768u:0u) &&
        ObservedReport.LsoV2.IPv4.MinSegmentCount==(enabled?2u:0u) &&
        ObservedReport.LsoV2.IPv4.Encapsulation==(enabled?PB_ETH_ENCAPS:0u) &&
        !ObservedReport.Checksum.IPv4Transmit.IpChecksum && !ObservedReport.Checksum.IPv4Transmit.TcpChecksum;
#else
    return ObservedReport.LsoV2.IPv6.MaxOffLoadSize==(enabled?32768u:0u) &&
        ObservedReport.LsoV2.IPv6.MinSegmentCount==(enabled?2u:0u) &&
        ObservedReport.LsoV2.IPv6.Encapsulation==(enabled?PB_ETH_ENCAPS:0u) &&
        ObservedReport.LsoV2.IPv6.IpExtensionHeadersSupported==(enabled?NDIS_OFFLOAD_SUPPORTED:0u) &&
        ObservedReport.LsoV2.IPv6.TcpOptionsSupported==(enabled?NDIS_OFFLOAD_SUPPORTED:0u) &&
        !ObservedReport.Checksum.IPv6Transmit.TcpChecksum;
#endif
}
static VOID OidChecksum(ULONG minor)
{
    Initialize(minor);ObservedReports=0;ULONG lso=LsoBit();NDIS_OFFLOAD_PARAMETERS *p=MemoryAlloc(ParameterSizes[1]);
    p->Header=(NDIS_OBJECT_HEADER){NDIS_OBJECT_TYPE_DEFAULT,1,ParameterSizes[1]};
#if PB_TEST_IP_VERSION == 4
    p->IPv4Checksum=NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED;p->TCPIPv4Checksum=NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED;
    ULONG checksums=PB_TX_IP4_CHECKSUM|PB_TX_TCP4_CHECKSUM;
#else
    p->TCPIPv6Checksum=NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED;ULONG checksums=PB_TX_TCP6_CHECKSUM;
#endif
    NDIS_OID_REQUEST request;Oid(&request,OID_TCP_OFFLOAD_PARAMETERS,p,p->Header.Size);
    CHECK(PbSetOffloadParameters(&Ctx,&request)==NDIS_STATUS_SUCCESS && request.DATA.SET_INFORMATION.BytesRead==p->Header.Size);
    CHECK(((ULONG)Ctx.Probe.NdisMask&lso)==lso && !((ULONG)Ctx.Probe.NdisMask&checksums));
    CHECK(ObservedReports==1);Expected((LastReport&lso)==lso && (PbEffectiveMask(&Ctx)&lso)==lso && EncodedLso(TRUE));
    memset((UCHAR *)p+sizeof(p->Header),0,ParameterSizes[1]-sizeof(p->Header));
#if PB_TEST_IP_VERSION == 4
    p->LsoV2IPv4=NDIS_OFFLOAD_PARAMETERS_LSOV2_DISABLED;
#else
    p->LsoV2IPv6=NDIS_OFFLOAD_PARAMETERS_LSOV2_DISABLED;
#endif
    Oid(&request,OID_TCP_OFFLOAD_PARAMETERS,p,p->Header.Size);CHECK(PbSetOffloadParameters(&Ctx,&request)==NDIS_STATUS_SUCCESS);
    CHECK(ObservedReports==2);Expected(!(LastReport&lso) && !((ULONG)Ctx.Probe.NdisMask&lso) && EncodedLso(FALSE));
#if PB_TEST_IP_VERSION == 4
    p->LsoV2IPv4=NDIS_OFFLOAD_PARAMETERS_LSOV2_ENABLED;
#else
    p->LsoV2IPv6=NDIS_OFFLOAD_PARAMETERS_LSOV2_ENABLED;
#endif
    Oid(&request,OID_TCP_OFFLOAD_PARAMETERS,p,p->Header.Size);CHECK(PbSetOffloadParameters(&Ctx,&request)==NDIS_STATUS_SUCCESS);
    CHECK(ObservedReports==3 && !((ULONG)Ctx.Probe.NdisMask&checksums));Expected((LastReport&lso)==lso && EncodedLso(TRUE));
    CHECK(Policy(lso)==STATUS_SUCCESS && ObservedReports==4);Expected(PbEffectiveMask(&Ctx)==lso && EncodedLso(TRUE));
    CHECK(Policy(0)==STATUS_SUCCESS && ObservedReports==5);Expected(!PbEffectiveMask(&Ctx) && EncodedLso(FALSE));
    MemoryFree(p);End();printf("OID_LSO ip%u ndis%lu independent-checksums checked owners=0\n",PB_TEST_IP_VERSION,minor);
}
static ULONG ChecksumBits(ULONG subset)
{
#if PB_TEST_IP_VERSION == 4
    return (subset&1?PB_TX_IP4_CHECKSUM:0)|(subset&2?PB_TX_TCP4_CHECKSUM:0)|(subset&4?PB_TX_UDP4_CHECKSUM:0);
#else
    return (subset&1?PB_TX_TCP6_CHECKSUM:0)|(subset&2?PB_TX_UDP6_CHECKSUM:0);
#endif
}
static VOID GateMatrix(VOID)
{
    ULONG lso=LsoBit();const ULONG subsets=PB_TEST_IP_VERSION==4?8:4;
    for(ULONG active=0;active<2;++active)for(ULONG gates=0;gates<8;++gates)
        for(ULONG a=0;a<subsets;++a)for(ULONG r=0;r<subsets;++r)for(ULONG e=0;e<subsets;++e){
            ULONG allowed=ChecksumBits(a)|(gates&1?lso:0),requested=ChecksumBits(r)|(gates&2?lso:0),encap=ChecksumBits(e)|(gates&4?lso:0);
            ULONG result=PbResolveOffloads(allowed,requested,encap,active);
            Expected(!!(result&lso)==(active && gates==7));
            CHECK(!(result&~(allowed&requested&encap&PB_PROBE_SUPPORTED)));
        }
    /* Existing receive coalescing validation prerequisites are unchanged. */
    CHECK(!(PbResolveOffloads(PB_RX_RSC4|PB_RX_RSC6|PB_RX_URO,PB_PROBE_SUPPORTED,PB_PROBE_SUPPORTED,1)&(PB_RX_RSC4|PB_RX_RSC6|PB_RX_URO)));
}
int main(VOID)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    const ULONG versions[]={20,30,83,89};for(ULONG i=0;i<_countof(versions);++i)OidChecksum(versions[i]);GateMatrix();
    CHECK(!Handles && !Memory && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("LSO_POLICY ip%u checks%lu violations%lu handles%lu/%lu owners=0\n",PB_TEST_IP_VERSION,PolicyCases,PolicyViolations,before,after);
    if(PolicyViolations){puts("LSO_CHECKSUM_CONFIG_DEPENDENCY: actual OID disables checksums while LSO remains requested; current resolver incorrectly removes LSO; all contexts/events reclaimed before140");return 140;}
    puts("PASS: LSO policy follows its own session/allowed/requested/encapsulation gates independently of TX checksum configuration");return 0;
}
