/* SPDX-License-Identifier: GPL-2.0 */
/* Operation/byte accounting for32 ordinary valid IP packets, not a timing or
 * kernel-performance benchmark. Original hosted NT_ASSERT=CHECK stays active. */
#ifdef PB_API_TESTS
#error TUN32 SYS requires actual production paths
#endif
#ifndef NDEBUG
#error TUN32 SYS requires NDEBUG configuration
#endif
#ifndef PB_TEST_IP_VERSION
#define PB_TEST_IP_VERSION 0
#endif
#if PB_TEST_IP_VERSION != 0 && PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error TUN32 SYS requires IP version 0, 4 or 6
#endif
#define T32_FIRST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 4)
#define T32_LAST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 6)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 SYS requires shipping fingerprint");
#ifdef PB_TUN32_SEND
#include PB_TUN32_SEND_HOST
#else
#include "../../common/v2_core.h"
static PB_RESULT T32CompletePushRing(PB_QUEUE_VIEW *,const PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t,uint32_t *,uint32_t *);
#define PbQueuePushRing T32CompletePushRing
#define main UnusedDrainMain
#include PB_TUN32_RECEIVE_HOST
#undef main
#undef PbQueuePushRing
#endif
static ULONG T32Cases,T32FrameOwners;

static VOID T32W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID T32W32(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>24);p[1]=(UCHAR)(n>>16);p[2]=(UCHAR)(n>>8);p[3]=(UCHAR)n;}
static VOID T32Packet(UCHAR *p,ULONG ip,ULONG protocol,ULONG index,ULONG queues,ULONG target)
{
    const ULONG length=1500;ULONG offset=ip==4?20:40,head=offset+(protocol==6?20:8);memset(p,0,length);
    if(ip==4){p[0]=0x45;T32W16(p+2,length);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=10;p[17]=1;p[19]=1;}
    else{p[0]=0x60;T32W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[10]=p[26]=0x0d;p[11]=p[27]=0xb8;p[39]=1;}
    BOOL found=FALSE;for(ULONG suffix=1;suffix<256;++suffix){p[ip==4?15:23]=(UCHAR)suffix;if(PbSelectQueue(p,length,queues)==target){found=TRUE;break;}}CHECK(found);
    T32W16(p+offset,4000);T32W16(p+offset+2,5000);
    if(protocol==6){T32W32(p+offset+4,1000+index*(length-head));p[offset+12]=0x50;p[offset+13]=0x10;T32W16(p+offset+14,8192);}
    else T32W16(p+offset+4,length-offset);
    memset(p+head,(int)(0x30+index),length-head);CHECK(PbFinalizeChecksums(p,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
}

#ifdef PB_TUN32_SEND
static ULONG T32PushCalls,T32Pushed,T32Notify,T32CompleteCalls,T32Batch[32],T32Queues[32];
static PB_RESULT T32Push(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    CHECK(T32PushCalls<32);ULONG at=T32PushCalls++;T32Batch[at]=count;T32Queues[at]=items[0].Token.Queue;
    PB_RESULT result=PbQueuePush(view,items,count,accepted,notify);CHECK(result==PB_OK && *accepted==count);T32Pushed+=*accepted;T32Notify+=!!*notify;return result;
}
static VOID T32Complete(NDIS_HANDLE adapter,NET_BUFFER_LIST *lists){++T32CompleteCalls;PbCompleteSend(adapter,lists);}
/* The fixture names the real address-only selector ActualSelectQueue. */



static VOID T32Send(ULONG ip,ULONG protocol,BOOL multi,ULONG queues,ULONG version)
{
#if PB_TEST_IP_VERSION
    CHECK(ip==PB_TEST_IP_VERSION);
#endif
    Setup(queues,64,128);RealEthernet=RealMdl=TRUE;HostedNdisVersion=version;FutureInfoReads=0;T32PushCalls=T32Pushed=T32Notify=T32CompleteCalls=0;
    UCHAR *frames[32];MDL mdls[32][4];NET_BUFFER buffers[32];NET_BUFFER_LIST lists[32];memset(buffers,0,sizeof(buffers));memset(lists,0,sizeof(lists));
    for(ULONG q=0;q<queues;++q){uint32_t ready;CHECK(PbQueueArm(Consumers+q,&ready)==PB_OK && !ready);}
    for(ULONG i=0;i<32;++i){frames[i]=PbTestGuardAlloc(1514);T32FrameOwners++;memset(frames[i],0,1514);frames[i][0]=frames[i][6]=2;T32W16(frames[i]+12,ip==4?0x0800:0x86dd);
        T32Packet(frames[i]+14,ip,protocol,i,queues,i%queues);SourceCreate(mdls[i],frames[i],1514);
        buffers[i]=(NET_BUFFER){.Length=1514,.Queue=i%queues,.Frame=frames[i],.Mdl=mdls[i],.MdlOffset=7};
        if(multi){lists[i].Nb=buffers+i;lists[i].Next=i+1<32?lists+i+1:NULL;}else buffers[i].Next=i+1<32?buffers+i+1:NULL;
    }
    if(!multi)lists[0].Nb=buffers;
    CHECK(PbV2Send(Current,lists) && Completed==(multi?32u:1u) && T32CompleteCalls==1 && AfterPinRelease==1 && !Session->Active && !Session->Returns.Value && !Faults);
    CHECK(Describes==32 && Reserves==32 && Reads==32 && PrefixCalls==32 && !Controls && !Replies && Session->TxObjects==32 && PrefixBytes==32u*59 && PayloadBytes==32u*1500 && PayloadMaps==96 && PrefixMaps==64 && T32Pushed==32 && T32PushCalls==(queues==1?1u:32u) && T32Notify==queues);
    CHECK(Windows==2 && MaxWork==32 && MaxBytes==32u*1514 && TxLockAcquires==(queues==1?1u:32u) && (version>=0x60053 || !FutureInfoReads));
    for(ULONG i=0;i<(multi?32u:1u);++i)CHECK(lists[i].Status==NDIS_STATUS_SUCCESS);
    for(ULONG i=0;i<T32PushCalls;++i)CHECK(T32Batch[i]==(queues==1?32u:1u) && T32Queues[i]==(queues==1?0u:i%queues));
    for(ULONG q=0;q<queues;++q){PB_DESCRIPTOR items[32];uint32_t n,accepted,notify;
        CHECK(PbQueuePop(Consumers+q,items,32,&n)==PB_OK && n==32/queues);
        for(ULONG j=0;j<n;++j){ULONG i=q+j*queues;CHECK(items[j].Length==1500 && items[j].Token.Queue==q && items[j].Token.Slot==j && items[j].Token.Generation==1 && !items[j].Type && !items[j].Flags && !memcmp(Session->Region+items[j].DataOffset,frames[i]+14,1500));}
        CHECK(PbQueuePush(Recyclers+q,items,n,&accepted,&notify)==PB_OK && accepted==n);KLOCK_QUEUE_HANDLE lock;
        KeAcquireInStackQueuedSpinLock(&Session->Queues[q].TxLock,&lock);CHECK(PbV2RecycleLocked(Session->Queues+q)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);
        for(ULONG i=0;i<64;++i)CHECK(Session->Queues[q].TxSlots[i].State==PB_SLOT_FREE);
    }
    for(ULONG i=0;i<32;++i){SourceFree(mdls[i]);PbTestGuardFree(frames[i]);CHECK(T32FrameOwners);T32FrameOwners--;}Destroy();CHECK(!HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees && !T32FrameOwners);
    T32Cases++;printf("CASE: SEND32 ip%lu protocol%lu multi%u queues%lu ndis%x pushCalls%lu descriptors%lu prefixBytes%llu payloadBytes%llu completeCalls%lu notify%lu\n",ip,protocol,(unsigned)multi,queues,version,T32PushCalls,T32Pushed,PrefixBytes,PayloadBytes,T32CompleteCalls,T32Notify);
}

#else
static ULONG T32Pops,T32Popped,T32Indications,T32Indicated,T32CompletePush,T32CompleteItems,T32HeaderCopies,T32DescriptorCopies,T32FullCopies,T32EthernetHeaders;
static ULONG64 T32HeaderBytes,T32DescriptorBytes;
static PB_RESULT T32Pop(PB_QUEUE_VIEW *view,PB_DESCRIPTOR *items,uint32_t count,uint32_t budget,uint32_t *received)
{T32Pops++;PB_RESULT result=PbQueuePopBudget(view,items,count,budget,received);T32Popped+=*received;return result;}
static PB_RESULT T32CompletePushRing(PB_QUEUE_VIEW *view,const PB_DESCRIPTOR *items,uint32_t cap,uint32_t head,uint32_t count,uint32_t *accepted,uint32_t *notify)
{T32CompletePush++;PB_RESULT result=PbQueuePushRing(view,items,cap,head,count,accepted,notify);T32CompleteItems+=*accepted;return result;}
static VOID T32Indicate(VOID *adapter,NET_BUFFER_LIST *lists,ULONG count,ULONG flags)
{T32Indications++;T32Indicated+=count;PbIndicateReceive(adapter,lists,count,flags);}
static VOID T32NblCopy(VOID *out,const VOID *input,SIZE_T length)
{if(length==40){T32HeaderCopies++;T32HeaderBytes+=length;}else if(length==sizeof(PB_DESCRIPTOR)){T32DescriptorCopies++;T32DescriptorBytes+=length;}else{T32FullCopies++;}memcpy(out,input,length);}
static VOID T32Ethernet(UCHAR *out,const UCHAR *local,const UCHAR *peer,const UCHAR *data,uint32_t length)
{T32EthernetHeaders++;PbEthernetHeader(out,local,peer,data,length);}
static VOID T32Receive(ULONG ip,ULONG protocol)
{
#if PB_TEST_IP_VERSION
    CHECK(ip==PB_TEST_IP_VERSION);
#endif
    FIXTURE *f=Create(1,64,128,0);f->Ctx.MiniportAdapterHandle=f;f->Ctx.Probe.LocalMac[0]=f->Ctx.Probe.PeerMac[0]=2;UnexpectedIndication=0;AutoConsume=FALSE;
    T32Pops=T32Popped=T32Indications=T32Indicated=T32CompletePush=T32CompleteItems=T32HeaderCopies=T32DescriptorCopies=T32FullCopies=T32EthernetHeaders=0;T32HeaderBytes=T32DescriptorBytes=0;
    PB_DESCRIPTOR items[32];for(ULONG i=0;i<32;++i){items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=1500,.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};T32Packet(f->S.Region+items[i].DataOffset,ip,protocol,i,1,0);}
    uint32_t count,notify;CHECK(PbQueuePush(&Submit,items,32,&count,&notify)==PB_OK && count==32);CHECK(ActualReceiveBatch(f->S.Queues) && Indicated);
    CHECK(T32Pops==1 && T32Popped==32 && T32Indications==1 && T32Indicated==32 && T32HeaderCopies==32 && T32HeaderBytes==1280 && T32DescriptorCopies==32 && T32DescriptorBytes==32*sizeof(PB_DESCRIPTOR) && !T32FullCopies && T32EthernetHeaders==32 && !SnapshotBytes && !EnabledMask);
    NET_BUFFER_LIST *nbl=Indicated;UCHAR expected[1500];for(ULONG i=0;i<32;++i){CHECK(nbl && nbl->Length==1514 && nbl->FirstMdl->Length==14 && nbl->FirstMdl->Next->Partial && nbl->FirstMdl->Next->Address==f->S.Region+items[i].DataOffset && nbl->FirstMdl->Next->Length==1500);
        /* Test oracle work is outside the instrumented production copies. */
        T32Packet(expected,ip,protocol,i,1,0);CHECK(!memcmp(nbl->FirstMdl->Next->Address,expected,sizeof(expected)));
        PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl)&~(ULONG_PTR)1);CHECK(record->Count==1 && record->Descriptors[0].Token.Slot==i && !memcmp(record->Descriptors,items+i,sizeof(*items)));nbl=nbl->Next;}
    CHECK(!nbl && f->S.Active==32 && Pools==32 && Mdls==64 && Nbls==32 && PbV2ReturnNbls(&f->Ctx,&Indicated,PB_MAX_BATCH) && !Indicated);
    CHECK(T32CompletePush==1 && T32CompleteItems==32 && !Pools && !Mdls && !Nbls && !f->S.Active && !f->S.Returns.References && !f->S.Queues[0].Credits);
    PB_DESCRIPTOR completed[32];CHECK(PbQueuePop(&Complete,completed,32,&count)==PB_OK && count==32);for(ULONG i=0;i<32;++i)CHECK(completed[i].Token.Slot==i && completed[i].Status==PB_OK);
    Destroy(f);T32Cases++;printf("CASE: RECEIVE32 ip%lu protocol%lu popCalls%lu descriptors%lu indications%lu nbls%lu prefixBytes%llu recordBytes%llu fullPayloadCopies%lu ethHeaders%lu completionPush%lu\n",ip,protocol,T32Pops,T32Popped,T32Indications,T32Indicated,T32HeaderBytes,T32DescriptorBytes,T32FullCopies,T32EthernetHeaders,T32CompletePush);
}

