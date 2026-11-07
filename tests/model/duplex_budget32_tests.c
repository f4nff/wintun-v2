/* SPDX-License-Identifier: GPL-2.0 */
/* Actual shipping facade worker/queues, hosted native producer and IO state.
 * Both32-packet directions stay ready while their512KiB byte budgets give
 * each direction one8-packet turn. This counts operations/copies, not CPU. */
#ifdef PB_API_TESTS
#error Shipping facade budget required
#endif
#ifndef NDEBUG
#error Shipping facade budget configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../safety/guard_memory.h"
#include "facade_stop_fixture.h"
#include <string.h>
#define CHECK SAFETY_CHECK
#define BYTES 65535u
enum { B_WAIT,B_WAIT_GO,B_TURN,B_TURN_GO,B_EVENTS };
typedef struct B_CONFIG {NP_CONFIG Native;FS_CONFIG Legacy;HANDLE Events[B_EVENTS];BOOL Udp,Wrap;ULONG StopTurn;} B_CONFIG;
typedef struct B_STATE {LONG Loans,Published,Closed,Stopping,Waits,CopyIn,CopyOut,Turns,Reserves,Commits,Receives,Releases,StopReserve,StopReturn,NativeActive;ULONG ReleaseCounts[5];} B_STATE;
static VOID Put16(BYTE *p,ULONG n){p[0]=(BYTE)(n>>8);p[1]=(BYTE)n;}
static ULONG Sum(ULONG sum,const BYTE *p,ULONG n){for(;n>1;n-=2,p+=2)sum+=((ULONG)p[0]<<8)|p[1];if(n)sum+=(ULONG)p[0]<<8;return sum;}
static USHORT Final(ULONG sum){while(sum>>16)sum=(sum&65535)+(sum>>16);return (USHORT)~sum;}
static VOID Packet(BYTE *p,BOOL udp,ULONG direction,ULONG sequence)
{
    memset(p,(int)(sequence+direction*37),65535);ULONG ip=PB_TEST_IP_VERSION==6?40u:20u,l4=65535-ip;memset(p,0,ip+(udp?8u:20u));
#if PB_TEST_IP_VERSION == 6
    p[0]=0x60;Put16(p+4,65495);p[6]=(BYTE)(udp?17:6);p[7]=64;p[8]=0x20;p[9]=1;p[23]=1;p[24]=0x20;p[25]=1;p[39]=2;
#else
    p[0]=0x45;Put16(p+2,65535);Put16(p+4,sequence);p[8]=64;p[9]=(BYTE)(udp?17:6);p[12]=10;p[15]=1;p[16]=10;p[19]=2;Put16(p+10,Final(Sum(0,p,20)));
#endif
    Put16(p+ip,1234);Put16(p+ip+2,4321);if(udp)Put16(p+ip+4,l4);else{p[ip+12]=0x50;p[ip+13]=0x10;Put16(p+ip+14,65535);}
    ULONG identity[2]={direction,sequence};memcpy(p+64,identity,sizeof(identity));ULONGLONG markers[2]={0x0123456789abcdefull+sequence,0xfedcba9876543210ull+direction};memcpy(p+96,markers,sizeof(markers));
    ULONG pseudo=PB_TEST_IP_VERSION==6?Sum(0,p+8,32):Sum(0,p+12,8);pseudo+=l4+(udp?17u:6u);USHORT checksum=Final(Sum(pseudo,p+ip,l4));Put16(p+ip+(udp?6u:16u),checksum?checksum:65535);
}

