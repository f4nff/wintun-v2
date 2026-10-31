/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted validation + checksum/copy cost; no driver or network traffic. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../../common/packet.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1); } } while(0)
static void Measure(uint32_t mss,uint32_t batch)
{
    uint8_t source[32808]={0},buffers[32][1500];PB_SEGMENT_OUTPUT outputs[32];
    uint32_t length=40+32*mss,written;source[0]=0x45;source[9]=6;source[32]=0x50;
    for(uint32_t i=40;i<length;++i)source[i]=(uint8_t)i;
    PB_SEGMENT_PLAN plan;CHECK(PbPlanSegments(source,length,PB_SEGMENT_TCP,mss,&plan)==PB_PACKET_OK && plan.SegmentCount==32);
    for(uint32_t i=0;i<32;++i)outputs[i]=(PB_SEGMENT_OUTPUT){buffers[i],sizeof(buffers[i]),0};
    for(uint32_t i=0;i<200;++i)CHECK(PbWriteSegmentBatchChecked(source,length,&plan,0,outputs,batch,&written)==PB_PACKET_OK && written==batch);
    LARGE_INTEGER frequency,start,end;ULONG64 first,last;CHECK(QueryPerformanceFrequency(&frequency));
    uint32_t iterations=mss==16?20000u:2000u;
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&first) && QueryPerformanceCounter(&start));
    for(uint32_t i=0;i<iterations;++i)CHECK(PbWriteSegmentBatchChecked(source,length,&plan,0,outputs,batch,&written)==PB_PACKET_OK && written==batch);
    CHECK(QueryPerformanceCounter(&end) && QueryThreadCycleTime(GetCurrentThread(),&last));
    printf("{\"mss\":%u,\"batch\":%u,\"iterations\":%u,\"ns_per_batch\":%.3f,\"cycles_per_batch\":%.3f}\n",mss,batch,iterations,
        (double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/iterations,(double)(last-first)/iterations);
}
int main(void)
{
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    CHECK(SetThreadAffinityMask(GetCurrentThread(),allowed&(~allowed+1)));
    const uint32_t batches[]={1,8,32};
    for(uint32_t i=0;i<3;++i)Measure(16,batches[i]);
    for(uint32_t i=0;i<3;++i)Measure(1024,batches[i]);
    return 0;
}
