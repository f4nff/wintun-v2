/* SPDX-License-Identifier: GPL-2.0 */
/* Native default SDK pool owns its OS threads. The test owns no callback-thread
 * handle and uses actual callback return, native tail pin and DLL detach. */
#ifdef PB_API_TESTS
#error Cleanup/namespace must use the shipping runtime
#endif
#include "../safety/guard_memory.h"
#include "cleanup_namespace_fixture.h"
#include <stdlib.h>
#include <wchar.h>
#define CHECK SAFETY_CHECK
typedef struct FIXTURE {HMODULE Module;CN_CONFIG *Config;CN_STATE *State;CN_CONFIGURE Configure;CN_QUEUE Queue;CN_SHUTDOWN Shutdown;CN_READ Read;CN_ACTION Repair;} FIXTURE;
static ULONG Scenarios,Cancels,Cached,Repairs;
static CN_STATE Read(FIXTURE *f){f->Read(f->State);return *f->State;}
static VOID Load(FIXTURE *f,LPCWSTR path,BOOL cancel)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Module=LoadLibraryW(path);CHECK(f->Module);
#define GET(field,name) do{*(FARPROC *)&f->field=GetProcAddress(f->Module,name);CHECK(f->field);}while(0)
    GET(Configure,"ConfigureCleanupNamespace");GET(Queue,"QueueCleanupNamespace");GET(Shutdown,"ShutdownCleanupNamespace");GET(Read,"ReadCleanupNamespace");GET(Repair,"RepairCleanupNamespace");
#undef GET
#ifdef PB_GUARD_TESTS
    f->Config=PbTestGuardAlloc(sizeof(*f->Config));f->State=PbTestGuardAlloc(sizeof(*f->State));memset(f->Config,0,sizeof(*f->Config));
#else
    f->Config=calloc(1,sizeof(*f->Config));f->State=malloc(sizeof(*f->State));CHECK(f->Config&&f->State);
#endif
    f->Config->Cancel=cancel;for(DWORD i=0;i<CN_EVENT_COUNT;++i){f->Config->Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f->Config->Events[i]);}f->Configure(f->Config);
}
static VOID Scenario(LPCWSTR path,BOOL cancel,BOOL allowNegative)
{
    FIXTURE f;Load(&f,path,cancel);CHECK(f.Queue()&&WaitForSingleObject(f.Config->Events[CN_CREATE_ENTERED],5000)==WAIT_OBJECT_0);CN_STATE s=Read(&f);
    CHECK(s.Users==1&&!s.Unloading&&s.Scheduled&&!s.NamespaceCached&&!s.BoundaryCached&&s.NamespaceOwners==1&&s.BoundaryOwners==1&&!s.MutexOwners&&s.PairEvents==2&&s.Callbacks==1&&!s.Returned&&!s.MutexCreates&&!s.UnloadCalls&&!s.CSAvailable&&!s.Compensations);
    if(cancel){
        CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);s=Read(&f);
        CHECK(s.Users==1&&s.Stopping&&s.Scheduled&&s.CleanupStopping&&s.NamespaceOwners==1&&s.BoundaryOwners==1&&!s.NamespaceCached&&!s.BoundaryCached);
        CHECK(SetEvent(f.Config->Events[CN_CREATE_GATE])&&WaitForSingleObject(f.Config->Events[CN_RELEASE_ENTERED],5000)==WAIT_OBJECT_0);s=Read(&f);
        CHECK(s.Users==1&&s.ReleaseHeld&&s.CSAvailable&&!s.NamespaceCached&&!s.BoundaryCached&&!s.NamespaceOwners&&!s.MutexOwners&&s.TakeError==ERROR_CANCELLED&&!s.LegacyCalls&&!s.OrphanCalls&&!s.MutexCreates&&!s.UnloadCalls&&!s.Returned);
        if(s.BoundaryOwners){CHECK(allowNegative&&s.BoundaryOwners==1&&!s.Compensations);f.Repair();++Repairs;s=Read(&f);CHECK(!s.BoundaryOwners&&s.Compensations==1&&s.Users==1&&s.ReleaseHeld);}
        CHECK(FreeLibrary(f.Module)&&GetModuleHandleW(path)&&WaitForSingleObject(f.Config->Events[CN_DETACHED],0)==WAIT_TIMEOUT);
        /* Public fixture calls have all returned. Only caller-owned OS gates
         * remain after dropping the ordinary loader reference. */
        CHECK(SetEvent(f.Config->Events[CN_RELEASE_GATE]));++Cancels;
    }else{
        CHECK(SetEvent(f.Config->Events[CN_CREATE_GATE])&&WaitForSingleObject(f.Config->Events[CN_CALLBACK_RETURNED],5000)==WAIT_OBJECT_0);s=Read(&f);
        CHECK(!s.Users&&!s.Unloading&&!s.Scheduled&&s.NamespaceCached&&s.BoundaryCached&&s.NamespaceOwners==1&&s.BoundaryOwners==1&&!s.MutexOwners&&s.UnloadCalls>0&&s.MutexCreates==2&&s.CSAvailable&&!s.Compensations);
        CHECK(f.Shutdown(5000)&&FreeLibrary(f.Module));++Cached;
    }
    CHECK(WaitForSingleObject(f.Config->Events[CN_DETACHED],5000)==WAIT_OBJECT_0&&!GetModuleHandleW(path));for(DWORD i=0;i<CN_EVENT_COUNT;++i)CHECK(CloseHandle(f.Config->Events[i]));
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(f.State);PbTestGuardFree(f.Config);
#else
    free(f.State);free(f.Config);
#endif
    ++Scenarios;
}
static VOID Matrix(LPCWSTR path){for(DWORD repeat=0;repeat<8;++repeat){Scenario(path,FALSE,FALSE);Scenario(path,TRUE,FALSE);}}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"128")));DWORD before,after;
    if(argc==3){Scenario(argv[1],TRUE,TRUE);Scenario(argv[1],TRUE,TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Scenario(argv[1],TRUE,TRUE);}
    else{Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: cleanup/namespace strict warmup/measured handles %lu/%lu; each actual DLL detach and owned-resource ledger checked\n",before,after);
    printf("PASS: %lu cleanup/namespace schedules, %lu pending cancellations, %lu cached successes, %lu actual boundary compensations; actual main/namespace/scheduler/Shutdown, default SDK callback return/tail, all test-owned resources reclaimed\n",Scenarios,Cancels,Cached,Repairs);
    if(Repairs){CHECK(argc==3&&Repairs==3);puts("CLEANUP_NAMESPACE_PENDING_BOUNDARY_NOT_DELETED: cancelled namespace unpublished and CS released but final boundary still owned; one actual delete per DLL before runtime-release gate, callback tail and actual unload complete");return 128;}
    return 0;
}
