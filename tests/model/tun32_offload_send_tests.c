/* Actual OOB -> Read/Describe/Send -> real LSO completion, hosted DDI only. */
#ifdef PB_API_TESTS
#error TUN32 offload send requires actual production paths
#endif
#ifndef NDEBUG
#error TUN32 offload send requires NDEBUG configuration
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error TUN32 offload send requires independent IP version4 or6
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 offload send requires shipping fingerprint");
_Static_assert(PB_VERSION_MAJOR==2 && PB_VERSION_MINOR==0 && PB_VERSION_PATCH==0,"TUN32 offload send version remains2.0.0");
#include "../../common/platform.h"
static ULONG O32Allowed,O32Late,O32PolicyChanges;
static uint32_t O32Requested;
static VOID O32Boundary(VOID);
#include PB_TUN32_SEND_HOST
static ULONG O32Kind,O32Mode,O32Cases,O32Frames,O32NblOwners,O32NbOwners,O32CompleteCalls,O32PushCalls,O32Published,O32LsoCompletions;
static ULONG O32IndependentFailures;
static NET_BUFFER_LIST *O32Lists;
static NET_BUFFER *O32Buffers;
static ULONG O32Statuses[32];
static VOID *O32Lso[32],*O32Uso[32],*O32Checksum[32];
static VOID O32Boundary(VOID)
{if(O32Late && Reads>=31){AssertUnlocked();CHECK(Reads==31 && O32Allowed==PB_PROBE_SUPPORTED && !O32PolicyChanges);O32Allowed=0;O32Late=0;++O32PolicyChanges;}}
static PB_RESULT T32Push(PB_QUEUE_VIEW *q,const PB_DESCRIPTOR *items,uint32_t count,uint32_t *accepted,uint32_t *notify)
{++O32PushCalls;CHECK(count==(O32Mode==2?31u:32u));PB_RESULT r=PbQueuePush(q,items,count,accepted,notify);CHECK(r==PB_OK && *accepted==count);O32Published+=*accepted;return r;}
static VOID T32Complete(NDIS_HANDLE adapter,NET_BUFFER_LIST *lists)
{CHECK(lists==O32Lists && O32NblOwners==1 && O32NbOwners==1 && !Current->TransitionLock && !Session->Queues[0].TxLock);++O32CompleteCalls;
 for(ULONG i=0;i<32;++i){CHECK(lists[i].Nb==O32Buffers+i && !O32Buffers[i].Next && lists[i].Next==(i<31?lists+i+1:NULL));O32Statuses[i]=lists[i].Status;
  if(O32Kind==2){NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=lists[i].Info[TcpLargeSendNetBufferListInfo]};CHECK(info.LsoV2TransmitComplete.Type==NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE && !info.LsoV2TransmitComplete.Reserved && !info.LsoV2TransmitComplete.Reserved2);++O32LsoCompletions;}
  else CHECK(lists[i].Info[TcpLargeSendNetBufferListInfo]==O32Lso[i]);CHECK(lists[i].Info[UdpSegmentationOffloadInfo]==O32Uso[i] && lists[i].Info[TcpIpChecksumNetBufferListInfo]==O32Checksum[i]);}
 PbCompleteSend(adapter,lists);PbTestGuardFree(O32Lists);O32Lists=NULL;--O32NblOwners;PbTestGuardFree(O32Buffers);O32Buffers=NULL;--O32NbOwners;}
static VOID O32W16(UCHAR *p,ULONG v){p[0]=(UCHAR)(v>>8);p[1]=(UCHAR)v;}
static VOID O32W32(UCHAR *p,ULONG v){for(ULONG i=0;i<4;++i)p[i]=(UCHAR)(v>>(24-8*i));}
static ULONG O32Packet(UCHAR *p,ULONG length,ULONG kind,ULONG index)
{ULONG ip=PB_TEST_IP_VERSION,protocol=(kind==1 || kind==3)?17:6,offset=ip==4?24:kind==3?40:48,head=offset+(protocol==6?32:8);memset(p,0,length);
 if(ip==4){p[0]=0x46;O32W16(p+2,kind==2?0:length);O32W16(p+4,(0x7ff0+index)&(kind==2?0x7fffu:0xffffu));p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;memset(p+20,1,4);}
 else{p[0]=0x60;O32W16(p+4,kind==2?0:length-40);p[6]=(UCHAR)(offset==40?protocol:0);p[7]=64;p[8]=0x20;p[24]=0x30;if(offset==48)p[40]=(UCHAR)protocol;}
 O32W16(p+offset,4000);O32W16(p+offset+2,5000);
 if(protocol==6){O32W32(p+offset+4,0xfffffff0u+index*(length-head));p[offset+12]=0x80;p[offset+13]=0x18;O32W16(p+offset+14,8192);p[offset+20]=p[offset+21]=1;p[offset+22]=8;p[offset+23]=10;O32W32(p+offset+24,0xfffffff0u+index);O32W32(p+offset+28,0x80000001u+index);}
 else O32W16(p+offset+4,length-offset);
 memset(p+head,(int)(0x30+index),length-head);O32W32(p+head,index);O32W32(p+head+4,0x01234567);O32W32(p+head+8,0x89abcdef);O32W32(p+head+12,0xfedcba98);O32W32(p+head+16,0x76543210);
 ULONG transport=length-offset,sum=PbChecksumAdd(0,p+(ip==4?12:8),ip==4?8:32)+protocol+(kind<2?transport:0);while(sum>>16)sum=(sum&0xffff)+(sum>>16);O32W16(p+offset+(protocol==6?16:6),sum?sum:0xffff);return offset;}
