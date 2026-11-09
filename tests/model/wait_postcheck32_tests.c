/* SPDX-License-Identifier: GPL-2.0 */
/* Actual SDK timeout returns before actual DLL stop postchecks.
 * Native registration/status/terminal are hosted; ownership is real DLL code. */
#ifdef PB_API_TESTS
#error Shipping wait postcheck required
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
enum { WP_SDK_RETURNED,WP_RESUME_WAIT,WP_AFTER_WAIT,WP_RETIRE,WP_EVENTS };
typedef struct WP_CONFIG {NP_CONFIG Native;HANDLE Events[WP_EVENTS];BOOL Udp,Terminal;DWORD Timeout,Owner;} WP_CONFIG;
typedef struct WP_STATE {NP_SNAPSHOT Core;LONG Fatal,Terminal,Calls,Returned,DataExclusive,LifetimeBlocked;ULONG64 Producer;} WP_STATE;
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
#ifdef WP_FIXTURE
static WP_CONFIG *WaitHost;
static volatile LONG WaitCalls,WaitReturned;
static DWORD WINAPI PostcheckWait(DWORD,const HANDLE *,BOOL,DWORD);
#include "../safety/native_policy_fixture.c"
static DWORD WINAPI PostcheckWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
 CHECK(Current && GetCurrentThreadId()==WaitHost->Owner && count==2 && !all && timeout==WaitHost->Timeout && events[0]==Current->ClosingEvent && events[1]==Current->Queues[0].UserEvent && InterlockedIncrement(&WaitCalls)==1);
 DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result==WAIT_TIMEOUT);
 /* The SDK has really returned TIMEOUT. Production still holds Lifetime SH,
  * and has not yet executed its CloseRequested/Terminal postchecks. */
 InterlockedExchange(&WaitReturned,1);CHECK(SetEvent(WaitHost->Events[WP_SDK_RETURNED]));Await(WaitHost->Events[WP_RESUME_WAIT]);return result;
}
__declspec(dllexport) VOID WINAPI PreparePostcheck(WP_CONFIG *config){CHECK(config && !WaitHost);WaitHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigurePostcheck(PB_SESSION *session)
{
 Configure(session);CHECK(Current->Layout.QueueCount==1 && Current->Layout.SlotCount==32 && Current->Layout.RingCapacity==64);PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);
 PB_DESCRIPTOR items[32]={0};for(ULONG i=0;i<32;++i){items[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};items[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;items[i].Length=1500;Packet(Current->Region+items[i].DataOffset,WaitHost->Udp,1,i);}uint32_t count,notify;CHECK(PbQueuePush(&producer,items,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI NativePostcheckTerminal(VOID)
{
 CHECK(Current && WaitHost->Terminal && ReadAcquire(&WaitReturned)==1 && LoansNow()==32 && !Current->CloseRequested && !Current->SharedLifecycle->Terminal);
 InterlockedExchange((LONG *)&Current->SharedLifecycle->Terminal,1);InterlockedIncrement64((LONG64 *)&Current->SharedLifecycle->Sequence);CHECK(SetEvent(Progress) && SetEvent(Current->Queues[0].UserEvent));
 /* Notification is not native quiescence/Active0 or physical-close authority. */
}
__declspec(dllexport) VOID WINAPI InspectPostcheck(WP_STATE *out,BOOL insideWait)
{
 CHECK(Current && out);Observe(&out->Core);out->Fatal=Current->Fatal;out->Terminal=(LONG)Current->SharedLifecycle->Terminal;out->Calls=ReadAcquire(&WaitCalls);out->Returned=ReadAcquire(&WaitReturned);
 if(insideWait){out->DataExclusive=TryAcquireSRWLockExclusive(&Current->DataGate);CHECK(out->DataExclusive);ReleaseSRWLockExclusive(&Current->DataGate);BOOL acquired=TryAcquireSRWLockExclusive(&Current->Lifetime);if(acquired)ReleaseSRWLockExclusive(&Current->Lifetime);out->LifetimeBlocked=!acquired;CHECK(out->LifetimeBlocked);}else out->DataExclusive=out->LifetimeBlocked=0;
 AcquireSRWLockShared(&Current->DataGate);PB_USER_QUEUE *q=Current->Queues;CHECK(q->Loans==0 || q->Loans==32);
 for(ULONG i=0;i<32;++i)CHECK(q->TxSlots[i].Generation==1 && q->TxSlots[i].State==(q->Loans?PB_SLOT_PUBLISHED:PB_SLOT_FREE));
 out->Producer=q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer;ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) ULONG WINAPI JoinPostcheckPrepared(VOID)
{
 CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2);return (ULONG)CallbackCount;
}
#else
static ULONG Cases,OwnerJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(WP_CONFIG *);
typedef VOID (WINAPI *INSPECT)(WP_STATE *,BOOL);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;WP_CONFIG *Config;ULONG Returned;} OWNER;
static VOID Verify(PB_LEASE *leases,BOOL udp){for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,udp,1,i);CHECK(leases[i].Descriptor.Token.Generation==1 && leases[i].Descriptor.Token.Slot==i && leases[i].Descriptor.Length==1500 && !memcmp(leases[i].Data,expected,sizeof(expected)));}}
static DWORD WINAPI Owner(VOID *opaque)
{
 OWNER *job=opaque;job->Config->Owner=GetCurrentThreadId();PB_LEASE leases[32];uint32_t count;CHECK(job->Api->ReceiveBatch(job->Queue,leases,32,&count)==PB_OK && count==32);Verify(leases,job->Config->Udp);
 CHECK(job->Api->Wait(job->Queue,job->Config->Timeout)==PB_STOPPED);Verify(leases,job->Config->Udp);CHECK(SetEvent(job->Config->Events[WP_AFTER_WAIT]));Await(job->Config->Events[WP_RETIRE]);
 PB_TOKEN tokens[32];for(ULONG i=0;i<32;++i)tokens[i]=leases[i].Descriptor.Token;CHECK(job->Api->ReleaseBatch(job->Queue,tokens,32,&count)==PB_OK && count==32);job->Returned=count;return 0;
}
static VOID Retained(const WP_STATE *s,LONG loans)
{CHECK(s->Core.Loans==loans && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses && s->Calls==1 && s->Returned==1);}
static VOID Scenario(LPCWSTR path,BOOL udp,DWORD timeout,BOOL nativeTerminal,BOOL measured)
{
 CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;ACTION terminal;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
 GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PreparePostcheck");GET(configure,"ConfigurePostcheck");GET(active,"SetNativeActive");GET(inspect,"InspectPostcheck");GET(terminal,"NativePostcheckTerminal");GET(joinPrepared,"JoinPostcheckPrepared");
#undef GET
 PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);WP_CONFIG host={0};host.Udp=udp;host.Timeout=timeout;host.Terminal=nativeTerminal;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(20);
 for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}for(ULONG i=0;i<WP_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
 prepare(&host);PB_CONFIG config={sizeof(config),1,32,64,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-wait-postcheck32",&config,&session)==PB_OK);configure(session);PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={&api,queue,&host,0};HANDLE owner=CreateThread(NULL,0,Owner,&job,0,NULL);CHECK(owner);Await(host.Events[WP_SDK_RETURNED]);
 WP_STATE state;inspect(&state,TRUE);Retained(&state,32);CHECK(!state.Terminal && !state.Core.CloseRequested && !state.Producer && state.DataExclusive && state.LifetimeBlocked);
 if(nativeTerminal){terminal();inspect(&state,TRUE);Retained(&state,32);CHECK(state.Terminal && !state.Core.CloseRequested);}else{CHECK(life.BeginStop(session)==PB_OK);inspect(&state,FALSE);Retained(&state,32);CHECK(!state.Terminal && state.Core.CloseRequested);}
 CHECK(SetEvent(host.Events[WP_RESUME_WAIT]));Await(host.Events[WP_AFTER_WAIT]);inspect(&state,FALSE);Retained(&state,32);CHECK(state.Core.CloseRequested && state.Terminal==(nativeTerminal?1:0) && !state.Producer && api.PrepareClose(session,0)==PB_BUSY);Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state,FALSE);Retained(&state,32);CHECK(!state.Core.Queries && state.Core.NativeActive==1 && state.Core.BlockStage==PB_BLOCK_LEASES);
 CHECK(SetEvent(host.Events[WP_RETIRE]));Await(owner);CHECK(job.Returned==32 && CloseHandle(owner));++OwnerJoins;host.Owner=0;inspect(&state,FALSE);Retained(&state,0);CHECK(state.Producer==32);
 CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state,FALSE);Retained(&state,0);CHECK(state.Core.NativeActive==1 && state.Core.Queries && state.Core.BlockStage==PB_BLOCK_KERNEL);
 active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
 ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<WP_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));++Cases;
 printf("CASE ip=%u protocol=%u timeout=%lu terminal=%u measured=%u received32 sdk_timeout=1 sdk_post_gate=1 lifetime_pinned=1 data_unlocked=1 stopped=1 released32 native_barrier=1 owner_join=1 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,timeout,nativeTerminal?1u:0u,measured?1u:0u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(DWORD timeout=0;timeout<2;++timeout)for(ULONG terminal=0;terminal<2;++terminal)Scenario(path,udp!=0,timeout,terminal!=0,measured);}
int wmain(int argc,WCHAR **argv)
{
 CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);CHECK(Cases==16 && OwnerJoins==Cases);
 printf("PASS: wait-postcheck32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu sdk_timeouts=%lu loans_returned=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,CallbackJoins,Cases,Cases*32);return 0;
}
#endif
