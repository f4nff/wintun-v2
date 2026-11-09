/* SPDX-License-Identifier: GPL-2.0 */
/* Actual invalid32 retirement remains atomic while stop arrives.
 * Native I/O is hosted; original owner retries real stopped retirement. */
#ifdef PB_API_TESTS
#error Shipping retire retry required
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
enum { RR_READY,RR_START,RR_INVALID,RR_LEAVE_INVALID,RR_EX_ATTEMPT,RR_ERROR_RETURNED,RR_RETRY,RR_EVENTS };
typedef struct RR_CONFIG {NP_CONFIG Native;HANDLE Events[RR_EVENTS];BOOL Udp,Cancel,Duplicate;DWORD Owner;} RR_CONFIG;
typedef struct RR_STATE {NP_SNAPSHOT Core;LONG Fatal,Validations,ExAttempts,OwnerSignals;ULONG64 RecycleProducer;} RR_STATE;
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
#ifdef RR_FIXTURE
static RR_CONFIG *RetryHost;
static volatile LONG Validations,ExAttempt,OwnerSignals;
static BOOL Saved;
static PB_POOL BeforeTx,BeforeRx;
static PB_SLOT BeforeTxSlots[32],BeforeRxSlots[32];
static PB_QUEUE_VIEW BeforeRings[PB_QUEUE_COUNT];
static PB_QUEUE_SHARED BeforeShared[PB_QUEUE_COUNT];
static PB_DESCRIPTOR BeforeEntries[PB_QUEUE_COUNT][64];
static PB_RESULT RetryValidate(PB_POOL *,const PB_TOKEN *,uint32_t,uint32_t);
static VOID WINAPI RetryExclusive(PSRWLOCK);
static BOOL WINAPI RetrySignal(HANDLE);
#include "../safety/native_policy_fixture.c"
static PB_RESULT RetryValidate(PB_POOL *pool,const PB_TOKEN *tokens,uint32_t count,uint32_t state)
{
 PB_RESULT result=PbValidateTokens(pool,tokens,count,state);
 if(Current && GetCurrentThreadId()==RetryHost->Owner && pool==(RetryHost->Cancel?&Current->Queues[0].Rx:&Current->Queues[0].Tx) && count==32){CHECK(Saved && state==(uint32_t)(RetryHost->Cancel?PB_SLOT_RESERVED:PB_SLOT_PUBLISHED));LONG ordinal=InterlockedIncrement(&Validations);CHECK(ordinal<=2);
  if(ordinal==1){CHECK(result==(RetryHost->Duplicate?PB_INVALID:PB_STALE) && SetEvent(RetryHost->Events[RR_INVALID]));Await(RetryHost->Events[RR_LEAVE_INVALID]);}else CHECK(result==PB_OK);
 }return result;
}
static VOID WINAPI RetryExclusive(PSRWLOCK lock)
{
 if(Current && lock==&Current->DataGate && ReadAcquire(&Validations)==1 && !InterlockedCompareExchange(&ExAttempt,1,0)){CHECK(GetCurrentThreadId()!=RetryHost->Owner);BOOL acquired=TryAcquireSRWLockExclusive(lock);if(acquired)ReleaseSRWLockExclusive(lock);CHECK(!acquired && SetEvent(RetryHost->Events[RR_EX_ATTEMPT]));}ObservedAcquire(lock);
}
static BOOL WINAPI RetrySignal(HANDLE event)
{if(Current && GetCurrentThreadId()==RetryHost->Owner && event==Progress)InterlockedIncrement(&OwnerSignals);return SetEvent(event);}
__declspec(dllexport) VOID WINAPI PrepareRetry(RR_CONFIG *config){CHECK(config && !RetryHost);RetryHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureRetry(PB_SESSION *session)
{
 Configure(session);CHECK(Current->Layout.QueueCount==1 && Current->Layout.SlotCount==32 && Current->Layout.RingCapacity==64);
 if(!RetryHost->Cancel){PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);PB_DESCRIPTOR items[32]={0};for(ULONG i=0;i<32;++i){items[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};items[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;items[i].Length=1500;Packet(Current->Region+items[i].DataOffset,RetryHost->Udp,1,i);}uint32_t count,notify;CHECK(PbQueuePush(&producer,items,32,&count,&notify)==PB_OK && count==32 && !notify);}
}
__declspec(dllexport) VOID WINAPI SaveRetryState(VOID)
{
 CHECK(Current && !Saved && !Validations && !Current->CloseRequested && LoansNow()==32);AcquireSRWLockShared(&Current->DataGate);PB_USER_QUEUE *q=Current->Queues;BeforeTx=q->Tx;BeforeRx=q->Rx;memcpy(BeforeTxSlots,q->TxSlots,sizeof(BeforeTxSlots));memcpy(BeforeRxSlots,q->RxSlots,sizeof(BeforeRxSlots));for(ULONG r=0;r<PB_QUEUE_COUNT;++r){BeforeRings[r]=q->Rings[r];BeforeShared[r]=*q->Rings[r].Shared;memcpy(BeforeEntries[r],q->Rings[r].Entries,sizeof(BeforeEntries[r]));}Saved=TRUE;ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) VOID WINAPI InspectRetry(RR_STATE *out,BOOL unchanged,BOOL ownerPinned)
{
 CHECK(Current && Saved && out);Observe(&out->Core);out->Fatal=Current->Fatal;out->Validations=ReadAcquire(&Validations);out->ExAttempts=ReadAcquire(&ExAttempt);out->OwnerSignals=ReadAcquire(&OwnerSignals);
 /* The first error gate freezes the owner under SH and the real EX waiter.
  * Do not enqueue a fresh SH reader behind that pending writer. */
 if(!ownerPinned)AcquireSRWLockShared(&Current->DataGate);PB_USER_QUEUE *q=Current->Queues;
 if(unchanged){CHECK(q->Loans==32 && !memcmp(&BeforeTx,&q->Tx,sizeof(BeforeTx)) && !memcmp(&BeforeRx,&q->Rx,sizeof(BeforeRx)) && !memcmp(BeforeTxSlots,q->TxSlots,sizeof(BeforeTxSlots)) && !memcmp(BeforeRxSlots,q->RxSlots,sizeof(BeforeRxSlots)));
  for(ULONG r=0;r<PB_QUEUE_COUNT;++r)CHECK(!memcmp(BeforeRings+r,q->Rings+r,sizeof(BeforeRings[r])) && !memcmp(BeforeShared+r,q->Rings[r].Shared,sizeof(BeforeShared[r])) && !memcmp(BeforeEntries[r],q->Rings[r].Entries,sizeof(BeforeEntries[r])));
 }else{PB_POOL *pool=RetryHost->Cancel?&q->Rx:&q->Tx;CHECK(!q->Loans && pool->FreeBits[0]==UINT32_MAX);for(ULONG i=0;i<32;++i)CHECK(pool->Slots[i].State==PB_SLOT_FREE && pool->Slots[i].Generation==1);}
 out->RecycleProducer=q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer;if(!ownerPinned)ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) ULONG WINAPI JoinRetryPrepared(VOID)
{
 CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2);return (ULONG)CallbackCount;
}
#else
static ULONG Cases,OwnerJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(RR_CONFIG *);
typedef VOID (WINAPI *INSPECT)(RR_STATE *,BOOL,BOOL);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;RR_CONFIG *Config;PB_RESULT Error;ULONG Accepted,Retired;} OWNER;
static VOID Verify(PB_LEASE *leases,BOOL udp){for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,udp,1,i);CHECK(leases[i].Descriptor.Token.Generation==1 && !memcmp(leases[i].Data,expected,sizeof(expected)));}}
static DWORD WINAPI Owner(VOID *opaque)
{
 OWNER *job=opaque;job->Config->Owner=GetCurrentThreadId();PB_LEASE leases[32];uint32_t count;PB_RESULT result=job->Config->Cancel?job->Api->ReserveBatch(job->Queue,leases,32,&count):job->Api->ReceiveBatch(job->Queue,leases,32,&count);CHECK(result==PB_OK && count==32);
 PB_TOKEN tokens[32],original[32];for(ULONG i=0;i<32;++i){tokens[i]=original[i]=leases[i].Descriptor.Token;if(job->Config->Cancel)Packet(leases[i].Data,job->Config->Udp,1,i);}Verify(leases,job->Config->Udp);CHECK(SetEvent(job->Config->Events[RR_READY]));Await(job->Config->Events[RR_START]);
 if(job->Config->Duplicate)tokens[31]=tokens[0];else tokens[31].Epoch^=1;PB_TOKEN bad[32];memcpy(bad,tokens,sizeof(bad));count=0xdeadbeefu;
 job->Error=job->Config->Cancel?job->Api->CancelBatch(job->Queue,tokens,32):job->Api->ReleaseBatch(job->Queue,tokens,32,&count);job->Accepted=job->Config->Cancel?0:count;
 CHECK(job->Error==(job->Config->Duplicate?PB_INVALID:PB_STALE) && !job->Accepted && !memcmp(tokens,bad,sizeof(bad)));Verify(leases,job->Config->Udp);CHECK(SetEvent(job->Config->Events[RR_ERROR_RETURNED]));Await(job->Config->Events[RR_RETRY]);
 /* The failed call is over. Restore only the caller-owned last input token;
  * no private pool/counter/event is repaired before the valid stopped retry. */
 tokens[31]=original[31];CHECK(!memcmp(tokens,original,sizeof(tokens)));result=job->Config->Cancel?job->Api->CancelBatch(job->Queue,tokens,32):job->Api->ReleaseBatch(job->Queue,tokens,32,&count);CHECK(result==PB_OK && (job->Config->Cancel || count==32));job->Retired=32;return 0;
}
static VOID Retained(const RR_STATE *s,LONG loans)
{CHECK(s->Core.Loans==loans && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses);}
static VOID Scenario(LPCWSTR path,BOOL udp,BOOL cancel,BOOL duplicate,BOOL measured)
{
 CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;ACTION save;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
 GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareRetry");GET(configure,"ConfigureRetry");GET(active,"SetNativeActive");GET(inspect,"InspectRetry");GET(save,"SaveRetryState");GET(joinPrepared,"JoinRetryPrepared");
#undef GET
 PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);RR_CONFIG host={0};host.Udp=udp;host.Cancel=cancel;host.Duplicate=duplicate;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(20);
 for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}for(ULONG i=0;i<RR_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
 prepare(&host);PB_CONFIG config={sizeof(config),1,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-retire-retry32",&config,&session)==PB_OK);configure(session);PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={&api,queue,&host,PB_OK,0,0};HANDLE owner=CreateThread(NULL,0,Owner,&job,0,NULL);CHECK(owner);Await(host.Events[RR_READY]);save();CHECK(SetEvent(host.Events[RR_START]));Await(host.Events[RR_INVALID]);
 CHECK(life.BeginStop(session)==PB_OK);Await(host.Events[RR_EX_ATTEMPT]);RR_STATE state;inspect(&state,TRUE,TRUE);Retained(&state,32);CHECK(state.Validations==1 && state.ExAttempts==1 && !state.OwnerSignals && !state.Core.Queries && state.Core.CloseRequested && !state.RecycleProducer);
 CHECK(SetEvent(host.Events[RR_LEAVE_INVALID]));Await(host.Events[RR_ERROR_RETURNED]);Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state,TRUE,FALSE);Retained(&state,32);CHECK(state.Validations==1 && !state.OwnerSignals && !state.Core.Queries && state.Core.BlockStage==PB_BLOCK_LEASES && api.PrepareClose(session,0)==PB_BUSY && job.Error==(duplicate?PB_INVALID:PB_STALE) && !job.Accepted);
 CHECK(SetEvent(host.Events[RR_RETRY]));Await(owner);CHECK(job.Retired==32 && CloseHandle(owner));++OwnerJoins;host.Owner=0;inspect(&state,FALSE,FALSE);Retained(&state,0);CHECK(state.Validations==2 && state.OwnerSignals==1 && state.RecycleProducer==(cancel?0u:32u));
 CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state,FALSE,FALSE);Retained(&state,0);CHECK(state.Core.NativeActive==1 && state.Core.Queries && state.Core.BlockStage==PB_BLOCK_KERNEL);
 active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
 ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<RR_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));++Cases;
 printf("CASE ip=%u protocol=%u cancel=%u duplicate=%u measured=%u loans32 invalid_last=1 rollback32 accepted=%s progress0 stopped_retry32 progress1 data_blocked=1 native_barrier=1 owner_join=1 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,cancel?1u:0u,duplicate?1u:0u,measured?1u:0u,cancel?"none":"0",prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(ULONG cancel=0;cancel<2;++cancel)for(ULONG duplicate=0;duplicate<2;++duplicate)Scenario(path,udp!=0,cancel!=0,duplicate!=0,measured);}
int wmain(int argc,WCHAR **argv)
{
 CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);CHECK(Cases==16 && OwnerJoins==Cases);
 printf("PASS: retire-retry32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu rejected_batches=%lu tokens_retried=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,CallbackJoins,Cases,Cases*32);return 0;
}
#endif