static VOID Verify(const BYTE *data,BOOL udp,ULONG direction,ULONG sequence){BYTE expected[BYTES];Packet(expected,udp,direction,sequence);CHECK(!memcmp(data,expected,BYTES));}
#ifdef B_FIXTURE
static B_CONFIG *BudgetHost;
static DWORD WINAPI BudgetWait(DWORD,const HANDLE *,BOOL,DWORD);
static VOID *BudgetCopy(VOID *,const VOID *,size_t);
#include PB_BUDGET_BASE
static PB_QUEUE_VIEW Ready,Submit,Complete;
static PB_DESCRIPTOR Produced[32];
static ULONG PeerCount,CopyHits[2][32],ReleaseCounts[5];
static LONG Waits,CopyIn,CopyOut,Turns,Reserves,Commits,Receives,Releases,StopReserve,StopReturn;
static VOID AwaitBudget(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI BudgetWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(count==3 && !all && timeout==INFINITE && events[2]==LegacyWake);++Waits;CHECK(SetEvent(BudgetHost->Events[B_WAIT]));AwaitBudget(BudgetHost->Events[B_WAIT_GO]);
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result>=WAIT_OBJECT_0 && result<WAIT_OBJECT_0+3);return result;
}
static VOID *BudgetCopy(VOID *dest,const VOID *source,size_t bytes)
{
    if(bytes==BYTES && Current && Facade){uintptr_t from=(uintptr_t)source,to=(uintptr_t)dest,base=(uintptr_t)Current->Region;ULONG id[2];memcpy(id,(const BYTE *)source+64,sizeof(id));CHECK(id[1]<32);
        if(from-base<Current->Layout.RegionSize){CHECK(id[0]==2 && to-(uintptr_t)Facade->Descriptor.Send.Ring->Data<Facade->Capacity+TUN_MAX_PACKET_SIZE && ++CopyHits[1][id[1]]==1);++CopyIn;}
        else{CHECK(to-base<Current->Layout.RegionSize && id[0]==1 && from-(uintptr_t)Facade->Descriptor.Receive.Ring->Data<Facade->Capacity+TUN_MAX_PACKET_SIZE && ++CopyHits[0][id[1]]==1);++CopyOut;}}
    return memcpy(dest,source,bytes);
}
static PB_RESULT WINAPI BudgetReserve(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t requested,uint32_t *count)
{
    CHECK(requested==8);PB_RESULT result=Raw.ReserveBatch(q,leases,requested,count);
    if(result==PB_STOPPED){CHECK(!*count && ++StopReserve==1);return result;}
    CHECK(result==PB_OK && *count==8 && ++Reserves<=4);for(ULONG i=0;i<*count;++i)CHECK(leases[i].Descriptor.Token.Generation==1);return result;
}
static PB_RESULT WINAPI BudgetCommit(PB_USER_QUEUE *q,const PB_LEASE *leases,uint32_t count,uint32_t *done)
{PB_RESULT result=Raw.CommitBatch(q,leases,count,done);CHECK(count==8 && result==PB_OK && *done==8 && ++Commits<=4);return result;}
static PB_RESULT WINAPI BudgetReceive(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t requested,uint32_t *count)
{PB_RESULT result=Raw.ReceiveBatch(q,leases,requested,count);if(*count)CHECK(requested==64 && *count==32 && result==PB_OK && ++Receives==1);return result;}
static PB_RESULT WINAPI BudgetRelease(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count,uint32_t *done)
{
    PB_RESULT result=Raw.ReleaseBatch(q,tokens,count,done);CHECK(result==PB_OK && *done==count && Releases<5);ReleaseCounts[Releases++]=count;
    /* The actual Release has returned and released its data/lifetime locks.
     * No test gate holds those locks or edits COMPAT_PENDING/queue counters. */
    if(Current->CloseRequested){CHECK(count==32-8*BudgetHost->StopTurn);StopReturn+=(LONG)count;}
    else{CHECK(count==8 && ++Turns<=4 && SetEvent(BudgetHost->Events[B_TURN]));AwaitBudget(BudgetHost->Events[B_TURN_GO]);}
    return result;
}
__declspec(dllexport) VOID WINAPI PrepareBudget(B_CONFIG *config){CHECK(config);BudgetHost=config;Initialize(&config->Native);PrepareFacade(&config->Legacy);(void)&FacadeWait;}
__declspec(dllexport) VOID *WINAPI StartBudget(VOID)
{WINTUN_ADAPTER adapter={0};adapter.InterfaceFilename=L"hosted-facade-budget32";VOID *session=WintunStartSession(&adapter,4u*1024*1024);CHECK(session && session==Facade);return session;}
__declspec(dllexport) VOID WINAPI ProduceBudget(VOID *session)
{
    /* First selected SDK wait hard-gates the sole worker. Only here are the
     * inherited observer table entries replaced, before subsequent use. */
    CHECK(session==Facade && Waits==1);Facade->Api.ReserveBatch=BudgetReserve;Facade->Api.CommitBatch=BudgetCommit;Facade->Api.CancelBatch=Raw.CancelBatch;Facade->Api.ReceiveBatch=BudgetReceive;Facade->Api.ReleaseBatch=BudgetRelease;
    ULONG base=BudgetHost->Wrap?Facade->Capacity-TUN_ALIGNMENT:0;Facade->Send.Head=Facade->Send.HeadRelease=base;Facade->Descriptor.Send.Ring->Head=Facade->Descriptor.Send.Ring->Tail=base;Facade->Receive.Tail=Facade->Receive.TailRelease=base;Facade->Descriptor.Receive.Ring->Head=Facade->Descriptor.Receive.Ring->Tail=base;
    CHECK(PbAttachQueue(&Ready,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK && PbAttachQueue(&Submit,Current->Region,&Current->Layout,0,PB_QUEUE_RX_SUBMIT,FALSE)==PB_OK && PbAttachQueue(&Complete,Current->Region,&Current->Layout,0,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);
    for(ULONG i=0;i<32;++i){Incoming[i]=(PB_DESCRIPTOR){.Token={Current->Epoch,1,(USHORT)i,0},.DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE,.Length=BYTES};Packet(Current->Region+Incoming[i].DataOffset,BudgetHost->Udp,2,i);}
    uint32_t done,notify;CHECK(PbQueuePush(&Ready,Incoming,32,&done,&notify)==PB_OK && done==32);if(notify)CHECK(SetEvent(Current->Queues[0].UserEvent));
    BYTE *packets[32];for(ULONG i=0;i<32;++i){packets[i]=WintunAllocateSendPacket(session,BYTES);CHECK(packets[i]);Packet(packets[i],BudgetHost->Udp,1,i);}for(ULONG i=32;i--;)WintunSendPacket(session,packets[i]);
}
__declspec(dllexport) VOID WINAPI ConsumeBudget(VOID *session,ULONG first)
{
    /* Every old public pointer is genuinely received/returned before Stop or
     * End. Native-peer payload reads happen while published ownership is held. */
    CHECK(session==Facade && first==PeerCount);BYTE *copies[8];for(ULONG i=0;i<8;++i){DWORD bytes;copies[i]=WintunReceivePacket(session,&bytes);CHECK(copies[i] && bytes==BYTES);Verify(copies[i],BudgetHost->Udp,2,first+i);}for(ULONG i=8;i--;)WintunReleaseReceivePacket(session,copies[i]);CHECK(!Facade->Send.PacketsToRelease);
    uint32_t done;CHECK(PbQueuePop(&Submit,Produced+PeerCount,8,&done)==PB_OK && done==8);for(ULONG i=0;i<8;++i){PB_DESCRIPTOR *d=Produced+PeerCount+i;CHECK(d->Length==BYTES && d->Token.Generation==1 && d->Token.Slot==PeerCount+i);Verify(Current->Region+d->DataOffset,BudgetHost->Udp,1,PeerCount+i);}PeerCount+=8;
}
__declspec(dllexport) VOID WINAPI CompleteBudget(VOID)
{uint32_t done,notify;CHECK(PeerCount==32 && PbQueuePush(&Complete,Produced,32,&done,&notify)==PB_OK && done==32);if(notify)CHECK(SetEvent(Current->Queues[0].UserEvent));}
__declspec(dllexport) VOID WINAPI StopBudget(VOID)
{CHECK(BudgetHost->StopTurn && !Facade->Send.PacketsToRelease && !Facade->Receive.PacketsToRelease && RawLife.BeginStop(Current)==PB_OK);}
__declspec(dllexport) VOID WINAPI ReadBudget(B_STATE *out)
{
    CHECK(Current && Facade && out);*out=(B_STATE){LoansNow(),Current->Queues[0].Published,Current->ResourcesClosed,Facade->Stopping,Waits,CopyIn,CopyOut,Turns,Reserves,Commits,Receives,Releases,StopReserve,StopReturn,NativeActive,{0}};memcpy(out->ReleaseCounts,ReleaseCounts,sizeof(ReleaseCounts));
}
__declspec(dllexport) BOOL WINAPI BudgetStopping(VOID){CHECK(Facade);return ReadAcquire(&Facade->Stopping)!=0;}
__declspec(dllexport) VOID WINAPI EndBudget(VOID *session)
{
    CHECK(session==Facade && !Facade->Send.PacketsToRelease && !Facade->Receive.PacketsToRelease);WintunEndSession(session);Facade=NULL;ULONG turns=BudgetHost->StopTurn?BudgetHost->StopTurn:4,packets=turns*8;
    CHECK(PeerCount==packets && CopyIn==(LONG)packets && CopyOut==(LONG)packets && Turns==(LONG)turns && Reserves==(LONG)turns && Commits==(LONG)turns && Receives==1);
    CHECK(StopReserve==(BudgetHost->StopTurn?1:0) && StopReturn==(LONG)(32-packets) && Releases==(LONG)(BudgetHost->StopTurn?turns+1:4));
    for(ULONG i=0;i<32;++i)CHECK(CopyHits[0][i]==(ULONG)(i<packets) && CopyHits[1][i]==(ULONG)(i<packets));for(ULONG i=0;i<turns;++i)CHECK(ReleaseCounts[i]==8);if(BudgetHost->StopTurn)CHECK(ReleaseCounts[turns]==32-packets);
    CHECK(!LegacyRuntime&&!LegacyHeap&&!LegacyRegions&&!LegacyHandles&&!LegacyPaths&&!LegacyPins&&!CompatRingBytes&&!RepairLoans);CHECK(HeapDestroy(ModuleHeap));ModuleHeap=NULL;
}
#else
static VOID AwaitBudget(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
typedef VOID (WINAPI *PREPARE)(B_CONFIG *);typedef VOID *(WINAPI *START)(VOID);typedef VOID (WINAPI *OBJECT)(VOID *);typedef VOID (WINAPI *CONSUME)(VOID *,ULONG);typedef VOID (WINAPI *ACTION)(VOID);typedef VOID (WINAPI *READ)(B_STATE *);typedef BOOL (WINAPI *STOPPING)(VOID);
typedef struct END_CALL {OBJECT End;VOID *Session;} END_CALL;
static DWORD WINAPI EndCaller(VOID *opaque){END_CALL *c=opaque;c->End(c->Session);return 0;}
static ULONG Cases,EndJoins,CallbackJoins;
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL wrap,ULONG stopTurn,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);PREPARE prepare;START start;OBJECT produce,end;CONSUME consume;ACTION complete,stop,recover;READ read;STOPPING stopping;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(prepare,"PrepareBudget");GET(start,"StartBudget");GET(produce,"ProduceBudget");GET(consume,"ConsumeBudget");GET(complete,"CompleteBudget");GET(stop,"StopBudget");GET(read,"ReadBudget");GET(end,"EndBudget");GET(recover,"RecoverFacadeNative");GET(stopping,"BudgetStopping");
#undef GET
    B_CONFIG config={0};config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60014;config.Legacy.Direction=FS_RECEIVE;config.Udp=udp;config.Wrap=wrap;config.StopTurn=stopTurn;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}for(ULONG i=0;i<FS_EVENT_COUNT;++i){config.Legacy.Events[i]=CreateEventW(NULL,TRUE,i==FS_STOP_GATE,NULL);CHECK(config.Legacy.Events[i]);}for(ULONG i=0;i<B_EVENTS;++i){config.Events[i]=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(config.Events[i]);}
    prepare(&config);VOID *session=start();AwaitBudget(config.Events[B_WAIT]);produce(session);CHECK(SetEvent(config.Events[B_WAIT_GO]));B_STATE state;ULONG turns=stopTurn?stopTurn:4;
    for(ULONG turn=1;turn<=turns;++turn){AwaitBudget(config.Events[B_TURN]);read(&state);CHECK(state.Turns==(LONG)turn && state.CopyIn==(LONG)(8*turn) && state.CopyOut==(LONG)(8*turn) && state.Published==(LONG)(8*turn) && state.Loans==(LONG)(32-8*turn) && state.Reserves==(LONG)turn && state.Commits==(LONG)turn && state.Receives==1 && state.Releases==(LONG)turn && state.NativeActive==1 && !state.Closed);consume(session,(turn-1)*8);if(turn<turns)CHECK(SetEvent(config.Events[B_TURN_GO]));}
    END_CALL call={end,session};HANDLE caller=NULL;
    if(stopTurn){stop();CHECK(SetEvent(config.Events[B_TURN_GO]));AwaitBudget(config.Legacy.Events[FS_CLOSE_ENTERED]);}
    else{complete();CHECK(SetEvent(config.Events[B_TURN_GO]));AwaitBudget(config.Events[B_WAIT]);read(&state);CHECK(!state.Loans&&!state.Published&&state.CopyIn==32&&state.CopyOut==32);caller=CreateThread(NULL,0,EndCaller,&call,0,NULL);CHECK(caller);ULONGLONG deadline=GetTickCount64()+10000;while(!stopping()){CHECK(GetTickCount64()<deadline);SwitchToThread();}CHECK(SetEvent(config.Events[B_WAIT_GO]));AwaitBudget(config.Legacy.Events[FS_CLOSE_ENTERED]);}
    AwaitBudget(config.Native.Events[NP_PARK_ENTERED]);read(&state);CHECK(!state.Loans && state.NativeActive==1 && !state.Closed && state.CopyIn==(LONG)(8*turns) && state.CopyOut==(LONG)(8*turns));recover();CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));AwaitBudget(config.Native.Events[NP_FINAL_ENTERED]);
    if(!caller){caller=CreateThread(NULL,0,EndCaller,&call,0,NULL);CHECK(caller);}AwaitBudget(caller);CHECK(CloseHandle(caller));++EndJoins;session=NULL;
    CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));ULONG joins=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){AwaitBudget(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;++joins;}CHECK(joins && FreeLibrary(library));AwaitBudget(config.Native.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<FS_EVENT_COUNT;++i)CHECK(CloseHandle(config.Legacy.Events[i]));for(ULONG i=0;i<B_EVENTS;++i)CHECK(CloseHandle(config.Events[i]));++Cases;
    printf("CASE family=%u protocol=%s wrap=%u stopTurn=%lu measured=%u turns=%lu in=%lu out=%lu reserveCommit=8x%lu receive=32 release=%s stoppedReserve=%u uncopiedRetired=%lu copyBytes=%lu endJoins=1 callbackJoins=%lu\n",PB_TEST_IP_VERSION,udp?"udp":"tcp",wrap,stopTurn,measured,turns,8*turns,8*turns,turns,stopTurn==1?"8/24":"8/8/8/8",stopTurn?1u:0u,32-8*turns,16*turns*BYTES,joins);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);DWORD before=0,after;const ULONG stops[3]={0,1,3};for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG udp=0;udp<2;++udp)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG stop=0;stop<3;++stop)Scenario(argv[1],udp,wrap,stops[stop],pass!=0);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: BUDGET32 family%u cases%lu warm12 measured12 endJoins%lu callbackJoins%lu handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,Cases,EndJoins,CallbackJoins,before,after);return 0;
}
#endif
