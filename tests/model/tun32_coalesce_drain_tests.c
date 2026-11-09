/* SPDX-License-Identifier: GPL-2.0 */
/* Actual bounded receive/coalesce/return/drain/query paths, hosted DDI only.
 * Original NT_ASSERT=CHECK remains stronger than native NDEBUG assertions. */
#ifdef PB_API_TESTS
#error TUN32 coalesce drain requires actual production paths
#endif
#ifndef NDEBUG
#error TUN32 coalesce drain requires NDEBUG configuration
#endif
#ifndef PB_TEST_IP_VERSION
#define PB_TEST_IP_VERSION 0
#endif
#if PB_TEST_IP_VERSION != 0 && PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error TUN32 coalesce drain requires IP version 0, 4 or 6
#endif
#define TCD_FIRST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 4)
#define TCD_LAST_IP (PB_TEST_IP_VERSION ? PB_TEST_IP_VERSION : 6)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 coalesce drain requires shipping fingerprint");
#include "../../common/v2_core.h"
#include "../../common/coalesce.h"
static ULONG TcdNdisVersion=0x60059;
#define PB_NBL_VERSION TcdNdisVersion
#ifndef PB_TUN32_COALESCE_DRAIN_HOST
#error Bind the complete frozen drain boundary host
#endif
#define main UnusedTun32CoalesceDrainMain
#include PB_TUN32_COALESCE_DRAIN_HOST
#undef main
static ULONG Pops,Indications,IndicatedNbls,PrefixCopies,PrefixBytes,RecordBytes,OtherNblCopies,Begins,Appends,Finishes,Cases,Joins;
static ULONG64 PrivateBytes,OutputBytes;
static ULONG PartialMdls,SnapshotMdls,RscInfoReads,UroInfoReads;
static VOID **ObserveNblInfo(NET_BUFFER_LIST *nbl,ULONG field)
{CHECK(field<InfoCount);if(field==TcpRecvSegCoalesceInfo || field==RscTcpTimestampDelta){CHECK(TcdNdisVersion>=0x6001e);++RscInfoReads;}if(field==UdpRecvSegCoalesceOffloadInfo){CHECK(TcdNdisVersion>=0x60059);++UroInfoReads;}return nbl->Info+field;}
static PB_RESULT ObservePop(PB_QUEUE_VIEW *v,PB_DESCRIPTOR *p,uint32_t n,uint32_t budget,uint32_t *out)
{++Pops;PB_RESULT result=PbQueuePopBudget(v,p,n,budget,out);CHECK(result==PB_OK && *out==16 && budget==PB_RX_BYTE_BUDGET);return result;}
static VOID ObserveIndicate(VOID *adapter,NET_BUFFER_LIST *lists,ULONG n,ULONG flags)
{++Indications;IndicatedNbls+=n;PbIndicateReceive(adapter,lists,n,flags);}
static VOID ObserveNblCopy(VOID *dst,const VOID *src,SIZE_T n)
{if(n==40){++PrefixCopies;PrefixBytes+=(ULONG)n;}else if(n==sizeof(PB_DESCRIPTOR) || n==2*sizeof(PB_DESCRIPTOR)){RecordBytes+=(ULONG)n;}else ++OtherNblCopies;memcpy(dst,src,n);}
static VOID ObserveSnapshot(VOID *dst,const VOID *src,SIZE_T n)
{CHECK(Current && Current->Queues[0].Scratch);uintptr_t offset=(uintptr_t)dst-(uintptr_t)Current->Queues[0].Scratch;
 CHECK(offset<PB_BLOCK_SIZE && n<=PB_BLOCK_SIZE-offset);PrivateBytes+=n;memcpy(dst,src,n);}
