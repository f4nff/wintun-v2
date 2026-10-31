/* SPDX-License-Identifier: GPL-2.0 */
/* Request geometry and retired ABI safety, against production shared functions. */
#include "guard_memory.h"
#include "../../common/packet.h"
#include <string.h>
static PB_SEGMENT_PLAN Seed(uint8_t bytes[64])
{
    memset(bytes,0,64);bytes[0]=0x45;bytes[9]=6;bytes[32]=0x50;
    PB_SEGMENT_PLAN plan;SAFETY_CHECK(PbPlanSegments(bytes,64,PB_SEGMENT_TCP,12,&plan)==PB_PACKET_OK);return plan;
}
static void Retired(void)
{
    PB_TEST_GUARD guard=PbTestGuardCreate(1);void *inaccessible=PbTestGuardSpan(&guard,0);
    uint32_t written=UINT32_MAX;
    SAFETY_CHECK(PbWriteSegment(inaccessible,inaccessible,UINT32_MAX,inaccessible,UINT32_MAX,&written)==PB_PACKET_UNSUPPORTED && !written);
    written=UINT32_MAX;
    SAFETY_CHECK(PbWriteSegmentBatch(inaccessible,inaccessible,UINT32_MAX,inaccessible,UINT32_MAX,&written)==PB_PACKET_UNSUPPORTED && !written);
    SAFETY_CHECK(PbWriteSegment(NULL,NULL,0,NULL,0,NULL)==PB_PACKET_UNSUPPORTED);
    SAFETY_CHECK(PbWriteSegmentBatch(NULL,NULL,0,NULL,0,NULL)==PB_PACKET_UNSUPPORTED);
    PbTestGuardDestroy(&guard);
    puts("PASS: retired writers reject inaccessible source/plan/output metadata without reads; no compatibility bypass.");
}
static void AliasesAndRanges(void)
{
    PB_TEST_GUARD input=PbTestGuardCreate(64),output=PbTestGuardCreate(128);
    uint8_t *source=PbTestGuardSpan(&input,64),*destination=PbTestGuardSpan(&output,128);
    PB_SEGMENT_PLAN plan=Seed(source);uint8_t original[64];memcpy(original,source,64);
    memset(destination,0xa5,128);uint32_t written=UINT32_MAX;
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,source,64,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(!memcmp(source,original,64));
    PB_SEGMENT_PLAN previous=plan;
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,(uint8_t *)&plan,sizeof(plan),&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(!memcmp(&plan,&previous,sizeof(plan)));
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,(void *)source,0,destination,64,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,destination,64,(void *)source)==PB_PACKET_INVALID);
    SAFETY_CHECK(!memcmp(source,original,64));
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,destination,64,&plan.SourceLength)==PB_PACKET_INVALID);
    SAFETY_CHECK(!memcmp(&plan,&previous,sizeof(plan)));
    union { uint64_t Align; uint8_t Bytes[256]; PB_SEGMENT_OUTPUT Items[16]; } storage;
    memset(&storage,0xa5,sizeof(storage));
    PB_SEGMENT_OUTPUT *items=storage.Items;items[0]=(PB_SEGMENT_OUTPUT){storage.Bytes,64,UINT32_MAX};
    items[1]=(PB_SEGMENT_OUTPUT){destination,64,UINT32_MAX};
    uint8_t snapshot[sizeof(storage)];memcpy(snapshot,&storage,sizeof(storage));
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(!memcmp(snapshot,&storage,sizeof(storage)));
    items[0]=(PB_SEGMENT_OUTPUT){destination,64,UINT32_MAX};items[1]=(PB_SEGMENT_OUTPUT){destination+32,64,UINT32_MAX};
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    items[1].Data=destination+64;
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&items[1].Length)==PB_PACKET_INVALID && items[1].Length==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,destination,64,(void *)destination)==PB_PACKET_INVALID);
    for(uint32_t i=0;i<128;++i) SAFETY_CHECK(destination[i]==0xa5);
    uint8_t *overflow=(uint8_t *)(UINTPTR_MAX-15u);
    SAFETY_CHECK(PbWriteSegmentChecked(overflow,64,&plan,0,destination,64,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,(void *)overflow,0,destination,64,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,overflow,64,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,(void *)overflow,2,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,destination,64,(void *)(UINTPTR_MAX-1u))==PB_PACKET_INVALID);
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,33,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    /* Adjacent capacities are disjoint. A later zero capacity preserves a prefix. */
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&written)==PB_PACKET_OK && written==2);
    SAFETY_CHECK(items[0].Length==52 && items[1].Length==52);
    items[0].Data=destination+64;items[1].Data=destination;
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&written)==PB_PACKET_OK && written==2);
    items[0].Data=destination;items[0].Capacity=128;
    items[1].Data=destination+32;items[1].Capacity=0;
    items[2]=(PB_SEGMENT_OUTPUT){destination+64,64,UINT32_MAX};written=UINT32_MAX;
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,3,&written)==PB_PACKET_INVALID && written==UINT32_MAX);
    items[0].Capacity=64;items[1].Data=destination+64;
    items[0].Length=items[1].Length=UINT32_MAX;items[1].Capacity=0;
    SAFETY_CHECK(PbWriteSegmentBatchChecked(source,64,&plan,0,items,2,&written)==PB_PACKET_NO_SPACE && written==1);
    SAFETY_CHECK(items[0].Length==52 && items[1].Length==UINT32_MAX);
    SAFETY_CHECK(PbWriteSegmentChecked(source,64,&plan,0,destination,0,&written)==PB_PACKET_NO_SPACE && !written);
    PbTestGuardDestroy(&input);PbTestGuardDestroy(&output);
    puts("PASS: source/plan/metadata/count/output aliases, pointer-range overflow, adjacent/reversed capacities, empty interval containment and partial completion.");
}
int main(void)
{
    __try { Retired();AliasesAndRanges(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr,"unexpected packet request fault: %08lx\n",GetExceptionCode());return 1; }
    return 0;
}