static VOID O32Source(MDL mdls[4],const UCHAR *frame,ULONG length)
{memset(mdls,0,4*sizeof(*mdls));const ULONG sizes[]={167,0,512,length-672};ULONG at=0;
 for(ULONG i=0;i<4;++i){mdls[i].Next=i<3?mdls+i+1:NULL;mdls[i].Length=sizes[i];if(!sizes[i])continue;mdls[i].Address=PbTestGuardAlloc(sizes[i]);ULONG pad=i?0:7;memset(mdls[i].Address,0xa5,pad);memcpy(mdls[i].Address+pad,frame+at,sizes[i]-pad);at+=sizes[i]-pad;DWORD old;CHECK(VirtualProtect(mdls[i].Address,sizes[i],PAGE_READONLY,&old));++SourceMappings;++SourceAllocs;}CHECK(at==length);}
static VOID O32SourceCheck(MDL mdls[4],const UCHAR *frame,ULONG length)
{ULONG at=0;for(ULONG i=0;i<4;++i)if(mdls[i].Address){ULONG pad=i?0:7;CHECK(!memcmp(mdls[i].Address+pad,frame+at,mdls[i].Length-pad));at+=mdls[i].Length-pad;}CHECK(at==length);}
static VOID O32Case(ULONG kind,ULONG mode)
{CHECK(kind<4 && mode<4 && (mode!=3 || kind==2) && !O32Frames && !O32NblOwners && !O32NbOwners);O32Kind=kind;O32Mode=mode;Setup(1,64,128);RealEthernet=RealMdl=TRUE;HostedNdisVersion=kind==3?0x60054:0x60014;FutureInfoReads=0;
 ULONG ip=PB_TEST_IP_VERSION,protocol=(kind==1 || kind==3)?17:6,length=mode==2?4083:2500,frameLength=length+14,expected=mode==1?0:mode==2?31:32;
 O32Allowed=mode==1?0:PB_PROBE_SUPPORTED;O32Late=mode==2;O32PolicyChanges=O32PushCalls=O32Published=O32CompleteCalls=O32LsoCompletions=0;
 O32Requested=PbPlatformOffloads(HostedNdisVersion);
 if(mode==3){if(ip==4)CHECK(PbApplyChecksumParameter(1,PB_TX_IP4_CHECKSUM,PB_RX_IP4_CHECKSUM,&O32Requested)==PB_OK);CHECK(PbApplyChecksumParameter(1,ip==4?PB_TX_TCP4_CHECKSUM:PB_TX_TCP6_CHECKSUM,ip==4?PB_RX_TCP4_CHECKSUM:PB_RX_TCP6_CHECKSUM,&O32Requested)==PB_OK);CHECK((O32Requested&(ip==4?PB_TX_LSO4:PB_TX_LSO6)) && !(O32Requested&(ip==4?(PB_TX_IP4_CHECKSUM|PB_TX_TCP4_CHECKSUM):PB_TX_TCP6_CHECKSUM)));}
 O32Lists=PbTestGuardAlloc(32*sizeof(*O32Lists));memset(O32Lists,0,32*sizeof(*O32Lists));++O32NblOwners;O32Buffers=PbTestGuardAlloc(32*sizeof(*O32Buffers));memset(O32Buffers,0,32*sizeof(*O32Buffers));++O32NbOwners;
 UCHAR *frames[32];MDL mdls[32][4];ULONG offset=0,header=0;for(ULONG i=0;i<32;++i){frames[i]=PbTestGuardAlloc(frameLength);++O32Frames;memset(frames[i],0,14);frames[i][0]=frames[i][6]=2;O32W16(frames[i]+12,ip==4?0x0800:0x86dd);offset=O32Packet(frames[i]+14,length,kind,i);header=offset+(protocol==6?32:8);CHECK(header+14<=160);O32Source(mdls[i],frames[i],frameLength);
  O32Buffers[i]=(NET_BUFFER){.Length=frameLength,.Frame=frames[i],.Mdl=mdls[i],.MdlOffset=7};O32Lists[i].Nb=O32Buffers+i;O32Lists[i].Next=i<31?O32Lists+i+1:NULL;
  NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO c={0};NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso={0};NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO uso={0};
  if(kind<2){c.Transmit.IsIPv4=ip==4;c.Transmit.IsIPv6=ip==6;c.Transmit.IpHeaderChecksum=ip==4;c.Transmit.TcpChecksum=protocol==6;c.Transmit.UdpChecksum=protocol==17;if(protocol==6)c.Transmit.TcpHeaderOffset=offset+14;}
  else if(kind==2){lso.LsoV2Transmit.Type=NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE;lso.LsoV2Transmit.MSS=1000;lso.LsoV2Transmit.TcpHeaderOffset=offset+14;lso.LsoV2Transmit.IPVersion=ip==6;}
  else{uso.Transmit.MSS=1000;uso.Transmit.UdpHeaderOffset=offset+14;uso.Transmit.IPVersion=ip==6;}
  O32Lso[i]=O32Lists[i].Info[0]=lso.Value;O32Uso[i]=O32Lists[i].Info[1]=uso.Value;O32Checksum[i]=O32Lists[i].Info[2]=c.Value;}
 NET_BUFFER_LIST *original=O32Lists;CHECK(PbV2Send(Current,original) && !O32Lists && !O32Buffers && O32CompleteCalls==1 && Completed==32 && AfterPinRelease==1 && !Session->Active && !Session->Returns.Value && !Faults);
 /* A baseline contract failure is observed separately, never asserted as a
  * successful send. Finish the complete ownership ledger before exit139. */
 if(mode==3 && !O32Published){expected=0;++O32IndependentFailures;}
 CHECK(Reads==32 && Describes==32 && Reserves==32 && PrefixCalls==32 && PrefixBytes==32ull*59 && PayloadBytes==32ull*length && PrefixMaps==32 && PayloadMaps==96 && !Controls && !Replies && O32Published==expected && Session->TxObjects==expected && O32PushCalls==(ULONG)!!expected && O32PolicyChanges==(ULONG)(mode==2) && !O32Late && O32LsoCompletions==(kind==2?32u:0u));
 CHECK(kind==3 || !FutureInfoReads);for(ULONG i=0;i<32;++i)CHECK(O32Statuses[i]==(ULONG)(i<expected?NDIS_STATUS_SUCCESS:NDIS_STATUS_NOT_SUPPORTED));CHECK(Current->Probe.SendDiagnostics.Reasons[PB_SEND_POLICY]==32-expected);
 CHECK(Current->Probe.LsoNbls==(kind==2?32:0) && Current->Probe.UsoNbls==(kind==3?32:0) && Current->Probe.ChecksumNbls==(kind<2?32:0));
 ULONG segments=kind<2?1:1+(length-header-1)/1000;CHECK(Current->Statistics.ifHCOutUcastPkts==(LONG64)expected*segments && Current->Statistics.ifHCOutOctets==(LONG64)expected*(length+(segments-1)*header));
 PB_DESCRIPTOR items[32];uint32_t n,accepted,notify;PB_RESULT got=PbQueuePop(Consumers,items,32,&n);CHECK(got==(expected?PB_OK:PB_EMPTY) && n==expected);UCHAR expectedBytes[PB_BLOCK_SIZE],wire[1200];
 for(ULONG i=0;i<n;++i){PB_DESCRIPTOR *d=items+i;CHECK(d->Token.Slot==i && d->Token.Generation==1 && d->Length==length && PbValidateDescriptor(&Session->Layout,Session->Epoch,0,FALSE,d,7)==PB_OK);memcpy(expectedBytes,frames[i]+14,length);
  if(kind<2){CHECK(!d->Type && PbFinalizeChecksums(expectedBytes,length,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);CHECK(!memcmp(expectedBytes,Session->Region+d->DataOffset,length));}
  else{CHECK(d->Type==(kind==2?PB_DESC_TCP_GSO:PB_DESC_UDP_GSO) && d->Mss==1000 && d->L4Offset==offset && d->HeaderLength==header && d->IpVersion==ip && d->Protocol==protocol && d->Flags==((ip==4?PB_CSUM_IPV4:0)|(kind==2?PB_CSUM_TCP:PB_CSUM_UDP)) && !memcmp(expectedBytes,Session->Region+d->DataOffset,length));PB_SEGMENT_PLAN plan;CHECK(PbPlanSegments(Session->Region+d->DataOffset,length,kind==2?PB_SEGMENT_TCP:PB_SEGMENT_UDP,1000,&plan)==PB_PACKET_OK && plan.SegmentCount==segments);
   for(ULONG j=0;j<segments;++j){uint32_t written;CHECK(PbWriteSegmentChecked(Session->Region+d->DataOffset,length,&plan,j,wire,sizeof(wire),&written)==PB_PACKET_OK);ULONG bytes=min(1000,length-header-j*1000);CHECK(written==header+bytes && !memcmp(wire+header,frames[i]+14+header+j*1000,bytes));if(ip==4)CHECK(!memcmp(wire+20,frames[i]+14+20,4));else if(offset==48)CHECK(!memcmp(wire+40,frames[i]+14+40,8));if(protocol==6)CHECK(!memcmp(wire+offset+20,frames[i]+14+offset+20,12));memcpy(expectedBytes,wire,written);CHECK(PbFinalizeChecksums(expectedBytes,written,ip==4,protocol==6,protocol==17)==PB_PACKET_OK && !memcmp(expectedBytes,wire,written));}}
 }
 if(n){CHECK(PbQueuePush(Recyclers,items,n,&accepted,&notify)==PB_OK && accepted==n);KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&Session->Queues[0].TxLock,&lock);CHECK(PbV2RecycleLocked(Session->Queues)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);}
 for(ULONG i=0;i<64;++i)CHECK(Session->Queues[0].TxSlots[i].State==PB_SLOT_FREE);
 /* The actual pool advances Next even when Describe later rolls back. */
 for(ULONG i=0;i<32;++i)CHECK(Session->Queues[0].TxSlots[i].Generation==1);for(ULONG i=32;i<64;++i)CHECK(!Session->Queues[0].TxSlots[i].Generation);
 for(ULONG i=0;i<32;++i){O32SourceCheck(mdls[i],frames[i],frameLength);SourceFree(mdls[i]);PbTestGuardFree(frames[i]);--O32Frames;}Destroy();CHECK(!HeapObjects && !RegionObjects && !SourceMappings && SourceAllocs==SourceFrees && !O32Frames && !O32NblOwners && !O32NbOwners);++O32Cases;
 printf("CASE: OFFLOAD32 ip%lu kind%lu mode%lu ndis%x length%lu published%lu pushes%lu prefix%llu payload%llu complete%lu lsoComplete%lu policyChanges%lu wirePackets%lu\n",ip,kind,mode,HostedNdisVersion,length,expected,O32PushCalls,PrefixBytes,PayloadBytes,O32CompleteCalls,O32LsoCompletions,O32PolicyChanges,expected*segments);}