static PB_PACKET_STATUS ObserveBegin(PB_COALESCE *s,uint8_t *out,uint32_t capacity,const uint8_t *p,uint32_t n)
{++Begins;PB_PACKET_STATUS r=PbCoalesceBegin(s,out,capacity,p,n);if(r==PB_PACKET_OK)OutputBytes+=n;return r;}
static PB_PACKET_STATUS ObserveAppend(PB_COALESCE *s,const uint8_t *p,uint32_t n)
{++Appends;ULONG before=s->Length;PB_PACKET_STATUS r=PbCoalesceAppend(s,p,n);if(r==PB_PACKET_OK)OutputBytes+=s->Length-before;return r;}
static PB_PACKET_STATUS ObserveFinish(PB_COALESCE *s){++Finishes;return PbCoalesceFinish(s);}
static VOID W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID W32(UCHAR *p,ULONG n){W16(p,n>>16);W16(p+2,n);}
static VOID Packet(UCHAR *p,ULONG ip,ULONG protocol,ULONG index)
{ULONG length=32768,offset=ip==4?20:40,header=offset+(protocol==6?20:8);memset(p,0,length);
 if(ip==4){p[0]=0x45;W16(p+2,length);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;}
 else{p[0]=0x60;W16(p+4,length-40);p[6]=(UCHAR)protocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
 W16(p+offset,4000);W16(p+offset+2,5000);if(protocol==6){W32(p+offset+4,1000+index*(length-header));p[offset+12]=0x50;p[offset+13]=0x10;W16(p+offset+14,8192);}else W16(p+offset+4,length-offset);
 memset(p+header,(int)(0x40+index),length-header);CHECK(PbFinalizeChecksums(p,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);}
static ULONG ValidateChain(FIXTURE *f,NET_BUFFER_LIST *head,ULONG first,ULONG originals,ULONG ip,ULONG protocol)
{ULONG nbls=0,at=first,header=(ip==4?20:40)+(protocol==6?20:8);UCHAR expected[32768];
 for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);CHECK(r->Count==1 || r->Count==2);CHECK(n->FirstMdl->Length==14 && n->FirstMdl->Next==r->Mdl && r->Mdl->Length==r->IndicationLength);
  if(r->Count==1){++PartialMdls;CHECK(r->CoalesceIndex==MAXULONG && r->Mdl->Partial && r->Mdl->Address==f->S.Region+r->Descriptors[0].DataOffset);Packet(expected,ip,protocol,at);CHECK(!memcmp(expected,r->Mdl->Address,32768));}
  else{++SnapshotMdls;CHECK(r->CoalesceIndex<2 && !r->Mdl->Partial && r->Mdl->Address==f->S.Queues[0].CoalesceBuffers[r->CoalesceIndex] && r->Mdl->Length==header+2*(32768-header));
   for(ULONG j=0;j<2;++j){UCHAR *bytes=(UCHAR *)r->Mdl->Address+header+j*(32768-header);for(ULONG k=0;k<32768-header;++k)CHECK(bytes[k]==(UCHAR)(0x40+at+j));}}
  if(TcdNdisVersion<0x60059)CHECK(n->Info[UdpRecvSegCoalesceOffloadInfo]==(VOID *)(ULONG_PTR)~0u);
  if(r->Count==2){if(protocol==6){NDIS_RSC_NBL_INFO info={.Value=n->Info[TcpRecvSegCoalesceInfo]};CHECK(info.Info.CoalescedSegCount==2 && r->ReceiveType==PB_DESC_TCP_GSO);}else{NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=n->Info[UdpRecvSegCoalesceOffloadInfo]};CHECK(info.Receive.SegCount==2 && info.Receive.SegSize==32768-header && r->ReceiveType==PB_DESC_UDP_GSO);}}
  for(ULONG j=0;j<r->Count;++j){CHECK(r->Descriptors[j].Token.Slot==at+j && r->Descriptors[j].Length==32768);Packet(expected,ip,protocol,at+j);CHECK(!memcmp(expected,f->S.Region+r->Descriptors[j].DataOffset,32768));}at+=r->Count;++nbls;}
 CHECK(at==first+originals);return nbls;}
