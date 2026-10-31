/* SPDX-License-Identifier: GPL-2.0 */
/* User-mode microbenchmark; this does not measure NDIS, kernel CPU or VPN throughput. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../common/v2_core.h"
static void Run(uint32_t slots,int mode)
{
    PB_POOL pool; PB_SLOT states[PB_MAX_SLOTS]; PB_TOKEN token;
    if (PbPoolInitialize(&pool,states,slots,123,0)!=PB_OK) ExitProcess(1);
    if (mode) for (uint32_t i=0;i<slots;++i) if (PbPoolReserve(&pool,&token)!=PB_OK) ExitProcess(1);
    if (mode==2 && PbPoolTransition(&pool,&token,PB_SLOT_RESERVED,PB_SLOT_FREE)!=PB_OK) ExitProcess(1);
    const uint32_t iterations=slots>64 && mode?100000:2000000;
    LARGE_INTEGER frequency,start,end; ULONG64 cyclesStart,cyclesEnd;
    QueryPerformanceFrequency(&frequency); QueryThreadCycleTime(GetCurrentThread(),&cyclesStart); QueryPerformanceCounter(&start);
    for (uint32_t i=0;i<iterations;++i) {
        PB_RESULT result=PbPoolReserve(&pool,&token);
        if (mode==1) { if(result!=PB_FULL) ExitProcess(1); }
        else if(result!=PB_OK || PbPoolTransition(&pool,&token,PB_SLOT_RESERVED,PB_SLOT_FREE)!=PB_OK) ExitProcess(1);
        if(mode==2 && token.Slot!=slots-1) ExitProcess(1);
    }
    QueryPerformanceCounter(&end); QueryThreadCycleTime(GetCurrentThread(),&cyclesEnd);
    printf("{\"slots\":%u,\"case\":\"%s\",\"iterations\":%u,\"ns_per_attempt\":%.3f,\"cycles_per_attempt\":%.3f}\n",
        slots,mode==1?"full":mode==2?"one-free":"reserve-release",iterations,1e9*(end.QuadPart-start.QuadPart)/frequency.QuadPart/iterations,
        (double)(cyclesEnd-cyclesStart)/iterations);
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--wrap")) {
        PB_POOL pool; PB_SLOT states[3]; PB_TOKEN token={0};
        if (PbPoolInitialize(&pool,states,3,123,0)!=PB_OK) return 1;
        PB_TOKEN held[3];for(uint32_t i=0;i<3;++i) if(PbPoolReserve(&pool,held+i)!=PB_OK) return 1;
        if(PbPoolTransition(&pool,held+1,PB_SLOT_RESERVED,PB_SLOT_FREE)!=PB_OK) return 1;
        pool.Next=UINT32_MAX-1;
        PB_RESULT result=PbPoolReserve(&pool,&token);
        printf("{\"wrap_result\":%u,\"slot\":%u}\n",(unsigned)result,(unsigned)token.Slot);
        return result==PB_OK && token.Slot==1 ? 0 : 2;
    }
    DWORD_PTR processMask,systemMask;
    if (!GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask) || !processMask) return 1;
    DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),processMask & (~processMask+1));
    if (!previous) return 1;
    Run(32,0); Run(31,0); Run(32,1); Run(31,1);
    Run(512,0);Run(512,1);Run(512,2);Run(511,2);
    if(previous) SetThreadAffinityMask(GetCurrentThread(),previous);
    return 0;
}
