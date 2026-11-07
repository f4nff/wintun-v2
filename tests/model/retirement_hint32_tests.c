/* SPDX-License-Identifier: GPL-2.0 */
/* Actual stopped Release32/Cancel32 notification tails. Native IO and Active
 * are hosted; queue/control/lifecycle, SDK waits and DLL unloading are real. */
#ifdef PB_API_TESTS
#error Shipping retirement hints required
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
typedef struct H_CONFIG {NP_CONFIG Native;HANDLE Waiting[2],Stopped[2],Retire[2],Signaled[2],Go[2];DWORD Owners[2];BOOL Udp;ULONG First;} H_CONFIG;
typedef struct H_STATE {NP_SNAPSHOT Core;LONG Signals[2],Waits,Wakes,Timeouts,EventState;BOOL DataFree,LifetimeFree;ULONG Recycle,Submit;} H_STATE;
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

#ifdef H_FIXTURE
static H_CONFIG *HintHost;
static volatile LONG Signals[2],Waits,Wakes,Timeouts;
static BOOL WINAPI HintSignal(HANDLE);
static DWORD WINAPI HintWait(DWORD,const HANDLE *,BOOL,DWORD);
static DWORD WINAPI HintProgress(HANDLE,DWORD);
#include "../safety/native_policy_fixture.c"
static DWORD WINAPI HintWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Current && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent);ULONG q;
    for(q=0;q<2 && events[1]!=Current->Queues[q].UserEvent;++q){}CHECK(q<2 && HintHost->Owners[q]==GetCurrentThreadId());
    /* Selected actual SDK entry; no claim about scheduler parking. */
    CHECK(SetEvent(HintHost->Waiting[q]));DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result==WAIT_OBJECT_0);return result;
}
static BOOL WINAPI HintSignal(HANDLE event)
{
    BOOL result=SetEvent(event);
    if(Current && event==Progress){ULONG q;for(q=0;q<2 && HintHost->Owners[q]!=GetCurrentThreadId();++q){}
        if(q<2){CHECK(result && Current->CloseRequested && !Current->Queues[q].Loans && InterlockedIncrement(Signals+q)==1);
            /* Actual finite work and SetEvent finished. The production caller
             * holds Lifetime shared, after dropping its DataGate shared pin. */
            CHECK(SetEvent(HintHost->Signaled[q]));Await(HintHost->Go[q]);}
    }return result;
}
static DWORD WINAPI HintProgress(HANDLE event,DWORD timeout)
{
    if(Current && event==Progress && timeout==1000){InterlockedIncrement(&Waits);DWORD result=ObservedWait(event,timeout);if(result==WAIT_OBJECT_0)InterlockedIncrement(&Wakes);else if(result==WAIT_TIMEOUT)InterlockedIncrement(&Timeouts);else CHECK(FALSE);return result;}
    return ObservedWait(event,timeout);
}
__declspec(dllexport) VOID WINAPI PrepareHints(H_CONFIG *config){CHECK(config && !HintHost);HintHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureHints(PB_SESSION *session)
{
    Configure(session);CHECK(Current->Layout.QueueCount==2 && Current->Layout.SlotCount==64 && Current->Layout.RingCapacity==128 && !LoansNow());
    PB_QUEUE_VIEW ready;CHECK(PbAttachQueue(&ready,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);
    PB_DESCRIPTOR packets[32]={0};for(ULONG i=0;i<32;++i){packets[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};packets[i].Length=1500;
        packets[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;Packet(Current->Region+packets[i].DataOffset,HintHost->Udp,2,i);}
    uint32_t count,notify;CHECK(PbQueuePush(&ready,packets,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI InspectHints(H_STATE *out)
{
    CHECK(Current && out && ParkHeld);Observe(&out->Core);memset(out->Signals,0,sizeof(out->Signals));
    out->DataFree=TryAcquireSRWLockExclusive(&Current->DataGate);if(out->DataFree)ReleaseSRWLockExclusive(&Current->DataGate);
    out->LifetimeFree=TryAcquireSRWLockExclusive(&Current->Lifetime);if(out->LifetimeFree)ReleaseSRWLockExclusive(&Current->Lifetime);
    typedef LONG (NTAPI *QUERY_EVENT)(HANDLE,ULONG,VOID *,ULONG,ULONG *);QUERY_EVENT query;*(FARPROC *)&query=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");CHECK(query);struct {LONG Type,State;} state;
    CHECK(query(Progress,0,&state,sizeof(state),NULL)>=0 && state.Type==1 && (state.State==0 || state.State==1));out->EventState=state.State;
    out->Waits=ReadAcquire(&Waits);out->Wakes=ReadAcquire(&Wakes);out->Timeouts=ReadAcquire(&Timeouts);
    AcquireSRWLockShared(&Current->DataGate);for(ULONG q=0;q<2;++q){PB_USER_QUEUE *queue=Current->Queues+q;LONG loans=ReadAcquire(&queue->Loans);CHECK(loans==0 || loans==32);out->Signals[q]=ReadAcquire(Signals+q);CHECK(!queue->Published);
        for(ULONG i=0;i<32;++i){CHECK((q?queue->RxSlots:queue->TxSlots)[i].State==(loans?(q?PB_SLOT_RESERVED:PB_SLOT_PUBLISHED):PB_SLOT_FREE));if(loans){BYTE expected[1500];Packet(expected,HintHost->Udp,q?1u:2u,i);ULONG base=q?Current->Layout.Queues[q].RxData:Current->Layout.Queues[q].TxData;CHECK(!memcmp(expected,Current->Region+base+i*PB_BLOCK_SIZE,sizeof(expected)));}}
    }
    out->Recycle=(ULONG)Current->Queues[0].Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer;out->Submit=(ULONG)Current->Queues[1].Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer;ReleaseSRWLockShared(&Current->DataGate);
}
__declspec(dllexport) ULONG WINAPI JoinHintsPrepared(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && RuntimeCount==1 && Pins==2 && Current->References==2);
    return (ULONG)CallbackCount;
}
__declspec(dllexport) VOID WINAPI InspectHintsPrepared(VOID)
{
    CHECK(Current && !Current->Scheduled && !ActiveCallbacks && !LoansNow() && Current->Progress.Phase==PB_LIFE_PREPARED && Regions==1 && !DeviceCloses && Pins==2 && RuntimeCount==1);
    CHECK(TryAcquireSRWLockExclusive(&Current->DataGate));ReleaseSRWLockExclusive(&Current->DataGate);CHECK(!TryAcquireSRWLockExclusive(&Current->Lifetime));
}
#else
static ULONG Cases,OwnerJoins,CallbackJoins;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(H_CONFIG *);
typedef VOID (WINAPI *INSPECT)(H_STATE *);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef VOID (WINAPI *ACTION)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;H_CONFIG *Config;ULONG Index,Retired;} OWNER;
static DWORD WINAPI Owner(VOID *opaque)
{
    OWNER *j=opaque;ULONG q=j->Index;PB_LEASE leases[32];PB_TOKEN tokens[32];uint32_t count;j->Config->Owners[q]=GetCurrentThreadId();
    CHECK((q?j->Api->ReserveBatch(j->Queue,leases,32,&count):j->Api->ReceiveBatch(j->Queue,leases,32,&count))==PB_OK && count==32);
    for(ULONG i=0;i<32;++i){tokens[i]=leases[i].Descriptor.Token;CHECK(tokens[i].Slot==i && tokens[i].Generation==1 && tokens[i].Queue==q);if(q)Packet(leases[i].Data,j->Config->Udp,1,i);}
    CHECK(j->Api->Wait(j->Queue,INFINITE)==PB_STOPPED && SetEvent(j->Config->Stopped[q]));Await(j->Config->Retire[q]);
    for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,j->Config->Udp,q?1u:2u,i);CHECK(!memcmp(expected,leases[i].Data,sizeof(expected)));}
    if(q){CHECK(j->Api->CancelBatch(j->Queue,tokens,32)==PB_OK);j->Retired=32;}
    else{CHECK(j->Api->ReleaseBatch(j->Queue,tokens,32,&count)==PB_OK && count==32);j->Retired=count;}
    return 0;
}
static VOID Retained(const H_STATE *s,LONG loans)
{CHECK(s->Core.Loans==loans && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses && s->DataFree && !s->Timeouts);}
static VOID Scenario(LPCWSTR path,BOOL udp,ULONG minor,ULONG first,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;INSPECT inspect;JOIN_PREPARED joinPrepared;ACTION inspectPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareHints");GET(configure,"ConfigureHints");GET(active,"SetNativeActive");GET(inspect,"InspectHints");GET(joinPrepared,"JoinHintsPrepared");GET(inspectPrepared,"InspectHintsPrepared");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);
    H_CONFIG host={0};host.Udp=udp;host.First=first;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(minor);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}
    for(ULONG q=0;q<2;++q){host.Waiting[q]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Stopped[q]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Retire[q]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Signaled[q]=CreateEventW(NULL,TRUE,FALSE,NULL);host.Go[q]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Waiting[q] && host.Stopped[q] && host.Retire[q] && host.Signaled[q] && host.Go[q]);}
    prepare(&host);PB_CONFIG config={sizeof(config),2,64,128,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-retirement-hint32",&config,&session)==PB_OK);configure(session);
    OWNER jobs[2];HANDLE owners[2];for(ULONG q=0;q<2;++q){PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,q,&queue)==PB_OK);jobs[q]=(OWNER){&api,queue,&host,q,0};owners[q]=CreateThread(NULL,0,Owner,jobs+q,0,NULL);CHECK(owners[q]);Await(host.Waiting[q]);}
    CHECK(life.BeginStop(session)==PB_OK);for(ULONG q=0;q<2;++q)Await(host.Stopped[q]);Await(host.Native.Events[NP_PARK_ENTERED]);H_STATE state;inspect(&state);Retained(&state,64);CHECK(state.Waits==1 && !state.Wakes && state.EventState==1 && !state.Signals[0] && !state.Signals[1] && !state.Core.Queries);
    /* Drain the real initial BeginStop hint before measuring the two returns. */
    CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,64);CHECK(state.Waits==2 && state.Wakes==1 && !state.EventState);
    for(ULONG turn=0;turn<2;++turn){ULONG q=first^turn;CHECK(SetEvent(host.Retire[q]));Await(host.Signaled[q]);inspect(&state);Retained(&state,turn?0:32);CHECK(state.EventState==1 && state.Signals[q]==1 && !state.LifetimeFree && state.Wakes==1);}
    inspect(&state);CHECK(state.Signals[0]==1 && state.Signals[1]==1 && state.Recycle==32 && !state.Submit && state.EventState==1);
    CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,0);CHECK(state.Waits==3 && state.Wakes==2 && !state.EventState && state.Core.NativeActive==1 && state.Core.Queries==1 && state.Core.BlockStage==PB_BLOCK_KERNEL && !state.LifetimeFree);
    /* Two real SetEvent hints became one auto-reset condition wake. DataGate
     * is available; the two already-updated finite calls still pin Lifetime. */
    active(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();inspectPrepared();
    PB_CLOSE_PROGRESS progress;CHECK(life.GetCloseProgress(session,&progress)==PB_OK && progress.Phase==PB_LIFE_PREPARED && !progress.UserLeases && !progress.Published && !progress.NativeReferences && progress.RetainedBytes);
    for(ULONG turn=0;turn<2;++turn){ULONG q=first^turn;CHECK(SetEvent(host.Go[q]));Await(owners[q]);CHECK(jobs[q].Retired==32 && CloseHandle(owners[q]));++OwnerJoins;}
    /* Every actual public queue owner joined before final Close can consume. */
    host.Owners[0]=host.Owners[1]=0;CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
    ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG q=0;q<2;++q)CHECK(CloseHandle(host.Waiting[q]) && CloseHandle(host.Stopped[q]) && CloseHandle(host.Retire[q]) && CloseHandle(host.Signaled[q]) && CloseHandle(host.Go[q]));++Cases;
    printf("CASE ip=%u protocol=%u ndis=%lu first=%lu measured=%u release=32 cancel=32 stop_waits=2 signals=2 merged_state=1 sdk_wakes=1 data_free=1 lifetime_free=0 prepared_pins=2 owner_join=2 prepared=%lu final=1 callbacks=%lu owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,minor,first,measured?1u:0u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured){for(ULONG udp=0;udp<2;++udp)for(ULONG version=0;version<2;++version)for(ULONG first=0;first<2;++first)Scenario(path,udp!=0,version?89u:20u,first,measured);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);
    Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);
    CHECK(OwnerJoins==Cases*2);printf("PASS: retirement-hint32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu coalesced_wakes=%lu owned=0 true_unload=1\n",Cases,Cases/2,Cases/2,OwnerJoins,CallbackJoins,Cases);return 0;
}
#endif
