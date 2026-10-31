/* SPDX-License-Identifier: GPL-2.0 */
/* Shared writer inputs; current SYS only constructs plans and does not call a writer. */
#include "guard_memory.h"
#include "../../common/packet.h"
#include <string.h>
static void Source(uint8_t bytes[64])
{
    memset(bytes,0,64);bytes[0]=0x45;bytes[9]=6;bytes[32]=0x50;
}
static PB_SEGMENT_PLAN Valid(void)
{
    uint8_t bytes[64];Source(bytes);PB_SEGMENT_PLAN p;
    SAFETY_CHECK(PbPlanSegments(bytes,64,PB_SEGMENT_TCP,12,&p)==PB_PACKET_OK);return p;
}
static void RejectSpan(PB_SEGMENT_PLAN p,uint32_t index,const uint8_t *bytes,uint32_t sourceLength)
{
    PB_TEST_GUARD src=PbTestGuardCreate(sourceLength),dst=PbTestGuardCreate(64),tail=PbTestGuardCreate(64);
    uint8_t *s=PbTestGuardSpan(&src,sourceLength),*d=PbTestGuardSpan(&dst,64),*e=PbTestGuardSpan(&tail,64);
    if(sourceLength) memcpy(s,bytes,sourceLength);memset(d,0xa5,64);memset(e,0xa5,64);
    uint32_t written=0xdeadbeefu; PB_PACKET_STATUS result=PB_PACKET_OK; BOOLEAN exception=FALSE;
    __try { result=PbWriteSegmentChecked(s,sourceLength,&p,index,d,64,&written); }
    __except(EXCEPTION_EXECUTE_HANDLER) { exception=TRUE; }
    if(exception) fputs("guard-page exception for malformed plan\n",stderr);
    SAFETY_CHECK(!exception && result==PB_PACKET_INVALID && !written);
    for(unsigned i=0;i<64;++i) SAFETY_CHECK(d[i]==0xa5);
    PB_SEGMENT_OUTPUT outputs[2]={{d,64,0xdeadbeefu},{e,64,0xdeadbeefu}};written=0xdeadbeefu;
    __try { result=PbWriteSegmentBatchChecked(s,sourceLength,&p,index,outputs,2,&written); }
    __except(EXCEPTION_EXECUTE_HANDLER) { exception=TRUE; }
    SAFETY_CHECK(!exception && result==PB_PACKET_INVALID && !written);
    SAFETY_CHECK(outputs[0].Length==0xdeadbeefu && outputs[1].Length==0xdeadbeefu);
    for(unsigned i=0;i<64;++i) SAFETY_CHECK(d[i]==0xa5 && e[i]==0xa5);
    PbTestGuardDestroy(&src);PbTestGuardDestroy(&dst);PbTestGuardDestroy(&tail);
}
static void Reject(PB_SEGMENT_PLAN p,uint32_t index)
{
    uint8_t bytes[64];Source(bytes);RejectSpan(p,index,bytes,64);
}
int main(int argc,char **argv)
{
    PB_SEGMENT_PLAN p=Valid();
    if(argc==2 && !strcmp(argv[1],"--offset")) { p.TransportOffset=65535; Reject(p,0); return 0; }
    p.Mss=0x80000000u; p.SegmentCount=3; Reject(p,2); /* 32-bit index*MSS used to wrap to zero. */
    p=Valid(); p.TransportOffset=65535; Reject(p,0);
    p=Valid(); p.PseudoDestinationOffset=65535; Reject(p,0);
    p=Valid(); p.HeaderLength=0; p.PayloadLength=64; Reject(p,0);
    p=Valid(); p.SourceLength=UINT32_MAX; p.PayloadLength=p.SourceLength-p.HeaderLength; Reject(p,0);
    p=Valid(); p.Type=255; Reject(p,0);
    p=Valid(); p.Protocol=255; Reject(p,0);
    p=Valid(); p.IpVersion=0; Reject(p,0);
    p=Valid(); p.TransportHeaderLength=60; Reject(p,0);
    p=Valid(); p.SegmentCount=UINT32_MAX; Reject(p,0);
    /* Self-consistent metadata formerly passed shape-only validation. */
    p=Valid();p.SourceLength=76;p.PayloadLength=36;p.SegmentCount=3;Reject(p,2);
    p=Valid();p.PseudoSourceOffset=0;Reject(p,0);
    p=Valid();p.PseudoDestinationOffset=12;Reject(p,0);
    p=Valid();p.TransportOffset=24;p.HeaderLength=44;p.PayloadLength=20;p.Mss=10;Reject(p,0);
    p=Valid();p.IpVersion=6;p.TransportOffset=40;p.HeaderLength=60;p.PayloadLength=4;p.Mss=2;
    p.PseudoSourceOffset=8;p.PseudoDestinationOffset=24;Reject(p,0);
    p=Valid();p.Protocol=17;p.Type=PB_SEGMENT_UDP;p.TransportHeaderLength=8;p.HeaderLength=28;
    p.PayloadLength=36;p.SegmentCount=3;Reject(p,0);
    uint8_t source[64];Source(source);
    p=Valid();source[33]=2;RejectSpan(p,0,source,64); /* Packet changed to an unsupported SYN. */
    Source(source);p=Valid();source[6]=0x20;RejectSpan(p,0,source,64);
    Source(source);p=Valid();source[0]=0x46;RejectSpan(p,0,source,64);
    Source(source);p=Valid();source[32]=0x60;RejectSpan(p,0,source,64);
    Source(source);
    for(uint32_t length=0;length<64;++length) RejectSpan(Valid(),0,source,length);
    /* Positive control proves rejection does not rely on an invalid seed packet. */
    uint8_t output[64];uint32_t written; p=Valid();
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&p,1,output,64,&written)==PB_PACKET_OK && written==52);
    PB_SEGMENT_OUTPUT outputs[1]={{output,64,0}};
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&p,1,outputs,1,&written)==PB_PACKET_OK && written==1 && outputs[0].Length==52);
    PB_SEGMENT_PLAN invalid;
    SAFETY_CHECK(PbPlanSegments(source,sizeof(source),(PB_SEGMENT_TYPE)-1,12,&invalid)==PB_PACKET_INVALID);
    puts("PASS: 20 malformed/stale plan cases and 64 truncated spans reject before writes in single/batch APIs; guarded buffers and valid positive controls.");
    return 0;
}
