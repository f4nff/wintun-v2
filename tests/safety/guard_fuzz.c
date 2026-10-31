/* SPDX-License-Identifier: GPL-2.0 */
/* Deterministic boundary/mutation coverage with inaccessible trailing pages, not kernel execution. */
#include "guard_memory.h"
#include "../../common/packet.h"
#include "../../common/coalesce.h"
#include "../../common/ethernet.h"
#include "../../common/v2_core.h"
#include <string.h>
#include <stdlib.h>
static uint32_t RandomState=0x1f5a2026u;
static volatile ULONG CurrentCase;
static uint32_t Next(void) { RandomState^=RandomState<<13; RandomState^=RandomState>>17; RandomState^=RandomState<<5; return RandomState; }
static void W16(uint8_t *p,uint32_t x) { p[0]=(uint8_t)(x>>8); p[1]=(uint8_t)x; }
static void W32(uint8_t *p,uint32_t x) { W16(p,x>>16); W16(p+2,x); }
static uint32_t Seed(uint8_t *p,uint32_t ip,uint32_t protocol,uint32_t payload)
{
    uint32_t header=ip==4?20u:40u,transport=protocol==6?20u:8u,length=header+transport+payload;
    memset(p,0,length); p[0]=(uint8_t)(ip==4?0x45:0x60);
    if(ip==4) { W16(p+2,length); p[6]=0x40; p[8]=64; p[9]=(uint8_t)protocol; p[12]=10;p[15]=1;p[16]=10;p[19]=2; }
    else { W16(p+4,length-40);p[6]=(uint8_t)protocol;p[7]=64;p[8]=0x20;p[9]=1;p[23]=1;p[24]=0x20;p[25]=1;p[39]=2; }
    W16(p+header,10001); W16(p+header+2,10002);
    if(protocol==6) { W32(p+header+4,0xfffffff0u);p[header+12]=0x50;p[header+13]=0x10;W16(p+header+14,32768); }
    else W16(p+header+4,transport+payload);
    for(uint32_t i=header+transport;i<length;++i) p[i]=(uint8_t)Next();
    SAFETY_CHECK(PbFinalizeChecksums(p,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
    return length;
}
static void PacketCases(void)
{
    PB_TEST_GUARD input=PbTestGuardCreate(65536),output=PbTestGuardCreate(65536),coalesced=PbTestGuardCreate(65536),control=PbTestGuardCreate(128);
    const uint32_t edges[]={0,1,19,20,39,40,63,64,127,128,129,65535,65536,UINT32_MAX};
    uint8_t scratch[4096],peer[6]={2,0,0,0,0,1}; ULONG validPlans=0,coalesces=0,mutatedPlans=0;
    for(ULONG i=0;i<100000;++i) {
        CurrentCase=i; uint32_t length;
        if((i&3)<2) {
            length=Seed(scratch,Next()&1?4:6,Next()&1?6:17,Next()%2048);
            if(i&1) for(uint32_t n=0;n<1+Next()%8;++n) scratch[Next()%length]^=(uint8_t)(1u<<(Next()&7));
        } else { length=i&1?edges[Next()%_countof(edges)]:Next()%256; }
        uint32_t actual=length<=65536?length:64; uint8_t *bytes=PbTestGuardSpan(&input,actual);
        if((i&3)<2) memcpy(bytes,scratch,length); else for(uint32_t j=0;j<actual;++j) bytes[j]=(uint8_t)Next();
        PB_SEGMENT_PLAN p; PB_SEGMENT_TYPE type=(PB_SEGMENT_TYPE)(Next()%5);
        PB_PACKET_STATUS status=PbPlanSegments(bytes,length,type,1+Next()%65535,&p);
        if(status==PB_PACKET_OK) {
            validPlans++; uint32_t capacity=Next()%65536,written; uint8_t *destination=PbTestGuardSpan(&output,capacity);
            uint32_t index=Next()%p.SegmentCount;
            PB_PACKET_STATUS write=PbWriteSegmentChecked(bytes,length,&p,index,destination,capacity,&written);
            SAFETY_CHECK(write==PB_PACKET_OK || write==PB_PACKET_NO_SPACE);
            if(write==PB_PACKET_OK) SAFETY_CHECK(written<=capacity && written<=65535);
            PB_SEGMENT_PLAN bad=p;((uint8_t *)&bad)[Next()%sizeof(bad)]^=(uint8_t)(1u<<(Next()&7));
            memset(destination,0xa5,capacity);written=UINT32_MAX;
            write=PbWriteSegmentChecked(bytes,length,&bad,index,destination,capacity,&written);
            SAFETY_CHECK(write==PB_PACKET_OK || write==PB_PACKET_INVALID || write==PB_PACKET_NO_SPACE);
            if(write==PB_PACKET_OK) SAFETY_CHECK(written<=capacity && written<=65535);
            else { SAFETY_CHECK(!written);for(uint32_t n=0;n<capacity;++n) SAFETY_CHECK(destination[n]==0xa5); }
            memset(destination,0xa5,capacity);PB_SEGMENT_OUTPUT item={destination,capacity,UINT32_MAX};written=UINT32_MAX;
            write=PbWriteSegmentBatchChecked(bytes,length,&bad,index,&item,1,&written);
            SAFETY_CHECK(write==PB_PACKET_OK || write==PB_PACKET_INVALID || write==PB_PACKET_NO_SPACE);
            if(write==PB_PACKET_OK) SAFETY_CHECK(written==1 && item.Length<=capacity && item.Length<=65535);
            else { SAFETY_CHECK(!written && item.Length==UINT32_MAX);for(uint32_t n=0;n<capacity;++n) SAFETY_CHECK(destination[n]==0xa5); }
            mutatedPlans++;
        }
        uint32_t checkedOffset=UINT32_MAX;
        PB_PACKET_STATUS finalized=PbFinalizeChecksumsWithOffset(bytes,length,Next()&1,Next()&1,Next()&1,&checkedOffset);
        if(finalized==PB_PACKET_OK) SAFETY_CHECK(checkedOffset<length);
        else SAFETY_CHECK(checkedOffset==UINT32_MAX);
        uint32_t capacity=Next()%65536; PB_COALESCE s={0}; uint8_t *target=PbTestGuardSpan(&coalesced,capacity);
        if(PbCoalesceBegin(&s,target,capacity,bytes,length)==PB_PACKET_OK) {
            coalesces++; (void)PbCoalesceAppend(&s,bytes,actual); (void)PbCoalesceFinish(&s);
            SAFETY_CHECK(s.Length<=capacity && s.Count<=32);
        }
        uint32_t ipOffset,written;
        uint32_t frameBytes=actual<128?actual:128; uint8_t *frame=PbTestGuardSpan(&input,frameBytes);
        (void)PbEthernetInspect(frame,frameBytes,&ipOffset);
        (void)PbEthernetControl(frame,frameBytes,peer,PbTestGuardSpan(&control,128),&written); SAFETY_CHECK(written<=128);
    }
    SAFETY_CHECK(validPlans>1000 && coalesces>100);
    printf("PASS: 100000 guarded packet mutations/edges, valid_plans=%lu, mutated_plans=%lu (single and batch), coalesce_begins=%lu.\n",validPlans,mutatedPlans,coalesces);
    PbTestGuardDestroy(&input); PbTestGuardDestroy(&output); PbTestGuardDestroy(&coalesced); PbTestGuardDestroy(&control);
}
static void DescriptorCases(void)
{
    PB_LAYOUT layout; SAFETY_CHECK(PbCalculateLayout(1,4,4,&layout)==PB_OK);
    PB_TEST_GUARD memory=PbTestGuardCreate(4*sizeof(PB_RX_SLOT)); PB_RX_SLOT *slots=(PB_RX_SLOT *)PbTestGuardSpan(&memory,4*sizeof(PB_RX_SLOT)); memset(slots,0,4*sizeof(*slots));
    for(ULONG i=0;i<1000000;++i) {
        CurrentCase=100000+i; PB_DESCRIPTOR d; uint8_t *b=(uint8_t *)&d;
        for(uint32_t n=0;n<sizeof(d);++n) b[n]=(uint8_t)Next();
        uint32_t queue=Next()%10;
        if(!(i&3)) { memset(&d,0,sizeof(d)); d.Token=(PB_TOKEN){42,i+1,(uint16_t)(Next()%4),0};d.Length=20;d.DataOffset=layout.Queues[0].RxData+d.Token.Slot*PB_BLOCK_SIZE;queue=0; }
        uint32_t complete=0; PB_RESULT result=PbAdmitReceive(&layout,42,queue,slots,&d,0,&complete);
        if(result==PB_OK) { SAFETY_CHECK(d.Token.Slot<4 && queue==0 && d.DataOffset+d.Length<=layout.RegionSize); slots[d.Token.Slot].Active=0; }
    }
    const uint64_t addresses[]={0,1,63,64,4096,UINT64_MAX,UINT64_MAX-63,UINT64_MAX-4095};
    const uint32_t sizes[]={0,1,64,4096,PB_MAX_REGION,PB_MAX_REGION+1,UINT32_MAX};
    for(uint32_t a=0;a<_countof(addresses);++a) for(uint32_t n=0;n<_countof(sizes);++n) {
        BOOLEAN valid=addresses[a] && !(addresses[a]&63u) && sizes[n] && sizes[n]<=PB_MAX_REGION && addresses[a]<=UINT64_MAX-sizes[n];
        SAFETY_CHECK((PbValidateRegionAddress(addresses[a],sizes[n])==PB_OK)==valid);
    }
    PbTestGuardDestroy(&memory); puts("PASS: 1000000 guarded descriptor admissions and 56 region-address overflow/alignment edges.");
}
int main(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    __try { PacketCases(); DescriptorCases(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr,"guard fault case=%lu seed=%08x code=%08lx\n",CurrentCase,RandomState,GetExceptionCode()); return 1; }
    return 0;
}
