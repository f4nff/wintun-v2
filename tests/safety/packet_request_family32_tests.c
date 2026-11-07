/* SPDX-License-Identifier: GPL-2.0 */
/* Maximum checked request geometry. Only the selected IP family is built. */
#include "guard_memory.h"
#include "../../include/packetbridge_version.h"
#include "../../common/packet.h"
#include <stdlib.h>
#include <string.h>
#ifdef PB_API_TESTS
#error This test uses the actual shared packet engine, without PB_API_TESTS
#endif
#ifndef NDEBUG
#error NDEBUG is required; SAFETY_CHECK remains active
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family with PB_TEST_IP_VERSION=4 or6
#endif
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping fingerprint required");
_Static_assert(PB_VERSION_MAJOR==2&&PB_VERSION_MINOR==0&&PB_VERSION_PATCH==0,"Version remains2.0.0");
_Static_assert(sizeof(void *)==8&&sizeof(PB_SEGMENT_OUTPUT)==16,"This hosted request fixture targets x64");
enum { N=32, MSS=64, CAP=128, PAYLOAD=N*MSS, MAX_SOURCE=40+20+PAYLOAD };
typedef struct { uint8_t *Data; SIZE_T Size; } BLOCK;
static uint32_t Blocks,Cases;
static SIZE_T OwnedBytes;
static BLOCK Allocate(SIZE_T bytes)
{
#ifdef PB_GUARD_TESTS
    uint8_t *data=PbTestGuardAlloc(bytes);
#else
    uint8_t *data=malloc(bytes);SAFETY_CHECK(data);
#endif
    ++Blocks;OwnedBytes+=bytes;memset(data,0,bytes);BLOCK block={data,bytes};return block;
}
static void Release(BLOCK *block)
{
    SAFETY_CHECK(block->Data&&Blocks&&OwnedBytes>=block->Size);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(block->Data);
#else
    free(block->Data);
#endif
    --Blocks;OwnedBytes-=block->Size;block->Data=NULL;block->Size=0;
}
static struct {
    BOOL Active;
    const uint8_t *Source;
    const PB_SEGMENT_OUTPUT *All,*Metadata;
    uint32_t Header,MetadataCount,MetadataCopies,HeaderCopies,PayloadCopies;
    uint32_t HeaderHits[N],OutputHits[N],PayloadHits[PAYLOAD];
    uint64_t MetadataBytes,HeaderBytes,PayloadBytes;
} Copy;
static void *ObservedCopy(void *destination,const void *source,size_t bytes)
{
    SAFETY_CHECK(Copy.Active);
    if(source==Copy.Metadata&&bytes==(size_t)Copy.MetadataCount*sizeof(PB_SEGMENT_OUTPUT)){
        ++Copy.MetadataCopies;Copy.MetadataBytes+=bytes;
    }else{
        BOOL found=FALSE;
        for(uint32_t i=0;i<N;++i){
            if(source==Copy.Source&&bytes==Copy.Header&&destination==Copy.All[i].Data){
                ++Copy.HeaderCopies;++Copy.HeaderHits[i];Copy.HeaderBytes+=bytes;found=TRUE;break;
            }
            if(source==Copy.Source+Copy.Header+i*MSS&&bytes==MSS&&destination==Copy.All[i].Data+Copy.Header){
                ++Copy.PayloadCopies;++Copy.OutputHits[i];Copy.PayloadBytes+=bytes;
                for(uint32_t j=0;j<MSS;++j)++Copy.PayloadHits[i*MSS+j];found=TRUE;break;
            }
        }
        SAFETY_CHECK(found);
    }
    return memcpy(destination,source,bytes);
}
#ifndef PB_REQUEST_PACKET_SOURCE
#define PB_REQUEST_PACKET_SOURCE "../../common/packet.c"
#endif
/* Only explicit production packet.c memcpy call sites are counted. Compiler
 * lowering, setup/oracle copies, and CPU/runtime performance are not counted. */
#define memcpy ObservedCopy
#include PB_REQUEST_PACKET_SOURCE
#undef memcpy

