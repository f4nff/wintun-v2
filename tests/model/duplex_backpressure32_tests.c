/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted native producer, actual shipping DLL/legacy worker and SDK wait.
 * Terminal is a hint; the independent hosted Active=1 barrier remains until
 * the native cleanup callback parks. No driver/device/registry is accessed. */
#ifdef PB_API_TESTS
#error Shipping native paths required
#endif
#ifndef NDEBUG
#error Shipping facade branch required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../safety/guard_memory.h"
#include "facade_stop_fixture.h"
#include <string.h>
#define CHECK SAFETY_CHECK
enum { DP_WAIT,DP_GO,DP_EVENTS };
typedef struct DP_CONFIG {NP_CONFIG Native;FS_CONFIG Legacy;HANDLE Events[DP_EVENTS];BOOL Udp,Wrap,Stop;} DP_CONFIG;
typedef struct DP_STATE {LONG Waits,Loans,Published,Full,TargetReserved,TargetCommitted,TargetCopiedIn,TargetCopiedOut,Retired,ReadyArms;ULONG Head,Tail,ReserveBatches,CommitBatches,PollBatches,ReleaseBatches;} DP_STATE;
#define MARK 0x44555032u
static void Put16(BYTE *p,ULONG x){p[0]=(BYTE)(x>>8);p[1]=(BYTE)x;}
static ULONG Sum(ULONG sum,const BYTE *p,ULONG n){for(;n>1;n-=2,p+=2)sum+=((ULONG)p[0]<<8)|p[1];if(n)sum+=(ULONG)p[0]<<8;return sum;}
static USHORT Final(ULONG sum){while(sum>>16)sum=(sum&65535)+(sum>>16);return (USHORT)~sum;}
static void Packet(BYTE *p,ULONG n,BOOL udp,ULONG direction,ULONG sequence,BOOL target)
{
    CHECK(n>=1500 && n<=65535);memset(p,(int)(sequence+direction*31),n);
    ULONG ip=PB_TEST_IP_VERSION==6?40u:20u,l4=n-ip,header=udp?8u:20u;memset(p,0,ip+header);
#if PB_TEST_IP_VERSION == 6
    p[0]=0x60;Put16(p+4,n-40);p[6]=(BYTE)(udp?17:6);p[7]=64;p[8]=0x20;p[9]=1;p[23]=1;p[24]=0x20;p[25]=1;p[39]=2;
#else
    p[0]=0x45;Put16(p+2,n);Put16(p+4,sequence);p[8]=64;p[9]=(BYTE)(udp?17:6);p[12]=10;p[15]=1;p[16]=10;p[19]=2;Put16(p+10,Final(Sum(0,p,20)));
#endif
    Put16(p+ip,1234);Put16(p+ip+2,4321);if(udp)Put16(p+ip+4,l4);else{p[ip+12]=0x50;p[ip+13]=0x10;Put16(p+ip+14,65535);}
    ULONG values[4]={MARK,direction,sequence,(ULONG)target};memcpy(p+64,values,sizeof(values));
    ULONGLONG marks[2]={0x1020304050607080ull+sequence,0xf1e2d3c4b5a69788ull+direction};memcpy(p+96,marks,sizeof(marks));
    ULONG pseudo=PB_TEST_IP_VERSION==6?Sum(0,p+8,32):Sum(0,p+12,8);pseudo+=l4+(udp?17u:6u);
    USHORT checksum=Final(Sum(pseudo,p+ip,l4));Put16(p+ip+(udp?6u:16u),checksum?checksum:65535);
}
static void Verify(const BYTE *p,ULONG n,BOOL udp,ULONG direction,ULONG sequence,BOOL target)
{BYTE expected[1500];CHECK(n==1500);Packet(expected,n,udp,direction,sequence,target);CHECK(!memcmp(p,expected,n));}
#ifdef BP_FIXTURE
static DP_CONFIG *DuplexHost;
static DWORD WINAPI BackWait(DWORD,const HANDLE *,BOOL,DWORD);
static PB_RESULT BackArm(PB_QUEUE_VIEW *,uint32_t *);
static void *DuplexCopy(void *,const void *,size_t);
#include PB_BACKPRESSURE_BASE
static PB_QUEUE_VIEW Ready,Recycle,Submit,Complete;
static PB_DESCRIPTOR Seed[64],Target[32];
static LONG Waits,Full,TargetReserved,TargetCommitted,TargetCopiedIn,TargetCopiedOut,Retired,ReadyArms;
static ULONG CopyHits[2][32],ReserveBatches,CommitBatches,PollBatches,ReleaseBatches;
static BOOL Targets;
static void Await(HANDLE e){CHECK(WaitForSingleObject(e,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI BackWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(count==3 && !all && timeout==INFINITE && events[2]==LegacyWake);
    /* Entry observation is before the real SDK call, not proof of kernel park.
     * This hard gate also makes all private pump/counter snapshots stable. */
    InterlockedIncrement(&Waits);CHECK(SetEvent(DuplexHost->Events[DP_WAIT]));Await(DuplexHost->Events[DP_GO]);
    DWORD r=WaitForMultipleObjects(count,events,all,timeout);CHECK(r>=WAIT_OBJECT_0 && r<WAIT_OBJECT_0+3);return r;
}
static PB_RESULT BackArm(PB_QUEUE_VIEW *view,uint32_t *ready)
{if(Current && view==Current->Queues[0].Rings+PB_QUEUE_TX_READY)++ReadyArms;return PbQueueArm(view,ready);}
static void *DuplexCopy(void *dest,const void *source,size_t bytes)
{
    if(bytes==1500 && Current && Facade) {
        uintptr_t from=(uintptr_t)source,to=(uintptr_t)dest,base=(uintptr_t)Current->Region;ULONG fields[4];memcpy(fields,(const BYTE *)source+64,sizeof(fields));
        if(fields[0]==MARK && fields[3]) {
            uintptr_t incoming=(uintptr_t)Facade->Descriptor.Send.Ring->Data,outgoing=(uintptr_t)Facade->Descriptor.Receive.Ring->Data;
            CHECK(fields[2]<32);
            if(from-base<Current->Layout.RegionSize){CHECK(fields[1]==2 && to-incoming<Facade->Capacity+TUN_MAX_PACKET_SIZE && ++CopyHits[1][fields[2]]==1);++TargetCopiedIn;}
            else if(to-base<Current->Layout.RegionSize){CHECK(fields[1]==1 && from-outgoing<Facade->Capacity+TUN_MAX_PACKET_SIZE && ++CopyHits[0][fields[2]]==1);++TargetCopiedOut;}
        }
    }
    return memcpy(dest,source,bytes);
}
static PB_RESULT WINAPI RecordReserve(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t requested,uint32_t *count)
{
    PB_RESULT r=Raw.ReserveBatch(q,leases,requested,count);
    if(Targets){CHECK(requested<=32);if(r==PB_FULL){CHECK(!*count);++Full;}if(*count){CHECK(requested==32 && *count==(DuplexHost->Stop?7u:32u) && ++ReserveBatches==1);for(ULONG i=0;i<*count;++i)CHECK(leases[i].Descriptor.Token.Generation==2);TargetReserved+=(LONG)*count;}}
    return r;
}
static PB_RESULT WINAPI RecordCommit(PB_USER_QUEUE *q,const PB_LEASE *leases,uint32_t count,uint32_t *done)
{PB_RESULT r=Raw.CommitBatch(q,leases,count,done);CHECK(r==PB_OK && *done==count);if(Targets){CHECK(count==(DuplexHost->Stop?7u:32u) && ++CommitBatches==1);TargetCommitted+=(LONG)*done;}return r;}
static PB_RESULT WINAPI RecordRelease(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count,uint32_t *done)
{PB_RESULT r=Raw.ReleaseBatch(q,tokens,count,done);CHECK(r==PB_OK && *done==count && count==(ReleaseBatches?26u:8u) && ++ReleaseBatches<=2);Retired+=(LONG)*done;return r;}
static PB_RESULT WINAPI RecordPoll(PB_USER_QUEUE *q,PB_DESCRIPTOR *items,uint32_t requested,uint32_t *count)
{PB_RESULT r=Raw.PollCompletions(q,items,requested,count);if(*count){CHECK(r==PB_OK && *count==(PollBatches?64u:(DuplexHost->Stop?7u:32u)) && ++PollBatches<3);}return r;}
__declspec(dllexport) VOID WINAPI DuplexPrepare(DP_CONFIG *config)
{CHECK(config);DuplexHost=config;Initialize(&config->Native);PrepareFacade(&config->Legacy);(void)&FacadeWait;}
__declspec(dllexport) VOID *WINAPI DuplexStart(VOID){return StartFacade();}
__declspec(dllexport) VOID WINAPI DuplexSeed(VOID *session)
{
    CHECK(session==Facade && Waits==1);Facade->Api.ReserveBatch=RecordReserve;Facade->Api.CommitBatch=RecordCommit;Facade->Api.CancelBatch=Raw.CancelBatch;Facade->Api.ReleaseBatch=RecordRelease;Facade->Api.PollCompletions=RecordPoll;
    ULONG cursor=DuplexHost->Wrap?Facade->Capacity-TUN_ALIGNMENT:0;
    Facade->Send.Head=Facade->Send.HeadRelease=cursor;Facade->Descriptor.Send.Ring->Head=Facade->Descriptor.Send.Ring->Tail=cursor;
    Facade->Receive.Tail=Facade->Receive.TailRelease=cursor;Facade->Descriptor.Receive.Ring->Head=Facade->Descriptor.Receive.Ring->Tail=cursor;
    CHECK(PbAttachQueue(&Ready,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK && PbAttachQueue(&Recycle,Current->Region,&Current->Layout,0,PB_QUEUE_TX_RECYCLE,FALSE)==PB_OK);
    CHECK(PbAttachQueue(&Submit,Current->Region,&Current->Layout,0,PB_QUEUE_RX_SUBMIT,FALSE)==PB_OK && PbAttachQueue(&Complete,Current->Region,&Current->Layout,0,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);
    for(ULONG i=0;i<34;++i){ULONG length=i<2?61000u:1500u;Incoming[i]=(PB_DESCRIPTOR){.Token={Current->Epoch,1,(USHORT)i,0},.DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE,.Length=length};Packet(Current->Region+Incoming[i].DataOffset,length,DuplexHost->Udp,2,i<2?i:i-2,i>=2);}
    uint32_t done,notify;CHECK(PbQueuePush(&Ready,Incoming,34,&done,&notify)==PB_OK && done==34);CHECK(SetEvent(Current->Queues[0].UserEvent));
    BYTE *packets[64];for(ULONG i=0;i<64;++i){packets[i]=WintunAllocateSendPacket(session,1500);CHECK(packets[i]);Packet(packets[i],1500,DuplexHost->Udp,1,i,FALSE);}for(ULONG i=64;i--;)WintunSendPacket(session,packets[i]);
}
__declspec(dllexport) VOID WINAPI DuplexTargets(VOID *session)
{
    CHECK(session==Facade && !Targets && LoansNow()==26 && Current->Queues[0].Published==64);
    uint32_t done;CHECK(PbQueuePop(&Submit,Seed,64,&done)==PB_OK && done==64);for(ULONG i=0;i<64;++i)Verify(Current->Region+Seed[i].DataOffset,Seed[i].Length,DuplexHost->Udp,1,i,FALSE);
    BYTE *packets[32];Targets=TRUE;for(ULONG i=0;i<32;++i){packets[i]=WintunAllocateSendPacket(session,1500);CHECK(packets[i]);Packet(packets[i],1500,DuplexHost->Udp,1,i,TRUE);}for(ULONG i=32;i--;)WintunSendPacket(session,packets[i]);
}
__declspec(dllexport) BYTE *WINAPI DuplexReceive(VOID *session,ULONG index)
{
    DWORD length;BYTE *packet=WintunReceivePacket(session,&length);CHECK(packet && length==(index<2?61000u:1500u));
    if(index>=2)Verify(packet,length,DuplexHost->Udp,2,index-2,TRUE);else{BYTE *expected=malloc(length);CHECK(expected);Packet(expected,length,DuplexHost->Udp,2,index,FALSE);CHECK(!memcmp(packet,expected,length));free(expected);}return packet;
}
__declspec(dllexport) VOID WINAPI DuplexReturn(VOID *session,BYTE *packet){WintunReleaseReceivePacket(session,packet);}
__declspec(dllexport) VOID WINAPI DuplexComplete(ULONG stage)
{
    uint32_t done,notify;ULONG n=DuplexHost->Stop?7u:32u;
    if(!stage){CHECK(PbQueuePush(&Complete,Seed,n,&done,&notify)==PB_OK && done==n);}
    else{CHECK(!DuplexHost->Stop && stage==1);CHECK(PbQueuePop(&Submit,Target,32,&done)==PB_OK && done==32);for(ULONG i=0;i<32;++i)Verify(Current->Region+Target[i].DataOffset,Target[i].Length,DuplexHost->Udp,1,i,TRUE);PB_DESCRIPTOR all[64];memcpy(all,Seed+32,32*sizeof(*all));memcpy(all+32,Target,sizeof(Target));CHECK(PbQueuePush(&Complete,all,64,&done,&notify)==PB_OK && done==64);}
    CHECK(SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) VOID WINAPI DuplexStop(VOID)
{
    CHECK(Current && Facade);
    if(DuplexHost->Stop){uint32_t done;CHECK(PbQueuePop(&Submit,Target,7,&done)==PB_OK && done==7);for(ULONG i=0;i<7;++i)Verify(Current->Region+Target[i].DataOffset,Target[i].Length,DuplexHost->Udp,1,i,TRUE);}
    InterlockedExchange((LONG *)&Current->SharedLifecycle->Terminal,1);InterlockedIncrement64((LONG64 *)&Current->SharedLifecycle->Sequence);CHECK(SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) VOID WINAPI DuplexRead(DP_STATE *state)
{CHECK(state && Current && Facade);*state=(DP_STATE){Waits,LoansNow(),Current->Queues[0].Published,Full,TargetReserved,TargetCommitted,TargetCopiedIn,TargetCopiedOut,Retired,ReadyArms,Facade->Descriptor.Receive.Ring->Head,Facade->Descriptor.Receive.Ring->Tail,ReserveBatches,CommitBatches,PollBatches,ReleaseBatches};}
__declspec(dllexport) VOID WINAPI DuplexEnd(VOID *session)
{
    CHECK(session==Facade && !Facade->Send.PacketsToRelease && !Facade->Receive.PacketsToRelease);WintunEndSession(session);Facade=NULL;
    CHECK(TargetReserved==(DuplexHost->Stop?7:32) && TargetCommitted==TargetReserved && TargetCopiedOut==TargetReserved && TargetCopiedIn==(DuplexHost->Stop?6:32));
    for(ULONG i=0;i<32;++i){CHECK(CopyHits[0][i]==(ULONG)(i<(ULONG)TargetCopiedOut) && CopyHits[1][i]==(ULONG)(i<(ULONG)TargetCopiedIn));}
    CHECK(ReserveBatches==1 && CommitBatches==1 && ReleaseBatches==2 && PollBatches==(DuplexHost->Stop?1u:2u) && Retired==34 && Full>=1);
    CHECK(!LegacyRuntime && !LegacyHeap && !LegacyRegions && !LegacyHandles && !LegacyPaths && !LegacyPins && !CompatRingBytes && !RepairLoans);CHECK(HeapDestroy(ModuleHeap));ModuleHeap=NULL;
}
#else
typedef VOID (WINAPI *DP_PREPARE)(DP_CONFIG *);
typedef VOID *(WINAPI *DP_START)(VOID);
typedef VOID (WINAPI *DP_SESSION)(VOID *);
typedef BYTE *(WINAPI *DP_RECEIVE)(VOID *,ULONG);
typedef VOID (WINAPI *DP_RETURN)(VOID *,BYTE *);
typedef VOID (WINAPI *DP_NUMBER)(ULONG);
typedef VOID (WINAPI *DP_READ)(DP_STATE *);
typedef VOID (WINAPI *DP_ACTION)(VOID);
static void Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
typedef struct END_CALL {DP_SESSION End;VOID *Session;} END_CALL;
static DWORD WINAPI EndCaller(VOID *opaque){END_CALL *call=opaque;call->End(call->Session);return 0;}
static ULONG Cases,CallbackJoins,EndJoins;
static void Scenario(LPCWSTR path,BOOL udp,BOOL wrap,BOOL stop,BOOL reverse,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);DP_PREPARE prepare;DP_START start;DP_SESSION seed,targets,end;DP_RECEIVE receive;DP_RETURN release;DP_NUMBER complete;DP_READ read;DP_ACTION terminal,recover;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(prepare,"DuplexPrepare");GET(start,"DuplexStart");GET(seed,"DuplexSeed");GET(targets,"DuplexTargets");GET(receive,"DuplexReceive");GET(release,"DuplexReturn");GET(complete,"DuplexComplete");GET(read,"DuplexRead");GET(end,"DuplexEnd");GET(terminal,"DuplexStop");GET(recover,"RecoverFacadeNative");
#undef GET
    DP_CONFIG config={0};config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60059;config.Legacy.Direction=FS_RECEIVE;config.Udp=udp;config.Wrap=wrap;config.Stop=stop;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}
    for(ULONG i=0;i<FS_EVENT_COUNT;++i){config.Legacy.Events[i]=CreateEventW(NULL,TRUE,i==FS_STOP_GATE,NULL);CHECK(config.Legacy.Events[i]);}
    for(ULONG i=0;i<DP_EVENTS;++i){config.Events[i]=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(config.Events[i]);}
    prepare(&config);VOID *session=start();Await(config.Events[DP_WAIT]);seed(session);CHECK(SetEvent(config.Events[DP_GO]));Await(config.Events[DP_WAIT]);
    DP_STATE state;read(&state);CHECK(state.Loans==26 && state.Published==64 && state.TargetCopiedIn==6 && !state.TargetCopiedOut && state.Retired==8);
    BYTE *held[8];for(ULONG i=0;i<8;++i)held[i]=receive(session,i);for(ULONG i=8;i-->1;)release(session,held[i]);targets(session);
    CHECK(SetEvent(config.Events[DP_GO]));Await(config.Events[DP_WAIT]);read(&state);CHECK(state.Full>=1 && state.Loans==26 && state.Published==64 && state.TargetCopiedIn==6 && !state.TargetCopiedOut);LONG armed=state.ReadyArms;
    /* Both orderings publish real auto-reset hints while the worker remains
     * at its selected SDK entry. Neither event is manually reset or repaired. */
    if(!stop && reverse)release(session,held[0]);complete(0);if(!stop && !reverse)release(session,held[0]);CHECK(SetEvent(config.Events[DP_GO]));Await(config.Events[DP_WAIT]);read(&state);
    CHECK(state.TargetReserved==(stop?7:32) && state.TargetCommitted==(stop?7:32) && state.TargetCopiedOut==(stop?7:32) && state.TargetCopiedIn==(stop?6:32) && state.Loans==(stop?26:0) && state.Published==64 && state.ReserveBatches==1 && state.CommitBatches==1);
    if(stop){CHECK(state.ReadyArms==armed);terminal();release(session,held[0]);}
    else{
        BYTE *rest[26];for(ULONG i=0;i<26;++i)rest[i]=receive(session,i+8);for(ULONG i=26;i--;)release(session,rest[i]);complete(1);
        for(ULONG turns=0;;++turns){CHECK(turns<16);CHECK(SetEvent(config.Events[DP_GO]));Await(config.Events[DP_WAIT]);read(&state);if(!state.Published)break;}
        CHECK(!state.Loans && state.TargetCopiedIn==32 && state.TargetCopiedOut==32 && state.Retired==34 && state.Head==state.Tail);terminal();
    }
    CHECK(SetEvent(config.Events[DP_GO]));Await(config.Legacy.Events[FS_STOP_ENTERED]);Await(config.Legacy.Events[FS_CLOSE_ENTERED]);Await(config.Native.Events[NP_PARK_ENTERED]);
    read(&state);CHECK(!state.Loans && state.TargetCopiedIn==(stop?6:32) && state.TargetCopiedOut==(stop?7:32) && state.Retired==34);
    recover();CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_FINAL_ENTERED]);
    END_CALL call={end,session};HANDLE caller=CreateThread(NULL,0,EndCaller,&call,0,NULL);CHECK(caller);Await(caller);CHECK(CloseHandle(caller));++EndJoins;session=NULL;
    CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));ULONG joined=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;++joined;}
    CHECK(joined);CHECK(FreeLibrary(library));Await(config.Native.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<FS_EVENT_COUNT;++i)CHECK(CloseHandle(config.Legacy.Events[i]));for(ULONG i=0;i<DP_EVENTS;++i)CHECK(CloseHandle(config.Events[i]));
    ++Cases;printf("CASE family=%u protocol=%s wrap=%u stop=%u reverse=%u measured=%u in=%u out=%u pending_retired=%u reserve=1 commit=1 release=2 poll=%u copied_bytes=%u waits=%ld joins=%lu\n",PB_TEST_IP_VERSION,udp?"udp":"tcp",wrap,stop,reverse,measured,stop?6u:32u,stop?7u:32u,stop?26u:0u,stop?1u:2u,(stop?13u:64u)*1500,state.Waits,joined);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);DWORD before=0,after;for(ULONG pass=0;pass<4;++pass){if(pass==2)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG udp=0;udp<2;++udp)for(ULONG wrap=0;wrap<2;++wrap){Scenario(argv[1],udp,wrap,FALSE,FALSE,pass>=2);Scenario(argv[1],udp,wrap,FALSE,TRUE,pass>=2);Scenario(argv[1],udp,wrap,TRUE,FALSE,pass>=2);}}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: family%u duplex backpressure32 cases%lu warm24 measured24 endjoins%lu callbackjoins%lu handles%lu/%lu\n",PB_TEST_IP_VERSION,Cases,EndJoins,CallbackJoins,before,after);return 0;
}
#endif
