/* SPDX-License-Identifier: GPL-2.0 */
/* User-mode helper microbenchmark, not kernel CPU or VPN throughput. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../../common/flow.h"
static volatile uint32_t Sink;
static VOID Run(uint32_t ip,uint32_t queues)
{
    uint8_t packet[40]={0}; packet[0]=(uint8_t)(ip<<4);
    for(uint32_t i=8;i<sizeof(packet);++i) packet[i]=(uint8_t)(17*i+3);
    const uint32_t iterations=2000000;
    LARGE_INTEGER frequency,start,end; ULONG64 cyclesStart,cyclesEnd;
    QueryPerformanceFrequency(&frequency); QueryThreadCycleTime(GetCurrentThread(),&cyclesStart); QueryPerformanceCounter(&start);
    uint32_t sum=0;
    for(uint32_t i=0;i<iterations;++i) sum+=PbSelectQueue(packet,ip==4?20:40,queues);
    QueryPerformanceCounter(&end); QueryThreadCycleTime(GetCurrentThread(),&cyclesEnd); Sink=sum;
    if(queues==1 && sum) ExitProcess(1);
    printf("{\"ip\":%u,\"queues\":%u,\"iterations\":%u,\"ns_per_call\":%.3f,\"cycles_per_call\":%.3f,\"sum\":%u}\n",ip,queues,iterations,
        1e9*(end.QuadPart-start.QuadPart)/frequency.QuadPart/iterations,(double)(cyclesEnd-cyclesStart)/iterations,sum);
}
int main(VOID)
{
    DWORD_PTR processMask,systemMask;
    if(!GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask) || !processMask) return 1;
    DWORD_PTR prior=SetThreadAffinityMask(GetCurrentThread(),processMask&(~processMask+1)); if(!prior) return 1;
    Run(4,1); Run(6,1); Run(4,4); Run(6,4);
    SetThreadAffinityMask(GetCurrentThread(),prior); return 0;
}
