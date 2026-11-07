/* SPDX-License-Identifier: GPL-2.0 */
/* Cancellation at the hosted metadata boundary, with the default production
 * 120000ms deadline unchanged; no simulated native installation or timeout. */
#ifdef PB_API_TESTS
#error Operation/adapter must use the shipping executor
#endif
#include "../safety/guard_memory.h"
#include "operation_adapter_fixture.h"
#include <stdlib.h>
#include <wchar.h>
#define CHECK SAFETY_CHECK
typedef struct CALL {OPA_OPEN Open;VOID *Adapter;DWORD Error;} CALL;
typedef struct FIXTURE {HMODULE Module;OPA_CONFIG *Config;OPA_STATE *State;OPA_CONFIGURE Configure;OPA_OPEN Open;OPA_WAIT Stop,WaitRuntime;OPA_READ Read;OPA_ACTION Repair;} FIXTURE;
static ULONG Scenarios,CallersJoined,CallbacksJoined,Repairs;
static DWORD WINAPI Caller(VOID *opaque)
{CALL *call=opaque;call->Adapter=call->Open();call->Error=GetLastError();return 0;}
static OPA_STATE Read(FIXTURE *f){f->Read(f->State);return *f->State;}
static VOID Load(FIXTURE *f,LPCWSTR path)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Module=LoadLibraryW(path);CHECK(f->Module);
#define GET(field,name) do{*(FARPROC *)&f->field=GetProcAddress(f->Module,name);CHECK(f->field);}while(0)
    GET(Configure,"ConfigureOperationAdapter");GET(Open,"OpenOperationAdapter");GET(Stop,"StopOperationAdapter");GET(WaitRuntime,"WaitOperationAdapterRuntime");GET(Read,"ReadOperationAdapter");GET(Repair,"RepairOperationAdapter");
#undef GET
#ifdef PB_GUARD_TESTS
    f->Config=PbTestGuardAlloc(sizeof(*f->Config));f->State=PbTestGuardAlloc(sizeof(*f->State));memset(f->Config,0,sizeof(*f->Config));
#else
    f->Config=calloc(1,sizeof(*f->Config));f->State=malloc(sizeof(*f->State));CHECK(f->Config&&f->State);
#endif
    for(DWORD i=0;i<OPA_EVENT_COUNT;++i){f->Config->Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f->Config->Events[i]);}f->Configure(f->Config);
}
static VOID Scenario(LPCWSTR path,BOOL allowNegative)
{
    FIXTURE f;Load(&f,path);CALL call={f.Open,NULL,0};HANDLE thread=CreateThread(NULL,0,Caller,&call,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(f.Config->Events[OPA_POPULATE_ENTERED],5000)==WAIT_OBJECT_0);OPA_STATE s=Read(&f);
    CHECK(s.Users==3&&s.Jobs==1&&s.NativeBlocks==2&&s.DeviceSet&&s.InstallationMutex&&s.RuntimeOwned&&!s.Closes&&!s.Stopping&&!s.Retiring&&s.LoadedDependencies);
    CHECK(s.SetsCreated==1&&!s.SetsClosed&&s.MutexesCreated==1&&!s.MutexesClosed&&s.CoreAllocations==1&&s.CoreWorks==1&&s.CorePools==1);
    CHECK(!f.Stop(0)&&GetLastError()==ERROR_TIMEOUT);CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&!call.Adapter&&call.Error==ERROR_CANCELLED&&CloseHandle(thread));++CallersJoined;
    s=Read(&f);CHECK(s.Users==3&&s.Jobs==1&&s.NativeBlocks==2&&s.DeviceSet&&s.InstallationMutex&&s.RuntimeOwned&&!s.Closes&&s.Stopping&&!s.Retiring&&!s.UnloadCalls);
    CHECK(!f.WaitRuntime(0)&&GetLastError()==ERROR_TIMEOUT);CHECK(SetEvent(f.Config->Events[OPA_POPULATE_GATE])&&WaitForSingleObject(f.Config->Events[OPA_RETIRING_ENTERED],5000)==WAIT_OBJECT_0);
    s=Read(&f);CHECK(s.Jobs==1&&!s.DeviceSet&&!s.InstallationMutex&&s.Retiring&&s.Abandoned&&s.JobReferences==1&&s.Stopping&&!s.Unloading&&!s.UnloadCalls&&s.LoadedDependencies&&!s.Compensations);
    CHECK(s.SetsCreated==s.SetsClosed&&s.MutexesCreated==s.MutexesClosed&&s.CoreAllocations==1&&s.CoreWorks==1&&s.CorePools==1);
    if(s.NativeBlocks){
        /* Safe observer control: callback/job pin and operation lease remain
         * parked; ordinary LoadLibrary still owns the real Close call. */
        CHECK(allowNegative&&s.Users==2&&s.NativeBlocks==2&&!s.Closes&&s.RuntimeOwned);f.Repair();++Repairs;s=Read(&f);CHECK(s.Compensations==1);
    }
    CHECK(s.Users==1&&!s.NativeBlocks&&!s.RuntimeOwned&&s.Closes==1&&s.NativeAllocations==s.NativeFrees);
    CHECK(f.Config->CallbackThread&&f.Config->CallbackThreadId&&WaitForSingleObject(f.Config->CallbackThread,0)==WAIT_TIMEOUT);
    CHECK(FreeLibrary(f.Module)&&GetModuleHandleW(path)&&WaitForSingleObject(f.Config->Events[OPA_DETACHED],0)==WAIT_TIMEOUT);
    /* The public caller and every fixture export have returned before this
     * ordinary release. Only external OS gates/witness handles are used now. */
    CHECK(SetEvent(f.Config->Events[OPA_RETIRING_GATE])&&WaitForSingleObject(f.Config->Events[OPA_DETACHED],5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(f.Config->CallbackThread,5000)==WAIT_OBJECT_0&&CloseHandle(f.Config->CallbackThread)&&!GetModuleHandleW(path));++CallbacksJoined;
    for(DWORD i=0;i<OPA_EVENT_COUNT;++i)CHECK(CloseHandle(f.Config->Events[i]));
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(f.State);PbTestGuardFree(f.Config);
#else
    free(f.State);free(f.Config);
#endif
    ++Scenarios;
}
static VOID Matrix(LPCWSTR path){for(DWORD i=0;i<16;++i)Scenario(path,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"125")));
    if(argc==3)Scenario(argv[1],TRUE);
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
        printf("PASS: operation/adapter full warmup/measured handles %lu/%lu; all native objects, genuine dependency unload, caller/callback joins and DLL detach checked\n",before,after);}
    printf("PASS: %lu operation/adapter schedules, %lu public caller joins, %lu callback-thread joins, %lu actual Close compensations; unchanged deadline, real stop broadcast, late Open success and actual Dispose/RuntimeOwned handoff\n",Scenarios,CallersJoined,CallbacksJoined,Repairs);
    if(Repairs){CHECK(argc==3&&Repairs==1);puts("OPERATION_LATE_ADAPTER_NOT_DISPOSED: abandoned actual Open left RuntimeOwned adapter; one real Close compensation while loader owned, final operation release/unload, all joins and DLL detach completed");return 125;}
    return 0;
}
