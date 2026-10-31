/* SPDX-License-Identifier: GPL-2.0 */
/* Actual kernel coalescing orchestration and core, with byte/call accounting.
 * Eligibility checksums are counted separately from finalization. No SYS timing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../include/packetbridge_probe.h"
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}} while(0)
#define NT_ASSERT CHECK
#define MAXULONG 0xffffffffu
typedef struct {LONG Unused;} KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct _TUN_CTX {struct {volatile LONG64 RscAborts;} Probe;} TUN_CTX;
static ULONG Begins,Appends,Finishes;
static ULONG FlowGroupSize;
static ULONG64 ScratchBytes,OutputBytes,ChecksumBytes;
static PB_KERNEL_QUEUE *Queue;
#define PbChecksumAdd ActualChecksumAdd
#ifndef PB_PACKET_SOURCE
#define PB_PACKET_SOURCE "../../common/packet.c"
#endif
#include PB_PACKET_SOURCE
#undef PbChecksumAdd
uint32_t PbChecksumAdd(uint32_t sum,const uint8_t *data,uint32_t length)
{ChecksumBytes+=length;return ActualChecksumAdd(sum,data,length);}
static VOID CopySnapshot(VOID *dst,const VOID *src,SIZE_T length)
{CHECK(dst==Queue->Scratch && length<=PB_BLOCK_SIZE);ScratchBytes+=length;memcpy(dst,src,length);}
static PB_PACKET_STATUS Begin(PB_COALESCE *s,uint8_t *output,uint32_t capacity,const uint8_t *data,uint32_t length)
{Begins++;PB_PACKET_STATUS r=PbCoalesceBegin(s,output,capacity,data,length);if(r==PB_PACKET_OK) OutputBytes+=length;return r;}
static PB_PACKET_STATUS Append(PB_COALESCE *s,const uint8_t *data,uint32_t length)
{Appends++;ULONG old=s->Length;PB_PACKET_STATUS r=PbCoalesceAppend(s,data,length);if(r==PB_PACKET_OK) OutputBytes+=s->Length-old;return r;}
static PB_PACKET_STATUS Finish(PB_COALESCE *s) {Finishes++;return PbCoalesceFinish(s);}
#undef RtlCopyMemory
#define RtlCopyMemory CopySnapshot
#define PbCoalesceBegin Begin
#define PbCoalesceAppend Append
#define PbCoalesceFinish Finish
#ifndef PB_COALESCE_SOURCE
#define PB_COALESCE_SOURCE "../../driver/v2/session_coalesce.h"
#endif
#include PB_COALESCE_SOURCE
#undef PbCoalesceBegin
#undef PbCoalesceAppend
#undef PbCoalesceFinish
static VOID W16(UCHAR *p,ULONG n) {p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID W32(UCHAR *p,ULONG n) {p[0]=(UCHAR)(n>>24);p[1]=(UCHAR)(n>>16);p[2]=(UCHAR)(n>>8);p[3]=(UCHAR)n;}
static VOID Packet(UCHAR *p,ULONG length,ULONG ip,ULONG protocol,ULONG sequence)
{
    memset(p,0,length);ULONG offset=ip==4?20:40;
    if(ip==4) {p[0]=0x45;W16(p+2,length);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;}
    else {p[0]=0x60;W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
    W16(p+offset,4000);W16(p+offset+2,5000);
    if(protocol==6) {W32(p+offset+4,sequence);p[offset+12]=0x50;p[offset+13]=0x10;W16(p+offset+14,8192);}
    else W16(p+offset+4,length-offset);
    memset(p+offset+(protocol==6?20:8),0x5a,length-offset-(protocol==6?20:8));
    CHECK(PbFinalizeChecksums(p,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
}
static VOID Run(ULONG ip,ULONG protocol,ULONG mask,ULONG length,BOOLEAN busy,BOOLEAN invalid)
{
    PB_KERNEL_SESSION s={0};TUN_CTX ctx={0};s.Ctx=&ctx;
    CHECK(PbCalculateLayout(1,32,32,&s.Layout)==PB_OK);
    s.Region=VirtualAlloc(NULL,s.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(s.Region);
    PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;Queue=q;
    q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);
    for(ULONG b=0;b<2;++b) {q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);q->CoalesceBusy[b]=busy;}
    PB_DESCRIPTOR items[32]={0};BOOLEAN valid[32];ULONG header=(ip==4?20:40)+(protocol==6?20:8);
    for(ULONG i=0;i<32;++i) {
        items[i].Length=length;items[i].DataOffset=s.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE;valid[i]=TRUE;
        Packet(s.Region+items[i].DataOffset,length,ip,protocol,100+i*(length-header));
        if(FlowGroupSize) {
            W16(s.Region+items[i].DataOffset+(ip==4?20:40),4000+i/FlowGroupSize);
            CHECK(PbFinalizeChecksums(s.Region+items[i].DataOffset,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
        }
        if(invalid) s.Region[items[i].DataOffset+length-1]^=1;
    }
    Begins=Appends=Finishes=0;ScratchBytes=OutputBytes=ChecksumBytes=0;
    ULONG groups=0;
    for(ULONG at=0;at<32;) {
        ULONG copied;PB_COALESCE coal={0};
        ULONG n=PbV2TryCoalesce(q,items+at,valid+at,32-at,mask,&copied,&coal);CHECK(n && n<=32-at);groups++;
        if(n>1) {
            CHECK(copied<2 && coal.Count==n && coal.Length==header+n*(length-header));
            PB_SEGMENT_PLAN plan;CHECK(PbPlanSegments(coal.Buffer,coal.Length,PB_SEGMENT_NONE,0,&plan)==PB_PACKET_OK);
            for(ULONG j=header;j<coal.Length;++j) CHECK(coal.Buffer[j]==0x5a);
            if(protocol==6) CHECK(PbChecksumFinish(ActualChecksumAdd(0,coal.Buffer,ip==4?20:0))==0 || ip==6);
            InterlockedExchange(&q->CoalesceBusy[copied],0);
        } else CHECK(copied==MAXULONG);
        at+=n;
    }
    BOOLEAN enabled=(mask&(protocol==17?PB_RX_URO:ip==4?PB_RX_RSC4:PB_RX_RSC6))!=0;
    if(!mask || busy) CHECK(!Begins && !Appends && !ScratchBytes && !OutputBytes && !ChecksumBytes);
    else if(!enabled) {
#ifdef PB_COALESCE_BASELINE
        CHECK(Begins==31 && !Appends && ScratchBytes==31ull*length && OutputBytes==31ull*length && ChecksumBytes>0);
#else
        CHECK(!Begins && !Appends && ScratchBytes==31ull*length && !OutputBytes && !ChecksumBytes);
#endif
        CHECK(groups==32 && !ctx.Probe.RscAborts);
    } else if(invalid) CHECK(groups==32 && Begins==31 && !Appends && !OutputBytes);
    else if(FlowGroupSize) CHECK(groups==8 && Begins==8 && Appends==31 && Finishes==8 && ctx.Probe.RscAborts==7);
    else CHECK(groups==1 && Begins==1 && Appends==31 && Finishes==1 && ScratchBytes==32ull*length && !ctx.Probe.RscAborts);
    printf("PASS: ip=%lu protocol=%lu mask=%lu length=%lu busy=%u invalid=%u groups=%lu begin=%lu append=%lu snapshot_bytes=%llu output_bytes=%llu eligibility_checksum_bytes=%llu.\n",ip,protocol,mask,length,busy,invalid,groups,Begins,Appends,ScratchBytes,OutputBytes,ChecksumBytes);
    for(ULONG b=0;b<2;++b) {CHECK(q->CoalesceBusy[b]==busy);PbTestGuardFree(q->CoalesceBuffers[b]);}
    PbTestGuardFree(q->Scratch);CHECK(VirtualFree(s.Region,0,MEM_RELEASE));Queue=NULL;
}
/* mode: three valid packets, exact two-packet tail, invalid next descriptor,
 * bad short checksum, disabled URO. Busy=1 selects the other output buffer. */
