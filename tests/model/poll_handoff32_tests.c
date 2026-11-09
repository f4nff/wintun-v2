/* SPDX-License-Identifier: GPL-2.0 */
/* Actual public completion Pop32 and private retirement handoff.
 * Native peer/status are hosted; SDK locks, DLL ownership and unload are real. */
#ifdef PB_API_TESTS
#error Shipping poll handoff required
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
enum { PH_SUBMITTED,PH_START_POLL,PH_POPPED,PH_SETTLE,PH_EX_ATTEMPT,PH_TAIL,PH_RESUME,PH_EVENTS };
typedef struct PH_CONFIG {NP_CONFIG Native;HANDLE Events[PH_EVENTS];BOOL Udp,Wrap;DWORD Owner;} PH_CONFIG;
typedef struct PH_STATE {NP_SNAPSHOT Core;LONG Fatal,PopCalls,ExAttempts,DataAvailable,LifetimeBlocked,Published;ULONG64 Cursor,Producer;} PH_STATE;
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
    ULONG pseudo=PB_TEST_IP_VERSION==6?Sum(0,p+8,32):Sum(0,p+12,8);pseudo+=l4+(udp?17u:6u);USHORT checksum=Final(Sum(pseudo,p+ip,l4));Put16(p+ip+(udp?6u:16u),udp&&!checksum?65535:checksum);
}
#ifdef PH_FIXTURE
static PH_CONFIG *PollHost;
static volatile LONG Popped,ExAttempt,TailArmed;
static uint64_t Base;
static PB_RESULT HandoffPop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t *);
static VOID WINAPI HandoffExclusive(PSRWLOCK);
static VOID WINAPI HandoffSharedRelease(PSRWLOCK);
#include "../safety/native_policy_fixture.c"
static PB_RESULT HandoffPop(PB_QUEUE_VIEW *view,PB_DESCRIPTOR *items,uint32_t capacity,uint32_t *count)
{
 PB_RESULT result=PbQueuePop(view,items,capacity,count);
 if(Current && view==Current->Queues[0].Rings+PB_QUEUE_RX_COMPLETE && GetCurrentThreadId()==PollHost->Owner && result==PB_OK){CHECK(capacity==32 && *count==32 && InterlockedIncrement(&Popped)==1);InterlockedExchange(&TailArmed,1);CHECK(SetEvent(PollHost->Events[PH_POPPED]));Await(PollHost->Events[PH_SETTLE]);}
 return result;
}
static VOID WINAPI HandoffExclusive(PSRWLOCK lock)
{
 if(Current && lock==&Current->DataGate && ReadAcquire(&Popped) && !InterlockedCompareExchange(&ExAttempt,1,0)){
  CHECK(GetCurrentThreadId()!=PollHost->Owner);BOOL acquired=TryAcquireSRWLockExclusive(lock);if(acquired)ReleaseSRWLockExclusive(lock);CHECK(!acquired && SetEvent(PollHost->Events[PH_EX_ATTEMPT]));
 }
 ObservedAcquire(lock);
}
static VOID WINAPI HandoffSharedRelease(PSRWLOCK lock)
{
 ReleaseSRWLockShared(lock);
 if(Current && lock==&Current->DataGate && GetCurrentThreadId()==PollHost->Owner && InterlockedCompareExchange(&TailArmed,0,1)==1){CHECK(Current->Queues[0].Published==0 && !LoansNow() && SetEvent(PollHost->Events[PH_TAIL]));Await(PollHost->Events[PH_RESUME]);}
}
__declspec(dllexport) VOID WINAPI PrepareHandoff(PH_CONFIG *config){CHECK(config && !PollHost);PollHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureHandoff(PB_SESSION *session)
{
 Configure(session);CHECK(Current->Layout.QueueCount==1 && Current->Layout.SlotCount==32 && Current->Layout.RingCapacity==64);Base=PollHost->Wrap?UINT64_MAX-15:0;
 for(ULONG r=PB_QUEUE_RX_SUBMIT;r<=PB_QUEUE_RX_COMPLETE;++r){PB_QUEUE_VIEW *v=Current->Queues[0].Rings+r;v->Cursor=v->PeerCursor=Base;v->Shared->Producer=v->Shared->Consumer=Base;}
}
__declspec(dllexport) VOID WINAPI CompleteHandoff(VOID)
{
 CHECK(Current && Current->Queues[0].Published==32 && !LoansNow() && !Popped && !Current->CloseRequested);PB_QUEUE_VIEW submitted,complete;CHECK(PbAttachQueue(&submitted,Current->Region,&Current->Layout,0,PB_QUEUE_RX_SUBMIT,FALSE)==PB_OK && PbAttachQueue(&complete,Current->Region,&Current->Layout,0,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);submitted.Cursor=submitted.PeerCursor=complete.Cursor=complete.PeerCursor=Base;
 PB_DESCRIPTOR items[32];uint32_t count,notify;CHECK(PbQueuePop(&submitted,items,32,&count)==PB_OK && count==32);uint32_t seen=0;for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,PollHost->Udp,1,i);CHECK(items[i].Token.Slot<32 && !(seen&(1u<<items[i].Token.Slot)) && items[i].Token.Generation==1 && items[i].Token.Epoch==Current->Epoch && !items[i].Token.Queue && items[i].Length==1500 && !memcmp(Current->Region+items[i].DataOffset,expected,sizeof(expected)));seen|=1u<<items[i].Token.Slot;}CHECK(seen==UINT32_MAX);
 CHECK(PbQueuePush(&complete,items,32,&count,&notify)==PB_OK && count==32 && !notify);InterlockedIncrement64((LONG64 *)&Current->SharedLifecycle->Sequence);CHECK(SetEvent(Progress));
}
__declspec(dllexport) VOID WINAPI InspectHandoff(PH_STATE *out,ULONG stage)
{
 CHECK(Current && out && stage<3);Observe(&out->Core);out->Fatal=Current->Fatal;out->Published=Current->Queues[0].Published;out->PopCalls=ReadAcquire(&Popped);out->ExAttempts=ReadAcquire(&ExAttempt);out->DataAvailable=out->LifetimeBlocked=0;
 if(stage){out->DataAvailable=TryAcquireSRWLockExclusive(&Current->DataGate);CHECK(out->DataAvailable);ReleaseSRWLockExclusive(&Current->DataGate);BOOL acquired=TryAcquireSRWLockExclusive(&Current->Lifetime);if(acquired)ReleaseSRWLockExclusive(&Current->Lifetime);out->LifetimeBlocked=!acquired;CHECK(out->LifetimeBlocked);}
 /* At stage0 the owner is frozen before private writes and the queued real
  * EX caller cannot enter. Do not add an SH reader behind that pending writer. */
 if(stage)AcquireSRWLockShared(&Current->DataGate);PB_USER_QUEUE *q=Current->Queues;CHECK(!q->Loans && q->Published==(stage?0:32));for(ULONG i=0;i<32;++i)CHECK(q->RxSlots[i].Generation==1 && q->RxSlots[i].State==(stage?PB_SLOT_FREE:PB_SLOT_PUBLISHED));if(stage)CHECK(q->Rx.FreeBits[0]==UINT32_MAX);
 out->Cursor=q->Rings[PB_QUEUE_RX_COMPLETE].Cursor-Base;out->Producer=q->Rings[PB_QUEUE_RX_COMPLETE].Shared->Producer-Base;if(stage)ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) ULONG WINAPI JoinHandoffPrepared(VOID)
{
 CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && !Current->Queues[0].Published && RuntimeCount==1 && Pins==2 && Current->References==2);return (ULONG)CallbackCount;
}
#else
static ULONG Cases,OwnerJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(PH_CONFIG *);
typedef VOID (WINAPI *INSPECT)(PH_STATE *,ULONG);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;PH_CONFIG *Config;PB_DESCRIPTOR Expected[32],Output[32];uint32_t Count;BOOL Empty;} OWNER;
static DWORD WINAPI Owner(VOID *opaque)
{
 OWNER *job=opaque;job->Config->Owner=GetCurrentThreadId();PB_LEASE leases[32];uint32_t count;CHECK(job->Api->ReserveBatch(job->Queue,leases,32,&count)==PB_OK && count==32);for(ULONG i=0;i<32;++i){Packet(leases[i].Data,job->Config->Udp,1,i);leases[i].Descriptor.Length=1500;job->Expected[i]=leases[i].Descriptor;}
 CHECK(job->Api->CommitBatch(job->Queue,leases,32,&count)==PB_OK && count==32 && SetEvent(job->Config->Events[PH_SUBMITTED]));Await(job->Config->Events[PH_START_POLL]);
 CHECK(job->Api->PollCompletions(job->Queue,job->Output,32,&job->Count)==PB_OK && job->Count==32 && !memcmp(job->Expected,job->Output,sizeof(job->Expected)));PB_DESCRIPTOR unused;CHECK(job->Api->PollCompletions(job->Queue,&unused,1,&count)==PB_EMPTY && !count);job->Empty=TRUE;return 0;
}
static VOID Retained(const PH_STATE *s,LONG published)
{CHECK(!s->Core.Loans && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses && s->PopCalls==1 && s->Published==published && s->Cursor==32 && s->Producer==32);}
static VOID Scenario(LPCWSTR path,BOOL udp,ULONG minor,BOOL wrap,BOOL measured)
{
 CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;ACTION complete;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
 GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareHandoff");GET(configure,"ConfigureHandoff");GET(active,"SetNativeActive");GET(inspect,"InspectHandoff");GET(complete,"CompleteHandoff");GET(joinPrepared,"JoinHandoffPrepared");
#undef GET
 PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);PH_CONFIG host={0};host.Udp=udp;host.Wrap=wrap;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(minor);
 for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}for(ULONG i=0;i<PH_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
 prepare(&host);PB_CONFIG config={sizeof(config),1,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-poll-handoff32",&config,&session)==PB_OK);configure(session);PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={0};job.Api=&api;job.Queue=queue;job.Config=&host;HANDLE owner=CreateThread(NULL,0,Owner,&job,0,NULL);CHECK(owner);Await(host.Events[PH_SUBMITTED]);complete();CHECK(SetEvent(host.Events[PH_START_POLL]));Await(host.Events[PH_POPPED]);
 CHECK(life.BeginStop(session)==PB_OK);Await(host.Events[PH_EX_ATTEMPT]);PH_STATE state;inspect(&state,0);Retained(&state,32);CHECK(state.ExAttempts==1 && !state.Core.Queries && state.Core.CloseRequested && !job.Count);
 CHECK(SetEvent(host.Events[PH_SETTLE]));Await(host.Events[PH_TAIL]);Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state,1);Retained(&state,0);CHECK(state.Core.NativeActive==1 && state.Core.Queries && state.Core.BlockStage==PB_BLOCK_KERNEL && job.Count==32 && !memcmp(job.Output,job.Expected,sizeof(job.Output)));
 active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]) && api.PrepareClose(session,5000)==PB_OK);ULONG prepared=joinPrepared();inspect(&state,2);Retained(&state,0);CHECK(state.Core.Phase==PB_LIFE_PREPARED && !state.Core.NativeActive && state.DataAvailable && state.LifetimeBlocked);
 CHECK(SetEvent(host.Events[PH_RESUME]));Await(owner);CHECK(job.Count==32 && job.Empty && CloseHandle(owner));++OwnerJoins;host.Owner=0;CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
 ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<PH_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));++Cases;
 printf("CASE ip=%u protocol=%u ndis=%lu wrap=%u measured=%u submitted32 completed32 pop32 published_before32 after0 data_blocked=1 tail_lifetime=1 prepared_retained=1 poll32_then_empty=1 native_barrier=1 owner_join=1 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,minor,wrap?1u:0u,measured?1u:0u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)Scenario(path,udp!=0,minor,wrap!=0,measured);}
int wmain(int argc,WCHAR **argv)
{
 CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);CHECK(Cases==16 && OwnerJoins==Cases);
 printf("PASS: poll-handoff32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu public_completions=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,CallbackJoins,Cases*32);return 0;
}
#endif
