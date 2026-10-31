/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Send with identical hosted fixture checks/counters and synthetic DDIs.
 * Includes recycle for admitted sends; never a native kernel CPU measurement. */
#define main PbSendRegressionMain
#include "../model/send_window_tests.c"
#undef main

int main(VOID)
{
    DWORD_PTR allowed,system;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    DWORD_PTR cpu=allowed&(~allowed+1);CHECK(cpu && SetThreadAffinityMask(GetCurrentThread(),cpu));
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));
    const ULONG batches[]={1,8,32};
    for(ULONG phase=0;phase<3;++phase) for(ULONG ci=0;ci<(phase==2?1u:3u);++ci) {
        ULONG batch=batches[ci];Setup(1,64,128);
        NET_BUFFER buffers[PB_MAX_BATCH]={0};NET_BUFFER_LIST lists[PB_MAX_BATCH]={0};
        for(ULONG i=0;i<batch;++i) {buffers[i].Length=64;lists[i].Nb=buffers+i;lists[i].Next=i+1<batch?lists+i+1:NULL;}
        if(phase==1) {Current->Running=0;ExpectedPin=0;}
        if(phase==2) {Current->V2=NULL;ExpectedPin=0;}
        const ULONG warmup=2000,iterations=200000;
        LARGE_INTEGER start,end;ULONG64 first,last;
        for(ULONG i=0;i<warmup;++i) {CHECK(PbV2Send(Current,lists)==(phase!=2));if(!phase) Recycle();}
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&first) && QueryPerformanceCounter(&start));
        for(ULONG i=0;i<iterations;++i) {CHECK(PbV2Send(Current,lists)==(phase!=2));if(!phase) Recycle();}
        CHECK(QueryPerformanceCounter(&end) && QueryThreadCycleTime(GetCurrentThread(),&last));
        for(ULONG i=0;i<batch && phase!=2;++i) CHECK(lists[i].Status==(phase?NDIS_STATUS_PAUSED:NDIS_STATUS_SUCCESS));
        CHECK(Completed==(phase==2?0u:(iterations+warmup)*batch));
        CHECK(Reads==(phase?0u:(iterations+warmup)*batch) && !Session->Active && !Session->Returns.Value);
        printf("{\"case\":\"%s\",\"batch\":%lu,\"iterations\":%lu,\"ns_per_send\":%.3f,\"cycles_per_send\":%.3f,\"window_bytes\":%zu}\n",
            phase==0?"send-recycle":phase==1?"stopped":"no-session",batch,iterations,
            (double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/iterations,(double)(last-first)/iterations,sizeof(PB_TX_WINDOW));
        Destroy();
    }
    return 0;
}
