/* SPDX-License-Identifier: GPL-2.0 */
/* Real security/private-heap ownership and InitOnce retry; namespace/installer
 * entry points are hosted. Ordinary LoadLibrary owns every caller and repair. */
#ifdef PB_API_TESTS
#error Runtime initialization must not compile the native test shortcut
#endif
#include "../safety/guard_memory.h"
#include "runtime_init_fixture.h"
#include <wchar.h>
#include <stdlib.h>
#define CHECK SAFETY_CHECK
typedef struct CALL {RI_ACQUIRE Acquire;BOOL Result;DWORD Error;} CALL;
typedef struct FIXTURE {HMODULE Module;RI_CONFIG *Config;RI_CONFIGURE Configure;RI_ACQUIRE Acquire;RI_ACTION Release,Repair;RI_READ Read;RI_STATE *State;} FIXTURE;
static ULONG Scenarios,Joined,Detected,Repairs,PointMask,Serialized,Concurrent;
static DWORD WINAPI Caller(VOID *opaque)
{CALL *call=opaque;call->Result=call->Acquire();call->Error=GetLastError();return 0;}
static RI_STATE Read(FIXTURE *f){f->Read(f->State);return *f->State;}
static VOID Load(FIXTURE *f,LPCWSTR path,DWORD point,BOOL concurrent)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Module=LoadLibraryW(path);CHECK(f->Module);
#define GET(field,name) do{*(FARPROC *)&f->field=GetProcAddress(f->Module,name);CHECK(f->field);}while(0)
    GET(Configure,"ConfigureRuntimeInit");GET(Acquire,"AcquireRuntimeInit");GET(Release,"ReleaseRuntimeInit");GET(Repair,"RepairRuntimeInit");GET(Read,"ReadRuntimeInit");
#undef GET
#ifdef PB_GUARD_TESTS
    f->State=PbTestGuardAlloc(sizeof(*f->State));f->Config=PbTestGuardAlloc(sizeof(*f->Config));memset(f->Config,0,sizeof(*f->Config));
#else
    f->State=malloc(sizeof(*f->State));f->Config=calloc(1,sizeof(*f->Config));CHECK(f->State&&f->Config);
