/* SPDX-License-Identifier: GPL-2.0 */
/* Actual kernel prefilter and coalescer, deterministic byte/call accounting.
 * Shared-source protocol mutation is explicitly hostile mapping injection,
 * not permission to change a transferred native/Rust loan concurrently. */
#ifdef PB_API_TESTS
#error Shipping coalesce prefilter paths required
#endif
#ifndef NDEBUG
#error Shipping coalesce prefilter configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../include/packetbridge_version.h"
#include "../../include/packetbridge_probe.h"
#define CHECK(x) SAFETY_CHECK(x)
#define NT_ASSERT CHECK
#define MAXULONG 0xffffffffu
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"coalesce prefilter requires shipping fingerprint");
typedef struct {LONG Unused;} KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct _TUN_CTX {struct {volatile LONG64 RscAborts;} Probe;} TUN_CTX;
enum {STRIDE=2048,PAD=64};
enum {ENABLED,WRONG_PROTOCOL,ZERO_MASK,BUSY,SMALL,BAD_CHECKSUM,FLOW_FOUR,INJECT_PREFIX,WRONG_FAMILY,BOUNDARY};
typedef struct BUFFER {UCHAR *Raw,*Data;SIZE_T Size;} BUFFER;
static LONG Blocks;
static ULONG CaseShape,CaseProtocol,CaseLength,Attempt,CurrentAt,Groups,Begins,Appends,Finishes,Copies,Injected;
static ULONG Cases;
static ULONG64 ScratchBytes,OutputBytes,ChecksumBytes;
static PB_KERNEL_QUEUE *Queue;
static UCHAR *Hits,*Tags,*ExpectedSource;
static BOOLEAN Counting;
static BUFFER NewBuffer(SIZE_T size)
{
    BUFFER b={0};b.Size=size;
#ifdef PB_GUARD_TESTS
    b.Raw=PbTestGuardAlloc(size+2*PAD);
#else
    b.Raw=HeapAlloc(GetProcessHeap(),0,size+2*PAD);
#endif
    CHECK(b.Raw);memset(b.Raw,0xb7,size+2*PAD);b.Data=b.Raw+PAD;Blocks++;return b;
}
static VOID CheckBuffer(const BUFFER *b)
{for(ULONG i=0;i<PAD;++i)CHECK(b->Raw[i]==0xb7&&b->Data[b->Size+i]==0xb7);}
static VOID DeleteBuffer(BUFFER *b)
{
    CheckBuffer(b);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(b->Raw);
#else
    CHECK(HeapFree(GetProcessHeap(),0,b->Raw));
#endif
    b->Raw=b->Data=NULL;CHECK(--Blocks>=0);
}
#define PbChecksumAdd PrefilterActualChecksumAdd
#include PB_PREFILTER_PACKET
#undef PbChecksumAdd
uint32_t PbChecksumAdd(uint32_t sum,const uint8_t *data,uint32_t length)
{if(Counting)ChecksumBytes+=length;return PrefilterActualChecksumAdd(sum,data,length);}
static VOID CopySnapshot(VOID *destination,const VOID *source,SIZE_T length)
{
    uintptr_t relative=(uintptr_t)source-((uintptr_t)Queue->Session->Region+Queue->Session->Layout.Queues[0].RxData);
    ULONG slot=(ULONG)(relative/PB_BLOCK_SIZE),offset=(ULONG)(relative%PB_BLOCK_SIZE);
    CHECK(slot<32&&slot>=CurrentAt&&offset<=CaseLength&&length<=CaseLength-offset&&destination==Queue->Scratch+offset);
    for(SIZE_T j=0;j<length;++j){ULONG at=slot*STRIDE+offset+(ULONG)j;CHECK(Tags[at]!=Attempt);Tags[at]=(UCHAR)Attempt;CHECK(Hits[at]<2);Hits[at]++;}
    ScratchBytes+=length;Copies++;memcpy(destination,source,length);
    if(CaseShape==INJECT_PREFIX&&!Injected&&slot==0&&!offset){
        CHECK(length==40);ULONG at=PB_TEST_IP_VERSION==4?9:6;CHECK(Queue->Scratch[at]==CaseProtocol);
        UCHAR changed=(UCHAR)(CaseProtocol==6?17:6);Queue->Session->Region[Queue->Session->Layout.Queues[0].RxData+at]=changed;ExpectedSource[at]=changed;Injected++;
    }
}
static PB_PACKET_STATUS ObserveBegin(PB_COALESCE *state,uint8_t *output,uint32_t capacity,const uint8_t *data,uint32_t length)
{Begins++;PB_PACKET_STATUS result=PbCoalesceBegin(state,output,capacity,data,length);if(result==PB_PACKET_OK)OutputBytes+=length;return result;}
static PB_PACKET_STATUS ObserveAppend(PB_COALESCE *state,const uint8_t *data,uint32_t length)
{Appends++;ULONG old=state->Length;PB_PACKET_STATUS result=PbCoalesceAppend(state,data,length);if(result==PB_PACKET_OK)OutputBytes+=state->Length-old;return result;}
static PB_PACKET_STATUS ObserveFinish(PB_COALESCE *state){Finishes++;return PbCoalesceFinish(state);}
#undef RtlCopyMemory
#define RtlCopyMemory CopySnapshot
#define PbCoalesceBegin ObserveBegin
#define PbCoalesceAppend ObserveAppend
#define PbCoalesceFinish ObserveFinish
#include PB_PREFILTER_HEADER
#undef RtlCopyMemory
#undef PbCoalesceBegin
#undef PbCoalesceAppend
#undef PbCoalesceFinish
static VOID W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID W32(UCHAR *p,ULONG n){W16(p,n>>16);W16(p+2,n);}
static ULONG OwnAdd(ULONG sum,const UCHAR *p,ULONG length)
{for(ULONG i=0;i<length;i+=2)sum+=((ULONG)p[i]<<8)|(i+1<length?p[i+1]:0);return sum;}
static USHORT OwnFinish(ULONG sum){while(sum>>16)sum=(sum&65535u)+(sum>>16);return (USHORT)~sum;}
static ULONG Pseudo(const UCHAR *p,ULONG transport,ULONG protocol)
{return OwnAdd(0,p+(PB_TEST_IP_VERSION==4?12:8),PB_TEST_IP_VERSION==4?8:32)+transport+protocol;}
static ULONG Header(ULONG protocol){return (PB_TEST_IP_VERSION==4?20:40)+(protocol==6?20:8);}
static VOID Wire(UCHAR *p,ULONG length,ULONG protocol,ULONG first,ULONG originals,BOOLEAN uro)
{
    ULONG family=PB_TEST_IP_VERSION,ip=family==4?20:40,header=Header(protocol),piece=CaseLength-header;
    memset(p,0,length);p[0]=family==4?0x45:0x60;
    if(family==4){p[1]=0x2e;W16(p+2,length);W16(p+4,(0x7ff0u+first*17u)&65535u);W16(p+6,0x4000);p[8]=(UCHAR)(protocol==6?64-((first+originals-1)%8):64);p[9]=(UCHAR)protocol;p[12]=10;p[15]=1;p[16]=192;p[18]=2;p[19]=1;}
    else{W32(p,0x62e34567);W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=(UCHAR)(protocol==6?64-((first+originals-1)%8):64);p[8]=0x20;p[9]=1;p[10]=0x0d;p[11]=0xb8;p[23]=1;p[24]=0x20;p[25]=1;p[26]=0x0d;p[27]=0xb8;p[39]=2;}
    ULONG flow=CaseShape==FLOW_FOUR?first/4:0;W16(p+ip,4000+flow);W16(p+ip+2,5000);
    if(protocol==6){W32(p+ip+4,0xfffffff0u+first*piece);W32(p+ip+8,0x80000001);p[ip+12]=0x50;p[ip+13]=(UCHAR)(first+originals==32?0x18:0x10);W16(p+ip+14,8192);}
    else W16(p+ip+4,length-ip);
    for(ULONG i=0;i<originals;++i){UCHAR *payload=p+header+i*piece;memset(payload,(int)(0x30+first+i),piece);if(piece>=20){W32(payload,first+i);W32(payload+4,0x01234567u+first+i);W32(payload+8,0x89abcdefu);W32(payload+12,0xfedcba98u);W32(payload+16,0x76543210u);}}
    if(!uro){USHORT sum=OwnFinish(OwnAdd(Pseudo(p,length-ip,protocol),p+ip,length-ip));W16(p+ip+(protocol==6?16:6),protocol==17&&!sum?65535u:sum);if(family==4)W16(p+10,OwnFinish(OwnAdd(0,p,20)));}
}
static VOID Partial(UCHAR *p,ULONG length,ULONG protocol)
{
    ULONG family=PB_TEST_IP_VERSION,ip=family==4?20:40;memset(p,0,length);if(length)p[0]=family==4?0x45:0x60;
    if(family==4){if(length>=4)W16(p+2,length);if(length>8)p[8]=64;if(length>9)p[9]=(UCHAR)protocol;}
    else{if(length>=6)W16(p+4,length>=40?length-40:0);if(length>6)p[6]=(UCHAR)protocol;if(length>7)p[7]=64;}
    if(protocol==6){if(length>ip+12)p[ip+12]=0x50;if(length>ip+13)p[ip+13]=0x10;}
    else if(length>=ip+6)W16(p+ip+4,length-ip);
    if(family==4&&length>=20&&OwnFinish(OwnAdd(0,p,20))==0)p[10]^=1;
}
static VOID Run(ULONG protocol,ULONG shape,ULONG length,BOOLEAN measured)
{
    PB_KERNEL_SESSION session={0};TUN_CTX ctx={0};session.Ctx=&ctx;CHECK(PbCalculateLayout(1,32,32,&session.Layout)==PB_OK);
#ifdef PB_GUARD_TESTS
    session.Region=PbTestGuardAlloc(session.Layout.RegionSize);
#else
    session.Region=VirtualAlloc(NULL,session.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#endif
    CHECK(session.Region);Blocks++;memset(session.Region,0x6d,session.Layout.RegionSize);Queue=session.Queues;Queue->Session=&session;
    BUFFER scratch=NewBuffer(PB_BLOCK_SIZE),outputs[2]={NewBuffer(PB_BLOCK_SIZE),NewBuffer(PB_BLOCK_SIZE)},hits=NewBuffer(32*STRIDE),tags=NewBuffer(32*STRIDE),expected=NewBuffer(32*STRIDE),oracle=NewBuffer(PB_BLOCK_SIZE);
    Queue->Scratch=scratch.Data;memset(scratch.Data,0xa5,scratch.Size);for(ULONG b=0;b<2;++b){Queue->CoalesceBuffers[b]=outputs[b].Data;memset(outputs[b].Data,0xa5,outputs[b].Size);Queue->CoalesceBusy[b]=shape==BUSY;}
    Hits=hits.Data;Tags=tags.Data;ExpectedSource=expected.Data;memset(Hits,0,hits.Size);memset(Tags,0,tags.Size);memset(ExpectedSource,0x6d,expected.Size);
    CaseShape=shape;CaseProtocol=protocol;CaseLength=length;PB_DESCRIPTOR items[32]={0};BOOLEAN valid[32];
    for(ULONG i=0;i<32;++i){items[i].Token=(PB_TOKEN){42,1,(USHORT)i,0};items[i].Length=length;items[i].DataOffset=session.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE;valid[i]=TRUE;UCHAR *data=session.Region+items[i].DataOffset;
        if(shape==BOUNDARY)Partial(data,length,protocol);else{Wire(data,length,protocol,i,1,FALSE);if(shape==BAD_CHECKSUM)data[length-1]^=1;}
        memcpy(ExpectedSource+i*STRIDE,data,length);
    }
    ULONG correct=protocol==17?PB_RX_URO:PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6,mask=correct;
    if(shape==WRONG_PROTOCOL)mask=protocol==6?PB_RX_URO:(PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6);
    if(shape==WRONG_FAMILY)mask=PB_TEST_IP_VERSION==4?PB_RX_RSC6:PB_RX_RSC4;if(shape==ZERO_MASK)mask=0;
    PB_DESCRIPTOR originalItems[32];memcpy(originalItems,items,sizeof(items));
    Groups=Begins=Appends=Finishes=Copies=Injected=0;ScratchBytes=OutputBytes=ChecksumBytes=0;Counting=TRUE;
    for(ULONG at=0;at<32;){CurrentAt=at;Attempt=Groups+1;ULONG copied;PB_COALESCE state={0};ULONG n=PbV2TryCoalesce(Queue,items+at,valid+at,32-at,mask,&copied,&state);CHECK(n&&n<=32-at);Groups++;
        if(n>1){ULONG wanted=shape==FLOW_FOUR?4:32,header=Header(protocol),size=header+n*(length-header);CHECK(n==wanted&&copied==0&&state.Buffer==outputs[0].Data&&state.Capacity==65535&&state.Length==size&&state.Count==n&&state.SegmentSize==length-header&&state.PayloadLength==n*(length-header)&&state.HeaderLength==header&&state.L4Offset==(PB_TEST_IP_VERSION==4?20u:40u)&&state.IpVersion==PB_TEST_IP_VERSION&&state.Protocol==protocol&&!state.ShortFinal&&state.Finalized);
            CHECK(state.NextSequence==(protocol==6?0xfffffff0u+(at+n)*(length-header):0));Wire(oracle.Data,size,protocol,at,n,protocol==17);CHECK(!memcmp(state.Buffer,oracle.Data,size));CHECK(Queue->CoalesceBusy[copied]==1);InterlockedExchange(Queue->CoalesceBusy+copied,0);
        }else CHECK(copied==MAXULONG&&!state.Buffer&&!state.Count);
        at+=n;
    }
    Counting=FALSE;CHECK(!memcmp(originalItems,items,sizeof(items)));for(ULONG i=0;i<32;++i)CHECK(valid[i]);
    BOOLEAN selected=shape!=WRONG_PROTOCOL&&shape!=WRONG_FAMILY&&shape!=ZERO_MASK&&shape!=BUSY;
    BOOLEAN good=selected&&shape!=BAD_CHECKSUM&&shape!=BOUNDARY;ULONG expectedGroups=good?(shape==FLOW_FOUR?8:1):32;
    CHECK(Groups==expectedGroups&&Begins==(good?(shape==FLOW_FOUR?8u:1u):selected?(shape==BOUNDARY?(length>=(PB_TEST_IP_VERSION==4?20u:40u)?31u:0u):31u):0u)&&Appends==(good?31u:0u)&&Finishes==(good?(shape==FLOW_FOUR?8u:1u):0u));
    ULONG64 wantedScratch=0,wantedOutput=good?(ULONG64)(shape==FLOW_FOUR?8:1)*Header(protocol)+(ULONG64)32*(length-Header(protocol)):0;
    for(ULONG i=0;i<32;++i){ULONG read=0,multiplicity=0;
        if(shape!=ZERO_MASK&&shape!=BUSY){if(good){read=length;multiplicity=1+(shape==FLOW_FOUR&&i&&i%4==0);}else if(i<31){read=selected&&shape!=BOUNDARY?length:shape==BOUNDARY&&length>=(PB_TEST_IP_VERSION==4?20u:40u)?length:(length<40?length:40);multiplicity=1;}}
        wantedScratch+=(ULONG64)read*multiplicity;for(ULONG j=0;j<STRIDE;++j)CHECK(Hits[i*STRIDE+j]==(UCHAR)(j<read?multiplicity:0));
        UCHAR *data=session.Region+items[i].DataOffset;CHECK(!memcmp(data,ExpectedSource+i*STRIDE,length));for(ULONG j=0;j<PAD;++j)CHECK(data[-(LONG)PAD+(LONG)j]==0x6d&&data[length+j]==0x6d);
    }
    CHECK(ScratchBytes==wantedScratch&&OutputBytes==wantedOutput&&ctx.Probe.RscAborts==(shape==FLOW_FOUR&&protocol==6?7:0)&&Injected==(ULONG)(shape==INJECT_PREFIX));
    ULONG64 wantedChecksum=0;if(good||shape==BAD_CHECKSUM)wantedChecksum=(shape==FLOW_FOUR?39ull:good?32ull:31ull)*(PB_TEST_IP_VERSION==4?length+8:length-8);
    else if(shape==BOUNDARY&&PB_TEST_IP_VERSION==4&&((protocol==6&&length>=41)||(protocol==17&&length>=28)))wantedChecksum=31*20;
    CHECK(ChecksumBytes==wantedChecksum);
    for(ULONG b=0;b<2;++b){CHECK(Queue->CoalesceBusy[b]==(LONG)(shape==BUSY));if(!good)for(SIZE_T j=0;j<outputs[b].Size;++j)CHECK(outputs[b].Data[j]==0xa5);DeleteBuffer(outputs+b);}
    ULONG observedCopies=Copies;ULONG64 observedScratch=ScratchBytes,observedOutput=OutputBytes,observedChecksum=ChecksumBytes;
    DeleteBuffer(&scratch);DeleteBuffer(&hits);DeleteBuffer(&tags);DeleteBuffer(&expected);DeleteBuffer(&oracle);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(session.Region);
#else
    CHECK(VirtualFree(session.Region,0,MEM_RELEASE));
#endif
    CHECK(--Blocks==0);Queue=NULL;Hits=Tags=ExpectedSource=NULL;Cases++;
    printf("PREFILTER32 ip%u protocol%lu shape%lu length%lu measured%u groups%lu begin%lu append%lu finish%lu snapshot%llu output%llu checksum%llu copies%lu injection%lu owners0\n",PB_TEST_IP_VERSION,protocol,shape,length,measured,Groups,Begins,Appends,Finishes,observedScratch,observedOutput,observedChecksum,observedCopies,Injected);
}
int main(VOID)
{
    DWORD before=0,after;const ULONG lengths[]={0,1,19,20,39,40,41};
    for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG protocol=6;protocol<=17;protocol+=11){for(ULONG shape=ENABLED;shape<=INJECT_PREFIX;++shape)Run(protocol,shape,shape==SMALL?Header(protocol)+1:1500,(BOOLEAN)pass);if(protocol==6)Run(protocol,WRONG_FAMILY,1500,(BOOLEAN)pass);for(ULONG i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i)Run(protocol,BOUNDARY,lengths[i],(BOOLEAN)pass);}}
    CHECK(Cases==62&&!Blocks&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: PREFILTER32 family%u cases62 warm31 measured31 handles%lu/%lu owners0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
