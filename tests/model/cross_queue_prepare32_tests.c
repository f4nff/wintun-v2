/* SPDX-License-Identifier: GPL-2.0 */
/* PrepareClose is non-final: public callers can still hold Lifetime pins.
 * All those callers must join before the only final Close consumes the handle.
 * Native IO/state are hosted, queue/lifecycle/waits and DLL unload are real. */
#ifdef PB_API_TESTS
#error Actual queue/lifecycle paths required
#endif
#ifndef NDEBUG
#error Shipping queue/lifecycle branch required
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
enum { CQ_PIN_ENTERED,CQ_LOAN_ENTERED,CQ_PIN_TAIL,CQ_PIN_GO,CQ_RETIRE,CQ_CV_ENTERED,CQ_EVENTS };
typedef struct CQ_CONFIG {NP_CONFIG Native;HANDLE Events[CQ_EVENTS];ULONG PinQueue;BOOL Udp,Wrap;DWORD Owners[2];} CQ_CONFIG;
typedef struct CQ_STATE {LONG EventState,Loans,NativeActive,Regions,Closed,Waits,Wakes,Timeouts,PrepareSleeps,Refs,Pins,Callbacks;ULONG Phase;BOOL LifetimeAvailable;} CQ_STATE;
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
#ifdef CT_FIXTURE
static CQ_CONFIG *CrossHost;
static DWORD WINAPI CrossWait(DWORD,const HANDLE *,BOOL,DWORD);
static DWORD WINAPI CrossProgress(HANDLE,DWORD);
static BOOL WINAPI CrossSleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
#include "../safety/native_policy_fixture.c"
static volatile LONG Waits,Wakes,Timeouts,PrepareSleeps;
static DWORD WINAPI CrossWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Current && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent);ULONG q;
    for(q=0;q<2 && events[1]!=Current->Queues[q].UserEvent;++q){}CHECK(q<2 && CrossHost->Owners[q]==GetCurrentThreadId());
    CHECK(SetEvent(CrossHost->Events[q==CrossHost->PinQueue?CQ_PIN_ENTERED:CQ_LOAN_ENTERED]));
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result==WAIT_OBJECT_0);
    /* The SDK has returned; only the DLL Wait tail remains pinned. This is
     * deliberately not described as an OS scheduler parking witness. */
    if(q==CrossHost->PinQueue){CHECK(SetEvent(CrossHost->Events[CQ_PIN_TAIL]));Await(CrossHost->Events[CQ_PIN_GO]);}
    return result;
}
static DWORD WINAPI CrossProgress(HANDLE event,DWORD timeout)
{
    if(Current && event==Progress && timeout==1000){InterlockedIncrement(&Waits);DWORD result=ObservedWait(event,timeout);if(result==WAIT_OBJECT_0)InterlockedIncrement(&Wakes);else if(result==WAIT_TIMEOUT)InterlockedIncrement(&Timeouts);else CHECK(FALSE);return result;}
    return ObservedWait(event,timeout);
}
static BOOL WINAPI CrossSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    CHECK(Current && cv==&Current->Changed && lock==&Current->State && !flags && timeout && timeout<=5000);
    /* Entry is a condition-variable call marker, not a hidden kernel park.
     * The real SDK releases/reacquires State; no hard gate holds State here. */
    InterlockedIncrement(&PrepareSleeps);CHECK(SetEvent(CrossHost->Events[CQ_CV_ENTERED]));return SleepConditionVariableSRW(cv,lock,timeout,flags);
}
__declspec(dllexport) VOID WINAPI PrepareCross(CQ_CONFIG *config){CHECK(config && !CrossHost);CrossHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureCross(PB_SESSION *session)
{
    Configure(session);ULONG q=CrossHost->PinQueue;PB_USER_QUEUE *queue=Current->Queues+q;uint64_t base=CrossHost->Wrap?UINT64_MAX-15:0;
    for(ULONG r=PB_QUEUE_TX_READY;r<=PB_QUEUE_TX_RECYCLE;++r){PB_QUEUE_VIEW *view=queue->Rings+r;view->Cursor=view->PeerCursor=base;view->Shared->Producer=view->Shared->Consumer=base;}
    PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,q,PB_QUEUE_TX_READY,TRUE)==PB_OK);producer.Cursor=producer.PeerCursor=base;
    PB_DESCRIPTOR packets[32]={0};for(ULONG i=0;i<32;++i){packets[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,(USHORT)q};packets[i].DataOffset=Current->Layout.Queues[q].TxData+i*PB_BLOCK_SIZE;packets[i].Length=1500;Packet(Current->Region+packets[i].DataOffset,CrossHost->Udp,2,i);}
    uint32_t count,notify;CHECK(PbQueuePush(&producer,packets,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI InspectCross(CQ_STATE *out)
{
    CHECK(Current && out);BOOL acquired=TryAcquireSRWLockExclusive(&Current->Lifetime);if(acquired)ReleaseSRWLockExclusive(&Current->Lifetime);
    typedef LONG (NTAPI *QUERY_EVENT)(HANDLE,ULONG,VOID *,ULONG,ULONG *);QUERY_EVENT query;*(FARPROC *)&query=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");CHECK(query);struct {LONG Type,State;} event;
    CHECK(query(Progress,0,&event,sizeof(event),NULL)>=0 && event.Type==1 && (event.State==0 || event.State==1));
    AcquireSRWLockShared(&Current->State);*out=(CQ_STATE){event.State,LoansNow(),NativeActive,Regions,Current->ResourcesClosed,Waits,Wakes,Timeouts,PrepareSleeps,Current->References,Pins,CallbackCount,Current->Progress.Phase,acquired};ReleaseSRWLockShared(&Current->State);
}
__declspec(dllexport) VOID WINAPI JoinPreparedCross(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2);
    for(ULONG q=0;q<2;++q)for(ULONG i=0;i<32;++i)CHECK(Current->Queues[q].RxSlots[i].State==PB_SLOT_FREE && Current->Queues[q].TxSlots[i].State==PB_SLOT_FREE);
}
#else
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(CQ_CONFIG *);
typedef VOID (WINAPI *INSPECT)(CQ_STATE *);
typedef VOID (WINAPI *ACTION)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;CQ_CONFIG *Config;INSPECT Inspect;ULONG Index;} OWNER;
typedef struct PREPARER {PB_INTERFACE_V2 *Api;PB_SESSION *Session;PB_RESULT Result;} PREPARER;
static DWORD WINAPI Owner(VOID *opaque)
{
    OWNER *owner=opaque;PB_LEASE leases[32];PB_TOKEN tokens[32];uint32_t count;owner->Config->Owners[owner->Index]=GetCurrentThreadId();BOOL pin=owner->Index==owner->Config->PinQueue;
    CHECK((pin?owner->Api->ReceiveBatch(owner->Queue,leases,32,&count):owner->Api->ReserveBatch(owner->Queue,leases,32,&count))==PB_OK && count==32);
    for(ULONG i=0;i<32;++i){tokens[i]=leases[i].Descriptor.Token;if(pin){BYTE expected[1500];Packet(expected,owner->Config->Udp,2,i);CHECK(!memcmp(leases[i].Data,expected,1500));}else{Packet(leases[i].Data,owner->Config->Udp,1,i);leases[i].Descriptor.Length=1500;}}
    if(pin)CHECK(owner->Api->ReleaseBatch(owner->Queue,tokens,32,&count)==PB_OK && count==32);
    CHECK(owner->Api->Wait(owner->Queue,INFINITE)==PB_STOPPED);
    if(!pin){Await(owner->Config->Events[CQ_RETIRE]);CHECK(owner->Api->CommitBatch(owner->Queue,leases,32,&count)==PB_STOPPED && !count);CQ_STATE state;owner->Inspect(&state);CHECK(!state.EventState && state.Loans==32);CHECK(owner->Api->CancelBatch(owner->Queue,tokens,32)==PB_OK);}
    return 0;
}
static DWORD WINAPI Preparer(VOID *opaque){PREPARER *p=opaque;p->Result=p->Api->PrepareClose(p->Session,5000);return 0;}
static ULONG Cases,PublicJoins,CallbackJoins;
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL wrap,ULONG pin,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;ACTION joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareCross");GET(configure,"ConfigureCross");GET(active,"SetNativeActive");GET(inspect,"InspectCross");GET(joinPrepared,"JoinPreparedCross");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);CQ_CONFIG config={0};config.PinQueue=pin;config.Udp=udp;config.Wrap=wrap;config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60014;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}for(ULONG i=0;i<CQ_EVENTS;++i){config.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(config.Events[i]);}
    prepare(&config);PB_CONFIG c={sizeof(c),2,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-crossqueue-prepare32",&c,&session)==PB_OK);configure(session);
    OWNER jobs[2];HANDLE owners[2];for(ULONG i=0;i<2;++i){PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,i,&queue)==PB_OK);jobs[i]=(OWNER){&api,queue,&config,inspect,i};owners[i]=CreateThread(NULL,0,Owner,jobs+i,0,NULL);CHECK(owners[i]);}
    Await(config.Events[CQ_PIN_ENTERED]);Await(config.Events[CQ_LOAN_ENTERED]);CHECK(life.BeginStop(session)==PB_OK);Await(config.Events[CQ_PIN_TAIL]);Await(config.Native.Events[NP_PARK_ENTERED]);CQ_STATE state;inspect(&state);CHECK(state.EventState==1 && state.Loans==32 && !state.LifetimeAvailable && state.Waits==1);
    CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);inspect(&state);CHECK(!state.EventState && state.Wakes==1 && state.Waits==2 && !state.Timeouts && state.Loans==32);
    /* Actual CommitStopped accepts zero and emits no success progress hint;
     * the same owner then returns every real reservation with Cancel32. */
    CHECK(SetEvent(config.Events[CQ_RETIRE]));Await(owners[1-pin]);CHECK(CloseHandle(owners[1-pin]));owners[1-pin]=NULL;++PublicJoins;inspect(&state);CHECK(state.EventState==1 && !state.Loans && !state.LifetimeAvailable);
    PREPARER prep={&api,session,PB_INVALID};HANDLE control=CreateThread(NULL,0,Preparer,&prep,0,NULL);CHECK(control);Await(config.Events[CQ_CV_ENTERED]);inspect(&state);CHECK(state.EventState==1 && state.PrepareSleeps && !state.Closed);
    CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);inspect(&state);CHECK(state.Waits==3 && state.Wakes==2 && !state.Timeouts && !state.EventState && !state.Loans && state.NativeActive==1 && !state.LifetimeAvailable && state.Phase<PB_LIFE_PREPARED);
    active(0);inspect(&state);CHECK(state.EventState==1 && !state.NativeActive);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(control);CHECK(CloseHandle(control) && prep.Result==PB_OK);++PublicJoins;joinPrepared();inspect(&state);
    CHECK(!state.LifetimeAvailable && state.Phase==PB_LIFE_PREPARED && !state.Closed && state.Regions==1 && state.Waits==3 && state.Wakes==3 && !state.Timeouts);ULONG waits=state.Waits;ULONG prepCallbacks=(ULONG)state.Callbacks;
    CHECK(SetEvent(config.Events[CQ_PIN_GO]));Await(owners[pin]);CHECK(CloseHandle(owners[pin]));++PublicJoins;inspect(&state);CHECK(state.LifetimeAvailable);
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(config.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));Await(config.Native.Events[NP_DETACHED]);
    ULONG joined=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;++joined;}CHECK(!GetModuleHandleW(path) && joined==prepCallbacks+1);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<CQ_EVENTS;++i)CHECK(CloseHandle(config.Events[i]));++Cases;
    printf("CASE family=%u protocol=%s wrap=%u pin=%lu measured=%u receive=32 release=32 reserve=32 stopped=0 cancel=32 progress=0/1/1/0/1 waits=%lu timeout=0 prepared_pinned=1 public_joins=3 callback_joins=%lu\n",PB_TEST_IP_VERSION,udp?"udp":"tcp",wrap,pin,measured,waits,joined);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);DWORD before=0,after;for(ULONG pass=0;pass<4;++pass){if(pass==2)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG udp=0;udp<2;++udp)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG pin=0;pin<2;++pin)Scenario(argv[1],udp,wrap,pin,pass>=2);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: CROSSQUEUE32 family%u cases%lu warm16 measured16 publicJoins%lu callbackJoins%lu handles%lu/%lu owners=0\n",PB_TEST_IP_VERSION,Cases,PublicJoins,CallbackJoins,before,after);return 0;
}
#endif
