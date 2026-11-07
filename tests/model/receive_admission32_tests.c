/* SPDX-License-Identifier: GPL-2.0 */
/* Defensive shared-wire admission, not a claim that the public Commit API
 * publishes malformed/duplicate tokens. All calls here are synchronous. */
#ifdef PB_API_TESTS
#error Shipping receive admission paths required
#endif
#ifndef NDEBUG
#error Shipping receive admission configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
static unsigned AdmissionNdis;
#define PB_NBL_VERSION (0x00060000u | AdmissionNdis)
#define main UnusedAdmissionDrainMain
#include PB_ADMISSION_HOST
#undef main
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"receive admission requires shipping fingerprint");
static ULONG AdmissionClaims,AdmissionInvalid,AdmissionStopped,AdmissionSeen[64];
static VOID ObserveAdmissionComplete(PB_KERNEL_QUEUE *q,const PB_DESCRIPTOR *d,ULONG status)
{
    CHECK(d->Token.Epoch==42 && d->Token.Queue==0 && d->Token.Slot<64 && d->Token.Generation==1);
    CHECK(!AdmissionSeen[d->Token.Slot]++);AdmissionClaims++;
    if(status==PB_INVALID) AdmissionInvalid++;
    else {CHECK(status==PB_STOPPED);AdmissionStopped++;}
    PbV2CompleteLocked(q,d,status);
}
static VOID A16(UCHAR *p,ULONG value){p[0]=(UCHAR)(value>>8);p[1]=(UCHAR)value;}
static VOID A32(UCHAR *p,ULONG value){A16(p,value>>16);A16(p+2,value);}
static ULONG AddBytes(ULONG sum,const UCHAR *p,ULONG length)
{for(ULONG i=0;i<length;i+=2) sum+=((ULONG)p[i]<<8)|(i+1<length?p[i+1]:0u);return sum;}
static USHORT FinishSum(ULONG sum){while(sum>>16) sum=(sum&65535u)+(sum>>16);return (USHORT)~sum;}
static ULONG Pseudo(const UCHAR *p,ULONG transport,ULONG protocol)
{return AddBytes(0,p+(PB_TEST_IP_VERSION==4?12:8),PB_TEST_IP_VERSION==4?8:32)+transport+protocol;}
static ULONG AdmissionPacket(UCHAR *p,ULONG protocol,ULONG flow,ULONG sequence,ULONG marker)
{
    ULONG family=PB_TEST_IP_VERSION,ip=family==4?20:40,l4=protocol==6?20:8,length=ip+l4+24;
    memset(p,0,128);p[0]=PB_TEST_IP_VERSION==4?0x45:0x60;
    if(family==4){A16(p+2,length);A16(p+4,0x7ff0u+marker);p[8]=61;p[9]=(UCHAR)protocol;p[12]=10;p[15]=1;p[16]=192;p[18]=2;p[19]=1;}
    else {A16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=61;p[8]=0x20;p[9]=1;p[10]=0x0d;p[11]=0xb8;p[23]=1;p[24]=0x20;p[25]=1;p[26]=0x0d;p[27]=0xb8;p[39]=2;}
    A16(p+ip,flow);A16(p+ip+2,5000);
    if(protocol==6){A32(p+ip+4,sequence);A32(p+ip+8,900);p[ip+12]=0x50;p[ip+13]=0x10;A16(p+ip+14,8192);}
    else A16(p+ip+4,l4+24);
    for(ULONG i=0;i<24;++i)p[ip+l4+i]=(UCHAR)(marker*17u+i);
    A32(p+ip+l4,marker);A32(p+ip+l4+4,0xfedcba98u^marker);A32(p+ip+l4+8,0x01234567u+marker);
    USHORT sum=FinishSum(AddBytes(Pseudo(p,l4+24,protocol),p+ip,l4+24));
    A16(p+ip+(protocol==6?16:6),sum?sum:65535u);
    if(family==4) A16(p+10,FinishSum(AddBytes(0,p,20)));
    CHECK(!FinishSum(AddBytes(Pseudo(p,l4+24,protocol),p+ip,l4+24)));
    return length;
}
static PB_DESCRIPTOR AdmissionItem(FIXTURE *f,ULONG slot,ULONG length)
{PB_DESCRIPTOR d={.Token={42,1,(USHORT)slot,0},.Length=length,.DataOffset=f->S.Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE};return d;}
static VOID CheckOld(FIXTURE *f,NET_BUFFER_LIST *old,UCHAR expected[32][128],ULONG length,ULONG protocol)
{
    ULONG originals=0,objects=0,ip=PB_TEST_IP_VERSION==4?20:40,l4=protocol==6?20:8;
    for(NET_BUFFER_LIST *nbl=old;nbl;nbl=nbl->Next){
        PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)nbl->Reserved[1]&~(ULONG_PTR)1);
        CHECK(record->Queue==f->S.Queues && !record->LocallyCancelled && record->Protocol==protocol && record->IpVersion==PB_TEST_IP_VERSION);
        ULONG count=objects?1:16;CHECK(record->Count==count && record->CoalesceIndex==(objects?MAXULONG:0));
        CHECK(nbl->Length==(objects?length:ip+l4+16*24)+PB_ETH_HEADER);
        for(ULONG i=0;i<count;++i){PB_DESCRIPTOR *d=record->Descriptors+i;CHECK(d->Token.Slot==originals && d->Token.Generation==1 && d->Token.Epoch==42);
            CHECK(f->S.Queues[0].RxSlots[originals].Active && f->S.Queues[0].RxSlots[originals].Generation==1);
            CHECK(!memcmp(f->S.Region+d->DataOffset,expected[originals],length));originals++;}
        UCHAR *bytes=record->Mdl->Address;
        if(!objects){
            CHECK(record->Mdl->Length==ip+l4+16*24 && !record->Mdl->Partial);
            for(ULONG i=0;i<16;++i) CHECK(!memcmp(bytes+ip+l4+i*24,expected[i]+ip+l4,24));
            CHECK((protocol==6?nbl->Info[TcpRecvSegCoalesceInfo]:nbl->Info[UdpRecvSegCoalesceOffloadInfo])!=NULL);
        }else CHECK(record->Mdl->Partial && !memcmp(bytes,expected[originals-1],length));
        if(AdmissionNdis<89) CHECK(nbl->Info[UdpRecvSegCoalesceOffloadInfo]==(VOID *)(ULONG_PTR)~0u);
        objects++;
    }
    CHECK(objects==17 && originals==32 && f->S.Active==17 && f->S.RxObjects==32 && !f->S.Returns.References);
}
static VOID AdmissionRun(ULONG protocol,ULONG shape,BOOLEAN wrap,BOOLEAN measured)
{
    AdmissionNdis=protocol==6?30:89;CHECK(PbPlatformOffloads(PB_NBL_VERSION)&(protocol==6?(PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6):PB_RX_URO));
    FIXTURE *f=Create(1,64,128,wrap?UINT64_MAX-15:0);PB_KERNEL_QUEUE *q=f->S.Queues;
    UCHAR expected[32][128];PB_DESCRIPTOR oldItems[32],fresh[32];uint32_t n,notify;
    EnabledMask=protocol==6?(PB_TEST_IP_VERSION==4?PB_RX_RSC4:PB_RX_RSC6):PB_RX_URO;
    q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG i=0;i<2;++i)q->CoalesceBuffers[i]=PbTestGuardAlloc(PB_BLOCK_SIZE);
    ULONG length=0;for(ULONG i=0;i<32;++i){length=AdmissionPacket(expected[i],protocol,i<16?4000:4000+i,0xfffffff0u+i*24,i);oldItems[i]=AdmissionItem(f,i,length);memcpy(f->S.Region+oldItems[i].DataOffset,expected[i],length);}
    UnexpectedIndication=0;CHECK(PbQueuePush(&Submit,oldItems,32,&n,&notify)==PB_OK && n==32 && ActualReceiveBatch(q));
    NET_BUFFER_LIST *old=Indicated;Indicated=NULL;CHECK(old && q->Credits==32 && !q->PendingCount);CheckOld(f,old,expected,length,protocol);
    memset(AdmissionSeen,0,sizeof(AdmissionSeen));AdmissionClaims=AdmissionInvalid=AdmissionStopped=0;
    for(ULONG i=0;i<32;++i){ULONG slot=shape==2?(i==15?32:i<15?32+i:31+i):32+i;UCHAR packet[128];ULONG size=AdmissionPacket(packet,protocol,6000+i,100+i*24,100+i);fresh[i]=AdmissionItem(f,slot,size);memcpy(f->S.Region+fresh[i].DataOffset,packet,size);}
    if(shape<2){fresh[15]=oldItems[0];fresh[15].Reserved16=1;if(shape==1)fresh[15].Token.Generation=2;}
    else {fresh[0].Reserved16=1;fresh[15]=fresh[0];fresh[15].Reserved16=0;}
    /* The observer only forwards calls in the actual ReceiveBatch region.
     * Fault handling retires private pending entries instead of publishing them. */
    CHECK(PbQueuePush(&Submit,fresh,32,&n,&notify)==PB_OK && n==32 && ActualReceiveBatch(q));
    CHECK(!Indicated && f->S.Faulted && f->S.Draining && !f->S.Completions && q->Credits==32 && !q->PendingCount);
    CHECK(AdmissionClaims==31 && AdmissionInvalid==(shape==2?1u:0u) && AdmissionStopped==(shape==2?30u:31u));
    CHECK(PbQueuePop(&Complete,fresh,32,&n)==PB_EMPTY && !n);CheckOld(f,old,expected,length,protocol);
    for(ULONG i=32;i<64;++i) CHECK(!q->RxSlots[i].Active);
    CHECK(q->RxSlots[0].Generation==1 && q->RxSlots[0].Active);
    if(shape==2)CHECK(q->RxSlots[32].Generation==1 && AdmissionSeen[32]==1);
    if(wrap){NET_BUFFER_LIST *reversed=NULL;while(old){NET_BUFFER_LIST *next=old->Next;old->Next=reversed;reversed=old;old=next;}old=reversed;}
    CHECK(PbV2ReturnNbls(&f->Ctx,&old,PB_MAX_BATCH) && !old && !f->S.Active && !f->S.Returns.References && f->S.RxReturned==32);
    CHECK(!Pools && !Mdls && !Nbls && !q->Credits && !q->PendingCount && !f->S.Completions && PbQueuePop(&Complete,fresh,32,&n)==PB_EMPTY);
    for(ULONG i=0;i<64;++i)CHECK(!q->RxSlots[i].Active);
    CHECK(!q->CoalesceBusy[0] && !q->CoalesceBusy[1]);PbTestGuardFree(q->Scratch);for(ULONG i=0;i<2;++i)PbTestGuardFree(q->CoalesceBuffers[i]);
    Destroy(f);
    printf("RECEIVE_ADMISSION32 ip%u protocol%lu ndis%u shape%lu wrap%u measured%u oldObjects32 oldNbls17 claims31 invalid%u stopped%u shared0 returned32 owners0 threads0\n",PB_TEST_IP_VERSION,protocol,AdmissionNdis,shape,wrap,measured,shape==2?1:0,shape==2?30:31);
}
int main(int argc,char **argv)
{
    (void)argv;CHECK(argc==1);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));ULONG cases=0;
    for(ULONG phase=0;phase<2;++phase)for(ULONG p=0;p<2;++p)for(ULONG shape=0;shape<3;++shape)for(ULONG wrap=0;wrap<2;++wrap){AdmissionRun(p?17:6,shape,(BOOLEAN)wrap,(BOOLEAN)phase);cases++;}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("RECEIVE_ADMISSION32_SUMMARY ip%u cases%lu warm12 measured12 handles%lu/%lu threads0\n",PB_TEST_IP_VERSION,cases,before,after);return 0;
}
