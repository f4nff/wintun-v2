/* SPDX-License-Identifier: GPL-2.0 */
/* Actual shipping return-input alias, immediate natural generation2 reuse,
 * stale wait hint and terminal. Native protocol peer and IO are hosted. */
#ifdef PB_API_TESTS
#error Shipping reuse terminal required
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
#include "../../common/platform.h"
#include <string.h>
#define CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"shipping fingerprint required");
enum { R_WAIT1,R_ALIAS_READY,R_ALIAS_GO,R_GEN2,R_GEN2_GO,R_STALE,R_NEXT_WAIT,R_WAIT3,R_STOPPED,R_RETURN,R_EVENTS };
typedef struct R_CONFIG {NP_CONFIG Native;HANDLE Events[R_EVENTS];BOOL Udp,Wrap,Early;DWORD Owner;} R_CONFIG;
typedef struct R_STATE {NP_SNAPSHOT Core;LONG Fatal,Terminal,WaitCalls,UserState,Armed;ULONG UserGeneration,PeerGeneration,PeerReturned,Republished;ULONGLONG ReadyCursor,ReadyProducer,RecycleProducer,RecycleConsumer;} R_STATE;
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
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


#ifndef R_FIXTURE
static VOID Rechecksum(BYTE *p,BOOL udp)
{
    ULONG ip=PB_TEST_IP_VERSION==6?40u:20u,l4=1500-ip;Put16(p+ip+(udp?6u:16u),0);
    ULONG pseudo=PB_TEST_IP_VERSION==6?Sum(0,p+8,32):Sum(0,p+12,8);pseudo+=l4+(udp?17u:6u);USHORT value=Final(Sum(pseudo,p+ip,l4));Put16(p+ip+(udp?6u:16u),value?value:65535);
#if PB_TEST_IP_VERSION == 4
    Put16(p+10,0);Put16(p+10,Final(Sum(0,p,20)));
#endif
    CHECK(!Final(Sum(pseudo,p+ip,l4)));
#if PB_TEST_IP_VERSION == 4
    CHECK(!Final(Sum(0,p,20)));
#endif
}
#endif
#ifdef R_FIXTURE
static R_CONFIG *ReuseHost;
static PB_POOL PeerPool;
static PB_SLOT PeerSlots[32];
static PB_QUEUE_VIEW PeerReady,PeerRecycle;
static ULONG PeerReturned,Republished;
static volatile LONG WaitCalls;
static ULONGLONG Base;
static PB_RESULT ReusePush(PB_QUEUE_VIEW *,const PB_TOKEN *,uint32_t,uint32_t *,uint32_t *);
static DWORD WINAPI ReuseWait(DWORD,const HANDLE *,BOOL,DWORD);
#include "../safety/native_policy_fixture.c"
static VOID PublishGeneration(ULONG generation)
{
    PB_DESCRIPTOR packets[32]={0};for(ULONG i=0;i<32;++i){CHECK(PbPoolReserve(&PeerPool,&packets[i].Token)==PB_OK && packets[i].Token.Generation==generation && packets[i].Token.Slot==i);
        packets[i].Length=1500;packets[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;Packet(Current->Region+packets[i].DataOffset,ReuseHost->Udp,generation,i);
        CHECK(PbPoolTransition(&PeerPool,&packets[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);}
    uint32_t n,notify;CHECK(PbQueuePush(&PeerReady,packets,32,&n,&notify)==PB_OK && n==32 && notify==(uint32_t)(generation==2));
    if(generation==2){Republished+=n;CHECK(SetEvent(Current->Queues[0].UserEvent));}
}
static PB_RESULT ReusePush(PB_QUEUE_VIEW *view,const PB_TOKEN *tokens,uint32_t count,uint32_t *accepted,uint32_t *notify)
{
    CHECK(Current && view==Current->Queues[0].Rings+PB_QUEUE_TX_RECYCLE && count==32 && GetCurrentThreadId()==ReuseHost->Owner);
    PB_RESULT result=PbQueuePushTokens(view,tokens,count,accepted,notify);CHECK(result==PB_OK && *accepted==32);
    /* Only the now-published recycle descriptors authorize native reuse. Do not
     * read caller token/payload input after ownership has been published. */
    PB_TOKEN returned[32];uint32_t n;CHECK(PbQueuePopTokens(&PeerRecycle,returned,32,&n)==PB_OK && n==32 && PbValidateTokens(&PeerPool,returned,n,PB_SLOT_PUBLISHED)==PB_OK);
    for(ULONG i=0;i<n;++i){CHECK(returned[i].Slot==i && returned[i].Generation==(PeerReturned?2u:1u));CHECK(PbPoolTransition(&PeerPool,returned+i,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);}
    PeerReturned+=n;
    if(PeerReturned==32){CHECK(!Current->CloseRequested && !Current->SharedLifecycle->Terminal);PublishGeneration(2);}
    else CHECK(PeerReturned==64 && Current->CloseRequested && Current->SharedLifecycle->Terminal && Republished==32);
    return result;
}
static DWORD WINAPI ReuseWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Current && ReuseHost->Owner==GetCurrentThreadId() && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent && events[1]==Current->Queues[0].UserEvent);
    LONG ordinal=InterlockedIncrement(&WaitCalls);CHECK(ordinal>=1 && ordinal<=3);if(ordinal==1)CHECK(SetEvent(ReuseHost->Events[R_WAIT1]));if(ordinal==3)CHECK(SetEvent(ReuseHost->Events[R_WAIT3]));
    /* This is selected real SDK entry, not scheduler park evidence. */
    return WaitForMultipleObjects(count,events,all,timeout);
}
__declspec(dllexport) VOID WINAPI PrepareReuse(R_CONFIG *config){CHECK(config && !ReuseHost);ReuseHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureReuse(PB_SESSION *session)
{
    Configure(session);CHECK(Current->Layout.QueueCount==1 && Current->Layout.SlotCount==32 && Current->Layout.RingCapacity==64);Base=ReuseHost->Wrap?UINT64_MAX-15:0;
    for(ULONG r=PB_QUEUE_TX_READY;r<=PB_QUEUE_TX_RECYCLE;++r){PB_QUEUE_VIEW *v=Current->Queues[0].Rings+r;v->Cursor=v->PeerCursor=Base;v->Shared->Producer=v->Shared->Consumer=Base;}
    CHECK(PbAttachQueue(&PeerReady,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK && PbAttachQueue(&PeerRecycle,Current->Region,&Current->Layout,0,PB_QUEUE_TX_RECYCLE,FALSE)==PB_OK);
    PeerReady.Cursor=PeerReady.PeerCursor=PeerRecycle.Cursor=PeerRecycle.PeerCursor=Base;
    CHECK(PbPoolInitialize(&PeerPool,PeerSlots,32,Current->Epoch,0)==PB_OK);PublishGeneration(1);
}
__declspec(dllexport) VOID WINAPI TriggerReuseTerminal(VOID)
{
    CHECK(Current && LoansNow()==32 && Republished==32 && PeerReturned==32 && !Current->CloseRequested && !Current->SharedLifecycle->Terminal);
    /* Model only terminal/sequence/progress/user hints, not SYS quiescence. */
    InterlockedExchange((LONG *)&Current->SharedLifecycle->Terminal,1);InterlockedIncrement64((LONG64 *)&Current->SharedLifecycle->Sequence);
    CHECK(SetEvent(Progress) && SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) VOID WINAPI InspectReuse(R_STATE *out)
{
    CHECK(Current && out);Observe(&out->Core);out->Fatal=Current->Fatal;out->Terminal=(LONG)Current->SharedLifecycle->Terminal;out->WaitCalls=ReadAcquire(&WaitCalls);out->PeerReturned=PeerReturned;out->Republished=Republished;
    typedef LONG (NTAPI *QUERY_EVENT)(HANDLE,ULONG,VOID *,ULONG,ULONG *);QUERY_EVENT query;*(FARPROC *)&query=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");CHECK(query);struct {LONG Type,State;} state;
    CHECK(query(Current->Queues[0].UserEvent,0,&state,sizeof(state),NULL)>=0 && state.Type==1 && (state.State==0 || state.State==1));out->UserState=state.State;
    AcquireSRWLockShared(&Current->DataGate);PB_USER_QUEUE *q=Current->Queues;out->UserGeneration=q->TxSlots[0].Generation;out->PeerGeneration=PeerSlots[0].Generation;
    for(ULONG i=0;i<32;++i){CHECK(q->TxSlots[i].Generation==out->UserGeneration && PeerSlots[i].Generation==out->PeerGeneration && q->TxSlots[i].State==(out->Core.Loans?PB_SLOT_PUBLISHED:PB_SLOT_FREE));CHECK(PeerSlots[i].State==(PeerReturned==64?PB_SLOT_FREE:PB_SLOT_PUBLISHED));}
    out->Armed=(LONG)q->Rings[PB_QUEUE_TX_READY].Shared->Armed;out->ReadyCursor=q->Rings[PB_QUEUE_TX_READY].Cursor-Base;out->ReadyProducer=PeerReady.Cursor-Base;out->RecycleProducer=q->Rings[PB_QUEUE_TX_RECYCLE].Cursor-Base;out->RecycleConsumer=PeerRecycle.Cursor-Base;ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) ULONG WINAPI JoinReusePrepared(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2 && PeerReturned==64 && Republished==32);
    return (ULONG)CallbackCount;
}
#else
static ULONG Cases,OwnerJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(R_CONFIG *);
typedef VOID (WINAPI *INSPECT)(R_STATE *);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;R_CONFIG *Config;ULONG Returned,StaleHints;} OWNER;
static VOID CheckPackets(PB_LEASE *leases,BOOL udp,ULONG generation)
{for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,udp,generation,i);CHECK(leases[i].Descriptor.Token.Generation==generation && leases[i].Descriptor.Token.Slot==i && leases[i].Descriptor.Length==1500 && !memcmp(leases[i].Data,expected,sizeof(expected)));}}
static DWORD WINAPI Owner(VOID *opaque)
{
    OWNER *j=opaque;j->Config->Owner=GetCurrentThreadId();PB_LEASE leases[32];uint32_t count;CHECK(j->Api->ReceiveBatch(j->Queue,leases,32,&count)==PB_OK && count==32);CheckPackets(leases,j->Config->Udp,1);
    CHECK(j->Api->Wait(j->Queue,INFINITE)==PB_OK);
    /* Input tokens are explicitly allowed in a held packet being returned.
     * This caller-owned payload modification is intentional, not corruption. */
    PB_TOKEN *alias=(PB_TOKEN *)(leases[0].Data+64);for(ULONG i=0;i<32;++i)alias[i]=leases[i].Descriptor.Token;Rechecksum(leases[0].Data,j->Config->Udp);
    CHECK(SetEvent(j->Config->Events[R_ALIAS_READY]));Await(j->Config->Events[R_ALIAS_GO]);
    CHECK(j->Api->ReleaseBatch(j->Queue,alias,32,&count)==PB_OK && count==32);j->Returned=count;
    /* The old payload/input is no longer caller-owned. Do not touch its old
     * pointers; wait for an explicit next-generation Receive before reading. */
    CHECK(j->Api->Wait(j->Queue,0)==PB_OK && j->Api->ReceiveBatch(j->Queue,leases,32,&count)==PB_OK && count==32);CheckPackets(leases,j->Config->Udp,2);
    CHECK(SetEvent(j->Config->Events[R_GEN2]));Await(j->Config->Events[R_GEN2_GO]);
    if(!j->Config->Early){
        CHECK(j->Api->Wait(j->Queue,INFINITE)==PB_OK);PB_LEASE unused;CHECK(j->Api->ReceiveBatch(j->Queue,&unused,1,&count)==PB_EMPTY && !count);CheckPackets(leases,j->Config->Udp,2);j->StaleHints++;
        CHECK(SetEvent(j->Config->Events[R_STALE]));Await(j->Config->Events[R_NEXT_WAIT]);
    }
    CHECK(j->Api->Wait(j->Queue,INFINITE)==PB_STOPPED);CheckPackets(leases,j->Config->Udp,2);
    CHECK(SetEvent(j->Config->Events[R_STOPPED]));Await(j->Config->Events[R_RETURN]);PB_TOKEN tokens[32];for(ULONG i=0;i<32;++i)tokens[i]=leases[i].Descriptor.Token;
    CHECK(j->Api->ReleaseBatch(j->Queue,tokens,32,&count)==PB_OK && count==32 && j->Api->Wait(j->Queue,0)==PB_STOPPED);j->Returned+=count;return 0;
}
static VOID Retained(const R_STATE *s,LONG loans)
{CHECK(s->Core.Loans==loans && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses);}
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL wrap,BOOL early,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;ACTION terminal;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareReuse");GET(configure,"ConfigureReuse");GET(active,"SetNativeActive");GET(inspect,"InspectReuse");GET(terminal,"TriggerReuseTerminal");GET(joinPrepared,"JoinReusePrepared");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);
    R_CONFIG host={0};host.Udp=udp;host.Wrap=wrap;host.Early=early;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(20);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}
    for(ULONG i=0;i<R_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
    prepare(&host);PB_CONFIG config={sizeof(config),1,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-reuse-terminal32",&config,&session)==PB_OK);configure(session);
    PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={&api,queue,&host,0,0};HANDLE owner=CreateThread(NULL,0,Owner,&job,0,NULL);CHECK(owner);Await(host.Events[R_WAIT1]);
    CHECK(api.Wake(session,0)==PB_OK);Await(host.Events[R_ALIAS_READY]);R_STATE state;inspect(&state);Retained(&state,32);
    CHECK(state.UserGeneration==1 && state.PeerGeneration==1 && state.WaitCalls==1 && state.Armed==1 && !state.UserState && !state.PeerReturned && !state.Republished && state.ReadyCursor==32 && state.ReadyProducer==32 && !state.Terminal && !state.Core.CloseRequested);
    CHECK(SetEvent(host.Events[R_ALIAS_GO]));Await(host.Events[R_GEN2]);inspect(&state);Retained(&state,32);
    CHECK(state.UserGeneration==2 && state.PeerGeneration==2 && state.WaitCalls==1 && !state.Armed && state.UserState==1 && state.PeerReturned==32 && state.Republished==32 && state.ReadyCursor==64 && state.ReadyProducer==64 && state.RecycleProducer==32 && state.RecycleConsumer==32 && !state.Terminal && !state.Core.CloseRequested);
    if(early){terminal();CHECK(SetEvent(host.Events[R_GEN2_GO]));}
    else {CHECK(SetEvent(host.Events[R_GEN2_GO]));Await(host.Events[R_STALE]);inspect(&state);Retained(&state,32);CHECK(state.WaitCalls==2 && !state.UserState && state.Armed==1 && !state.Terminal && !state.Core.CloseRequested);
        CHECK(SetEvent(host.Events[R_NEXT_WAIT]));Await(host.Events[R_WAIT3]);terminal();}
    Await(host.Events[R_STOPPED]);CHECK(life.BeginStop(session)==PB_OK);Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,32);
    CHECK(state.Terminal && state.Core.CloseRequested && state.WaitCalls==(early?1:3) && state.UserGeneration==2 && state.PeerGeneration==2 && state.Core.NativeActive==1 && !state.Core.Queries && state.Core.BlockStage==PB_BLOCK_LEASES && api.PrepareClose(session,0)==PB_BUSY);
    CHECK(SetEvent(host.Events[R_RETURN]));Await(owner);CHECK(job.Returned==64 && job.StaleHints==(early?0u:1u) && CloseHandle(owner));++OwnerJoins;host.Owner=0;
    inspect(&state);Retained(&state,0);CHECK(state.PeerReturned==64 && state.Republished==32 && state.UserGeneration==2 && state.PeerGeneration==2 && state.RecycleProducer==64 && state.RecycleConsumer==64);
    CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,0);CHECK(state.Core.NativeActive==1 && state.Core.Queries && state.Core.BlockStage==PB_BLOCK_KERNEL);
    active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
    ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<R_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));++Cases;
    printf("CASE ip=%u protocol=%u wrap=%u early=%u measured=%u old32 alias_offset=64 reused32 generation=2 returned64 stale_hints=%u sdk_waits=%u terminal=1 native_barrier=1 owner_join=1 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,wrap?1u:0u,early?1u:0u,measured?1u:0u,early?0u:1u,early?1u:3u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG early=0;early<2;++early)Scenario(path,udp!=0,wrap!=0,early!=0,measured);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);
    Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);
    CHECK(OwnerJoins==Cases);printf("PASS: reuse-terminal32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu gen2_loans=%lu returned_tokens=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,CallbackJoins,Cases*32,Cases*64);return 0;
}
#endif