#endif
    f->Config->Point=point;f->Config->Error=ERROR_NOT_ENOUGH_MEMORY;
    if(concurrent)for(DWORD i=0;i<RI_EVENT_COUNT;++i){f->Config->Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f->Config->Events[i]);}
    f->Configure(f->Config);
}
static VOID CheckReady(RI_STATE s,ULONG users,BOOL loaded)
{CHECK(s.Heaps==1&&!s.Tokens&&s.Locals==2&&s.NamespaceReady==1&&s.Users==users&&!s.Unloading&&s.RuntimeReady&&s.FileDescriptor&&s.PipeDescriptor&&s.ModuleHeapPresent&&(s.LoadedDependencies!=0)==loaded&&s.Failures==1);}
static VOID RepairMissing(FIXTURE *f,BOOL allowNegative)
{
    RI_STATE s=Read(f);if(!s.Users)return;
    CHECK(allowNegative&&s.Users==1&&!s.Unloading&&!s.ClientLeases&&!s.InFlight&&!s.Repairs);
    f->Repair();s=Read(f);CHECK(!s.Users&&!s.Unloading&&!s.LoadedDependencies&&s.Repairs==1);++Detected;++Repairs;
}
static VOID Scenario(LPCWSTR path,DWORD point,BOOL concurrent,BOOL allowNegative)
{
    FIXTURE f;Load(&f,path,point,concurrent);CALL first={f.Acquire,FALSE,0},second={f.Acquire,FALSE,0};RI_STATE s;
    if(concurrent){
        HANDLE a=CreateThread(NULL,0,Caller,&first,0,NULL);CHECK(a&&WaitForSingleObject(f.Config->Events[RI_FAILURE_ENTERED],5000)==WAIT_OBJECT_0);
        HANDLE b=CreateThread(NULL,0,Caller,&second,0,NULL);CHECK(b&&WaitForSingleObject(f.Config->Events[RI_SECOND_ENTERED],5000)==WAIT_OBJECT_0);
        s=Read(&f);CHECK(s.Users==2&&!s.Unloading&&!s.RuntimeReady&&!s.UnloadCalls&&s.InFlight==2&&!s.ClientLeases&&WaitForSingleObject(b,0)==WAIT_TIMEOUT);
        /* A's failed callback has retired every acquired object before B may
         * initialize. Gate A only after the real InitOnce FALSE return. */
        CHECK(SetEvent(f.Config->Events[RI_FAILURE_GATE])&&WaitForSingleObject(f.Config->Events[RI_FAILED_RETURNED],5000)==WAIT_OBJECT_0&&WaitForSingleObject(b,5000)==WAIT_OBJECT_0);
        s=Read(&f);CheckReady(s,2,TRUE);CHECK(second.Result&&s.ClientLeases==1&&s.InFlight==1&&!s.UnloadCalls&&WaitForSingleObject(a,0)==WAIT_TIMEOUT);
        CHECK(SetEvent(f.Config->Events[RI_FAILED_GATE])&&WaitForSingleObject(a,5000)==WAIT_OBJECT_0&&CloseHandle(a)&&CloseHandle(b));Joined+=2;
        CHECK(!first.Result&&first.Error==f.Config->Error);s=Read(&f);CHECK(!s.InFlight&&s.ClientLeases==1&&!s.UnloadCalls);CheckReady(s,s.Users,TRUE);
        CHECK(s.Users==1||(allowNegative&&s.Users==2));f.Release();s=Read(&f);CHECK(!s.InFlight&&!s.ClientLeases);
        RepairMissing(&f,allowNegative);
    }else{
        CHECK(!f.Acquire()&&GetLastError()==f.Config->Error);s=Read(&f);
        CHECK(!s.Heaps&&!s.Tokens&&!s.Locals&&!s.NamespaceReady&&!s.Unloading&&s.Calls==(LONG)point&&s.Failures==1&&!s.RuntimeReady&&!s.FileDescriptor&&!s.PipeDescriptor&&!s.ModuleHeapPresent&&!s.ClientLeases&&!s.InFlight);
        RepairMissing(&f,allowNegative);s=Read(&f);CHECK(!s.Users&&!s.LoadedDependencies);
        CHECK(f.Acquire());s=Read(&f);CheckReady(s,1,TRUE);CHECK(s.ClientLeases==1&&!s.InFlight);f.Release();
    }
    s=Read(&f);CheckReady(s,0,FALSE);
    CHECK(!s.LoadedDependencies&&!s.ClientLeases&&!s.InFlight&&s.UnloadCalls>0);
    CHECK(FreeLibrary(f.Module)&&!GetModuleHandleW(path));for(DWORD i=0;i<RI_EVENT_COUNT;++i)if(f.Config->Events[i])CHECK(CloseHandle(f.Config->Events[i]));
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(f.State);PbTestGuardFree(f.Config);
#else
    free(f.State);free(f.Config);
#endif
    ++Scenarios;PointMask|=1u<<(point-1);if(concurrent)++Concurrent;else ++Serialized;
}
static VOID Matrix(LPCWSTR path){for(DWORD repeat=0;repeat<2;++repeat)for(DWORD point=1;point<=7;++point)for(DWORD concurrent=0;concurrent<2;++concurrent)Scenario(path,point,concurrent!=0,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"122")));
    if(argc==3){Scenario(argv[1],7,FALSE,TRUE);Scenario(argv[1],7,TRUE,TRUE);}
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        printf("PASS: runtime initialization full warmup/measured handles %lu/%lu; exact token/LocalFree/heap/lease owners and DLL detach checked\n",before,after);}
    printf("PASS: %lu runtime initialization schedules, %lu joined acquisition callers, %lu real release compensations; prefix mask0x%02lx, serialized%lu/concurrent%lu, native security objects, actual InitOnce retry and dependency lease/unload\n",Scenarios,Joined,Repairs,PointMask,Serialized,Concurrent);
    if(Detected){CHECK(argc==3&&Detected==2&&Repairs==2);puts("RUNTIME_INIT_FAILURE_LEASE_NOT_RELEASED: no caller lease remained but real DependencyUsers stayed one; genuine release compensation, dependency unload, all caller joins and DLL detach completed");return 122;}
    CHECK(!Repairs);return 0;
}
