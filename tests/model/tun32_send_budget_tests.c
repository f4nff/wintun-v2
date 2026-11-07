/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Read/Describe/Send at32 packet byte-budget and admission boundaries. */
#ifdef PB_API_TESTS
#error TUN32 send budget requires actual production paths
#endif
#ifndef NDEBUG
#error TUN32 send budget requires NDEBUG configuration
#endif
#ifndef PB_TEST_IP_VERSION
#define PB_TEST_IP_VERSION 0
#endif
#if PB_TEST_IP_VERSION != 0 && PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error TUN32 send budget requires IP version 0, 4 or 6
#endif
#define B32_FIRST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 4)
#define B32_LAST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 6)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 send budget requires shipping fingerprint");
#include PB_TUN32_SEND_HOST
static ULONG B32Cases,B32Pushes,B32Published,B32Completes,B32Batch[32],B32FrameOwners;
static PB_RESULT T32Push(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    CHECK(B32Pushes<32);B32Batch[B32Pushes++]=count;
    PB_RESULT result=PbQueuePush(view,items,count,accepted,notify);
    CHECK(result==PB_OK && *accepted==count);B32Published+=*accepted;return result;
}
static VOID T32Complete(NDIS_HANDLE adapter,NET_BUFFER_LIST *lists){B32Completes++;PbCompleteSend(adapter,lists);}
static VOID B32W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID B32W32(UCHAR *p,ULONG n){for(ULONG i=0;i<4;++i)p[i]=(UCHAR)(n>>(24-8*i));}
static VOID B32W64(UCHAR *p,ULONG64 n){for(ULONG i=0;i<8;++i)p[i]=(UCHAR)(n>>(56-8*i));}
static VOID B32Packet(UCHAR *p,ULONG length,ULONG ip,ULONG protocol,ULONG index)
{
    ULONG offset=ip==4?20:40,header=offset+(protocol==6?20:8);memset(p,0,length);
    if(ip==4){p[0]=0x45;B32W16(p+2,length);B32W16(p+4,index);p[8]=64;p[9]=(UCHAR)protocol;p[12]=p[16]=10;p[15]=1;p[19]=2;}
    else{p[0]=0x60;B32W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[10]=p[26]=0x0d;p[11]=p[27]=0xb8;p[23]=1;p[39]=2;}
    B32W16(p+offset,4000);B32W16(p+offset+2,5000);
    if(protocol==6){B32W32(p+offset+4,0xfffffff0u+index*(length-header));p[offset+12]=0x50;p[offset+13]=0x10;B32W16(p+offset+14,8192);}
    else B32W16(p+offset+4,length-offset);
    memset(p+header,(int)(0x30+index),length-header);B32W32(p+header,index);
    /* Opaque application fields: no packet scheduling clock is introduced. */
    B32W64(p+header+4,UINT64_C(0x0123456789abcdef));B32W64(p+header+12,UINT64_C(0xfedcba9876543210));
    CHECK(PbFinalizeChecksums(p,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
}
static VOID B32Case(ULONG ip,ULONG protocol,BOOL multi,ULONG version,ULONG length,ULONG action)
{
#if PB_TEST_IP_VERSION
    CHECK(ip==PB_TEST_IP_VERSION);
#endif
    CHECK(!action || length==4083);Setup(1,64,128);RealEthernet=RealMdl=TRUE;HostedNdisVersion=version;FutureInfoReads=0;
    B32Pushes=B32Published=B32Completes=0;memset(B32Batch,0,sizeof(B32Batch));
    ULONG frameLength=length+14,perWindow=PB_TX_BYTE_BUDGET/min(frameLength,PB_BLOCK_SIZE),expected=action?31:32;
    CHECK(perWindow && perWindow<=32);if(action){Trigger=31;Action=action;}
    UCHAR *frames[32];MDL mdls[32][4];NET_BUFFER buffers[32];NET_BUFFER_LIST lists[32];memset(buffers,0,sizeof(buffers));memset(lists,0,sizeof(lists));
    uint32_t ready;CHECK(PbQueueArm(Consumers,&ready)==PB_OK && !ready);
    for(ULONG i=0;i<32;++i){frames[i]=PbTestGuardAlloc(frameLength);B32FrameOwners++;memset(frames[i],0,14);frames[i][0]=frames[i][6]=2;B32W16(frames[i]+12,ip==4?0x0800:0x86dd);
        B32Packet(frames[i]+14,length,ip,protocol,i);SourceCreate(mdls[i],frames[i],frameLength);
        buffers[i]=(NET_BUFFER){.Length=frameLength,.Frame=frames[i],.Mdl=mdls[i],.MdlOffset=7};
        if(multi){lists[i].Nb=buffers+i;lists[i].Next=i+1<32?lists+i+1:NULL;}else buffers[i].Next=i+1<32?buffers+i+1:NULL;
    }
    if(!multi)lists[0].Nb=buffers;
    CHECK(PbV2Send(Current,lists) && Completed==(multi?32u:1u) && B32Completes==1 && AfterPinRelease==1 && !Session->Active && !Session->Returns.Value);
    CHECK(Reads==expected && PrefixCalls==expected && Reserves==expected && Describes==expected && !Controls && !Replies && Session->TxObjects==expected);
    CHECK(PrefixBytes==(ULONG64)expected*59 && PayloadBytes==(ULONG64)expected*length && PrefixMaps==2*expected && PayloadMaps==3*expected);
    CHECK(B32Published==expected && B32Pushes==(expected+perWindow-1)/perWindow && TxLockAcquires==B32Pushes);
    for(ULONG i=0,remaining=expected;i<B32Pushes;++i){ULONG n=min(remaining,perWindow);CHECK(B32Batch[i]==n);remaining-=n;}
    CHECK(MaxWork==perWindow && MaxBytes==perWindow*min(frameLength,PB_BLOCK_SIZE) && Windows==B32Pushes+1+(action?1:0));
    CHECK(version>=0x60053 || !FutureInfoReads);CHECK(Faults==(action==4?1u:0u));
    for(ULONG i=0;i<(multi?32u:1u);++i){NDIS_STATUS status=action && (!multi || i==31)?(action==2?NDIS_STATUS_MEDIA_DISCONNECTED:NDIS_STATUS_PAUSED):NDIS_STATUS_SUCCESS;CHECK(lists[i].Status==status);}
    PB_DESCRIPTOR items[32];uint32_t count,accepted,notify;CHECK(PbQueuePop(Consumers,items,32,&count)==PB_OK && count==expected);
    for(ULONG i=0;i<count;++i)CHECK(items[i].Length==length && items[i].Token.Slot==i && items[i].Token.Generation==1 && !items[i].Type && !items[i].Flags && !memcmp(Session->Region+items[i].DataOffset,frames[i]+14,length));
    CHECK(PbQueuePush(Recyclers,items,count,&accepted,&notify)==PB_OK && accepted==count);KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&Session->Queues[0].TxLock,&lock);CHECK(PbV2RecycleLocked(Session->Queues)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);
    for(ULONG i=0;i<64;++i)CHECK(Session->Queues[0].TxSlots[i].State==PB_SLOT_FREE);
    for(ULONG i=0;i<32;++i){SourceFree(mdls[i]);PbTestGuardFree(frames[i]);CHECK(B32FrameOwners);B32FrameOwners--;}
    Destroy();CHECK(!HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees && !B32FrameOwners);
    B32Cases++;printf("CASE: BUDGET32 ip%lu protocol%lu multi%u ndis%x length%lu action%lu published%lu pushes%lu prefix%llu payload%llu\n",ip,protocol,(unsigned)multi,version,length,action,expected,B32Pushes,PrefixBytes,PayloadBytes);
}
static VOID B32Matrix(VOID)
{
    const ULONG lengths[]={4082,4083,65535},versions[]={0x60014,0x60059};
    for(ULONG ip=B32_FIRST_IP;ip<=B32_LAST_IP;ip+=2)for(ULONG p=0;p<2;++p)for(ULONG multi=0;multi<2;++multi)for(ULONG v=0;v<2;++v){
        for(ULONG n=0;n<3;++n)B32Case(ip,p?17:6,multi,versions[v],lengths[n],0);
        B32Case(ip,p?17:6,multi,versions[v],4083,1);B32Case(ip,p?17:6,multi,versions[v],4083,2);B32Case(ip,p?17:6,multi,versions[v],4083,4);
    }
}
int main(VOID)
{
#if PB_TEST_IP_VERSION
    printf("FAMILY: IPv%u only\n",(unsigned)PB_TEST_IP_VERSION);
#endif
    DWORD before,after;ULONG warm=PB_TEST_IP_VERSION?48u:96u;B32Matrix();CHECK(B32Cases==warm && GetProcessHandleCount(GetCurrentProcess(),&before));B32Matrix();
    CHECK(B32Cases==2*warm && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    CHECK(!B32FrameOwners && !HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees);
    printf("PASS: TUN32 send budget cases%lu warm%lu measure%lu handles%lu/%lu mappings%lu/%lu; exact byte budgets, prefix ownership and no test threads\n",B32Cases,warm,warm,before,after,SourceAllocs,SourceFrees);return 0;
}
