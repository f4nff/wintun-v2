/* SPDX-License-Identifier: GPL-2.0 */
/* Real shipping DLL calls and native-policy ownership ledger. Native terminal,
 * IOCTL status and references are hosted; WAIT_FAILED is defensive boundary
 * injection, not an observed Windows failure or a production defect. */
#ifdef PB_API_TESTS
#error Shipping local retirement required
#endif
#ifndef NDEBUG
#error Shipping NDEBUG required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../safety/guard_memory.h"
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
#include <string.h>
#define CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"shipping fingerprint required");
enum { LR_LOANS,LR_WAIT,LR_TRIGGER,LR_GO,LR_DONE,LR_EVENTS };
typedef struct LR_CONFIG {NP_CONFIG Native;HANDLE Events[LR_EVENTS];ULONG LoanQueue;BOOL Fault,Udp,Wrap;DWORD WaitOwner;} LR_CONFIG;
typedef struct LR_STATE {LONG Fatal,Closing,Terminal,Loans,NativeActive,Regions,Closed,EventState,Waits,Wakes,Timeouts,Refs,Pins,Callbacks;ULONG Phase;ULONG64 Producer;} LR_STATE;
static VOID Put16(BYTE *p,ULONG n){p[0]=(BYTE)(n>>8);p[1]=(BYTE)n;}
static ULONG Sum(ULONG sum,const BYTE *p,ULONG n){for(;n>1;n-=2,p+=2)sum+=((ULONG)p[0]<<8)|p[1];if(n)sum+=(ULONG)p[0]<<8;return sum;}
static USHORT Final(ULONG sum){while(sum>>16)sum=(sum&65535)+(sum>>16);return (USHORT)~sum;}
static VOID Packet(BYTE *p,BOOL udp,ULONG direction,ULONG sequence)
{
    memset(p,(int)(sequence+direction*37),1500);ULONG ip=PB_TEST_IP_VERSION==6?40u:20u,l4=1500-ip;memset(p,0,ip+(udp?8u:20u));
#if PB_TEST_IP_VERSION == 6
    p[0]=0x60;Put16(p+4,1460);p[6]=(BYTE)(udp?17:6);p[7]=64;p[8]=0x20;p[9]=1;p[23]=1;p[24]=0x20;p[25]=1;p[39]=2;
#else
    p[0]=0x45;Put16(p+2,1500);Put16(p+4,sequence);p[8]=64;p[9]=(BYTE)(udp?17:6);p[12]=10;p[15]=1;p[16]=10;p[19]=2;Put16(p+10,Final(Sum(0,p,20)));
#endif
    Put16(p+ip,1234);Put16(p+ip+2,4321);if(udp)Put16(p+ip+4,l4);else{p[ip+12]=0x50;p[ip+13]=0x10;Put16(p+ip+14,65535);}
    ULONG identity[2]={direction,sequence};memcpy(p+64,identity,sizeof(identity));ULONGLONG markers[2]={0x0123456789abcdefull+sequence,0xfedcba9876543210ull+direction};memcpy(p+96,markers,sizeof(markers));
    ULONG pseudo=PB_TEST_IP_VERSION==6?Sum(0,p+8,32):Sum(0,p+12,8);pseudo+=l4+(udp?17u:6u);USHORT checksum=Final(Sum(pseudo,p+ip,l4));Put16(p+ip+(udp?6u:16u),checksum?checksum:65535);
}

