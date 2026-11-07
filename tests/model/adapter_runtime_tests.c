/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted discovery -> shipping unified adapter -> real legacy runtime lease.
 * Device handles are real protected events; no installed device is opened. */
#ifdef PB_API_TESTS
#error Adapter/runtime tests must not compile the session shortcut
#endif
#include "../safety/guard_memory.h"
#include "adapter_runtime_fixture.h"
#include <wchar.h>
#define CHECK SAFETY_CHECK
typedef struct FIXTURE {
    HMODULE Module;AR_CONFIG Config;AR_CREATE Create;AR_ADAPTER Start,Close;AR_ACTION Stop,Shutdown;AR_READ Read;
    AR_VOID Recover,Repair,Finish;
} FIXTURE;
typedef struct WAIT_CALL {AR_ACTION Action;BOOL Result;DWORD Error;} WAIT_CALL;
typedef struct REPAIR_CALL {AR_VOID Action;} REPAIR_CALL;
static ULONG Scenarios,CallbacksJoined,ShutdownsJoined,Compensations,Detected;
static DWORD WINAPI WaitShutdown(VOID *opaque)
{WAIT_CALL *call=opaque;call->Result=call->Action();call->Error=GetLastError();return 0;}
static DWORD WINAPI RepairLease(VOID *opaque){REPAIR_CALL *call=opaque;call->Action();return 0;}
static VOID Load(FIXTURE *f,LPCWSTR path,DWORD failure)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Module=LoadLibraryW(path);CHECK(f->Module);
#define GET(field,name) do{*(FARPROC *)&f->field=GetProcAddress(f->Module,name);CHECK(f->field);}while(0)
    GET(Create,"CreateCombined");GET(Start,"StartCombinedFailure");GET(Close,"CloseCombinedPublic");GET(Stop,"StopCombined");
    GET(Shutdown,"WaitCombinedShutdown");GET(Read,"ReadCombined");GET(Recover,"RecoverCombined");GET(Repair,"CompensateCombined");GET(Finish,"FinishCombined");
