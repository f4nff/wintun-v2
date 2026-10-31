/* SPDX-License-Identifier: GPL-2.0 */
/* Actual DLL RX reserve/commit/poll cycle with real Windows event calls.
 * The hosted peer copies descriptors only; no driver I/O or NDIS timing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/test.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
static PB_INTERFACE_V2 Api;
static ULONG64 Checksum;
static VOID Cycle(PB_USER_QUEUE *q,PB_QUEUE_VIEW *submit,PB_QUEUE_VIEW *complete,ULONG count,ULONG iterations,BOOLEAN reverse)
{
    PB_LEASE leases[PB_MAX_BATCH];PB_DESCRIPTOR input[PB_MAX_BATCH],output[PB_MAX_BATCH],items[PB_MAX_BATCH];
    const ULONG statuses[]={PB_OK,PB_INVALID,PB_RX_REJECTED,PB_STOPPED};
    for(ULONG i=0;i<iterations;++i) {
        uint32_t n,notify;
        CHECK(Api.ReserveBatch(q,leases,count,&n)==PB_OK && n==count);
        for(ULONG j=0;j<count;++j) leases[j].Descriptor.Length=20;
        CHECK(Api.CommitBatch(q,leases,count,&n)==PB_OK && n==count);
        /* Match the kernel submission reader, so an ordinary-pop change is
         * timed only in the actual DLL completion consumer below. */
        CHECK(PbQueuePopBudget(submit,input,count,128u*1024u,&n)==PB_OK && n==count);
        for(ULONG j=0;j<count;++j) {items[j]=input[reverse?count-1-j:j];items[j].Status=statuses[j%4];}
        CHECK(PbQueuePush(complete,items,count,&n,&notify)==PB_OK && n==count);
        CHECK(Api.PollCompletions(q,output,count,&n)==PB_OK && n==count);
        Checksum+=output[n-1].Token.Generation+output[n-1].Status;
    }
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK);
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    DWORD_PTR cpu=allowed&(~allowed+1);CHECK(cpu && SetThreadAffinityMask(GetCurrentThread(),cpu));
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));const ULONG counts[]={1,2,4,8,16,32};
    for(ULONG reverse=0;reverse<2;++reverse) for(ULONG ci=0;ci<sizeof(counts)/sizeof(counts[0]);++ci) {
        ULONG count=counts[ci];PB_CONFIG config={sizeof(config),1,32,64,0,0};PB_SESSION *s;PB_USER_QUEUE *q;
        VOID *region;PB_LAYOUT layout;uint64_t epoch;CHECK(PbTestOpen(&config,&s,&region,&layout,&epoch)==PB_OK && Api.OpenQueue(s,0,&q)==PB_OK);
        PB_QUEUE_VIEW submit,complete;
        CHECK(PbAttachQueue(&submit,region,&layout,0,PB_QUEUE_RX_SUBMIT,0)==PB_OK);
        CHECK(PbAttachQueue(&complete,region,&layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
        const ULONG warmup=2000,iterations=100000;Cycle(q,&submit,&complete,count,warmup,(BOOLEAN)reverse);
        LARGE_INTEGER start,end;ULONG64 first,last;
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&first) && QueryPerformanceCounter(&start));
        Cycle(q,&submit,&complete,count,iterations,(BOOLEAN)reverse);
        CHECK(QueryPerformanceCounter(&end) && QueryThreadCycleTime(GetCurrentThread(),&last));
        printf("{\"case\":\"completion-cycle\",\"reverse\":%lu,\"batch\":%lu,\"iterations\":%lu,\"ns_per_batch\":%.3f,\"cycles_per_batch\":%.3f,\"affinity\":%llu,\"checksum\":%llu}\n",
            reverse,count,iterations,(double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/iterations,(double)(last-first)/iterations,
            (unsigned long long)cpu,(unsigned long long)Checksum);
        CHECK(Api.Close(s,5000)==PB_OK);
    }
    return 0;
}