static VOID ShortFinal(ULONG ip,ULONG mode,ULONG busy)
{
    PB_KERNEL_SESSION s={0};TUN_CTX ctx={0};s.Ctx=&ctx;CHECK(PbCalculateLayout(1,4,8,&s.Layout)==PB_OK);
    s.Region=VirtualAlloc(NULL,s.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(s.Region);
    PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;Queue=q;q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);
    for(ULONG b=0;b<2;++b) {q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);q->CoalesceBusy[b]=b<busy;}
    PB_DESCRIPTOR items[3]={0};BOOLEAN valid[3]={TRUE,TRUE,mode!=2};const ULONG lengths[]={60000,1000,60000};
    ULONG header=ip==4?28:48;
    for(ULONG i=0;i<3;++i) {
        items[i].DataOffset=s.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE;items[i].Length=lengths[i];
        UCHAR *p=s.Region+items[i].DataOffset;Packet(p,lengths[i],ip,17,0);memset(p+header,0x50+i,lengths[i]-header);
        CHECK(PbFinalizeChecksums(p,lengths[i],ip==4,0,1)==PB_PACKET_OK);
    }
    UCHAR *expected=PbTestGuardAlloc(PB_BLOCK_SIZE);PB_COALESCE oracle={0};
    CHECK(PbCoalesceBegin(&oracle,expected,65535,s.Region+items[0].DataOffset,60000)==PB_PACKET_OK);
    CHECK(PbCoalesceAppend(&oracle,s.Region+items[1].DataOffset,1000)==PB_PACKET_OK && oracle.ShortFinal);
    CHECK(PbCoalesceFinish(&oracle)==PB_PACKET_OK);
    if(mode==3) s.Region[items[1].DataOffset+999]^=1;
    Begins=Appends=Finishes=0;ScratchBytes=OutputBytes=ChecksumBytes=0;ULONG copied;PB_COALESCE coal={0};
    ULONG count=mode==1?2:3,n=PbV2TryCoalesce(q,items,valid,count,mode==4?0:PB_RX_URO,&copied,&coal);
    if(busy==2 || mode==4) CHECK(n==1 && copied==MAXULONG && !Begins && !Appends && !ScratchBytes && !OutputBytes && !ChecksumBytes);
    else if(mode==3) CHECK(n==1 && copied==MAXULONG && Begins==1 && Appends==1 && Finishes==1 && ScratchBytes==61000 && OutputBytes==60000);
    else {
        CHECK(n==2 && copied==busy && Begins==1 && Finishes==1);
        CHECK(coal.Count==oracle.Count && coal.Length==oracle.Length && coal.SegmentSize==oracle.SegmentSize && coal.PayloadLength==oracle.PayloadLength);
        CHECK(coal.HeaderLength==oracle.HeaderLength && coal.L4Offset==oracle.L4Offset && coal.Protocol==17 && coal.IpVersion==ip && coal.ShortFinal && coal.Finalized);
        CHECK(!memcmp(coal.Buffer,expected,coal.Length) && OutputBytes==coal.Length && ChecksumBytes==(ip==4?61016ull:60984ull));
#if defined(PB_COALESCE_TERMINAL_BASELINE) || defined(PB_COALESCE_BASELINE)
        CHECK(Appends==(mode==0?2u:1u) && ScratchBytes==(mode==0?121000ull:61000ull));
#else
        CHECK(Appends==1 && ScratchBytes==61000);
#endif
        InterlockedExchange(&q->CoalesceBusy[copied],0);
    }
    CHECK(!ctx.Probe.RscAborts);
    printf("PASS: UDP short-final ip=%lu mode=%lu busy=%lu: group=%lu append_calls=%lu snapshot_bytes=%llu checksum_bytes=%llu, exact core output/state and buffer ownership.\n",ip,mode,busy,n,Appends,ScratchBytes,ChecksumBytes);
    for(ULONG b=0;b<2;++b) {CHECK(q->CoalesceBusy[b]==(LONG)(b<busy));PbTestGuardFree(q->CoalesceBuffers[b]);}
    PbTestGuardFree(expected);PbTestGuardFree(q->Scratch);CHECK(VirtualFree(s.Region,0,MEM_RELEASE));Queue=NULL;
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Run(4,17,PB_RX_RSC4,1500,FALSE,FALSE);Run(6,6,PB_RX_RSC4,1500,FALSE,FALSE);
    Run(4,6,PB_RX_RSC4,1500,FALSE,FALSE);Run(6,17,PB_RX_URO,1500,FALSE,FALSE);
    Run(4,6,PB_RX_RSC4,64,FALSE,FALSE);Run(6,6,PB_RX_RSC6,1500,FALSE,FALSE);
    Run(4,6,0,1500,FALSE,FALSE);Run(4,6,PB_RX_RSC4,1500,TRUE,FALSE);
    Run(4,6,PB_RX_RSC4,1500,FALSE,TRUE);
    Run(4,17,PB_RX_URO,1500,FALSE,FALSE);
    FlowGroupSize=4;Run(4,6,PB_RX_RSC4,1500,FALSE,FALSE);FlowGroupSize=0;
    for(ULONG ip=4;ip<=6;ip+=2) {for(ULONG mode=0;mode<=4;++mode) ShortFinal(ip,mode,0);ShortFinal(ip,0,1);ShortFinal(ip,0,2);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
