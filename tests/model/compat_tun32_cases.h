/* SPDX-License-Identifier: GPL-2.0 */
/* Actual legacy worker and actual hosted v2 batch calls, not a pump stand-in.
 * The packet's two opaque 64-bit markers are application payload; this project
 * has no timestamp API for them and must preserve both directions byte-for-byte. */
#ifndef PB_TEST_IP_VERSION
#define PB_TEST_IP_VERSION 0
#endif
#if PB_TEST_IP_VERSION != 0 && PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error PB_TEST_IP_VERSION must be0,4 or6
#endif
#ifdef PB_TUN32_DECLARATIONS
enum { TUN32_RESERVE,TUN32_COMMIT,TUN32_RECEIVE,TUN32_RELEASE,TUN32_POLL,TUN32_METHODS };
enum { TUN32_SEED_READY,TUN32_SEED_GO,TUN32_PREFIX_READY,TUN32_PREFIX_GO,TUN32_FULL_READY,TUN32_FULL_GO,TUN32_TARGET_READY,TUN32_TARGET_GO,TUN32_RETIRED_READY,TUN32_RETIRED_GO,TUN32_PARTIAL_EVENTS };
enum { TUN32_SEED,TUN32_TARGET,TUN32_PREFIX,TUN32_RECOVER,TUN32_FINAL };
typedef struct TUN32_CALL {uint32_t Requested,Accepted;PB_RESULT Result;} TUN32_CALL;
static struct {
    volatile LONG Enabled;
    HANDLE Completed,Resume;
    BYTE *LegacySend,*LegacyReceive,*NativeSend,*NativeReceive;
    SIZE_T LegacyBytes,NativeBytes;
    uint32_t Calls[TUN32_METHODS],Empty[TUN32_METHODS];
    TUN32_CALL Vector[TUN32_METHODS][8];
    uint32_t Copies[2];uint64_t Bytes[2];
    BOOL Partial,FullObserved;
    volatile LONG Stage;
    HANDLE PartialEvents[TUN32_PARTIAL_EVENTS];
} Tun32;
static VOID *Tun32Copy(VOID *,const VOID *,size_t);
static VOID Tun32Record(uint32_t,uint32_t,uint32_t,PB_RESULT);
static VOID Tun32BeforeReceive(VOID);
static VOID Tun32AfterReserve(PB_RESULT,uint32_t);
static VOID Tun32AfterPoll(uint32_t);
#else
#ifdef PB_GUARD_TESTS
#include "../safety/guard_memory.h"
#endif
static DWORD Tun32FullCount,Tun32PartialCount;
static BOOL Tun32Selected(BOOL ipv6)
{
#if PB_TEST_IP_VERSION
    return PB_TEST_IP_VERSION==(ipv6?6:4);
#else
    (void)ipv6;
    return TRUE;
#endif
}
static VOID Tun32Record(uint32_t method,uint32_t requested,uint32_t accepted,PB_RESULT result)
{
    if(!ReadAcquire(&Tun32.Enabled))return;
    CHECK(method<TUN32_METHODS);
    if(!accepted){++Tun32.Empty[method];return;}
    uint32_t n=Tun32.Calls[method]++;CHECK(n<_countof(Tun32.Vector[method])&&result==PB_OK);
    Tun32.Vector[method][n]=(TUN32_CALL){requested,accepted,result};
}
static VOID Tun32Gate(uint32_t entered,uint32_t gate)
{CHECK(SetEvent(Tun32.PartialEvents[entered])&&WaitForSingleObject(Tun32.PartialEvents[gate],10000)==WAIT_OBJECT_0);}
static VOID Tun32BeforeReceive(VOID)
{
    if(!ReadAcquire(&Tun32.Enabled)||!Tun32.Partial)return;
    /* These are API-boundary observers on the real sole worker, after Send
     * published its actual legacy Head; they are not native scheduler parks. */
    if(ReadAcquire(&Tun32.Stage)==TUN32_SEED&&Tun32.Calls[TUN32_COMMIT]==1)Tun32Gate(TUN32_SEED_READY,TUN32_SEED_GO);
    else if(ReadAcquire(&Tun32.Stage)==TUN32_TARGET&&Tun32.Calls[TUN32_COMMIT]==1)Tun32Gate(TUN32_PREFIX_READY,TUN32_PREFIX_GO);
    else if(ReadAcquire(&Tun32.Stage)==TUN32_RECOVER&&Tun32.Calls[TUN32_COMMIT]==2)Tun32Gate(TUN32_TARGET_READY,TUN32_TARGET_GO);
}
static VOID Tun32AfterReserve(PB_RESULT result,uint32_t accepted)
{
    if(ReadAcquire(&Tun32.Enabled)&&Tun32.Partial&&ReadAcquire(&Tun32.Stage)==TUN32_PREFIX&&result==PB_FULL&&!accepted&&!Tun32.FullObserved){
        Tun32.FullObserved=TRUE;Tun32Gate(TUN32_FULL_READY,TUN32_FULL_GO);
    }
}
static VOID Tun32AfterPoll(uint32_t accepted)
{
    if(!ReadAcquire(&Tun32.Enabled)||!accepted)return;
    if(Tun32.Partial){if(ReadAcquire(&Tun32.Stage)==TUN32_FINAL){CHECK(accepted==64);Tun32Gate(TUN32_RETIRED_READY,TUN32_RETIRED_GO);}}
    else CHECK(accepted==32&&SetEvent(Tun32.Completed)&&WaitForSingleObject(Tun32.Resume,10000)==WAIT_OBJECT_0);
}
static BOOL Tun32Span(const VOID *pointer,size_t bytes,const BYTE *base,SIZE_T span)
{uintptr_t offset=(uintptr_t)pointer-(uintptr_t)base;return base&&offset<span&&bytes<=span-offset;}
static VOID *Tun32Copy(VOID *destination,const VOID *source,size_t bytes)
{
    if(ReadAcquire(&Tun32.Enabled)){
        BOOL send=Tun32Span(destination,bytes,Tun32.NativeSend,Tun32.NativeBytes)&&Tun32Span(source,bytes,Tun32.LegacySend,Tun32.LegacyBytes);
        BOOL receive=Tun32Span(source,bytes,Tun32.NativeReceive,Tun32.NativeBytes)&&Tun32Span(destination,bytes,Tun32.LegacyReceive,Tun32.LegacyBytes);
        if(send||receive){CHECK(send!=receive);uint32_t direction=receive?1:0;++Tun32.Copies[direction];Tun32.Bytes[direction]+=bytes;}
    }
    return memcpy(destination,source,bytes);
}
static VOID Tun32Put16(BYTE *p,uint32_t value){p[0]=(BYTE)(value>>8);p[1]=(BYTE)value;}
static VOID Tun32Put32(BYTE *p,uint32_t value){for(uint32_t i=0;i<4;++i)p[i]=(BYTE)(value>>(24-8*i));}
static VOID Tun32Put64(BYTE *p,uint64_t value){for(uint32_t i=0;i<8;++i)p[i]=(BYTE)(value>>(56-8*i));}
static uint32_t Tun32Sum(const BYTE *p,uint32_t bytes,uint32_t sum)
{while(bytes>1){sum+=((uint32_t)p[0]<<8)|p[1];p+=2;bytes-=2;}if(bytes)sum+=(uint32_t)*p<<8;return sum;}
static uint16_t Tun32Checksum(uint32_t sum)
{while(sum>>16)sum=(sum&65535)+(sum>>16);return (uint16_t)~sum;}
static VOID Tun32Packet(BYTE *p,uint32_t length,BOOL ipv6,BOOL udp,uint32_t direction,uint32_t sequence)
{
    CHECK(length==1500||length==65535);memset(p,(int)(0x30+direction+sequence),length);
    uint32_t ip=ipv6?40:20,protocol=udp?17:6,transport=length-ip;
    memset(p,0,ip+(udp?8:20));
    if(ipv6){p[0]=0x60;Tun32Put16(p+4,transport);p[6]=(BYTE)protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=(BYTE)(1+direction);p[39]=(BYTE)(2-direction);}
    else {p[0]=0x45;Tun32Put16(p+2,length);Tun32Put16(p+4,sequence);p[8]=64;p[9]=(BYTE)protocol;p[12]=p[16]=192;p[13]=p[17]=0;p[14]=p[18]=2;p[15]=(BYTE)(1+direction);p[19]=(BYTE)(2-direction);Tun32Put16(p+10,Tun32Checksum(Tun32Sum(p,20,0)));}
    BYTE *l4=p+ip;Tun32Put16(l4,10000+direction);Tun32Put16(l4+2,20000+direction);
    uint32_t header=udp?8:20;
    if(udp)Tun32Put16(l4+4,transport);
    else {Tun32Put32(l4+4,sequence+1);Tun32Put32(l4+8,0x12345678);l4[12]=0x50;l4[13]=0x10;Tun32Put16(l4+14,32768);}
    BYTE *payload=l4+header;payload[0]='T';payload[1]='3';payload[2]='2';payload[3]=(BYTE)direction;Tun32Put32(payload+4,sequence);
    /* Deliberately distinct opaque markers; no wall/CPU clock or fabricated
     * send-time API is involved in the preservation assertion. */
    Tun32Put64(payload+8,direction?UINT64_C(0xaaaabbbbccccdddd):UINT64_C(0x1111222233334444));
    Tun32Put64(payload+16,direction?UINT64_C(0x123456789abcdef0):UINT64_C(0xfedcba9876543210));
    uint32_t pseudo=ipv6?Tun32Sum(p+8,32,0):Tun32Sum(p+12,8,0);
    pseudo+=protocol+transport;
    uint16_t checksum=Tun32Checksum(Tun32Sum(l4,transport,pseudo));if(udp&&!checksum)checksum=0xffff;
    Tun32Put16(l4+(udp?6:16),checksum);
    CHECK(!Tun32Checksum(Tun32Sum(l4,transport,pseudo)));
    if(!ipv6)CHECK(!Tun32Checksum(Tun32Sum(p,20,0)));
}
static BYTE *Tun32Buffer(SIZE_T bytes)
{
#ifdef PB_GUARD_TESTS
    return PbTestGuardAlloc(bytes);
#else
    BYTE *p=malloc(bytes);CHECK(p);return p;
#endif
}
static VOID Tun32Free(BYTE *p)
{
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID Tun32CheckVectors(uint32_t length)
{
    uint32_t turns=length==1500?1:4,perTurn=32/turns;
    for(uint32_t method=0;method<TUN32_METHODS;++method){
        uint32_t expected=method==TUN32_RECEIVE||method==TUN32_POLL?1:turns;
        CHECK(Tun32.Calls[method]==expected);
        for(uint32_t i=0;i<expected;++i){TUN32_CALL *call=&Tun32.Vector[method][i];
            CHECK(call->Requested==((method==TUN32_RECEIVE||method==TUN32_POLL)?64:perTurn));
            CHECK(call->Accepted==((method==TUN32_RECEIVE||method==TUN32_POLL)?32:perTurn)&&call->Result==PB_OK);
        }
    }
    CHECK(Tun32.Copies[0]==32&&Tun32.Copies[1]==32&&Tun32.Bytes[0]==(uint64_t)32*length&&Tun32.Bytes[1]==(uint64_t)32*length);
}
static VOID Tun32Case(BOOL ipv6,BOOL udp,BOOL wrap,uint32_t length,const char *phase)
{
    CHECK(!ReadAcquire(&Tun32.Enabled)&&!BatchMode);memset(&Tun32,0,sizeof(Tun32));InterlockedExchange(&Tun32.Enabled,TRUE);
    Tun32.Completed=CreateEventW(NULL,TRUE,FALSE,NULL);Tun32.Resume=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Tun32.Completed&&Tun32.Resume);
    BatchMode=1;ReserveCalls=CommitCalls=ReceiveCalls=ReleaseCalls=Cancelled=ReadSignals=WakeSignals=ReleasedPackets=0;
    CHECK(!ReserveLimit&&!CommitLimit&&!ReceiveLimit&&!ReleaseLimit);
    BatchEntered=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGate=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGateUsed=0;CHECK(BatchEntered&&BatchGate);
    WINTUN_ADAPTER adapter={0};adapter.InterfaceFilename=L"hosted-v2";
    TUN_SESSION *s=WintunStartSession(&adapter,length==1500?WINTUN_MIN_RING_CAPACITY:4*1024*1024);CHECK(s&&WaitForSingleObject(BatchEntered,5000)==WAIT_OBJECT_0);
    Tun32.LegacySend=s->Descriptor.Receive.Ring->Data;Tun32.LegacyReceive=s->Descriptor.Send.Ring->Data;
    Tun32.LegacyBytes=s->Capacity+TUN_MAX_PACKET_SIZE-TUN_ALIGNMENT;
    Tun32.NativeSend=(BYTE *)Region+Layout.Queues[0].RxData;Tun32.NativeReceive=(BYTE *)Region+Layout.Queues[0].TxData;Tun32.NativeBytes=(SIZE_T)Layout.SlotCount*PB_BLOCK_SIZE;
    if(wrap){ULONG base=s->Capacity-TUN_ALIGNMENT;
        s->Receive.Tail=s->Receive.TailRelease=base;s->Descriptor.Receive.Ring->Head=s->Descriptor.Receive.Ring->Tail=base;
        s->Send.Head=s->Send.HeadRelease=base;s->Descriptor.Send.Ring->Head=s->Descriptor.Send.Ring->Tail=base;
    }
    BYTE *outgoing=Tun32Buffer((SIZE_T)32*length),*incoming=Tun32Buffer((SIZE_T)32*length),*out[32],*in[32];
    for(uint32_t i=0;i<32;++i){
        Tun32Packet(outgoing+(SIZE_T)i*length,length,ipv6,udp,0,i);Tun32Packet(incoming+(SIZE_T)i*length,length,ipv6,udp,1,i);
        CHECK(PbTestSend(Native,0,incoming+(SIZE_T)i*length,length,0,0)==PB_OK);
        out[i]=WintunAllocateSendPacket(s,length);CHECK(out[i]);memcpy(out[i],outgoing+(SIZE_T)i*length,length);
    }
    for(uint32_t i=32;i--;)WintunSendPacket(s,out[i]);
    CHECK(WakeSignals==1&&SetEvent(BatchGate));
    PB_DESCRIPTOR descriptors[32];Take(descriptors,32);
    for(uint32_t i=0;i<32;++i){
        CHECK(descriptors[i].Length==length&&!memcmp((BYTE *)Region+descriptors[i].DataOffset,outgoing+(SIZE_T)i*length,length));
        DWORD size;in[i]=Receive(s,&size);CHECK(size==length&&!memcmp(in[i],incoming+(SIZE_T)i*length,length));
    }
    CHECK(PbTestComplete(Native,0,descriptors,32)==PB_OK&&WaitForSingleObject(Tun32.Completed,10000)==WAIT_OBJECT_0);
    CHECK(ReleasedPackets==32&&!Cancelled);Tun32CheckVectors(length);
    LONG wakes=WakeSignals;
    for(uint32_t i=32;i--;){WintunReleaseReceivePacket(s,in[i]);if(i)CHECK(s->Descriptor.Send.Ring->Head==(wrap?s->Capacity-TUN_ALIGNMENT:0));}
    CHECK(WakeSignals-wakes==1&&s->Descriptor.Send.Ring->Head==s->Send.Head);
    /* The real poll boundary pins the sole queue owner while reading vectors.
     * Empty turns are allowed and are not an SDK scheduling/count assertion. */
    printf("TUN32 phase=%s ip=%u transport=%s wrap=%u length=%u reserve=%u commit=%u receive=%u release=%u poll-nonempty=%u copies=%u/%u bytes=%llu/%llu opaque=preserved\n",phase,ipv6?6:4,udp?"UDP":"TCP",wrap,length,Tun32.Calls[0],Tun32.Calls[1],Tun32.Calls[2],Tun32.Calls[3],Tun32.Calls[4],Tun32.Copies[0],Tun32.Copies[1],(unsigned long long)Tun32.Bytes[0],(unsigned long long)Tun32.Bytes[1]);
    static const char *names[TUN32_METHODS]={"reserve","commit","receive","release","poll"};
    printf("TUN32_VECTOR phase=%s ip=%u transport=%s wrap=%u length=%u",phase,ipv6?6:4,udp?"UDP":"TCP",wrap,length);
    for(uint32_t method=0;method<TUN32_METHODS;++method){
        printf(" %s=",names[method]);
        for(uint32_t i=0;i<Tun32.Calls[method];++i){TUN32_CALL *call=&Tun32.Vector[method][i];printf("%s%u/%u/%u",i?",":"",call->Requested,call->Accepted,(unsigned)call->Result);}
    }
    putchar('\n');
    InterlockedExchange(&Tun32.Enabled,FALSE);CHECK(SetEvent(Tun32.Resume));WintunEndSession(s);EmptyRuntime();BatchMode=0;
    CHECK(CloseHandle(BatchEntered)&&CloseHandle(BatchGate)&&CloseHandle(Tun32.Completed)&&CloseHandle(Tun32.Resume));BatchEntered=BatchGate=NULL;
    Tun32Free(outgoing);Tun32Free(incoming);memset(&Tun32,0,sizeof(Tun32));
    ++Tun32FullCount;
}
static VOID Tun32Matrix(const char *phase)
{for(uint32_t large=0;large<2;++large)for(uint32_t ipv6=0;ipv6<2;++ipv6)if(Tun32Selected(ipv6))for(uint32_t udp=0;udp<2;++udp)for(uint32_t wrap=0;wrap<2;++wrap)Tun32Case(ipv6,udp,wrap,large?65535:1500,phase);}
static VOID Tun32Cases(VOID)
{
    Tun32FullCount=0;Tun32Matrix("warm");DWORD warm=Tun32FullCount,before,after;CHECK(warm==(PB_TEST_IP_VERSION?8u:16u)&&GetProcessHandleCount(GetCurrentProcess(),&before));Tun32Matrix("measure");
    CHECK(Tun32FullCount==2*warm&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    const char *family=PB_TEST_IP_VERSION==4?"IPv4":PB_TEST_IP_VERSION==6?"IPv6":"IPv4/IPv6";
    printf("PASS: %lu actual legacy/v2 32-packet duplex cases (%lu warm/%lu measured), valid %s TCP/UDP and opaque direction/sequence/64-bit markers; exact nonempty batch vectors and one payload copy per direction/packet; handles%lu/%lu.\n",Tun32FullCount,warm,Tun32FullCount-warm,family,before,after);
}
static VOID Tun32PartialAwait(uint32_t event)
{CHECK(WaitForSingleObject(Tun32.PartialEvents[event],10000)==WAIT_OBJECT_0);}
static VOID Tun32PartialPublish(TUN_SESSION *s,const BYTE *bytes,uint32_t count)
{
    BYTE *packets[64];CHECK(count<=_countof(packets));
    for(uint32_t i=0;i<count;++i){packets[i]=WintunAllocateSendPacket(s,1500);CHECK(packets[i]);memcpy(packets[i],bytes+(SIZE_T)i*1500,1500);}
    for(uint32_t i=count;i--;)WintunSendPacket(s,packets[i]);
}
static VOID Tun32PartialVectors(uint32_t available,BOOL recovered)
{
    uint32_t calls=recovered?2:1;
    CHECK(Tun32.Calls[TUN32_RESERVE]==calls&&Tun32.Calls[TUN32_COMMIT]==calls&&!Tun32.Calls[TUN32_RECEIVE]&&!Tun32.Calls[TUN32_RELEASE]);
    CHECK(Tun32.Vector[TUN32_RESERVE][0].Requested==32&&Tun32.Vector[TUN32_RESERVE][0].Accepted==available);
    CHECK(Tun32.Vector[TUN32_COMMIT][0].Requested==available&&Tun32.Vector[TUN32_COMMIT][0].Accepted==available);
    if(recovered){
        CHECK(Tun32.Vector[TUN32_RESERVE][1].Requested==32-available&&Tun32.Vector[TUN32_RESERVE][1].Accepted==32-available);
        CHECK(Tun32.Vector[TUN32_COMMIT][1].Requested==32-available&&Tun32.Vector[TUN32_COMMIT][1].Accepted==32-available);
    }
    CHECK(Tun32.Copies[0]==(recovered?32:available)&&!Tun32.Copies[1]&&Tun32.Bytes[0]==(uint64_t)(recovered?32:available)*1500&&!Tun32.Bytes[1]);
}
static VOID Tun32PartialCheck(const PB_DESCRIPTOR *items,uint32_t first,uint32_t count,uint32_t available,const BYTE *expected)
{
    for(uint32_t i=0;i<count;++i){uint32_t index=first+i;const PB_DESCRIPTOR *d=items+index;
        CHECK(d->Length==1500&&d->Token.Epoch==Epoch&&!d->Token.Queue&&d->Token.Slot==(index<available?64-available+index:index-available)&&d->Token.Generation==(index<available?1u:2u));
        CHECK(!memcmp((BYTE *)Region+d->DataOffset,expected+(SIZE_T)index*1500,1500));
        for(uint32_t j=0;j<index;++j)CHECK(d->Token.Slot!=items[j].Token.Slot||d->Token.Generation!=items[j].Token.Generation);
    }
}
static DWORD WINAPI Tun32PartialEnd(VOID *session){WintunEndSession(session);return 0;}
static VOID Tun32PartialCase(uint32_t available,BOOL stop,BOOL wrap,BOOL ipv6,BOOL udp,const char *phase)
{
    CHECK(!ReadAcquire(&Tun32.Enabled)&&!BatchMode&&(available==1||available==7||available==31));
    memset(&Tun32,0,sizeof(Tun32));Tun32.Partial=TRUE;InterlockedExchange(&Tun32.Enabled,TRUE);InterlockedExchange(&Tun32.Stage,TUN32_SEED);
    for(uint32_t i=0;i<TUN32_PARTIAL_EVENTS;++i){Tun32.PartialEvents[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Tun32.PartialEvents[i]);}
    BatchMode=1;ReserveCalls=CommitCalls=ReceiveCalls=ReleaseCalls=Cancelled=ReadSignals=WakeSignals=ReleasedPackets=0;
    CHECK(!ReserveLimit&&!CommitLimit&&!ReceiveLimit&&!ReleaseLimit);
    BatchEntered=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGate=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGateUsed=0;CHECK(BatchEntered&&BatchGate);
    TUN_SESSION *s=Start();CHECK(s&&WaitForSingleObject(BatchEntered,5000)==WAIT_OBJECT_0);
    Tun32.LegacySend=s->Descriptor.Receive.Ring->Data;Tun32.LegacyReceive=s->Descriptor.Send.Ring->Data;Tun32.LegacyBytes=s->Capacity+TUN_MAX_PACKET_SIZE-TUN_ALIGNMENT;
    Tun32.NativeSend=(BYTE *)Region+Layout.Queues[0].RxData;Tun32.NativeReceive=(BYTE *)Region+Layout.Queues[0].TxData;Tun32.NativeBytes=(SIZE_T)Layout.SlotCount*PB_BLOCK_SIZE;
    if(wrap){ULONG base=s->Capacity-TUN_ALIGNMENT;s->Receive.Tail=s->Receive.TailRelease=base;s->Descriptor.Receive.Ring->Head=s->Descriptor.Receive.Ring->Tail=base;}
    uint32_t seeded=64-available;
    BYTE *seed=Tun32Buffer((SIZE_T)seeded*1500),*target=Tun32Buffer(32*1500);
    for(uint32_t i=0;i<seeded;++i)Tun32Packet(seed+(SIZE_T)i*1500,1500,ipv6,udp,1,i);
    for(uint32_t i=0;i<32;++i)Tun32Packet(target+(SIZE_T)i*1500,1500,ipv6,udp,0,i);
    Tun32PartialPublish(s,seed,seeded);CHECK(SetEvent(BatchGate));Tun32PartialAwait(TUN32_SEED_READY);
    CHECK(Tun32.Calls[TUN32_RESERVE]==1&&Tun32.Vector[TUN32_RESERVE][0].Requested==seeded&&Tun32.Vector[TUN32_RESERVE][0].Accepted==seeded);
    CHECK(Tun32.Calls[TUN32_COMMIT]==1&&Tun32.Copies[0]==seeded&&Tun32.Bytes[0]==(uint64_t)seeded*1500&&s->Descriptor.Receive.Ring->Head==s->Descriptor.Receive.Ring->Tail);
    PB_DESCRIPTOR occupied[64],submitted[32];Take(occupied,seeded);
    for(uint32_t i=0;i<seeded;++i)CHECK(occupied[i].Token.Slot==i&&occupied[i].Token.Generation==1&&!memcmp((BYTE *)Region+occupied[i].DataOffset,seed+(SIZE_T)i*1500,1500));
    /* Real committed slots stay PUBLISHED until actual completion polling.
     * Removing descriptors from Submit does not make their slots free. */
    PB_CLOSE_PROGRESS progress;CHECK(s->Life.GetCloseProgress(Native,&progress)==PB_OK&&!progress.UserLeases&&progress.Published==seeded);
    memset(Tun32.Calls,0,sizeof(Tun32.Calls));memset(Tun32.Empty,0,sizeof(Tun32.Empty));memset(Tun32.Vector,0,sizeof(Tun32.Vector));memset(Tun32.Copies,0,sizeof(Tun32.Copies));memset(Tun32.Bytes,0,sizeof(Tun32.Bytes));
    ULONG targetStart=s->Descriptor.Receive.Ring->Head;InterlockedExchange(&Tun32.Stage,TUN32_TARGET);Tun32PartialPublish(s,target,32);
    CHECK(SetEvent(Tun32.PartialEvents[TUN32_SEED_GO]));Tun32PartialAwait(TUN32_PREFIX_READY);
    ULONG prefixHead=TUN_RING_WRAP(targetStart+available*TUN_ALIGN(1504),s->Capacity);
    CHECK(s->Descriptor.Receive.Ring->Head==prefixHead);Tun32PartialVectors(available,FALSE);Take(submitted,available);Tun32PartialCheck(submitted,0,available,available,target);
    CHECK(s->Life.GetCloseProgress(Native,&progress)==PB_OK&&!progress.UserLeases&&progress.Published==64);
    InterlockedExchange(&Tun32.Stage,TUN32_PREFIX);CHECK(SetEvent(Tun32.PartialEvents[TUN32_PREFIX_GO]));Tun32PartialAwait(TUN32_FULL_READY);
    CHECK(Tun32.FullObserved&&s->Descriptor.Receive.Ring->Head==prefixHead);Tun32PartialVectors(available,FALSE);
    CHECK(SetEvent(Tun32.PartialEvents[TUN32_FULL_GO]));
    ULONGLONG deadline=GetTickCount64()+10000;
    while(!PbTestWaiters(Native)){CHECK(GetTickCount64()<deadline);SwitchToThread();}
    /* This existing shim marks the actual wait code segment, not scheduler
     * park. Native completion may race SDK entry; arm/recheck must cover both. */
    if(stop){
        InterlockedExchange(&Tun32.Enabled,FALSE);HANDLE ending=CreateThread(NULL,0,Tun32PartialEnd,s,0,NULL);CHECK(ending);
        CHECK(WaitForSingleObject(ending,10000)==WAIT_OBJECT_0&&CloseHandle(ending));s=NULL;
        Tun32PartialVectors(available,FALSE);
    }else{
        /* Free exactly the target suffix's needed slots using real original
         * tokens. No fake FULL, counter edit, event repair, or extra reserve. */
        InterlockedExchange(&Tun32.Stage,TUN32_RECOVER);CHECK(PbTestComplete(Native,0,occupied,32-available)==PB_OK);Tun32PartialAwait(TUN32_TARGET_READY);
        CHECK(s->Descriptor.Receive.Ring->Head==s->Descriptor.Receive.Ring->Tail);Tun32PartialVectors(available,TRUE);
        CHECK(Tun32.Calls[TUN32_POLL]==1&&Tun32.Vector[TUN32_POLL][0].Requested==64&&Tun32.Vector[TUN32_POLL][0].Accepted==32-available);
        Take(submitted+available,32-available);
        /* Check needs the complete target vector because earlier generations
         * can have the same slot index but must never have the same token. */
        Tun32PartialCheck(submitted,available,32-available,available,target);
        CHECK(s->Life.GetCloseProgress(Native,&progress)==PB_OK&&!progress.UserLeases&&progress.Published==64);
        CHECK(PbTestComplete(Native,0,occupied+32-available,32)==PB_OK&&PbTestComplete(Native,0,submitted,32)==PB_OK);
        InterlockedExchange(&Tun32.Stage,TUN32_FINAL);CHECK(SetEvent(Tun32.PartialEvents[TUN32_TARGET_GO]));Tun32PartialAwait(TUN32_RETIRED_READY);
        CHECK(Tun32.Calls[TUN32_POLL]==2&&Tun32.Vector[TUN32_POLL][1].Requested==64&&Tun32.Vector[TUN32_POLL][1].Accepted==64);
        CHECK(s->Life.GetCloseProgress(Native,&progress)==PB_OK&&!progress.UserLeases&&!progress.Published);
        InterlockedExchange(&Tun32.Enabled,FALSE);CHECK(SetEvent(Tun32.PartialEvents[TUN32_RETIRED_GO]));WintunEndSession(s);s=NULL;
    }
    EmptyRuntime();CHECK(!Cancelled&&!ReleasedPackets);BatchMode=0;
    CHECK(CloseHandle(BatchEntered)&&CloseHandle(BatchGate));BatchEntered=BatchGate=NULL;
    for(uint32_t i=0;i<TUN32_PARTIAL_EVENTS;++i)CHECK(CloseHandle(Tun32.PartialEvents[i]));
    uint32_t tail=stop?0:32-available;
    printf("TUN32_PARTIAL phase=%s ip=%u transport=%s free=%u stop=%u wrap=%u reserve-first=32/%u reserve-calls=%u reserve-tail=%u/%u commit-first=%u/%u commit-calls=%u commit-tail=%u/%u poll-calls=%u polled=%u copies=%u bytes=%llu prefix-head=exact wait-code=entered owners=0\n",phase,ipv6?6:4,udp?"UDP":"TCP",available,stop,wrap,available,Tun32.Calls[TUN32_RESERVE],tail,tail,available,available,Tun32.Calls[TUN32_COMMIT],tail,tail,Tun32.Calls[TUN32_POLL],stop?0:96-available,Tun32.Copies[0],(unsigned long long)Tun32.Bytes[0]);
    Tun32Free(seed);Tun32Free(target);memset(&Tun32,0,sizeof(Tun32));
    ++Tun32PartialCount;
}
static VOID Tun32PartialMatrix(const char *phase)
{
    const uint32_t available[]={1,7,31};
    /* Zero preserves the original IPv4/TCP matrix and its historical counts.
     * A selected family genuinely builds both transports in separate runs. */
    for(uint32_t udp=0;udp<(PB_TEST_IP_VERSION?2u:1u);++udp)for(uint32_t i=0;i<_countof(available);++i)for(uint32_t stop=0;stop<2;++stop)for(uint32_t wrap=0;wrap<2;++wrap)Tun32PartialCase(available[i],stop,wrap,PB_TEST_IP_VERSION==6,udp,phase);
}
static VOID Tun32PartialCases(VOID)
{
    Tun32PartialCount=0;Tun32PartialMatrix("warm");DWORD warm=Tun32PartialCount,before,after;CHECK(warm==(PB_TEST_IP_VERSION?24u:12u)&&GetProcessHandleCount(GetCurrentProcess(),&before));Tun32PartialMatrix("measure");
    CHECK(Tun32PartialCount==2*warm&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu natural partial Reserve32 cases (%lu warm/%lu measured), free1/7/31, real completion recovery or stopped tail, wrap, exact batch/copy/head ownership, handles%lu/%lu.\n",Tun32PartialCount,warm,Tun32PartialCount-warm,before,after);
}
#endif