#endif
static VOID T32Matrix(VOID)
{
#ifdef PB_TUN32_SEND
    const ULONG queues[]={1,2,4,8},versions[]={0x60014,0x60059};
    for(ULONG ip=T32_FIRST_IP;ip<=T32_LAST_IP;ip+=2)for(ULONG p=0;p<2;++p)for(ULONG multi=0;multi<2;++multi)for(ULONG q=0;q<4;++q)for(ULONG v=0;v<2;++v)T32Send(ip,p?17:6,multi,queues[q],versions[v]);
#else
    for(ULONG ip=T32_FIRST_IP;ip<=T32_LAST_IP;ip+=2)for(ULONG p=0;p<2;++p)T32Receive(ip,p?17:6);
#endif
}
int main(VOID)
{
#if PB_TEST_IP_VERSION
    printf("FAMILY: IPv%u only\n",(unsigned)PB_TEST_IP_VERSION);
#endif
    DWORD before,after;T32Matrix();ULONG warm=T32Cases;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    T32Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after && T32Cases==2*warm && !T32FrameOwners);
#ifdef PB_TUN32_SEND
    CHECK(warm==(PB_TEST_IP_VERSION?32u:64u) && !HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees);
    printf("PASS: TUN32 SYS send cases%lu warm%lu measure%lu handles%lu/%lu; copied source mappings%lu/%lu, all tokens references and owners reclaimed; no test threads\n",T32Cases,warm,T32Cases-warm,before,after,SourceAllocs,SourceFrees);
#else
    CHECK(warm==(PB_TEST_IP_VERSION?2u:4u) && !Current && !Pools && !Mdls && !Nbls);
    printf("PASS: TUN32 SYS receive cases%lu warm%lu measure%lu handles%lu/%lu; all NBL MDL pool reference credit owners reclaimed; no test threads\n",T32Cases,warm,T32Cases-warm,before,after);
#endif
    return 0;
}
