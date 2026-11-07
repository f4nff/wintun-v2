/* SPDX-License-Identifier: GPL-2.0 */
/* Close(0) returns non-success before the new reader starts. The public handle
 * stays owned until every reader has joined and the final Close consumes it. */
#ifdef PB_API_TESTS
#error Status lifetime host must not use PB_API_TESTS
#endif
#include "../safety/guard_memory.h"
#include "status_lifetime_fixture.h"
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct READER {PB_INTERFACE_V2 *Api;PB_SESSION *Session;SL_MARK Mark;BOOL Caps;PB_RESULT Result;} READER;
static ULONG Scenarios,JoinedReaders,JoinedCallbacks,MissingPins;
static DWORD WINAPI ReadCall(VOID *opaque)
{
    READER *call=opaque;call->Mark();PB_PROBE_STATUS caps;PB_V2_STATUS status;
    call->Result=call->Caps?call->Api->GetCapabilities(call->Session,&caps):call->Api->GetStatus(call->Session,&status);return 0;
}
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static VOID Scenario(LPCWSTR path,BOOL caps,BOOL fails,BOOL control)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);
    GET_API get;GET_LIFE life;NP_INITIALIZE initialize;NP_CONFIGURE configure;NP_SET_ACTIVE setActive;NP_OBSERVE observe;
    SL_CONFIGURE configureReader;SL_MARK mark;SL_READ read;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(initialize,"Initialize");GET(configure,"Configure");
    GET(setActive,"SetNativeActive");GET(observe,"Observe");GET(configureReader,"ConfigureStatusReader");GET(mark,"MarkStatusReader");GET(read,"ReadStatusLifetime");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE runtime;CHECK(get(2,sizeof(api),&api)==PB_OK && life(1,sizeof(runtime),&runtime)==PB_OK);
    NP_CONFIG host={0};host.Mode=NP_LATE_SUCCESS;host.NdisVersion=0x00060059;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i){host.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Events[i]);}
    SL_CONFIG readerConfig={.Fails=fails};
    for(DWORD i=0;i<SL_EVENT_COUNT;++i){readerConfig.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(readerConfig.Events[i]);}
    initialize(&host);PB_CONFIG config={sizeof(config),1,4,4,0,0};PB_SESSION *session;
    CHECK(api.Open(L"hosted-status-lifetime",&config,&session)==PB_OK);configure(session);configureReader(&readerConfig);
    CHECK(api.Close(session,0)==PB_QUARANTINED);Await(host.Events[NP_PARK_ENTERED]);
    /* Native ownership prevents final release until the new, legally entered
     * reader has reached actual I/O with its shared Lifetime pin. */
    READER call={&api,session,mark,caps,PB_INVALID};HANDLE thread=CreateThread(NULL,0,ReadCall,&call,0,NULL);CHECK(thread);
    Await(readerConfig.Events[SL_READ_ENTERED]);setActive(0);CHECK(SetEvent(host.Events[NP_PARK_GATE]));Await(readerConfig.Events[SL_EXCLUSIVE_ATTEMPTED]);
    SL_SNAPSHOT state;read(&state);CHECK(state.Core && state.ReaderHeld && state.Sessions==1 && state.Bytes && state.Regions==1);
    BOOL missing=state.ExclusiveImmediate!=0;
    if(missing) {
        CHECK(control && !caps);Await(readerConfig.Events[SL_EXCLUSIVE_HELD]);read(&state);CHECK(state.ReaderHeld && state.ExclusiveHeld);++MissingPins;
    } else CHECK(WaitForSingleObject(readerConfig.Events[SL_EXCLUSIVE_HELD],0)==WAIT_TIMEOUT && !state.ExclusiveHeld);
    NP_SNAPSHOT core;observe(&core);
    CHECK(core.Phase==PB_LIFE_RELEASING && core.BlockStage==PB_BLOCK_API_USERS && !core.ResourcesClosed && core.Regions==1 && !core.DeviceCloses);
    /* Even the negative holds real EX before ReleaseResources; finish and join
     * the still-live reader before the worker can close any native resource. */
    CHECK(SetEvent(readerConfig.Events[SL_READ_GATE]));Await(thread);
    CHECK(call.Result==(fails?(caps?PB_UNSUPPORTED:PB_CORRUPT):PB_OK));CHECK(CloseHandle(thread));++JoinedReaders;
    Await(readerConfig.Events[SL_EXCLUSIVE_HELD]);read(&state);CHECK(state.ExclusiveHeld && !state.ReaderHeld);LONG ioCalls=state.IoCalls;
    PB_V2_STATUS status;PB_PROBE_STATUS capability;
    if(!missing)CHECK(api.GetStatus(session,&status)==PB_BUSY);
    CHECK(api.GetCapabilities(session,&capability)==PB_BUSY);read(&state);CHECK(state.IoCalls==ioCalls);
    CHECK(SetEvent(readerConfig.Events[SL_EXCLUSIVE_GATE]));Await(host.Events[NP_FINAL_ENTERED]);observe(&core);
    CHECK(core.ResourcesClosed && core.Phase==PB_LIFE_CLOSED && core.DeviceCloses==1 && !core.Regions);
    CHECK(SetEvent(host.Events[NP_FINAL_GATE]));ULONGLONG deadline=GetTickCount64()+5000;
    do {observe(&core);if(!core.ActiveCallbacks && core.Finished==core.Callbacks && !core.Registered && !core.Sessions)break;
        CHECK(GetTickCount64()<deadline);SwitchToThread();}while(1);
    CHECK(core.References==1 && !core.PublicReleased && core.Allocations==1 && core.Pins==1 && core.Events==2);
    read(&state);ioCalls=state.IoCalls;
    CHECK(api.GetStatus(session,&status)==PB_OK && status.Size==sizeof(status) && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
    CHECK(api.GetCapabilities(session,&capability)==PB_STOPPED);uint64_t ticket=1;
    CHECK(runtime.BeginSetPolicy(session,0,&ticket)==PB_STOPPED && !ticket);
    CHECK(api.SetPolicy(session,0)==PB_STOPPED && api.Wake(session,0)==PB_STOPPED);read(&state);CHECK(state.IoCalls==ioCalls);
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(host.Threads[i]){Await(host.Threads[i]);CHECK(CloseHandle(host.Threads[i]));++JoinedCallbacks;}
    read(&state);CHECK(!state.Core && !state.ReaderHeld && !state.ExclusiveHeld && !state.Allocations && !state.Regions && !state.Events && !state.Works && !state.Pools && !state.Pins && !state.Sessions && !state.Bytes);
    CHECK(!state.Active && state.Callbacks && state.Callbacks==state.Finished);
    CHECK(FreeLibrary(library));Await(host.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Events[i]));
    for(DWORD i=0;i<SL_EVENT_COUNT;++i)CHECK(CloseHandle(readerConfig.Events[i]));++Scenarios;
}
static VOID Matrix(LPCWSTR path)
{for(DWORD repeat=0;repeat<8;++repeat)for(DWORD caps=0;caps<2;++caps)for(DWORD fails=0;fails<2;++fails)Scenario(path,caps!=0,fails!=0,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"113")));
    if(argc==3)Scenario(argv[1],FALSE,FALSE,TRUE);
    else {Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: status lifetime full warmup/measured process handles %lu/%lu\n",before,after);}
    printf("PASS: %lu actual GetStatus/GetCapabilities reader Lifetime schedules; %lu joined public readers, %lu callback-thread handle joins; I/O success/failure, physical release exclusion, Closed-public shell controls, final Close and actual DLL unload/resource convergence\n",Scenarios,JoinedReaders,JoinedCallbacks);
    if(MissingPins) {CHECK(argc==3 && MissingPins==1);puts("STATUS_LIFETIME_PIN_MISSING: real worker held Lifetime exclusive while status reader remained in I/O; reader released/joined before physical close, normal cleanup and DLL unload completed with zero owned residue");return 113;}
    return 0;
}
