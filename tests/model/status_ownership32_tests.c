/* SPDX-License-Identifier: GPL-2.0 */
/* Actual shipping session/status/lifecycle in an unloadable DLL. Malformed
 * control replies are defensive hosted inputs, not observed kernel failures. */
#ifdef PB_API_TESTS
#error Shipping status ownership required
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
enum { O_READY,O_GO,O_EVENTS };
enum { CAPS_OK,CAPS_SHORT,CAPS_LONG,CAPS_SIZE,CAPS_MAGIC,CAPS_SCHEMA,CAPS_FILE,CAPS_ABI,CAPS_SOURCE,CAPS_SOURCE_END,CAPS_IO,CAPS_COUNT };
enum { QUERY_OK,QUERY_SHORT,QUERY_LONG,QUERY_SIZE,QUERY_EPOCH,QUERY_IO,QUERY_COUNT };
typedef struct O_CONFIG {NP_CONFIG Native;HANDLE Events[O_EVENTS];BOOL Udp;ULONG Query;} O_CONFIG;
typedef struct O_STATE {NP_SNAPSHOT Core;LONG Published,Fatal,Scheduled;ULONG Recycle;DWORD Error;ULONG Result;} O_STATE;
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
#ifdef O_FIXTURE
static O_CONFIG *OwnerHost;
static volatile LONG CapsMode,QueryMode;
static BOOL WINAPI ReplyIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
#include "../safety/native_policy_fixture.c"
static BOOL WINAPI ReplyIo(HANDLE device,DWORD code,VOID *input,DWORD inSize,VOID *output,DWORD outSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    BOOL okay=HostedIo(device,code,input,inSize,output,outSize,returned,overlapped);if(!okay || !Current)return okay;
    if(code==PB_PROBE_IOCTL_QUERY){
        LONG mode=ReadAcquire(&CapsMode);PB_PROBE_STATUS *p=output;
        if(mode==CAPS_IO){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        if(mode==CAPS_SHORT)--*returned;else if(mode==CAPS_LONG)++*returned;
        else if(mode==CAPS_SIZE)--p->Size;else if(mode==CAPS_MAGIC)p->Magic^=1;else if(mode==CAPS_SCHEMA)++p->Schema;
        else if(mode==CAPS_FILE)p->FileVersion^=1;else if(mode==CAPS_ABI)++p->DataAbi;
        else if(mode==CAPS_SOURCE)p->SourceFingerprint[0]^=1;else if(mode==CAPS_SOURCE_END)p->SourceFingerprint[64]=1;
    }else if(code==PB_V2_IOCTL_QUERY){
        LONG mode=ReadAcquire(&QueryMode);PB_V2_STATUS *p=output;
        if(mode==QUERY_IO){SetLastError(ERROR_DEVICE_NOT_CONNECTED);return FALSE;}
        if(mode==QUERY_SHORT)--*returned;else if(mode==QUERY_LONG)++*returned;else if(mode==QUERY_SIZE)--p->Size;else if(mode==QUERY_EPOCH)p->Epoch^=1;
        SetLastError(ERROR_BUSY);
    }
    /* Long results change only the reported count, never the output span. */
    return okay;
}
__declspec(dllexport) VOID WINAPI PrepareOwnership(O_CONFIG *config){CHECK(config && !OwnerHost);OwnerHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureOwnership(PB_SESSION *session)
{
    Configure(session);CHECK(Current->Layout.SlotCount==64 && Current->Layout.RingCapacity==128 && !LoansNow());
    PB_QUEUE_VIEW ready;CHECK(PbAttachQueue(&ready,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);
    PB_DESCRIPTOR packets[32]={0};for(ULONG i=0;i<32;++i){packets[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};packets[i].Length=1500;
        packets[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;Packet(Current->Region+packets[i].DataOffset,OwnerHost->Udp,2,i);}
    uint32_t count,notify;CHECK(PbQueuePush(&ready,packets,32,&count,&notify)==PB_OK && count==32 && !notify);
}
__declspec(dllexport) VOID WINAPI SetReplyModes(ULONG caps,ULONG query)
{CHECK(Current && caps<CAPS_COUNT && query<QUERY_COUNT);InterlockedExchange(&CapsMode,(LONG)caps);InterlockedExchange(&QueryMode,(LONG)query);}
__declspec(dllexport) VOID WINAPI InspectOwnership(O_STATE *out)
{
    CHECK(out && Current);Observe(&out->Core);PB_USER_QUEUE *q=Current->Queues;
    AcquireSRWLockShared(&Current->DataGate);out->Published=ReadAcquire(&q->Published);out->Fatal=ReadAcquire(&Current->Fatal);
    out->Recycle=(ULONG)q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer;
    CHECK(out->Core.Loans==0 || out->Core.Loans==32);CHECK(out->Published==0 || out->Published==32);
    for(ULONG i=0;i<32;++i){
        CHECK(q->TxSlots[i].State==(out->Core.Loans?PB_SLOT_PUBLISHED:PB_SLOT_FREE));CHECK(q->RxSlots[i].State==(out->Published?PB_SLOT_PUBLISHED:PB_SLOT_FREE));
        BYTE expected[1500];if(out->Core.Loans){Packet(expected,OwnerHost->Udp,2,i);CHECK(!memcmp(expected,Current->Region+Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE,sizeof(expected)));}
        if(out->Published){Packet(expected,OwnerHost->Udp,1,i);CHECK(!memcmp(expected,Current->Region+Current->Layout.Queues[0].RxData+i*PB_BLOCK_SIZE,sizeof(expected)));}
    }
    ReleaseSRWLockShared(&Current->DataGate);AcquireSRWLockShared(&Current->State);out->Scheduled=Current->Scheduled;out->Error=Current->Progress.LastError;out->Result=Current->Progress.Result;ReleaseSRWLockShared(&Current->State);
}
__declspec(dllexport) ULONG WINAPI JoinOwnershipPrepared(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount && !Current->ResourcesClosed && Regions==1 && !NativeActive && !LoansNow() && !Current->Queues[0].Published && RuntimeCount==1 && Pins==2 && Current->References==2);
    return (ULONG)CallbackCount;
}
#else
static ULONG Scenarios,OwnerJoins,CallbackJoins,CapsRejected,QueryRejected;
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(O_CONFIG *);
typedef VOID (WINAPI *MODES)(ULONG,ULONG);
typedef VOID (WINAPI *INSPECT)(O_STATE *);
typedef ULONG (WINAPI *JOIN_PREPARED)(VOID);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;O_CONFIG *Config;ULONG Released;} OWNER;
static DWORD WINAPI Own(VOID *opaque)
{
    OWNER *j=opaque;PB_LEASE sent[32],received[32];uint32_t count;
    CHECK(j->Api->ReserveBatch(j->Queue,sent,32,&count)==PB_OK && count==32);
    for(ULONG i=0;i<32;++i){Packet(sent[i].Data,j->Config->Udp,1,i);sent[i].Descriptor.Length=1500;CHECK(sent[i].Descriptor.Token.Slot==i && sent[i].Descriptor.Token.Generation==1);}
    CHECK(j->Api->CommitBatch(j->Queue,sent,32,&count)==PB_OK && count==32);
    CHECK(j->Api->ReceiveBatch(j->Queue,received,32,&count)==PB_OK && count==32);
    CHECK(SetEvent(j->Config->Events[O_READY]));Await(j->Config->Events[O_GO]);PB_TOKEN tokens[32];
    for(ULONG i=0;i<32;++i){BYTE expected[1500];Packet(expected,j->Config->Udp,2,i);CHECK(received[i].Descriptor.Length==1500 && received[i].Descriptor.Token.Slot==i && received[i].Descriptor.Token.Generation==1 && !memcmp(expected,received[i].Data,sizeof(expected)));tokens[i]=received[i].Descriptor.Token;}
    CHECK(j->Api->ReleaseBatch(j->Queue,tokens,32,&count)==PB_OK && count==32);j->Released=count;return 0;
}
static VOID Retained(const O_STATE *s,LONG loans)
{
    CHECK(s->Core.Loans==loans && s->Published==32 && !s->Fatal && !s->Core.ResourcesClosed && s->Core.Regions==1 && s->Core.Sessions==1 && s->Core.Bytes && s->Core.Pins==2 && s->Core.Registered && !s->Core.PublicReleased && !s->Core.DeviceCloses);
}
static VOID Scenario(LPCWSTR path,BOOL udp,ULONG query,ULONG minor,BOOL measured)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);
    GET_API get;GET_LIFE getLife;PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE setActive;MODES modes;INSPECT inspect;JOIN_PREPARED joinPrepared;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareOwnership");GET(configure,"ConfigureOwnership");GET(setActive,"SetNativeActive");GET(modes,"SetReplyModes");GET(inspect,"InspectOwnership");GET(joinPrepared,"JoinOwnershipPrepared");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);
    O_CONFIG host={0};host.Udp=udp;host.Query=query;host.Native.Mode=NP_LATE_SUCCESS;host.Native.NdisVersion=PB_NDIS_VERSION(minor);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){host.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED && i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Native.Events[i]);}
    for(ULONG i=0;i<O_EVENTS;++i){host.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(host.Events[i]);}
    prepare(&host);PB_CONFIG config={sizeof(config),1,64,128,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-status-ownership32",&config,&session)==PB_OK);configure(session);
    PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,0,&queue)==PB_OK);OWNER job={&api,queue,&host,0};HANDLE owner=CreateThread(NULL,0,Own,&job,0,NULL);CHECK(owner);Await(host.Events[O_READY]);
    O_STATE state;inspect(&state);Retained(&state,32);CHECK(!state.Core.CloseRequested && !state.Core.Queries);
    for(ULONG bad=1;bad<CAPS_COUNT;++bad){
        modes(bad,QUERY_OK);PB_PROBE_STATUS caps;CHECK(api.GetCapabilities(session,&caps)==PB_UNSUPPORTED);++CapsRejected;
        inspect(&state);Retained(&state,32);CHECK(!state.Core.CloseRequested && !state.Core.Queries);
        modes(CAPS_OK,QUERY_OK);CHECK(api.GetCapabilities(session,&caps)==PB_OK && caps.NdisVersion==PB_NDIS_VERSION(minor) && caps.SessionRegistered==1 && !caps.AllowedMask);
        CHECK((caps.ImplementedMask&(PB_TX_USO4|PB_TX_USO6))==(minor==20?0u:(PB_TX_USO4|PB_TX_USO6)));
    }
    CHECK(life.BeginStop(session)==PB_OK);Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,32);
    CHECK(state.Core.BlockStage==PB_BLOCK_LEASES && state.Core.NativeActive==1 && !state.Core.Queries && api.PrepareClose(session,0)==PB_BUSY);
    CHECK(SetEvent(host.Events[O_GO]));Await(owner);CHECK(job.Released==32 && CloseHandle(owner));++OwnerJoins;
    CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,0);
    CHECK(state.Recycle==32 && state.Core.BlockStage==PB_BLOCK_KERNEL && state.Core.NativeActive==1 && state.Core.Queries && api.PrepareClose(session,0)==PB_QUARANTINED);
    modes(CAPS_OK,query);setActive(0);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));Await(host.Native.Events[NP_PARK_ENTERED]);inspect(&state);Retained(&state,0);
    DWORD expected=query==QUERY_IO?ERROR_DEVICE_NOT_CONNECTED:ERROR_INVALID_DATA;
    CHECK(state.Core.NativeActive==0 && state.Core.BlockStage==PB_BLOCK_KERNEL && state.Error==expected && state.Result==PB_CORRUPT);
    PB_V2_STATUS status;SetLastError(ERROR_SUCCESS);CHECK(api.GetStatus(session,&status)==PB_CORRUPT && GetLastError()==expected);++QueryRejected;
    inspect(&state);Retained(&state,0);modes(CAPS_OK,QUERY_OK);CHECK(SetEvent(host.Native.Events[NP_PARK_GATE]));
    ULONG prepared=joinPrepared();CHECK(api.PrepareClose(session,5000)==PB_OK);prepared=joinPrepared();inspect(&state);
    CHECK(prepared && !state.Published && !state.Core.Loans && !state.Core.NativeActive && state.Core.Phase==PB_LIFE_PREPARED && !state.Core.ResourcesClosed && state.Core.Regions==1 && !state.Scheduled);
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(host.Native.Events[NP_FINAL_ENTERED]);
    /* The final tail gate owns the internal callback/registry pins. No public
     * call or queue use occurs after successful final Close consumes its ref. */
    CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(host.Native.Events[NP_FINAL_GATE]));Await(host.Native.Events[NP_DETACHED]);
    ULONG callbacks=0;for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(host.Native.Threads[i]){CHECK(i==callbacks && host.Native.ThreadIds[i]);Await(host.Native.Threads[i]);CHECK(CloseHandle(host.Native.Threads[i]));++callbacks;}
    CHECK(callbacks==prepared+1 && !GetModuleHandleW(path));CallbackJoins+=callbacks;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Native.Events[i]));for(ULONG i=0;i<O_EVENTS;++i)CHECK(CloseHandle(host.Events[i]));++Scenarios;
    printf("CASE ip=%u protocol=%u query=%lu ndis=%lu measured=%u loans=32 published=32 caps_rejected=10 query_rejected=1 released=32 reconciled=32 prepared=%lu final=1 callbacks=%lu owner_join=1 owned=0 unload=1\n",PB_TEST_IP_VERSION,udp?17u:6u,query,minor,measured?1u:0u,prepared,callbacks);
}
static VOID Matrix(LPCWSTR path,BOOL measured)
{for(ULONG udp=0;udp<2;++udp)for(ULONG query=1;query<QUERY_COUNT;++query)for(ULONG version=0;version<2;++version)Scenario(path,udp!=0,query,version?89u:20u,measured);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);printf("FAMILY: IPv%u\n",PB_TEST_IP_VERSION);
    Matrix(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1],TRUE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("HANDLES before=%lu after=%lu\n",before,after);
    CHECK(OwnerJoins==Scenarios && CapsRejected==Scenarios*10 && QueryRejected==Scenarios);
    printf("PASS: status-ownership32 cases=%lu warm=%lu measured=%lu owner_joins=%lu callback_joins=%lu caps_rejected=%lu query_rejected=%lu defensive_replies=1 owned=0 true_unload=1\n",Scenarios,Scenarios/2,Scenarios/2,OwnerJoins,CallbackJoins,CapsRejected,QueryRejected);return 0;
}
#endif