static VOID Await(HANDLE e){CHECK(WaitForSingleObject(e,10000)==WAIT_OBJECT_0);}
#ifdef LR_FIXTURE
static LR_CONFIG *LocalHost;
static DWORD WINAPI LocalWait(DWORD,const HANDLE *,BOOL,DWORD);
static DWORD WINAPI LocalProgress(HANDLE,DWORD);
#include "../safety/native_policy_fixture.c"
static uint64_t Base;
static LONG Waits,Wakes,Timeouts;
static DWORD WINAPI LocalWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Current && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent && events[1]==Current->Queues[1-LocalHost->LoanQueue].UserEvent && LocalHost->WaitOwner==GetCurrentThreadId());
    CHECK(SetEvent(LocalHost->Events[LR_WAIT]));Await(LocalHost->Events[LR_TRIGGER]);
    /* Valid private handles reached the selected SDK boundary. Do not exhaust
     * actual memory or close/poison handles to manufacture a real OS failure. */
    if(LocalHost->Fault){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return WAIT_FAILED;}
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result==WAIT_OBJECT_0+1);return result;
}
static DWORD WINAPI LocalProgress(HANDLE event,DWORD timeout)
{
    if(Current && event==Progress && timeout==1000){InterlockedIncrement(&Waits);DWORD result=ObservedWait(event,timeout);if(result==WAIT_OBJECT_0)InterlockedIncrement(&Wakes);else if(result==WAIT_TIMEOUT)InterlockedIncrement(&Timeouts);else CHECK(FALSE);return result;}
    return ObservedWait(event,timeout);
}
__declspec(dllexport) VOID WINAPI PrepareLocal(LR_CONFIG *config){CHECK(config && !LocalHost);LocalHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureLocal(PB_SESSION *session)
{
    Configure(session);ULONG q=LocalHost->LoanQueue;Base=LocalHost->Wrap?UINT64_MAX-15:0;
    for(ULONG r=PB_QUEUE_TX_READY;r<=PB_QUEUE_TX_RECYCLE;++r){PB_QUEUE_VIEW *v=Current->Queues[q].Rings+r;v->Cursor=v->PeerCursor=Base;v->Shared->Producer=v->Shared->Consumer=Base;}
    PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,q,PB_QUEUE_TX_READY,TRUE)==PB_OK);producer.Cursor=producer.PeerCursor=Base;
    PB_DESCRIPTOR items[32]={0};for(ULONG i=0;i<32;++i){items[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,(USHORT)q};items[i].DataOffset=Current->Layout.Queues[q].TxData+i*PB_BLOCK_SIZE;items[i].Length=1500;Packet(Current->Region+items[i].DataOffset,LocalHost->Udp,2,i);}
    uint32_t count,notify;CHECK(PbQueuePush(&producer,items,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI TriggerLocal(VOID)
{
    CHECK(Current && LoansNow()==32);
    /* Model only the terminal/progress/user notification, not complete SYS
     * Fault/Quiesce/Free or an IRQ/NDIS execution path. */
    if(!LocalHost->Fault){InterlockedExchange((LONG *)&Current->SharedLifecycle->Terminal,1);InterlockedIncrement64((LONG64 *)&Current->SharedLifecycle->Sequence);CHECK(SetEvent(Progress));for(ULONG q=0;q<2;++q)CHECK(SetEvent(Current->Queues[q].UserEvent));}
    CHECK(SetEvent(LocalHost->Events[LR_TRIGGER]));
}
__declspec(dllexport) VOID WINAPI InspectLocal(LR_STATE *out)
{
    CHECK(Current && out);PB_USER_QUEUE *q=Current->Queues+LocalHost->LoanQueue;ULONG retired=32-(ULONG)q->Loans;CHECK(retired<=32);
    for(ULONG i=0;i<32;++i)CHECK(q->TxSlots[i].State==(i<retired?PB_SLOT_FREE:PB_SLOT_PUBLISHED) && q->TxSlots[i].Generation==1);
    typedef LONG (NTAPI *QUERY_EVENT)(HANDLE,ULONG,VOID *,ULONG,ULONG *);QUERY_EVENT query;*(FARPROC *)&query=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");CHECK(query);struct {LONG Type,State;} event;
    CHECK(query(Progress,0,&event,sizeof(event),NULL)>=0 && event.Type==1 && (event.State==0 || event.State==1));
    AcquireSRWLockShared(&Current->State);*out=(LR_STATE){Current->Fatal,Current->CloseRequested,(LONG)Current->SharedLifecycle->Terminal,LoansNow(),NativeActive,Regions,Current->ResourcesClosed,event.State,Waits,Wakes,Timeouts,Current->References,Pins,CallbackCount,Current->Progress.Phase,q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer-Base};ReleaseSRWLockShared(&Current->State);
}
__declspec(dllexport) ULONG WINAPI JoinPreparedLocal(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2);
    return (ULONG)CallbackCount;
}
#else
static VOID PacketVerify(const BYTE *p,BOOL udp,ULONG seq){BYTE expected[1500];Packet(expected,udp,2,seq);CHECK(!memcmp(p,expected,sizeof(expected)));}
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(LR_CONFIG *);
typedef VOID (WINAPI *INSPECT)(LR_STATE *);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct JOB {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;LR_CONFIG *Config;BOOL Loan;PB_RESULT WaitResult;} JOB;
static DWORD WINAPI Owner(VOID *opaque)
{
    JOB *j=opaque;if(!j->Loan){j->Config->WaitOwner=GetCurrentThreadId();j->WaitResult=j->Api->Wait(j->Queue,INFINITE);return 0;}
    PB_LEASE leases[32];PB_TOKEN tokens[32];uint32_t count;CHECK(j->Api->ReceiveBatch(j->Queue,leases,32,&count)==PB_OK && count==32);
    for(ULONG i=0;i<32;++i){PacketVerify(leases[i].Data,j->Config->Udp,i);tokens[i]=leases[i].Descriptor.Token;}
    CHECK(SetEvent(j->Config->Events[LR_LOANS]));ULONG offset=0;const ULONG counts[3]={1,7,24};
    /* Caller-selected valid batches: no fake FULL or partial API result. No
     * returned payload is read after that call transfers its ownership. */
    for(ULONG step=0;step<3;++step){Await(j->Config->Events[LR_GO]);CHECK(j->Api->ReleaseBatch(j->Queue,tokens+offset,counts[step],&count)==PB_OK && count==counts[step]);offset+=count;CHECK(SetEvent(j->Config->Events[LR_DONE]));}
    CHECK(j->Api->Wait(j->Queue,0)==(j->Config->Fault?PB_CORRUPT:PB_STOPPED));return 0;
}
static ULONG Cases,PublicJoins,CallbackJoins;
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL wrap,ULONG loanQueue,BOOL fault,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;ACTION trigger;INSPECT inspect;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareLocal");GET(configure,"ConfigureLocal");GET(active,"SetNativeActive");GET(trigger,"TriggerLocal");GET(inspect,"InspectLocal");GET(joinPrepared,"JoinPreparedLocal");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);LR_CONFIG config={0};config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60014;config.Udp=udp;config.Wrap=wrap;config.LoanQueue=loanQueue;config.Fault=fault;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}for(ULONG i=0;i<LR_EVENTS;++i){config.Events[i]=CreateEventW(NULL,i!=LR_GO&&i!=LR_DONE,FALSE,NULL);CHECK(config.Events[i]);}
    prepare(&config);PB_CONFIG c={sizeof(c),2,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-local-retirement32",&c,&session)==PB_OK);configure(session);
    JOB jobs[2];HANDLE owners[2];for(ULONG q=0;q<2;++q){PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,q,&queue)==PB_OK);jobs[q]=(JOB){&api,queue,&config,q==loanQueue,PB_INVALID};owners[q]=CreateThread(NULL,0,Owner,jobs+q,0,NULL);CHECK(owners[q]);}
    Await(config.Events[LR_LOANS]);Await(config.Events[LR_WAIT]);trigger();Await(owners[1-loanQueue]);CHECK(CloseHandle(owners[1-loanQueue]) && jobs[1-loanQueue].WaitResult==(fault?PB_CORRUPT:PB_STOPPED));++PublicJoins;
    CHECK(api.PrepareClose(session,0)==PB_BUSY);Await(config.Native.Events[NP_PARK_ENTERED]);LR_STATE state;inspect(&state);CHECK(state.Loans==32 && state.Fatal==fault && state.Closing && state.Terminal==!fault && state.EventState==1 && state.Waits==1);
    CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);inspect(&state);CHECK(!state.EventState && state.Waits==2 && state.Wakes==1 && !state.Timeouts);
    const ULONG totals[3]={1,8,32};for(ULONG step=0;step<3;++step){CHECK(SetEvent(config.Events[LR_GO]));Await(config.Events[LR_DONE]);if(step==2){Await(owners[loanQueue]);CHECK(CloseHandle(owners[loanQueue]));++PublicJoins;}
        inspect(&state);CHECK(state.Loans==(LONG)(32-totals[step]) && state.Producer==(fault?0:totals[step]) && state.EventState==1 && state.NativeActive==1 && state.Regions==1 && !state.Closed && state.Fatal==fault && state.Terminal==!fault);
        CHECK(api.PrepareClose(session,0)==(step==2?PB_QUARANTINED:PB_BUSY));CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);inspect(&state);CHECK(!state.EventState && state.Waits==(LONG)(3+step) && state.Wakes==(LONG)(2+step) && !state.Timeouts && state.NativeActive==1 && !state.Closed);
    }
    CHECK(!state.Loans && state.Phase<PB_LIFE_PREPARED);active(0);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));CHECK(api.PrepareClose(session,5000)==PB_OK);ULONG prep=joinPrepared();inspect(&state);CHECK(state.Waits==5 && state.Wakes==5 && !state.Timeouts && state.Producer==(fault?0u:32u));
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(config.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));Await(config.Native.Events[NP_DETACHED]);ULONG joined=0;
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;++joined;}CHECK(!GetModuleHandleW(path) && joined==prep+1);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<LR_EVENTS;++i)CHECK(CloseHandle(config.Events[i]));++Cases;
    printf("CASE family=%u protocol=%s wrap=%u loan=%lu fault=%u measured=%u release=1/7/24 loans=31/24/0 producer=%s progress=1/1/1 native_barrier=1 waits=5 timeout=0 public_joins=2 callback_joins=%lu\n",PB_TEST_IP_VERSION,udp?"udp":"tcp",wrap,loanQueue,fault,measured,fault?"0/0/0":"1/8/32",joined);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG udp=0;udp<2;++udp)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG loan=0;loan<2;++loan)for(ULONG fault=0;fault<2;++fault)Scenario(argv[1],udp,wrap,loan,fault,pass!=0);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: LOCALRETIRE32 family%u cases%lu warm16 measured16 publicJoins%lu callbackJoins%lu handles%lu/%lu owners=0\n",PB_TEST_IP_VERSION,Cases,PublicJoins,CallbackJoins,before,after);return 0;
}
#endif
