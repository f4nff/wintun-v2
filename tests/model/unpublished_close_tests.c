/* SPDX-License-Identifier: GPL-2.0 */
#ifdef PB_API_TESTS
#error Unpublished close host must not enable API test shortcuts
#endif
#include "../safety/guard_memory.h"
#include "unpublished_close_fixture.h"
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
static ULONG Scenarios,JoinedCallbacks,RepairedCases;
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static VOID Scenario(LPCWSTR path,DWORD failure,BOOL control)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);GET_API get;GET_LIFE life;UC_PREPARE prepare;UC_READ read;UC_REPAIR repair;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(prepare,"PrepareUnpublished");GET(read,"ReadUnpublished");GET(repair,"RepairUnpublishedHandoff");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE runtime;CHECK(get(2,sizeof(api),&api)==PB_OK && life(1,sizeof(runtime),&runtime)==PB_OK);
    UC_CONFIG config={0};config.Failure=failure;config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x00060014;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}
    prepare(&config);PB_CONFIG c={sizeof(c),2,4,4,0,0};PB_SESSION *session=(VOID *)(ULONG_PTR)1;
    CHECK(api.Open(L"hosted-unpublished-close",&c,&session)==PB_UNSUPPORTED && !session);
    DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_REVISION_MISMATCH,ERROR_NOT_ENOUGH_MEMORY};CHECK(GetLastError()==errors[failure]);
    UC_SNAPSHOT state;read(&state);CHECK(state.References==2 && state.Registered && state.Sessions==1 && state.Bytes && state.Pins==2 && state.Regions==1 && state.Works==1 && state.Pools==1);
    BOOL missing=!state.PublicReleased;
    if(missing) {
        CHECK(control && !state.Scheduled && !state.Callbacks && !state.Active && state.Phase==PB_LIFE_RELEASING && config.CloseFailures==1);
        repair();++RepairedCases;
    } else CHECK(state.Scheduled && !config.Repairs);
    Await(config.Native.Events[NP_PARK_ENTERED]);read(&state);
    CHECK(state.PublicReleased && state.Registered && state.References==2 && state.Scheduled && state.Callbacks==1 && state.Active==1);
    CHECK(state.Phase==PB_LIFE_RELEASING && state.Block==PB_BLOCK_DEVICE_CLOSE && state.Error==ERROR_INVALID_HANDLE);
    CHECK(!state.NativeRegistered && !state.NativeActive && !state.Drains && !state.Queries && state.Regions==1 && state.Pins==2 && config.CloseFailures>=2 && !config.CloseSuccesses);
    PB_CLOSE_PROGRESS progress;CHECK(runtime.GetRetained(0,&progress)==PB_OK && progress.Phase==PB_LIFE_RELEASING && progress.RetainedBytes && !progress.UserLeases && !progress.Published);
    PB_RUNTIME_STATUS status;CHECK(runtime.GetRuntimeStatus(&status)==PB_OK && status.Sessions==1 && status.RetainedBytes==state.Bytes);
    /* ALL DLL calls and diagnostic calls are now finished. Config and the
     * host-owned gates outlive OS-thread joins. Borrowed Device/Progress are
     * used only while the hard callback park still prevents their close. */
    CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(WaitForSingleObject(config.Native.Events[NP_DETACHED],0)==WAIT_TIMEOUT);
    CHECK(SetHandleInformation(config.Device,HANDLE_FLAG_PROTECT_FROM_CLOSE,0));CHECK(SetEvent(config.Progress));CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));
    /* No session or DLL function is called after ordinary FreeLibrary. */
    Await(config.Native.Events[NP_FINAL_ENTERED]);CHECK(config.CloseSuccesses==1 && config.CloseAttempts==config.CloseFailures+1);
    CHECK(GetModuleHandleW(path) && SetEvent(config.Native.Events[NP_FINAL_GATE]));Await(config.Native.Events[NP_DETACHED]);
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++JoinedCallbacks;}
    CHECK(!GetModuleHandleW(path) && config.Repairs==(missing?1:0));
    for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));++Scenarios;
}
static VOID Matrix(LPCWSTR path)
{for(DWORD repeat=0;repeat<8;++repeat)for(DWORD failure=0;failure<3;++failure)Scenario(path,failure,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"118")));
    if(argc==3)Scenario(argv[1],2,TRUE);
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: unpublished close full warmup/measured process handles %lu/%lu\n",before,after);}
    printf("PASS: %lu actual failed-Open retained-close schedules; %lu callback-thread handle joins, no observer Retain or public session; registry/work pins, unregistered direct retry, real module-tail/Destroy/unload and zero owned resources\n",Scenarios,JoinedCallbacks);
    if(RepairedCases){CHECK(argc==3 && RepairedCases==1);puts("UNPUBLISHED_OWNER_HANDOFF_MISSING: refs2 construction+registry with publicReleased0/scheduled0/callbacks0 observed; actual ReleaseSession used once for negative cleanup while ordinary loader remained owned; real close/joins/unload completed with zero owned residue");return 118;}
    return 0;
}