static VOID ReturnFirst(FIXTURE *f,NET_BUFFER_LIST **head)
{NET_BUFFER_LIST *one=*head;CHECK(one);*head=one->Next;one->Next=NULL;CHECK(PbV2ReturnOne(&f->Ctx,one));}
static DWORD WINAPI FaultWorker(VOID *arg){PbV2Worker(arg);return 0;}
static VOID Witness(FIXTURE *f)
{HANDLE thread=CreateThread(NULL,0,FaultWorker,f->S.Queues,0,NULL);CHECK(thread && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);DWORD code;CHECK(GetExitCodeThread(thread,&code) && !code && CloseHandle(thread));++Joins;CHECK(f->S.Queues[0].FaultQuiesced && Exits==1 && !Calls && !Idle);}
static VOID Run(ULONG ip,ULONG protocol,ULONG mode)
{CHECK(mode<6 && (!PB_TEST_IP_VERSION || ip==PB_TEST_IP_VERSION));FIXTURE *f=Create(1,32,32,0);PB_KERNEL_QUEUE *q=f->S.Queues;f->Ctx.MiniportAdapterHandle=f;f->Ctx.Probe.LocalMac[0]=f->Ctx.Probe.PeerMac[0]=2;AutoConsume=FALSE;UnexpectedIndication=0;EnabledMask=protocol==17?PB_RX_URO:ip==4?PB_RX_RSC4:PB_RX_RSC6;TcdNdisVersion=protocol==17?0x60059:0x6001e;PartialMdls=SnapshotMdls=RscInfoReads=UroInfoReads=0;
 q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);for(ULONG b=0;b<2;++b)q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);
 PB_DESCRIPTOR items[32];for(ULONG i=0;i<32;++i){items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=32768,.DataOffset=f->S.Layout.Queues[0].RxData+i*PB_BLOCK_SIZE};Packet(f->S.Region+items[i].DataOffset,ip,protocol,i);}
 Pops=Indications=IndicatedNbls=PrefixCopies=PrefixBytes=RecordBytes=OtherNblCopies=Begins=Appends=Finishes=0;PrivateBytes=OutputBytes=0;
 uint32_t count,notify;CHECK(PbQueuePush(&Submit,items,32,&count,&notify)==PB_OK && count==32);CHECK(ActualReceiveBatch(q) && Indicated && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==16);
 NET_BUFFER_LIST *first=Indicated;Indicated=NULL;CHECK(ValidateChain(f,first,0,16,ip,protocol)==14 && f->S.Active==14 && q->Credits==16 && q->CoalesceBusy[0] && q->CoalesceBusy[1]);
 CHECK(PrivateBytes==6ull*32768 && Begins==2 && Appends==4 && Finishes==2 && PrefixCopies==14 && RecordBytes==16*sizeof(PB_DESCRIPTOR));
 ULONG early=mode<3?mode:0;for(ULONG i=0;i<early;++i)ReturnFirst(f,&first);CHECK(q->CoalesceBusy[0]==(LONG)(early==0) && q->CoalesceBusy[1]==(LONG)(early<2));
 ULONG64 firstOutput=OutputBytes;ULONG header=(ip==4?20:40)+(protocol==6?20:8);CHECK(firstOutput==2ull*(header+2*(32768-header)));
 if(mode<3){CHECK(ActualReceiveBatch(q) && Indicated && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==32);NET_BUFFER_LIST *second=Indicated;Indicated=NULL;
  CHECK(ValidateChain(f,second,16,16,ip,protocol)==16-early && f->S.Active==(LONG)(30-2*early));NET_BUFFER_LIST *tail=first;CHECK(tail);while(tail->Next)tail=tail->Next;tail->Next=second;
  CHECK(PrivateBytes==(6ull+3ull*early)*32768 && OutputBytes==(2ull+early)*(header+2*(32768-header)) && IndicatedNbls==30-early);Drain(f);CHECK(State(f,f->S.Active)==PB_SESSION_QUARANTINED);
 }else if(mode==3){Drain(f);CHECK(q->DrainProducer==32 && f->S.DrainSnapshotReady && ActualReceiveBatch(q) && !Indicated && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==32 && PrivateBytes==6ull*32768 && IndicatedNbls==14);CHECK(State(f,14)==PB_SESSION_QUARANTINED);
 }else{PbV2Fault(&f->S);CHECK(!q->FaultQuiesced && q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==16);if(mode==5)Witness(f);CHECK(State(f,14)==PB_SESSION_QUARANTINED);}
 while(first)CHECK(PbV2ReturnNbls(&f->Ctx,&first,PB_MAX_BATCH));CHECK(!Pools && !Mdls && !Nbls && !f->S.Active && !q->Credits && !q->PendingCount && !f->S.Returns.References);
 if(mode==4){CHECK(State(f,0)==PB_SESSION_QUARANTINED && !q->FaultQuiesced);Witness(f);}CHECK(State(f,0)==PB_SESSION_STOPPED);
 PB_DESCRIPTOR returned[32];uint32_t n;PB_RESULT popped=PbQueuePop(&Complete,returned,32,&n);if(mode>=4)CHECK(popped==PB_EMPTY && !f->S.Completions);else{CHECK(popped==PB_OK && n==32);ULONG64 seen=0;for(ULONG i=0;i<n;++i){CHECK(returned[i].Token.Slot<32 && returned[i].Status==(ULONG)(mode==3 && returned[i].Token.Slot>=16?PB_STOPPED:PB_OK));ULONG64 bit=1ull<<returned[i].Token.Slot;CHECK(!(seen&bit));seen|=bit;}CHECK(seen==0xffffffffull);}
 CHECK(!OtherNblCopies && PrefixBytes==40*IndicatedNbls && RecordBytes==(mode<3?32u:16u)*sizeof(PB_DESCRIPTOR));
 CHECK(PartialMdls==(mode<3?28-2*early:12) && SnapshotMdls==2+early && RscInfoReads>0 && (protocol==17?UroInfoReads>0:!UroInfoReads));
 for(ULONG i=0;i<32;++i)CHECK(!q->RxSlots[i].Active);CHECK(!q->CoalesceBusy[0] && !q->CoalesceBusy[1]);for(ULONG b=0;b<2;++b)PbTestGuardFree(q->CoalesceBuffers[b]);PbTestGuardFree(q->Scratch);Destroy(f);++Cases;
 printf("CASE: COALESCE32 ip%lu proto%lu mode%lu ndis%x pops%lu nbls%lu partial%lu snapshotMdls%lu prefix%lu record%lu snapshot%llu output%llu workerJoins%lu\n",ip,protocol,mode,TcdNdisVersion,Pops,IndicatedNbls,PartialMdls,SnapshotMdls,PrefixBytes,RecordBytes,PrivateBytes,OutputBytes,(ULONG)(mode>=4));}
static VOID Matrix(VOID)
{for(ULONG ip=TCD_FIRST_IP;ip<=TCD_LAST_IP;ip+=2)for(ULONG p=6;p<=17;p+=11)for(ULONG mode=0;mode<6;++mode)Run(ip,p,mode);}
int main(VOID)
{setvbuf(stdout,NULL,_IONBF,0);
#if PB_TEST_IP_VERSION
 printf("FAMILY: IPv%u only\n",(unsigned)PB_TEST_IP_VERSION);
#endif
 Matrix();ULONG warm=Cases;CHECK(warm==(PB_TEST_IP_VERSION?12u:24u));DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix();CHECK(Cases==2*warm && Joins==(PB_TEST_IP_VERSION?8u:16u) && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: TUN32 coalesce drain cases%lu warm%lu measure%lu workerJoins%lu handles%lu/%lu; all NBL MDL slots credits references buffers and worker threads reclaimed\n",Cases,warm,warm,Joins,before,after);return 0;}
