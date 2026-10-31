/* SPDX-License-Identifier: GPL-2.0 */
/* Actual receive/return/core with the same hosted allocation/DDI checks. This
 * measures a hosted ACK workload, not native NDIS or kernel CPU utilization. */
#define main ReceiveRegressionCases
#include "../model/drain_boundary_tests.c"
#undef main
int main(VOID)
{
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    CHECK(SetThreadAffinityMask(GetCurrentThread(),allowed&(~allowed+1)));
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));
    for(ULONG test=0;test<3;++test) {
        ULONG batch=test?32:1;FIXTURE *f=Create(1,32,32,0);PB_KERNEL_QUEUE *q=f->S.Queues;
        PB_DESCRIPTOR items[32]={0},completed[32];uint32_t count,notify;
        EnabledMask=test==2?PB_RX_RSC4:0;UnexpectedIndication=0;ReturnDuringIndication=TRUE;
        if(test==2) {q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG b=0;b<2;++b) q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);}
        for(ULONG i=0;i<batch;++i) {
            items[i]=(PB_DESCRIPTOR){.Token={42,0,(USHORT)i,0},.Length=40,.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
            UCHAR *packet=f->S.Region+items[i].DataOffset;memset(packet,0,40);
            packet[0]=0x45;packet[3]=40;packet[8]=64;packet[9]=6;packet[12]=10;packet[16]=11;packet[21]=1;packet[23]=2;packet[32]=0x50;packet[33]=0x10;
            CHECK(PbFinalizeChecksums(packet,40,TRUE,TRUE,FALSE)==PB_PACKET_OK);
        }
        const ULONG warm=5000,iterations=100000;LARGE_INTEGER start={0},end;ULONG64 first=0,last;
        for(ULONG loop=0;loop<warm+iterations;++loop) {
            if(loop==warm) CHECK(QueryPerformanceCounter(&start) && QueryThreadCycleTime(GetCurrentThread(),&first));
            for(ULONG i=0;i<batch;++i) items[i].Token.Generation=loop+1;
            CHECK(PbQueuePush(&Submit,items,batch,&count,&notify)==PB_OK && count==batch);
            CHECK(ActualReceiveBatch(q) && !Indicated && !q->Credits && !q->PendingCount && !f->S.Active);
            CHECK(PbQueuePop(&Complete,completed,32,&count)==PB_OK && count==batch && !Pools && !Mdls && !Nbls);
        }
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&last) && QueryPerformanceCounter(&end));
        printf("{\"case\":\"%s\",\"batch\":%lu,\"iterations\":%lu,\"ns_per_receive\":%.3f,\"cycles_per_receive\":%.3f}\n",
            test==2?"rsc-pure-ack":"plain-ack",batch,iterations,(double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/iterations,(double)(last-first)/iterations);
        if(test==2) {PbTestGuardFree(q->Scratch);for(ULONG b=0;b<2;++b) {CHECK(!q->CoalesceBusy[b]);PbTestGuardFree(q->CoalesceBuffers[b]);}}
        Destroy(f);
    }
    return 0;
}
