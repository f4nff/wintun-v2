/* SPDX-License-Identifier: GPL-2.0 */
/* Exact raw-sum contract, with an independent wide accumulator and guard pages.
 * This verifies modular uint32 behavior, not only equivalent folded checksums. */
#include "../safety/guard_memory.h"
#include "../../common/packet.h"
#include <stdlib.h>
#define CHECK SAFETY_CHECK
static ULONG Comparisons;
static const uint32_t Seeds[]={0,1,0xffff,0x7fff0000,0xffff0000,0xfffffffe,UINT32_MAX};
static uint32_t Oracle(uint32_t seed,const uint8_t *bytes,uint32_t length)
{
    uint64_t wide=seed;
    for(uint32_t at=0;at<length;++at) wide+=(uint64_t)bytes[at]<<((at&1)?0:8);
    return (uint32_t)wide;
}
static VOID Fill(PB_TEST_GUARD *memory,ULONG pattern)
{
    uint32_t random=0x31571309;uint8_t *bytes=memory->End-memory->Capacity;
    for(SIZE_T i=0;i<memory->Capacity;++i) {
        random=random*1664525u+1013904223u;
        bytes[i]=pattern==0?0:pattern==1?0xff:pattern==2?(i&1?0x55:0xaa):(uint8_t)(random>>24);
    }
}
static VOID Compare(uint32_t seed,const uint8_t *bytes,uint32_t length)
{
    uint32_t expected=Oracle(seed,bytes,length),actual=PbChecksumAdd(seed,bytes,length);
    CHECK(actual==expected);
    uint32_t folded=(expected&0xffffu)+(expected>>16);folded=(folded&0xffffu)+(folded>>16);
    CHECK(PbChecksumFinish(actual)==(uint16_t)~folded);Comparisons++;
}
static VOID ExhaustiveShort(VOID)
{
    PB_TEST_GUARD memory=PbTestGuardCreate(1024);
    for(ULONG pattern=0;pattern<4;++pattern) {
        Fill(&memory,pattern);DWORD old;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,PAGE_READONLY,&old));
        for(ULONG length=0;length<=1024;++length) for(ULONG seed=0;seed<sizeof(Seeds)/sizeof(Seeds[0]);++seed)
            Compare(Seeds[seed],PbTestGuardSpan(&memory,length),length);
        DWORD ignored;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,old,&ignored));
    }
    PbTestGuardDestroy(&memory);
    puts("PASS: every length 0..1024 ends at a no-access page; all alignments, four byte patterns and seven seeds preserve exact raw/folded results without source writes.");
}
static VOID LargeAndAlignment(VOID)
{
    const ULONG sizes[]={1499,1500,1501,2047,2048,2049,32767,32768,32769,65533,65534,65535,65536,65537,131071,131072,131073,1048576};
    PB_TEST_GUARD memory=PbTestGuardCreate(1048576+16);
    for(ULONG pattern=0;pattern<4;++pattern) {
        Fill(&memory,pattern);DWORD old;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,PAGE_READONLY,&old));
        for(ULONG si=0;si<sizeof(sizes)/sizeof(sizes[0]);++si) for(ULONG padding=0;padding<16;++padding) {
            ULONG length=sizes[si];const uint8_t *p=PbTestGuardSpan(&memory,length+padding);
            for(ULONG seed=0;seed<sizeof(Seeds)/sizeof(Seeds[0]);++seed) Compare(Seeds[seed],p,length);
        }
        /* Starting exactly after a no-access page also excludes underreads. */
        Compare(UINT32_MAX,memory.End-memory.Capacity,(uint32_t)memory.Capacity);
        DWORD ignored;CHECK(VirtualProtect(memory.End-memory.Capacity,memory.Capacity,old,&ignored));
    }
    PbTestGuardDestroy(&memory);
    puts("PASS: MTU/32KiB/64KiB/128KiB boundaries through 1MiB, 16 alignments and seeded overflow match a uint64 byte oracle; guarded head and read-only input.");
}
static VOID EvenChunking(VOID)
{
    PB_TEST_GUARD memory=PbTestGuardCreate(65535);Fill(&memory,3);
    const uint8_t *p=PbTestGuardSpan(&memory,65535);const ULONG chunks[]={2,8,16,64,1500,4096};
    for(ULONG ci=0;ci<sizeof(chunks)/sizeof(chunks[0]);++ci) for(ULONG seed=0;seed<sizeof(Seeds)/sizeof(Seeds[0]);++seed) {
        uint32_t sum=Seeds[seed];
        for(ULONG at=0;at<65535;) {ULONG n=min(chunks[ci],65535-at);sum=PbChecksumAdd(sum,p+at,n);at+=n;}
        CHECK(sum==Oracle(Seeds[seed],p,65535));Comparisons++;
    }
    for(ULONG seed=0;seed<sizeof(Seeds)/sizeof(Seeds[0]);++seed) CHECK(PbChecksumAdd(Seeds[seed],NULL,0)==Seeds[seed]);
    PbTestGuardDestroy(&memory);puts("PASS: even-boundary chained additions retain seed/odd-final semantics; null zero-length input is untouched.");
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    ExhaustiveShort();LargeAndAlignment();EvenChunking();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("PASS: checksum comparisons=%lu, exact uint32 modular results, balanced guard mappings and handles.\n",Comparisons);return 0;
}
