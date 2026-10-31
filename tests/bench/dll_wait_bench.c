/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted DLL overhead only: both versions use their test status implementation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "../../api/v2/packetbridge.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
typedef PB_RESULT (WINAPI *FACTORY)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (*OPEN)(const PB_CONFIG *,PB_SESSION **,void **,void *,uint64_t *);
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);HMODULE module=LoadLibraryW(argv[1]);CHECK(module);
    FACTORY factory;OPEN open;*(FARPROC *)&factory=GetProcAddress(module,"PbGetInterfaceV2");
    *(FARPROC *)&open=GetProcAddress(module,"PbTestOpen");CHECK(factory&&open);
    PB_INTERFACE_V2 api;CHECK(factory(2,sizeof(api),&api)==PB_OK);
    PB_CONFIG config={sizeof(config),1,4,4,0,0};PB_SESSION *s;void *region;uint64_t epoch,opaqueLayout[64];
    /* The frozen DLL has a different data-layout minor; no layout fields are interpreted here. */
    CHECK(open(&config,&s,&region,opaqueLayout,&epoch)==PB_OK);PB_USER_QUEUE *q;CHECK(api.OpenQueue(s,0,&q)==PB_OK);
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    DWORD_PTR cpu=allowed&(~allowed+1);CHECK(cpu&&SetThreadAffinityMask(GetCurrentThread(),cpu));
    for(ULONG i=0;i<10000;++i)CHECK(api.Wait(q,0)==PB_EMPTY);
    LARGE_INTEGER freq,start,end;ULONG64 first,last;CHECK(QueryPerformanceFrequency(&freq));
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&first)&&QueryPerformanceCounter(&start));
    const ULONG iterations=200000;
    for(ULONG i=0;i<iterations;++i)CHECK(api.Wait(q,0)==PB_EMPTY);
    CHECK(QueryPerformanceCounter(&end)&&QueryThreadCycleTime(GetCurrentThread(),&last));
    double ns=(double)(end.QuadPart-start.QuadPart)*1e9/(double)freq.QuadPart/iterations;
    printf("{\"iterations\":%lu,\"ns_per_call\":%.3f,\"cycles_per_call\":%.3f,\"affinity\":%llu}\n",iterations,ns,(double)(last-first)/iterations,(unsigned long long)cpu);
    CHECK(api.Close(s,5000)==PB_OK&&FreeLibrary(module));return 0;
}