static VOID O32Matrix(VOID){for(ULONG kind=0;kind<4;++kind)for(ULONG mode=0;mode<3;++mode)O32Case(kind,mode);}
int main(int argc,char **argv){setvbuf(stdout,NULL,_IONBF,0);printf("FAMILY: IPv%d only\n",PB_TEST_IP_VERSION);DWORD before,after;
 if(argc==2 && !strcmp(argv[1],"--checksum-independent-lso")){O32Case(2,3);O32Case(2,3);CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));O32Case(2,3);CHECK(O32Cases==3 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("INDEPENDENT_LSO: ip%d contractPublish32 observed%lu requested%x failures%lu cases3 warm2 measure1 handles%lu/%lu sourceMappings%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,O32Published,O32Requested,O32IndependentFailures,before,after,SourceAllocs,SourceFrees);if(O32IndependentFailures){CHECK(O32IndependentFailures==3);puts("LSO_DEPENDS_ON_ENABLED_CHECKSUM");return 139;}return 0;}
 CHECK(argc==1);O32Matrix();CHECK(O32Cases==12 && GetProcessHandleCount(GetCurrentProcess(),&before));O32Matrix();CHECK(O32Cases==24 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: TUN32 offload send cases24 warm12 measure12 handles%lu/%lu sourceMappings%lu/%lu; all packets slots references and source owners reclaimed, no test threads\n",before,after,SourceAllocs,SourceFrees);return 0;}
