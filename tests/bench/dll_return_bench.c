/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted producer and DLL return-cycle costs, never kernel CPU measurements. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../api/v2/packetbridge.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
PB_RESULT PbTestOpen(const PB_CONFIG *,PB_SESSION **,void **,PB_LAYOUT *,uint64_t *);
static PB_INTERFACE_V2 Api;
static ULONG64 Checksum;
static PB_RESULT Publish(PB_QUEUE_VIEW *v,const PB_TOKEN *tokens,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
#ifdef PB_TOKEN_PUBLISH_BASELINE
    PB_DESCRIPTOR items[PB_MAX_BATCH]={0};
    for(ULONG i=0;i<count;++i) items[i].Token=tokens[i];
    return PbQueuePush(v,items,count,accepted,notify);
#else
    return PbQueuePushTokens(v,tokens,count,accepted,notify);
#endif
}
static VOID RingCycles(PB_QUEUE_VIEW *p,PB_QUEUE_VIEW *c,ULONG count,ULONG iterations)
{
    PB_TOKEN input[PB_MAX_BATCH],output[PB_MAX_BATCH];for(ULONG i=0;i<count;++i) input[i]=(PB_TOKEN){42,i+1,(uint16_t)i,0};
    for(ULONG i=0;i<iterations;++i) {
        uint32_t n,notify;
        CHECK(Publish(p,input,count,&n,&notify)==PB_OK && n==count);
        CHECK(PbQueuePopTokens(c,output,count,&n)==PB_OK && n==count);
        Checksum+=output[n-1].Generation;
    }
}
static VOID DllCycles(PB_USER_QUEUE *q,PB_QUEUE_VIEW *ready,PB_QUEUE_VIEW *recycle,PB_DESCRIPTOR *items,ULONG count,ULONG iterations)
{
    PB_LEASE leases[PB_MAX_BATCH];PB_TOKEN tokens[PB_MAX_BATCH],returned[PB_MAX_BATCH];
    for(ULONG i=0;i<iterations;++i) {
        uint32_t n,notify;
        for(ULONG j=0;j<count;++j) items[j].Token.Generation++;
        CHECK(PbQueuePush(ready,items,count,&n,&notify)==PB_OK && n==count);
        CHECK(Api.ReceiveBatch(q,leases,count,&n)==PB_OK && n==count);
        for(ULONG j=0;j<count;++j) tokens[j]=leases[j].Descriptor.Token;
        CHECK(Api.ReleaseBatch(q,tokens,count,&n)==PB_OK && n==count);
        CHECK(PbQueuePopTokens(recycle,returned,count,&n)==PB_OK && n==count);
        Checksum+=returned[n-1].Generation;
    }
}
static VOID DescriptorCycles(PB_QUEUE_VIEW *p,PB_QUEUE_VIEW *c,ULONG count,ULONG iterations,BOOL circular)
{
    PB_DESCRIPTOR items[PB_MAX_BATCH];PB_TOKEN output[PB_MAX_BATCH];
    for(ULONG i=0;i<PB_MAX_BATCH;++i) {memset(items+i,0xa5,sizeof(*items));items[i].Token.Generation=i+1;}
    for(ULONG i=0;i<iterations;++i) {
        uint32_t n,notify;
        PB_RESULT r=circular?PbQueuePushRing(p,items,PB_MAX_BATCH,PB_MAX_BATCH-1,count,&n,&notify):PbQueuePush(p,items,count,&n,&notify);
        CHECK(r==PB_OK && n==count);
        CHECK(PbQueuePopTokens(c,output,count,&n)==PB_OK && n==count);Checksum+=output[n-1].Generation;
    }
}
int main(int argc,char **argv)
{
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    DWORD_PTR cpu=allowed&(~allowed+1);CHECK(cpu && SetThreadAffinityMask(GetCurrentThread(),cpu));
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK);
    LARGE_INTEGER frequency;CHECK(QueryPerformanceFrequency(&frequency));
    const ULONG counts[]={1,2,4,8,16,32,64};
    const char *patterns[]={"sequential","stride-64","interleaved-64"};
    ULONG patternCount=argc==2 && !strcmp(argv[1],"--slot-patterns")?3:1;
    for(ULONG pattern=0;pattern<patternCount;++pattern) for(ULONG ci=0;ci<sizeof(counts)/sizeof(counts[0]);++ci) {
        ULONG count=counts[ci],slots=count>32?64:32;
        PB_CONFIG config={sizeof(config),1,pattern?512:slots,pattern?512:slots,0,0};PB_SESSION *s;PB_USER_QUEUE *q;
        VOID *region;PB_LAYOUT layout;uint64_t epoch;CHECK(PbTestOpen(&config,&s,&region,&layout,&epoch)==PB_OK && Api.OpenQueue(s,0,&q)==PB_OK);
        PB_QUEUE_VIEW ready,recycle,p,c;
        CHECK(PbAttachQueue(&ready,region,&layout,0,PB_QUEUE_TX_READY,1)==PB_OK);
        CHECK(PbAttachQueue(&recycle,region,&layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
        CHECK(PbAttachQueue(&p,region,&layout,0,PB_QUEUE_RX_SUBMIT,1)==PB_OK);
        CHECK(PbAttachQueue(&c,region,&layout,0,PB_QUEUE_RX_SUBMIT,0)==PB_OK);
        PB_DESCRIPTOR items[PB_MAX_BATCH]={0};
        for(ULONG i=0;i<count;++i) {
            ULONG slot=pattern==1?(i&7)*64+(i>>3):pattern==2?
                (count>32?(i&7)+(i>>3)*64:(i&3)+(i>>2)*64):i;
            items[i].Token=(PB_TOKEN){epoch,0,(uint16_t)slot,0};items[i].Length=20;items[i].DataOffset=layout.Queues[0].TxData+slot*PB_BLOCK_SIZE;
        }
        const char *names[]={"producer-cycle","dll-cycle","descriptor-cycle","pending-cycle"};
        for(ULONG mode=0;mode<4;++mode) {
            if(pattern && mode!=1) continue;
            const ULONG warmup=2000,iterations=100000;LARGE_INTEGER start,end;ULONG64 first,last;
            if(mode==0) RingCycles(&p,&c,count,warmup);
            else if(mode==1) DllCycles(q,&ready,&recycle,items,count,warmup);
            else DescriptorCycles(&p,&c,count,warmup,mode==3);
            CHECK(QueryThreadCycleTime(GetCurrentThread(),&first) && QueryPerformanceCounter(&start));
            if(mode==0) RingCycles(&p,&c,count,iterations);
            else if(mode==1) DllCycles(q,&ready,&recycle,items,count,iterations);
            else DescriptorCycles(&p,&c,count,iterations,mode==3);
            CHECK(QueryPerformanceCounter(&end) && QueryThreadCycleTime(GetCurrentThread(),&last));
            printf("{\"case\":\"%s\",\"pattern\":\"%s\",\"batch\":%lu,\"iterations\":%lu,\"ns_per_batch\":%.3f,\"cycles_per_batch\":%.3f,\"affinity\":%llu,\"checksum\":%llu}\n",
                names[mode],patterns[pattern],count,iterations,(double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/iterations,
                (double)(last-first)/iterations,(unsigned long long)cpu,(unsigned long long)Checksum);
        }
        CHECK(Api.Close(s,5000)==PB_OK);
    }
    return 0;
}