#undef GET
    f->Config.Failure=failure;f->Config.Native.Mode=NP_LATE_FAILURE;f->Config.Native.NdisVersion=0x00060059;f->Config.Native.PolicyError=ERROR_NOT_SUPPORTED;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i){BOOL manual=i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE;f->Config.Native.Events[i]=CreateEventW(NULL,manual,FALSE,NULL);CHECK(f->Config.Native.Events[i]);}
    for(DWORD i=0;i<AR_EVENT_COUNT;++i){f->Config.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f->Config.Events[i]);}
}
static AR_STATE Read(FIXTURE *f){AR_STATE state;f->Read(&state);return state;}
static VOID Scenario(LPCWSTR path,DWORD failure,BOOL allowNegative)
{
    FIXTURE f;Load(&f,path,failure);VOID *adapter=f.Create(&f.Config);CHECK(adapter);f.Start(adapter);
    CHECK(WaitForSingleObject(f.Config.Native.Events[NP_PARK_ENTERED],5000)==WAIT_OBJECT_0);f.Close(adapter);adapter=NULL;
    AR_STATE state=Read(&f);
    CHECK(state.Users==1&&!state.Unloading&&state.Stopping==0&&state.WrapperReferences==1&&state.RuntimeOwned&&state.Wrappers==1&&state.LegacyBlocks==2);
    CHECK(state.NativeSessions==1&&state.NativeMaps==1&&state.NativePins==2&&!state.NativeClosed&&state.Parked&&state.Core&&!state.ResourcesClosed&&state.Bytes&&state.LoadedDependencies);
    CHECK(!f.Stop()&&GetLastError()==ERROR_TIMEOUT);WAIT_CALL wait={f.Shutdown,FALSE,0};HANDLE shutdown=CreateThread(NULL,0,WaitShutdown,&wait,0,NULL);CHECK(shutdown);
    CHECK(WaitForSingleObject(f.Config.Events[AR_SHUTDOWN_ENTERED],5000)==WAIT_OBJECT_0);state=Read(&f);CHECK(state.Users==1&&state.Stopping&&!state.Unloading&&WaitForSingleObject(shutdown,0)==WAIT_TIMEOUT);
    f.Recover();CHECK(SetEvent(f.Config.Native.Events[NP_PARK_GATE]));HANDLE stages[]={f.Config.Events[AR_UNLOAD_ENTERED],f.Config.Native.Events[NP_FINAL_ENTERED]};
    DWORD stage=WaitForMultipleObjects(2,stages,FALSE,5000);CHECK(stage==WAIT_OBJECT_0||stage==WAIT_OBJECT_0+1);BOOL missing=stage==WAIT_OBJECT_0+1;
    if(!missing){
        state=Read(&f);CHECK(!state.Users&&state.Unloading&&state.UnloadingHeld&&state.NativeClosed==1&&!state.LegacyBlocks&&state.LegacyCloses==1);
        /* WintunCloseAdapter is still on-stack: wrapper/budget/core need not
         * be finalized merely because dependency release has begun. */
        CHECK(state.Core&&state.NativeSessions==1&&!state.NativeMaps&&!state.ResourcesClosed&&state.Wrappers==1&&WaitForSingleObject(shutdown,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(f.Config.Events[AR_UNLOAD_GATE]));CHECK(WaitForSingleObject(shutdown,5000)==WAIT_OBJECT_0&&wait.Result&&CloseHandle(shutdown));shutdown=NULL;++ShutdownsJoined;
        CHECK(WaitForSingleObject(f.Config.Native.Events[NP_FINAL_ENTERED],5000)==WAIT_OBJECT_0);
    }else CHECK(allowNegative);
    state=Read(&f);CHECK(state.Core&&state.ResourcesClosed&&state.Phase==PB_LIFE_CLOSED&&state.NativeClosed==1&&!state.Wrappers&&!state.LegacyBlocks&&state.LegacyCloses==1&&!state.NativeMaps&&state.NativeSessions==1);
    if(missing)CHECK(state.Users==1&&!state.Unloading&&!state.Repairs&&state.LoadedDependencies&&WaitForSingleObject(shutdown,0)==WAIT_TIMEOUT);
    else CHECK(!state.Users&&!state.Unloading&&!state.LoadedDependencies&&!state.Repairs);
    CHECK(SetEvent(f.Config.Native.Events[NP_FINAL_GATE]));ULONG joined=0;
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(f.Config.Native.Threads[i]){CHECK(f.Config.Native.ThreadIds[i]&&WaitForSingleObject(f.Config.Native.Threads[i],5000)==WAIT_OBJECT_0&&CloseHandle(f.Config.Native.Threads[i]));++joined;++CallbacksJoined;}
    CHECK(joined);state=Read(&f);CHECK(!state.Core&&!state.NativeSessions&&!state.Bytes&&!state.NativeMaps&&!state.NativePins&&!state.ActiveCallbacks&&state.Callbacks==state.Finished&&!state.Wrappers&&!state.LegacyBlocks);
    if(missing){
        /* Negative only: observe the leaked actual lease after all native
         * owners are gone, then repay it once through real MainRuntimeRelease.
         * Ordinary LoadLibrary stays held throughout both thread joins. */
        CHECK(state.Users==1&&!state.Unloading&&!f.Stop()&&GetLastError()==ERROR_TIMEOUT);
        REPAIR_CALL repair={f.Repair};HANDLE compensator=CreateThread(NULL,0,RepairLease,&repair,0,NULL);CHECK(compensator);
        CHECK(WaitForSingleObject(f.Config.Events[AR_UNLOAD_ENTERED],5000)==WAIT_OBJECT_0);state=Read(&f);
        CHECK(!state.Users&&state.Unloading&&state.UnloadingHeld&&state.Repairs==1&&!state.Core&&WaitForSingleObject(shutdown,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(f.Config.Events[AR_UNLOAD_GATE])&&WaitForSingleObject(compensator,5000)==WAIT_OBJECT_0&&CloseHandle(compensator));++Compensations;
        CHECK(WaitForSingleObject(shutdown,5000)==WAIT_OBJECT_0&&wait.Result&&CloseHandle(shutdown));++ShutdownsJoined;++Detected;
    }
    f.Finish();CHECK(FreeLibrary(f.Module)&&WaitForSingleObject(f.Config.Native.Events[NP_DETACHED],5000)==WAIT_OBJECT_0&&!GetModuleHandleW(path));
    for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(f.Config.Native.Events[i]));for(DWORD i=0;i<AR_EVENT_COUNT;++i)CHECK(CloseHandle(f.Config.Events[i]));++Scenarios;
}
static VOID Matrix(LPCWSTR path){for(DWORD repeat=0;repeat<4;++repeat)for(DWORD failure=AR_FAIL_PROBE;failure<=AR_FAIL_POLICY;++failure)Scenario(path,failure,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"119")));
    if(argc==3)Scenario(argv[1],AR_FAIL_POLICY,TRUE);
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        printf("PASS: adapter/runtime full warmup/measured handles %lu/%lu, each owned ledger and DLL detach balanced\n",before,after);}
    printf("PASS: %lu adapter/runtime schedules, %lu shutdown joins, %lu callback-thread joins, %lu compensation joins; actual discovery lease, shipping unified retention, failed PROBE/register/policy close barrier and real deferred dependency unload\n",Scenarios,ShutdownsJoined,CallbacksJoined,Compensations);
    if(Detected){CHECK(Detected==1&&Compensations==1);puts("ADAPTER_RUNTIME_LEASE_NOT_RELEASED: native owners closed but actual DependencyUsers remained one; one real release compensation, dependency unload, all joins and DLL detach completed");return 119;}
    CHECK(!Compensations);return 0;
}