static void Put16(uint8_t *p,uint32_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void Put32(uint8_t *p,uint32_t value){for(uint32_t i=0;i<4;++i)p[i]=(uint8_t)(value>>(24-8*i));}
static uint32_t Sum(const uint8_t *p,uint32_t length,uint32_t sum)
{while(length>1){sum+=((uint32_t)p[0]<<8)|p[1];p+=2;length-=2;}if(length)sum+=(uint32_t)*p<<8;return sum;}
static uint16_t Finish(uint32_t sum){while(sum>>16)sum=(sum&65535)+(sum>>16);return (uint16_t)~sum;}
static uint32_t Pseudo(const uint8_t *p,uint32_t length,uint32_t protocol)
{return Sum(p+(PB_TEST_IP_VERSION==4?12:8),PB_TEST_IP_VERSION==4?8:32,protocol+length);}
static void Checksums(uint8_t *p,uint32_t length,uint32_t ip,uint32_t protocol)
{
#if PB_TEST_IP_VERSION == 4
    Put16(p+10,0);Put16(p+10,Finish(Sum(p,ip,0)));
#endif
    uint32_t at=ip+(protocol==6?16:6);Put16(p+at,0);
    uint16_t value=Finish(Sum(p+ip,length-ip,Pseudo(p,length-ip,protocol)));
    Put16(p+at,protocol==17&&!value?65535:value);
}
static void Seed(uint8_t *p,uint32_t length,uint32_t ip,uint32_t protocol)
{
    memset(p,0,length);
#if PB_TEST_IP_VERSION == 4
    p[0]=0x45;Put16(p+2,length);Put16(p+4,0xfffe);Put16(p+6,0x4000);p[8]=64;p[9]=(uint8_t)protocol;p[12]=p[16]=10;p[15]=1;p[19]=2;
#else
    p[0]=0x60;Put16(p+4,length-40);p[6]=(uint8_t)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=1;p[39]=2;
#endif
    uint8_t *l4=p+ip;Put16(l4,10001);Put16(l4+2,10002);
    uint32_t header=ip+(protocol==6?20:8);
    if(protocol==6){Put32(l4+4,0xfffffff0);Put32(l4+8,42);l4[12]=0x50;l4[13]=0x19;Put16(l4+14,8192);}
    else Put16(l4+4,length-ip);
    for(uint32_t i=0;i<PAYLOAD;++i)p[header+i]=(uint8_t)((i/MSS)*17+i*3);
    Checksums(p,length,ip,protocol);
}
static void Expected(uint8_t *p,const uint8_t *source,uint32_t index,uint32_t ip,uint32_t header,uint32_t protocol)
{
    memcpy(p,source,header);memcpy(p+header,source+header+index*MSS,MSS);
    uint32_t length=header+MSS;
#if PB_TEST_IP_VERSION == 4
    Put16(p+2,length);Put16(p+4,(0xfffe+index)&(protocol==6?0x7fff:0xffff));
#else
    Put16(p+4,length-40);
#endif
    if(protocol==6){Put32(p+ip+4,0xfffffff0u+index*MSS);p[ip+13]=index==N-1?0x19:0x10;}
    else Put16(p+ip+4,length-ip);
    Checksums(p,length,ip,protocol);
}
static void CheckOutputs(const PB_SEGMENT_OUTPUT *items,const uint8_t *source,uint32_t count,uint32_t ip,uint32_t header,uint32_t protocol)
{
    for(uint32_t i=0;i<count;++i){
        uint8_t expected[CAP];Expected(expected,source,i,ip,header,protocol);
        SAFETY_CHECK(items[i].Length==header+MSS&&!memcmp(items[i].Data,expected,header+MSS));
        for(uint32_t j=header+MSS;j<CAP;++j)SAFETY_CHECK(items[i].Data[j]==0xa5);
        SAFETY_CHECK(!Finish(Sum(items[i].Data+ip,header+MSS-ip,Pseudo(items[i].Data,header+MSS-ip,protocol))));
#if PB_TEST_IP_VERSION == 4
        SAFETY_CHECK(!Finish(Sum(items[i].Data,ip,0)));
#endif
    }
}
static PB_PACKET_STATUS Invoke(const uint8_t *source,uint32_t length,const PB_SEGMENT_PLAN *plan,uint32_t first,PB_SEGMENT_OUTPUT *items,uint32_t count,uint32_t *written)
{
    SAFETY_CHECK(!Copy.Active);Copy.Metadata=items;Copy.MetadataCount=count;Copy.Active=TRUE;
    PB_PACKET_STATUS result=PbWriteSegmentBatchChecked(source,length,plan,first,items,count,written);
    Copy.Active=FALSE;return result;
}
static const char *Shapes[]={"ascending","descending","interleaved","overlap31","short31","short0"};
static void Case(uint32_t protocol,uint32_t shape,const char *phase)
{
    SAFETY_CHECK(!Blocks&&!OwnedBytes&&!Copy.Active&&shape<_countof(Shapes));
    uint32_t ip=PB_TEST_IP_VERSION==4?20:40,header=ip+(protocol==6?20:8),length=header+PAYLOAD;
    BLOCK source=Allocate(length),output=Allocate(N*CAP),control=Allocate(sizeof(PB_SEGMENT_PLAN)),metadata=Allocate(N*sizeof(PB_SEGMENT_OUTPUT)),countBlock=Allocate(sizeof(uint32_t));
    PB_SEGMENT_PLAN *plan=(void *)control.Data;PB_SEGMENT_OUTPUT *items=(void *)metadata.Data;uint32_t *written=(void *)countBlock.Data;
    Seed(source.Data,length,ip,protocol);uint8_t sourceBefore[MAX_SOURCE];memcpy(sourceBefore,source.Data,length);
    SAFETY_CHECK(PbPlanSegments(source.Data,length,protocol==6?PB_SEGMENT_TCP:PB_SEGMENT_UDP,MSS,plan)==PB_PACKET_OK);
    SAFETY_CHECK(plan->SegmentCount==N&&plan->HeaderLength==header&&plan->TransportOffset==ip&&plan->PayloadLength==PAYLOAD);
    PB_SEGMENT_PLAN planBefore=*plan;memset(output.Data,0xa5,output.Size);
    for(uint32_t i=0;i<N;++i){uint32_t slot=shape==1?N-1-i:shape==2?(i%2?i/2:i/2+N/2):i;items[i]=(PB_SEGMENT_OUTPUT){output.Data+slot*CAP,CAP,UINT32_MAX};}
    if(shape==3)items[N-1].Data=items[0].Data+CAP/2;
    uint32_t prefix=shape==4?N-1:0;if(shape>=4)items[prefix].Capacity=0;
    PB_SEGMENT_OUTPUT itemsBefore[N];memcpy(itemsBefore,items,sizeof(itemsBefore));
    memset(&Copy,0,sizeof(Copy));Copy.Source=source.Data;Copy.All=items;Copy.Header=header;
    *written=UINT32_MAX;PB_PACKET_STATUS result=Invoke(source.Data,length,plan,0,items,N,written);uint32_t accepted=*written,calls=1;
    SAFETY_CHECK(!memcmp(source.Data,sourceBefore,length)&&!memcmp(plan,&planBefore,sizeof(*plan)));
    if(shape==3){
        SAFETY_CHECK(result==PB_PACKET_INVALID&&accepted==UINT32_MAX&&!memcmp(items,itemsBefore,sizeof(itemsBefore)));
        for(uint32_t i=0;i<N*CAP;++i)SAFETY_CHECK(output.Data[i]==0xa5);
    }else{
        SAFETY_CHECK(result==(shape>=4?PB_PACKET_NO_SPACE:PB_PACKET_OK)&&accepted==(shape>=4?prefix:N));
        CheckOutputs(items,sourceBefore,accepted,ip,header,protocol);
        for(uint32_t i=0;i<N;++i){SAFETY_CHECK(items[i].Data==itemsBefore[i].Data&&items[i].Capacity==itemsBefore[i].Capacity);if(i>=accepted){SAFETY_CHECK(items[i].Length==UINT32_MAX);for(uint32_t j=0;j<CAP;++j)SAFETY_CHECK(items[i].Data[j]==0xa5);}}
        if(shape>=4){
            uint8_t beforeRetry[N*CAP];memcpy(beforeRetry,output.Data,sizeof(beforeRetry));
            items[prefix].Capacity=CAP;PB_PACKET_STATUS retry=Invoke(source.Data,length,plan,prefix,items+prefix,N-prefix,written);++calls;
            SAFETY_CHECK(retry==PB_PACKET_OK&&*written==N-prefix&&!memcmp(output.Data,beforeRetry,(size_t)prefix*CAP));
            SAFETY_CHECK(!memcmp(source.Data,sourceBefore,length)&&!memcmp(plan,&planBefore,sizeof(*plan)));
            for(uint32_t i=0;i<N;++i)SAFETY_CHECK(items[i].Data==itemsBefore[i].Data&&items[i].Capacity==CAP);
        }
        CheckOutputs(items,sourceBefore,N,ip,header,protocol);
    }
    uint32_t expected=shape==3?0:N;
    SAFETY_CHECK(Copy.MetadataCopies==calls&&Copy.HeaderCopies==expected&&Copy.PayloadCopies==expected&&Copy.PayloadBytes==(uint64_t)expected*MSS&&Copy.HeaderBytes==(uint64_t)expected*header);
    SAFETY_CHECK(Copy.MetadataBytes==(uint64_t)(N+(shape>=4?N-prefix:0))*sizeof(PB_SEGMENT_OUTPUT));
    for(uint32_t i=0;i<N;++i)SAFETY_CHECK(Copy.HeaderHits[i]==(expected?1u:0u)&&Copy.OutputHits[i]==(expected?1u:0u));
    for(uint32_t i=0;i<PAYLOAD;++i)SAFETY_CHECK(Copy.PayloadHits[i]==(expected?1u:0u));
    Release(&countBlock);Release(&metadata);Release(&control);Release(&output);Release(&source);SAFETY_CHECK(!Blocks&&!OwnedBytes);
    printf("REQUEST32 phase=%s ip=%u protocol=%u shape=%s calls=%u first-status=%u first-written=%u metadata=%u metadata-bytes=%llu headers=%u payloads=%u payload-bytes=%llu byte-hits=%u source-plan-sentinels=preserved owners=0\n",phase,PB_TEST_IP_VERSION,protocol,Shapes[shape],calls,(unsigned)result,accepted,Copy.MetadataCopies,(unsigned long long)Copy.MetadataBytes,Copy.HeaderCopies,Copy.PayloadCopies,(unsigned long long)Copy.PayloadBytes,expected?1:0);++Cases;
}
static void Matrix(const char *phase){for(uint32_t protocol=6;protocol<=17;protocol+=11)for(uint32_t shape=0;shape<_countof(Shapes);++shape)Case(protocol,shape,phase);}
int main(void)
{
    printf("FAMILY: IPv%d\n",PB_TEST_IP_VERSION);
    __try {
        Matrix("warm");uint32_t warm=Cases;DWORD before,after;SAFETY_CHECK(warm==12&&GetProcessHandleCount(GetCurrentProcess(),&before));Matrix("measure");
        SAFETY_CHECK(Cases==2*warm&&!Blocks&&!OwnedBytes&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        printf("PASS: %u packet request geometry cases (%u warm/%u measured), IPv%d, exact32 maximum disjoint/overlap/prefix calls, explicit production memcpy ownership, zero caller allocations; handles%lu/%lu.\n",Cases,warm,Cases-warm,PB_TEST_IP_VERSION,before,after);
    }__except(EXCEPTION_EXECUTE_HANDLER){fprintf(stderr,"unexpected request geometry fault:%08lx\n",GetExceptionCode());return 1;}
    return 0;
}
