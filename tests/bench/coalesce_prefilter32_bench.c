/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted actual coalescing helper microbenchmark. Validation is outside timing. */
#ifdef PB_API_TESTS
#error Actual coalescing helper required
#endif
#ifndef NDEBUG
#error Shipping NDEBUG required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include PB_BENCH_VERSION_SOURCE
#include PB_BENCH_GUARD_SOURCE
#include PB_BENCH_PROBE_SOURCE
#define CHECK SAFETY_CHECK
#define NT_ASSERT CHECK
#define MAXULONG 0xffffffffu
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"shipping fingerprint required");
typedef struct { LONG Unused; } KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include PB_BENCH_STATE_SOURCE
typedef struct _TUN_CTX { struct { volatile LONG64 RscAborts; } Probe; } TUN_CTX;
#include PB_COALESCE_SOURCE
static volatile ULONG64 ResultSink;
static VOID W16(BYTE *p,ULONG n){p[0]=(BYTE)(n>>8);p[1]=(BYTE)n;}
static VOID W32(BYTE *p,ULONG n){p[0]=(BYTE)(n>>24);p[1]=(BYTE)(n>>16);p[2]=(BYTE)(n>>8);p[3]=(BYTE)n;}
static ULONG Add(ULONG s,const BYTE *p,ULONG n){for(;n>1;n-=2,p+=2)s+=((ULONG)p[0]<<8)|p[1];if(n)s+=(ULONG)p[0]<<8;return s;}
static USHORT Final(ULONG s){while(s>>16)s=(s&65535)+(s>>16);return (USHORT)~s;}
static VOID Checksums(BYTE *p,ULONG length,BOOL udp)
{
 ULONG ip=PB_TEST_IP_VERSION==4?20u:40u,l4=length-ip,off=ip+(udp?6u:16u);W16(p+off,0);
 ULONG pseudo=PB_TEST_IP_VERSION==4?Add(0,p+12,8):Add(0,p+8,32);USHORT value=Final(Add(pseudo+l4+(udp?17u:6u),p+ip,l4));W16(p+off,udp&&!value?65535:value);
#if PB_TEST_IP_VERSION == 4
 W16(p+10,0);W16(p+10,Final(Add(0,p,20)));
#endif
}
static BOOL ValidChecksum(const BYTE *p,ULONG length,BOOL udp,BOOL uro)
{
 ULONG ip=PB_TEST_IP_VERSION==4?20u:40u,l4=length-ip;ULONG pseudo=PB_TEST_IP_VERSION==4?Add(0,p+12,8):Add(0,p+8,32);
#if PB_TEST_IP_VERSION == 4
 if(uro){if(p[10]||p[11])return FALSE;}else if(Final(Add(0,p,20)))return FALSE;
#endif
 if(uro)return udp&&!p[ip+6]&&!p[ip+7];
 return !Final(Add(pseudo+l4+(udp?17u:6u),p+ip,l4));
}
static VOID Packet(BYTE *p,ULONG length,BOOL udp,ULONG sequence,ULONG ordinal,BOOL flow4)
{
 ULONG ip=PB_TEST_IP_VERSION==4?20u:40u,header=ip+(udp?8u:20u);memset(p,0,length);
#if PB_TEST_IP_VERSION == 4
 p[0]=0x45;W16(p+2,length);W16(p+4,0x3000+ordinal);p[8]=64;p[9]=(BYTE)(udp?17:6);p[12]=10;p[15]=1;p[16]=10;p[19]=2;
#else
 p[0]=0x60;W16(p+4,length-40);p[6]=(BYTE)(udp?17:6);p[7]=64;p[8]=0x20;p[9]=1;p[23]=1;p[24]=0x20;p[25]=1;p[39]=2;
#endif
 W16(p+ip,4000+(flow4?ordinal/4:0));W16(p+ip+2,5000);if(udp)W16(p+ip+4,length-ip);else{W32(p+ip+4,sequence);p[ip+12]=0x50;p[ip+13]=0x10;W16(p+ip+14,8192);}
 for(ULONG i=header;i<length;++i)p[i]=(BYTE)(i+ordinal*13);Checksums(p,length,udp);CHECK(ValidChecksum(p,length,udp,FALSE));
}
static ULONG64 Hash(const BYTE *p,SIZE_T n){ULONG64 h=1469598103934665603ull;for(SIZE_T i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;}
static ULONG64 CpuTime(VOID){FILETIME created,exitTime,kernel,user;CHECK(GetThreadTimes(GetCurrentThread(),&created,&exitTime,&kernel,&user));ULARGE_INTEGER k={.LowPart=kernel.dwLowDateTime,.HighPart=kernel.dwHighDateTime},u={.LowPart=user.dwLowDateTime,.HighPart=user.dwHighDateTime};return k.QuadPart+u.QuadPart;}
static ULONG64 SourceHash(PB_KERNEL_SESSION *s,const PB_DESCRIPTOR *d){ULONG64 sum=0;for(ULONG i=0;i<32;++i)sum^=Hash(s->Region+d[i].DataOffset,d[i].Length)+(ULONG64)i*0x9e3779b97f4a7c15ull;return sum;}
/* Execute only actual helper and normal Busy ownership return. No host byte
 * oracle, checksum verification, logging or output comparison inside timing. */
static __declspec(noinline) ULONG64 Batch(PB_KERNEL_QUEUE *q,const PB_DESCRIPTOR *items,const BOOLEAN *valid,ULONG enabled)
{
 ULONG64 result=0;for(ULONG at=0;at<32;){ULONG copied;PB_COALESCE coal={0};ULONG n=PbV2TryCoalesce(q,items+at,valid+at,32-at,enabled,&copied,&coal);result+=n+((ULONG64)copied<<8)+((ULONG64)coal.Length<<16);if(copied<2)InterlockedExchange(q->CoalesceBusy+copied,0);at+=n;}return result;
}
static ULONG64 Validate(PB_KERNEL_QUEUE *q,const PB_DESCRIPTOR *items,const BOOLEAN *valid,ULONG enabled,BOOL udp,ULONG shape,ULONG *groups)
{
 ULONG64 digest=0;*groups=0;BYTE *expected=PbTestGuardAlloc(65535);ULONG header=(PB_TEST_IP_VERSION==4?20u:40u)+(udp?8u:20u);
 for(ULONG at=0;at<32;){ULONG copied;PB_COALESCE coal={0};ULONG n=PbV2TryCoalesce(q,items+at,valid+at,32-at,enabled,&copied,&coal);CHECK(n&&n<=32-at);++*groups;
  ULONG wanted=shape==0?32:shape==4?4:1;if(items[at].Length==65535||32-at==1)wanted=1;CHECK(n==wanted);
  if(n>1){CHECK(copied<2&&coal.Count==n&&coal.Finalized&&coal.IpVersion==PB_TEST_IP_VERSION&&coal.Protocol==(udp?17:6));ULONG payload=items[at].Length-header,total=header+n*payload;CHECK(total<=65535&&coal.Length==total);
   memcpy(expected,q->Session->Region+items[at].DataOffset,header);for(ULONG i=0;i<n;++i)memcpy(expected+header+i*payload,q->Session->Region+items[at+i].DataOffset+header,payload);
#if PB_TEST_IP_VERSION == 4
   W16(expected+2,total);
#else
   W16(expected+4,total-40);
#endif
   if(udp){ULONG ip=PB_TEST_IP_VERSION==4?20u:40u;W16(expected+ip+4,total-ip);W16(expected+ip+6,0);
#if PB_TEST_IP_VERSION == 4
    W16(expected+10,0);
#endif
   }else Checksums(expected,total,FALSE);
   CHECK(!memcmp(expected,coal.Buffer,total)&&ValidChecksum(coal.Buffer,total,udp,udp));digest^=Hash(coal.Buffer,total)+(ULONG64)at;InterlockedExchange(q->CoalesceBusy+copied,0);
  }else{CHECK(copied==MAXULONG);digest^=Hash(q->Session->Region+items[at].DataOffset,items[at].Length)+(ULONG64)at;}
  at+=n;
 }
 CHECK(!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]);PbTestGuardFree(expected);return digest;
}
static VOID Run(BOOL udp,ULONG length,ULONG shape,ULONG baseIterations,ULONG warm,ULONG minimumMs)
{
 const char *names[]={"eligible","wrong-mode","disabled-all","checksum-bad","flow4"};CHECK(shape<_countof(names));PB_KERNEL_SESSION s={0};TUN_CTX ctx={0};s.Ctx=&ctx;
 s.Region=VirtualAlloc(NULL,32u*PB_BLOCK_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(s.Region);PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG i=0;i<2;++i)q->CoalesceBuffers[i]=PbTestGuardAlloc(PB_BLOCK_SIZE);
 PB_DESCRIPTOR items[32]={0};BOOLEAN valid[32];ULONG payload=length-(PB_TEST_IP_VERSION==4?20u:40u)-(udp?8u:20u);for(ULONG i=0;i<32;++i){items[i].DataOffset=i*PB_BLOCK_SIZE;items[i].Length=length;valid[i]=TRUE;Packet(s.Region+items[i].DataOffset,length,udp,0xffff0000u+i*payload,i,shape==4);if(shape==3)s.Region[items[i].DataOffset+length-1]^=1;}
 DWORD oldProtect;CHECK(VirtualProtect(s.Region,32u*PB_BLOCK_SIZE,PAGE_READONLY,&oldProtect));BYTE *sourceCopy=PbTestGuardAlloc(32u*PB_BLOCK_SIZE);memcpy(sourceCopy,s.Region,32u*PB_BLOCK_SIZE);PB_DESCRIPTOR original[32];memcpy(original,items,sizeof(items));ULONG64 source=SourceHash(&s,items);
 ULONG own=udp?PB_RX_URO:PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6;ULONG enabled=shape==2?0:shape==1?(udp?PB_RX_RSC4:PB_RX_URO):own;
 ULONG groups0,groups1;ULONG64 output=Validate(q,items,valid,enabled,udp,shape,&groups0);for(ULONG i=0;i<warm;++i)ResultSink^=Batch(q,items,valid,enabled);
 ULONG iterations=length==65535?(baseIterations/16?baseIterations/16:1):baseIterations;LARGE_INTEGER first,last,frequency,calFirst,calLast;CHECK(QueryPerformanceFrequency(&frequency));ULONG calibration=0;
 for(;;){CHECK(QueryPerformanceCounter(&calFirst));for(ULONG i=0;i<iterations;++i)ResultSink^=Batch(q,items,valid,enabled);CHECK(QueryPerformanceCounter(&calLast));++calibration;if((calLast.QuadPart-calFirst.QuadPart)*1000.0/frequency.QuadPart>=minimumMs)break;CHECK(iterations<=50000000u);iterations*=2;}
 ULONG64 cycles0,cycles1,cpu0=CpuTime();CHECK(QueryThreadCycleTime(GetCurrentThread(),&cycles0)&&QueryPerformanceCounter(&first));
 for(ULONG i=0;i<iterations;++i)ResultSink^=Batch(q,items,valid,enabled);
 CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&cycles1));ULONG64 cpu1=CpuTime();CHECK(cpu1>=cpu0&&cycles1>=cycles0&&last.QuadPart>=first.QuadPart);
 CHECK(Validate(q,items,valid,enabled,udp,shape,&groups1)==output&&groups0==groups1&&SourceHash(&s,items)==source&&!memcmp(s.Region,sourceCopy,32u*PB_BLOCK_SIZE)&&!memcmp(items,original,sizeof(items)));
 PbTestGuardFree(sourceCopy);for(ULONG i=0;i<2;++i)PbTestGuardFree(q->CoalesceBuffers[i]);PbTestGuardFree(q->Scratch);CHECK(VirtualFree(s.Region,0,MEM_RELEASE));
 printf("{\"ip\":%u,\"protocol\":%u,\"length\":%lu,\"shape\":\"%s\",\"packets\":32,\"iterations\":%lu,\"warm\":%lu,\"target_ms\":%lu,\"calibration_passes\":%lu,\"calibration_ticks\":%lld,\"groups\":%lu,\"source_hash\":\"%016llx\",\"output_hash\":\"%016llx\",\"qpc_ticks\":%lld,\"qpc_frequency\":%lld,\"cycles\":%llu,\"cpu_100ns\":%llu,\"ns_per_batch\":%.6f,\"cycles_per_batch\":%.6f,\"cpu_ns_per_batch\":%.6f,\"validated\":true}\n",PB_TEST_IP_VERSION,udp?17u:6u,length,names[shape],iterations,warm,minimumMs,calibration,calLast.QuadPart-calFirst.QuadPart,groups0,source,output,last.QuadPart-first.QuadPart,frequency.QuadPart,cycles1-cycles0,cpu1-cpu0,1e9*(last.QuadPart-first.QuadPart)/frequency.QuadPart/iterations,(double)(cycles1-cycles0)/iterations,(double)(cpu1-cpu0)*100/iterations);
}
int main(int argc,char **argv)
{
 CHECK(argc==4);ULONG iterations=strtoul(argv[1],NULL,10),warm=strtoul(argv[2],NULL,10),minimumMs=strtoul(argv[3],NULL,10);CHECK(iterations&&iterations<=1000000&&warm&&warm<=10000&&minimumMs>=30&&minimumMs<=2000);DWORD_PTR processMask,systemMask;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask)&&processMask);DWORD_PTR selected=processMask&(~processMask+1),prior=SetThreadAffinityMask(GetCurrentThread(),selected);CHECK(prior);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
 for(ULONG udp=0;udp<2;++udp){const ULONG lengths[]={64,1500,65535};for(ULONG l=0;l<_countof(lengths);++l)for(ULONG shape=0;shape<5;++shape)Run(udp!=0,lengths[l],shape,iterations,warm,minimumMs);}
 CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&SetThreadAffinityMask(GetCurrentThread(),prior));printf("{\"summary\":true,\"ip\":%u,\"cases\":30,\"affinity\":%llu,\"handles_before\":%lu,\"handles_after\":%lu,\"owned\":0,\"sink\":%llu}\n",PB_TEST_IP_VERSION,(ULONG64)selected,before,after,ResultSink);return 0;
}
