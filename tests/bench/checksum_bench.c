/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted scalar-core timing only. Volatile indirect call prevents constant
 * folding or whole-loop elimination; the same allowed CPU runs both variants. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../common/packet.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
static uint32_t (*volatile Add)(uint32_t,const uint8_t *,uint32_t)=PbChecksumAdd;
static volatile uint32_t Sink;
int main(VOID)
{
    uint8_t *bytes=malloc(65536);CHECK(bytes);for(uint32_t i=0;i<65536;++i) bytes[i]=(uint8_t)(i*37u);
    DWORD_PTR process,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&process,&system));
    DWORD_PTR cpu=process&(~process+1);CHECK(SetThreadAffinityMask(GetCurrentThread(),cpu));
    const uint32_t sizes[]={0,1,2,4,7,8,16,20,32,40,64,128,1500,32768,65535};
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));
    for(ULONG si=0;si<sizeof(sizes)/sizeof(sizes[0]);++si) {
        uint32_t length=sizes[si],loops=length<128?2000000:length<2000?200000:10000,result=0;
        for(uint32_t i=0;i<1000;++i) result+=Add(i,bytes,length);
        LARGE_INTEGER start,end;ULONG64 c0,c1;
        CHECK(QueryPerformanceCounter(&start) && QueryThreadCycleTime(GetCurrentThread(),&c0));
        for(uint32_t i=0;i<loops;++i) result+=Add(i,bytes,length);
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&c1) && QueryPerformanceCounter(&end));Sink=result;
        printf("{\"length\":%u,\"calls\":%u,\"ns_per_call\":%.3f,\"cycles_per_call\":%.3f,\"result\":%u,\"cpu_mask\":%llu}\n",
            length,loops,1e9*(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart/loops,(double)(c1-c0)/loops,result,(unsigned long long)cpu);
    }
    free(bytes);return 0;
}
