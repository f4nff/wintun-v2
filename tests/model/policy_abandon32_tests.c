/* SPDX-License-Identifier: GPL-2.0 */
/* Actual shipping policy abandonment and replacement ownership. IO responses
 * and a single condition-variable failure are explicit hosted boundaries. */
#ifdef PB_API_TESTS
#error Shipping policy abandonment required
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
enum { P_OWNER_READY,P_OWNER_GO,P_EVENTS };
typedef struct P_CONFIG {NP_CONFIG Native;HANDLE Events[P_EVENTS],Waiting[2],Resumed[2],Go[2];BOOL Udp,SecondFails,ForgetSecond;} P_CONFIG;
typedef struct P_STATE {NP_SNAPSHOT Core;LONG Published,Fatal,SyncSleeps,SyncFaults,PollSleeps[2],GatedSleeps[2];ULONG Applied;} P_STATE;
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

#ifdef P_FIXTURE
static P_CONFIG *PolicyHost;
static volatile LONG SyncThread,SyncSleeps,SyncFaults,PollThreads[2],PollSleeps[2],PollGated[2],GatedSleeps[2];
static BOOL WINAPI AbandonSleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
#include "../safety/native_policy_fixture.c"
static BOOL WINAPI AbandonSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    LONG thread=(LONG)GetCurrentThreadId();
    if(thread==ReadAcquire(&SyncThread)){
        CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags && InterlockedIncrement(&SyncSleeps)==1);
        /* Model one failure after real policy IO entered; State must be free
         * while arranging IO and held again when returning the failure. */
        ReleaseSRWLockExclusive(lock);Await(PolicyHost->Native.Events[NP_IO_ENTERED]);AcquireSRWLockExclusive(lock);
        InterlockedIncrement(&SyncFaults);SetLastError(ERROR_OPERATION_ABORTED);return FALSE;
    }
    ULONG index=0;for(;index<2;++index)if(thread==ReadAcquire(PollThreads+index))break;
    if(index<2){CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);InterlockedIncrement(PollSleeps+index);CHECK(SetEvent(PolicyHost->Waiting[index]));}
    BOOL okay=SleepConditionVariableSRW(cv,lock,timeout,flags);DWORD error=GetLastError();
    if(index<2 && Current->Progress.PolicyCompleted==1 && !InterlockedExchange(PollGated+index,1)){
        /* Genuine SDK wake returned holding State; allow a peer to replace
         * the old request, then restore State before production resumes. */
        InterlockedExchange(GatedSleeps+index,ReadAcquire(PollSleeps+index));ReleaseSRWLockExclusive(lock);
        CHECK(SetEvent(PolicyHost->Resumed[index]));Await(PolicyHost->Go[index]);AcquireSRWLockExclusive(lock);
    }
    if(!okay)SetLastError(error);return okay;
}
__declspec(dllexport) VOID WINAPI PrepareAbandon(P_CONFIG *config){CHECK(config && !PolicyHost);PolicyHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureAbandon(PB_SESSION *session)
{
    Configure(session);CHECK(Current->Layout.SlotCount==64 && Current->Layout.RingCapacity==128 && !LoansNow());
    PB_QUEUE_VIEW ready;CHECK(PbAttachQueue(&ready,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);
    PB_DESCRIPTOR packets[32]={0};for(ULONG i=0;i<32;++i){packets[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};packets[i].Length=1500;
        packets[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;Packet(Current->Region+packets[i].DataOffset,PolicyHost->Udp,2,i);}
    uint32_t count,notify;CHECK(PbQueuePush(&ready,packets,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI MarkAbandonSync(VOID){CHECK(!ReadAcquire(&SyncThread));InterlockedExchange(&SyncThread,(LONG)GetCurrentThreadId());}
__declspec(dllexport) VOID WINAPI ClearAbandonSync(VOID){CHECK(SyncFaults==1 && SyncSleeps==1);InterlockedExchange(&SyncThread,0);}
__declspec(dllexport) VOID WINAPI MarkAbandonPoll(ULONG index){CHECK(index<2 && !ReadAcquire(PollThreads+index));InterlockedExchange(PollThreads+index,(LONG)GetCurrentThreadId());}
__declspec(dllexport) VOID WINAPI JoinAbandonFirst(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.PolicyRequested==1 && Current->Progress.PolicyCompleted==1 && !Current->PolicyOccupied && Current->PolicyAbandoned && Current->PolicyResult==PB_UNSUPPORTED && LastPolicyError==ERROR_ACCESS_DENIED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && RuntimeCount==1 && Current->References==2 && Pins==2);
}
__declspec(dllexport) VOID WINAPI ArmAbandonSecond(VOID)
{
    CHECK(Current && !Current->Scheduled && !ActiveCallbacks && Current->Progress.PolicyCompleted==1 && !Current->PolicyOccupied && PollGated[0] && PollGated[1]);
    /* Re-arm test-owned native IO gates/mode, never a production owner count. */
    Host->Mode=PolicyHost->SecondFails?NP_LATE_FAILURE:NP_LATE_SUCCESS;Host->PolicyError=ERROR_ACCESS_DENIED;
    CHECK(ResetEvent(Host->Events[NP_IO_ENTERED]) && ResetEvent(Host->Events[NP_IO_GATE]));InterlockedExchange(&IoArmed,1);
}
__declspec(dllexport) VOID WINAPI InspectAbandon(P_STATE *out)
{
    CHECK(Current && out);Observe(&out->Core);PB_USER_QUEUE *q=Current->Queues;
    AcquireSRWLockShared(&Current->DataGate);out->Published=ReadAcquire(&q->Published);out->Fatal=ReadAcquire(&Current->Fatal);CHECK(out->Core.Loans==0 || out->Core.Loans==32);CHECK(out->Published==0 || out->Published==32);
    for(ULONG i=0;i<32;++i){CHECK(q->TxSlots[i].State==(out->Core.Loans?PB_SLOT_PUBLISHED:PB_SLOT_FREE));CHECK(q->RxSlots[i].State==(out->Published?PB_SLOT_PUBLISHED:PB_SLOT_FREE));BYTE expected[1500];
        if(out->Core.Loans){Packet(expected,PolicyHost->Udp,2,i);CHECK(!memcmp(expected,Current->Region+Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE,sizeof(expected)));}
        if(out->Published){Packet(expected,PolicyHost->Udp,1,i);CHECK(!memcmp(expected,Current->Region+Current->Layout.Queues[0].RxData+i*PB_BLOCK_SIZE,sizeof(expected)));}
    }
    ReleaseSRWLockShared(&Current->DataGate);out->SyncSleeps=ReadAcquire(&SyncSleeps);out->SyncFaults=ReadAcquire(&SyncFaults);
    for(ULONG i=0;i<2;++i){out->PollSleeps[i]=ReadAcquire(PollSleeps+i);out->GatedSleeps[i]=ReadAcquire(GatedSleeps+i);}
    AcquireSRWLockShared(&Current->State);out->Applied=(ULONG)Current->PolicyResult;ReleaseSRWLockShared(&Current->State);
}
__declspec(dllexport) ULONG WINAPI JoinAbandonPrepared(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && !Current->Queues[0].Published && RuntimeCount==1 && Pins==2 && Current->References==2);
    return (ULONG)CallbackCount;
}
#else
static ULONG Cases,OwnerJoins,SyncJoins,PollJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef PB_RESULT (WINAPI *GET_PACKET)(uint32_t,uint32_t,PB_PACKET_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(P_CONFIG *);
typedef VOID (WINAPI *ACTION)(VOID);
typedef VOID (WINAPI *MARK_POLL)(ULONG);
typedef VOID (WINAPI *INSPECT)(P_STATE *);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;P_CONFIG *Config;ULONG Released;} OWNER;
typedef struct SYNC {PB_INTERFACE_V2 *Api;PB_SESSION *Session;ACTION Mark;ULONG Mask;PB_RESULT Result;DWORD Error;} SYNC;
typedef struct POLLER {PB_LIFECYCLE_INTERFACE *Life;PB_SESSION *Session;MARK_POLL Mark;ULONG Index;PB_RESULT Result,Operation;} POLLER;
static DWORD WINAPI Own(VOID *opaque)
{
    OWNER *j=opaque;PB_LEASE sent[32],received[32];uint32_t count;
    CHECK(j->Api->ReserveBatch(j->Queue,sent,32,&count)==PB_OK && count==32);
    for(ULONG i=0;i<32;++i){Packet(sent[i].Data,j->Config->Udp,1,i);sent[i].Descriptor.Length=1500;CHECK(sent[i].Descriptor.Token.Slot==i && sent[i].Descriptor.Token.Generation==1);}
    CHECK(j->Api->CommitBatch(j->Queue,sent,32,&count)==PB_OK && count==32);
    CHECK(j->Api->ReceiveBatch(j->Queue,received,32,&count)==PB_OK && count==32);
    CHECK(SetEvent(j->Config->Events[P_OWNER_READY]));Await(j->Config->Events[P_OWNER_GO]);PB_TOKEN tokens[32];
    for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,j->Config->Udp,2,i);CHECK(received[i].Descriptor.Length==1500 && received[i].Descriptor.Token.Slot==i && received[i].Descriptor.Token.Generation==1 && !memcmp(expected,received[i].Data,sizeof(expected)));tokens[i]=received[i].Descriptor.Token;}
    CHECK(j->Api->ReleaseBatch(j->Queue,tokens,32,&count)==PB_OK && count==32);j->Released=count;return 0;
}

static DWORD WINAPI SyncCall(VOID *opaque){SYNC *s=opaque;s->Mark();s->Result=s->Api->SetPolicy(s->Session,s->Mask);s->Error=GetLastError();return 0;}
static DWORD WINAPI PollCall(VOID *opaque){POLLER *p=opaque;p->Mark(p->Index);p->Operation=PB_INVALID;p->Result=p->Life->PollPolicy(p->Session,1,INFINITE,&p->Operation);return 0;}
static VOID Retained(const P_STATE *s,LONG loans)
{CHECK(s->Core.Loans==loans && s->Published==32 && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses);}
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL failure,BOOL forgotten,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;GET_PACKET getPacket;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;ACTION markSync,clearSync,joinFirst,armSecond;MARK_POLL markPoll;INSPECT inspect;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(getPacket,"PbGetPacketInterface");GET(prepare,"PrepareAbandon");GET(configure,"ConfigureAbandon");GET(active,"SetNativeActive");GET(markSync,"MarkAbandonSync");GET(clearSync,"ClearAbandonSync");GET(markPoll,"MarkAbandonPoll");GET(joinFirst,"JoinAbandonFirst");GET(armSecond,"ArmAbandonSecond");GET(inspect,"InspectAbandon");GET(joinPrepared,"JoinAbandonPrepared");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;PB_PACKET_INTERFACE packet;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK && getPacket(1,sizeof(packet),&packet)==PB_OK && (packet.Capabilities&PB_PACKET_REQUIRED_CAPS)==PB_PACKET_REQUIRED_CAPS);
    P_CONFIG host={0};host.Udp=udp;host.SecondFails=failure;host.ForgetSecond=forgotten;host.Native.Mode=NP_LATE_FAILURE;host.Native.PolicyError=ERROR_ACCESS_DENIED;host.Native.NdisVersion=PB_NDIS_VERSION(20);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}
    for(ULONG i=0;i<P_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
    for(ULONG i=0;i<2;++i){host.Waiting[i]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Resumed[i]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Go[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Waiting[i] && host.Resumed[i] && host.Go[i]);}
    prepare(&host);PB_CONFIG config={sizeof(config),1,64,128,0,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS};PB_SESSION *session;CHECK(api.Open(L"hosted-policy-abandon32",&config,&session)==PB_OK);configure(session);
    PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={&api,queue,&host,0};HANDLE owner=CreateThread(NULL,0,Own,&job,0,NULL);CHECK(owner);Await(host.Events[P_OWNER_READY]);
    ULONG mask=PB_TEST_IP_VERSION==6?PB_TX_TCP6_CHECKSUM:PB_TX_IP4_CHECKSUM;SYNC sync={&api,session,markSync,mask,PB_INVALID,0};HANDLE syncThread=CreateThread(NULL,0,SyncCall,&sync,0,NULL);CHECK(syncThread);Await(syncThread);CHECK(sync.Result==PB_CORRUPT && sync.Error==ERROR_OPERATION_ABORTED && CloseHandle(syncThread));++SyncJoins;clearSync();
    P_STATE state;inspect(&state);Retained(&state,32);CHECK(state.SyncSleeps==1 && state.SyncFaults==1 && state.Core.Requested==1 && !state.Core.Completed && state.Core.Occupied && state.Core.Abandoned && state.Core.PolicyCalls==1 && !state.Core.Drains && !state.Core.CloseRequested);
    uint64_t rejected=UINT64_MAX;CHECK(life.BeginSetPolicy(session,0,&rejected)==PB_BUSY && !rejected);
    POLLER polls[2];HANDLE readers[2];for(ULONG i=0;i<2;++i){polls[i]=(POLLER){&life,session,markPoll,i,PB_INVALID,PB_INVALID};readers[i]=CreateThread(NULL,0,PollCall,polls+i,0,NULL);CHECK(readers[i]);Await(host.Waiting[i]);}
    CHECK(SetEvent(host.Native.Events[NP_IO_GATE]));for(ULONG i=0;i<2;++i)Await(host.Resumed[i]);joinFirst();inspect(&state);Retained(&state,32);
    CHECK(state.Core.Requested==1 && state.Core.Completed==1 && !state.Core.Occupied && state.Core.Abandoned && state.Applied==PB_UNSUPPORTED && state.Core.LastPolicyError==ERROR_ACCESS_DENIED && !state.Core.CloseRequested);
    armSecond();uint64_t ticket=0;CHECK(life.BeginSetPolicy(session,mask,&ticket)==PB_OK && ticket==2);Await(host.Native.Events[NP_IO_ENTERED]);inspect(&state);Retained(&state,32);CHECK(state.Core.Occupied && !state.Core.Abandoned && state.Core.Requested==2 && state.Core.Completed==1 && state.Core.PolicyCalls==2);
    PB_RESULT operation=PB_INVALID;life.ForgetPolicy(session,1);CHECK(life.PollPolicy(session,1,0,&operation)==PB_STALE && operation==PB_INVALID);
    rejected=UINT64_MAX;CHECK(life.BeginSetPolicy(session,0,&rejected)==PB_BUSY && !rejected);
    for(ULONG i=0;i<2;++i){CHECK(SetEvent(host.Go[i]));Await(readers[i]);CHECK(polls[i].Result==PB_STALE && polls[i].Operation==PB_INVALID && CloseHandle(readers[i]));++PollJoins;}
    inspect(&state);Retained(&state,32);CHECK(state.Core.Occupied && !state.Core.Abandoned && state.Core.Requested==2 && state.Core.Completed==1);for(ULONG i=0;i<2;++i)CHECK(state.PollSleeps[i]>=1 && state.PollSleeps[i]==state.GatedSleeps[i]);
    if(forgotten)life.ForgetPolicy(session,2);operation=PB_INVALID;CHECK(life.PollPolicy(session,2,0,&operation)==PB_EMPTY && operation==PB_INVALID);
    CHECK(life.BeginStop(session)==PB_OK);rejected=UINT64_MAX;CHECK(life.BeginSetPolicy(session,0,&rejected)==PB_STOPPED && !rejected);CHECK(SetEvent(host.Native.Events[NP_IO_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);
    operation=PB_INVALID;PB_RESULT applied=failure?PB_UNSUPPORTED:PB_OK;CHECK(life.PollPolicy(session,2,5000,&operation)==(forgotten?PB_STALE:PB_OK) && operation==(forgotten?PB_INVALID:applied));inspect(&state);Retained(&state,32);
    CHECK(state.Core.Completed==2 && !state.Core.Occupied && state.Applied==(ULONG)applied && state.Core.PolicyCalls==2 && !state.Core.AfterStopCalls && state.Core.LastPolicyError==(DWORD)(failure?ERROR_ACCESS_DENIED:ERROR_SUCCESS) && state.Core.Drains==1 && !state.Core.Queries && state.Core.BlockStage==PB_BLOCK_LEASES);
    CHECK(api.PrepareClose(session,0)==PB_BUSY && SetEvent(host.Events[P_OWNER_GO]));Await(owner);CHECK(job.Released==32 && CloseHandle(owner));++OwnerJoins;
    CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,0);CHECK(state.Core.NativeActive==1 && state.Core.Queries && state.Core.BlockStage==PB_BLOCK_KERNEL && api.PrepareClose(session,0)==PB_QUARANTINED);
    active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();inspect(&state);CHECK(!state.Published && !state.Core.Loans && !state.Core.NativeActive && state.Core.Regions==1 && state.Core.Phase==PB_LIFE_PREPARED && !state.Core.ResourcesClosed);
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
    ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<P_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));for(ULONG i=0;i<2;++i)CHECK(CloseHandle(host.Waiting[i]) && CloseHandle(host.Resumed[i]) && CloseHandle(host.Go[i]));++Cases;
    printf("CASE ip=%u protocol=%u failure=%u forget=%u measured=%u loans=32 published=32 sync_failure=1 sync_join=1 old_stale=2 poll_join=2 owner_join=1 io_calls=2 released=32 reconciled=32 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,failure?1u:0u,forgotten?1u:0u,measured?1u:0u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(ULONG failure=0;failure<2;++failure)for(ULONG forgotten=0;forgotten<2;++forgotten)Scenario(path,udp!=0,failure!=0,forgotten!=0,measured);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);
    Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);
    CHECK(OwnerJoins==Cases && SyncJoins==Cases && PollJoins==Cases*2);
    printf("PASS: policy-abandon32 cases=%lu warm=%lu measured=%lu owner_joins=%lu sync_joins=%lu poll_joins=%lu callback_joins=%lu defensive_failures=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,SyncJoins,PollJoins,CallbackJoins,Cases);return 0;
}
#endif
