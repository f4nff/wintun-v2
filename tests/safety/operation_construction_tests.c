/* SPDX-License-Identifier: GPL-2.0 */
/* Actual executor construction reservation, before any job, callback or runtime
 * lease exists. The SDK allocation failure is hosted; shutdown/locks/pool and
 * test-owned OS threads remain real. No device or registry operation runs. */
#ifdef PB_API_TESTS
#error Construction reservation must use the shipping executor
#endif
#ifndef NDEBUG
#error Compile the production paths with NDEBUG and active test checks
#endif
#include "../../include/packetbridge_version.h"
#include "guard_memory.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define OC_CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping fingerprint required");
_Static_assert(PB_VERSION_MAJOR==2&&PB_VERSION_MINOR==0&&PB_VERSION_PATCH==0,"Version remains2.0.0");

enum { OC_ENTERED, OC_CONTINUE, OC_DETACHED, OC_EVENTS };
typedef struct OC_CONFIG {HANDLE Events[OC_EVENTS];} OC_CONFIG;
typedef struct OC_STATE {
    DWORD Jobs,Idle,Stop,Pool,IdleSignaled,StopSignaled;
    DWORD Handles,Allocations,Pools,Works,Pins,Results,Steps;
    DWORD Users,Unloading,Stopping,Ready,Heap,OperationStopping;
    DWORD GateVisits,FailedAllocations,Returned,Repairs,Negative;
} OC_STATE;

#ifdef PB_OPERATION_CONSTRUCTION_FIXTURE
#ifndef PB_OPERATION_CONSTRUCTION_SOURCE
#error Bind the fixed complete legacy SDK host through the builder
#endif
static LPVOID WINAPI ConstructionAllocate(HANDLE,DWORD,SIZE_T);
static VOID ConstructionFinalCheck(VOID);
#include PB_OPERATION_CONSTRUCTION_SOURCE
static OC_CONFIG *Owner;
static volatile LONG GateVisits,FailedAllocations,Returned,Repairs;
static VOID Snapshot(OC_STATE *state)
{
    memset(state,0,sizeof(*state));
    AcquireSRWLockShared(&OperationLock);
    state->Jobs=OperationCount;state->Idle=!!OperationIdle;state->Stop=!!OperationStop;state->Pool=!!OperationPool;
    if(OperationIdle)state->IdleSignaled=WaitForSingleObject(OperationIdle,0)==WAIT_OBJECT_0;
    if(OperationStop)state->StopSignaled=WaitForSingleObject(OperationStop,0)==WAIT_OBJECT_0;
    state->OperationStopping=OperationStopping;
    ReleaseSRWLockShared(&OperationLock);
    AcquireSRWLockShared(&DependencyLock);
    state->Users=DependencyUsers;state->Unloading=DependencyUnloading;state->Stopping=RuntimeStopping;
    state->Ready=RuntimeReady;state->Heap=!!ModuleHeap;
    ReleaseSRWLockShared(&DependencyLock);
    state->Handles=(DWORD)Handles;state->Allocations=(DWORD)Allocations;state->Pools=(DWORD)Pools;
    state->Works=(DWORD)Works;state->Pins=(DWORD)Pins;state->Results=(DWORD)Results;state->Steps=(DWORD)Steps;
    state->GateVisits=(DWORD)GateVisits;state->FailedAllocations=(DWORD)FailedAllocations;state->Returned=(DWORD)Returned;state->Repairs=(DWORD)Repairs;
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    state->Negative=1;
#endif
}
static LPVOID WINAPI ConstructionAllocate(HANDLE heap,DWORD flags,SIZE_T size)
{
    OC_CHECK(Owner&&heap==GetProcessHeap()&&flags==HEAP_ZERO_MEMORY&&size==sizeof(MAIN_OPERATION));
    OC_CHECK(InterlockedIncrement(&GateVisits)==1);
    OC_STATE state;Snapshot(&state);
    OC_CHECK(state.Jobs==1&&state.Idle&&state.Stop&&state.Pool&&!state.IdleSignaled&&!state.StopSignaled);
    OC_CHECK(state.Handles==2&&state.Pools==1&&state.Steps==4&&!state.Allocations&&!state.Works&&!state.Pins&&!state.Results);
    OC_CHECK(!state.Users&&!state.Unloading&&!state.Stopping&&!state.Ready&&!state.Heap&&!state.OperationStopping);
    OC_CHECK(SetEvent(Owner->Events[OC_ENTERED])&&WaitForSingleObject(Owner->Events[OC_CONTINUE],10000)==WAIT_OBJECT_0);
    LPVOID memory=Allocate(heap,flags,size);
    OC_CHECK(!memory&&Steps==5&&GetLastError()==ERROR_NOT_ENOUGH_MEMORY);
    InterlockedIncrement(&FailedAllocations);return memory;
}
__declspec(dllexport) VOID WINAPI ConfigureConstruction(OC_CONFIG *config)
{
    OC_CHECK(config&&!Owner&&!OperationCount&&!OperationIdle&&!OperationPool&&!RuntimeReady);
    for(DWORD i=0;i<OC_EVENTS;++i)OC_CHECK(config->Events[i]);
    Owner=config;Configure(config->Events[OC_DETACHED]);SetFailure(5);
}
__declspec(dllexport) ULONG_PTR WINAPI ExecuteConstruction(DWORD ambient)
{
    OP_TEST_INPUT input={0};wcscpy_s(input.Name,_countof(input.Name),L"owned");SetLastError(ambient);
    ULONG_PTR result=Execute(&input);DWORD error=GetLastError();
    OC_CHECK(!result&&GateVisits==1&&FailedAllocations==1&&Steps==5&&!Allocations&&!Works&&!Pins&&!Results);
    InterlockedExchange(&Returned,1);SetLastError(error);return result;
}
__declspec(dllexport) VOID WINAPI ReadConstruction(OC_STATE *state) {OC_CHECK(state);Snapshot(state);}
__declspec(dllexport) VOID WINAPI RepairConstruction(VOID)
{
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    OC_STATE state;Snapshot(&state);
    OC_CHECK(state.Returned&&state.Jobs==1&&!state.IdleSignaled&&state.FailedAllocations==1&&!state.Users&&!state.Allocations&&!state.Works&&!state.Pins&&!state.Results);
    OC_CHECK(InterlockedIncrement(&Repairs)==1);OperationUnreserve();
#else
    OC_CHECK(FALSE);
#endif
}
static VOID ConstructionFinalCheck(VOID)
{
    OC_CHECK(Owner&&GateVisits==1&&FailedAllocations==1&&Returned==1&&!Handles&&!Allocations&&!Works&&!Pins&&!Results&&!Pools&&!OperationCount);
    OC_CHECK(!DependencyUsers&&!DependencyUnloading&&!ModuleHeap&&!RuntimeReady);
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    OC_CHECK(Repairs==1);
#else
    OC_CHECK(!Repairs);
#endif
}
#else
typedef VOID (WINAPI *OC_CONFIGURE)(OC_CONFIG *);
typedef VOID (WINAPI *OC_READ)(OC_STATE *);
typedef VOID (WINAPI *OC_ACTION)(VOID);
typedef ULONG_PTR (WINAPI *OC_EXECUTE)(DWORD);
typedef BOOL (WINAPI *OC_SHUTDOWN)(DWORD);
typedef struct OC_CALL {OC_EXECUTE Execute;OC_SHUTDOWN Shutdown;DWORD Ambient,Id,Error;ULONG_PTR Result;} OC_CALL;
typedef struct OC_FIXTURE {HMODULE Module;OC_CONFIG *Config;OC_STATE *State;OC_CONFIGURE Configure;OC_READ Read;OC_ACTION Repair;OC_EXECUTE Execute;OC_SHUTDOWN Shutdown;} OC_FIXTURE;
static DWORD Scenarios,Joined,Compensations,HostHandles,HostBlocks;
static HANDLE NewEvent(VOID) {HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);OC_CHECK(event);++HostHandles;return event;}
static VOID CloseOwned(HANDLE object) {OC_CHECK(HostHandles&&CloseHandle(object));--HostHandles;}
static DWORD WINAPI Call(VOID *opaque)
{
    OC_CALL *call=opaque;call->Id=GetCurrentThreadId();
    call->Result=call->Execute?call->Execute(call->Ambient):(ULONG_PTR)call->Shutdown(0);
    call->Error=GetLastError();return 0;
}
static HANDLE Start(OC_CALL *call)
{HANDLE thread=CreateThread(NULL,0,Call,call,0,NULL);OC_CHECK(thread);++HostHandles;return thread;}
static VOID Join(HANDLE thread,OC_CALL *call)
{DWORD code;OC_CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0&&GetExitCodeThread(thread,&code)&&!code&&call->Id&&GetThreadId(thread)==call->Id);CloseOwned(thread);++Joined;}
static OC_STATE Read(OC_FIXTURE *fixture){fixture->Read(fixture->State);return *fixture->State;}
static VOID CheckCached(OC_STATE state)
{
    OC_CHECK(state.Idle&&state.Stop&&state.Pool&&state.Handles==2&&state.Pools==1);
    OC_CHECK(!state.Allocations&&!state.Works&&!state.Pins&&!state.Results&&!state.Users&&!state.Unloading&&!state.Ready&&!state.Heap);
}
static VOID Scenario(LPCWSTR path,DWORD ambient,const char *phase)
{
    OC_CHECK(!HostHandles&&!HostBlocks&&!GetModuleHandleW(path));
    OC_FIXTURE fixture={0};fixture.Module=LoadLibraryW(path);OC_CHECK(fixture.Module);
#define GET(field,name) do{*(FARPROC *)&fixture.field=GetProcAddress(fixture.Module,name);OC_CHECK(fixture.field);}while(0)
    GET(Configure,"ConfigureConstruction");GET(Read,"ReadConstruction");GET(Repair,"RepairConstruction");GET(Execute,"ExecuteConstruction");GET(Shutdown,"Shutdown");
#undef GET
#ifdef PB_GUARD_TESTS
    fixture.Config=PbTestGuardAlloc(sizeof(*fixture.Config));fixture.State=PbTestGuardAlloc(sizeof(*fixture.State));
#else
    fixture.Config=malloc(sizeof(*fixture.Config));fixture.State=malloc(sizeof(*fixture.State));OC_CHECK(fixture.Config&&fixture.State);
#endif
    HostBlocks+=2;memset(fixture.Config,0,sizeof(*fixture.Config));
    for(DWORD i=0;i<OC_EVENTS;++i)fixture.Config->Events[i]=NewEvent();fixture.Configure(fixture.Config);
    OC_CALL caller={.Execute=fixture.Execute,.Ambient=ambient};HANDLE thread=Start(&caller);
    OC_CHECK(WaitForSingleObject(fixture.Config->Events[OC_ENTERED],10000)==WAIT_OBJECT_0);
    OC_STATE state=Read(&fixture);CheckCached(state);
    OC_CHECK(state.Jobs==1&&!state.IdleSignaled&&!state.StopSignaled&&!state.Stopping&&!state.OperationStopping&&state.Steps==4&&state.GateVisits==1&&!state.FailedAllocations&&!state.Returned&&!state.Repairs);
    OC_CALL stop={.Shutdown=fixture.Shutdown};HANDLE stopper=Start(&stop);Join(stopper,&stop);
    OC_CHECK(!stop.Result&&stop.Error==ERROR_TIMEOUT);
    state=Read(&fixture);CheckCached(state);
    OC_CHECK(state.Jobs==1&&!state.IdleSignaled&&state.StopSignaled&&state.Stopping&&state.OperationStopping&&state.Steps==4&&!state.Returned);
    OC_CHECK(SetEvent(fixture.Config->Events[OC_CONTINUE]));Join(thread,&caller);
    OC_CHECK(!caller.Result&&caller.Error==ERROR_OUTOFMEMORY);
    state=Read(&fixture);CheckCached(state);
    OC_CHECK(state.Steps==5&&state.Returned==1&&state.FailedAllocations==1&&state.GateVisits==1&&state.Stopping&&state.OperationStopping&&!state.Repairs);
    DWORD negative=state.Negative;
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    OC_CHECK(negative==1&&state.Jobs==1&&!state.IdleSignaled);
    /* Caller and first shutdown are joined; the ordinary module reference and
     * all three cached SDK owners remain. Retire only the actual leaked
     * reservation through its original function before any loader release. */
    fixture.Repair();++Compensations;state=Read(&fixture);CheckCached(state);OC_CHECK(state.Repairs==1);
#else
    OC_CHECK(!negative&&!state.Jobs&&state.IdleSignaled);
#endif
    OC_CHECK(!state.Jobs&&state.IdleSignaled);
    OC_CALL retry={.Shutdown=fixture.Shutdown};HANDLE retrier=Start(&retry);Join(retrier,&retry);OC_CHECK(retry.Result);
    state=Read(&fixture);CheckCached(state);OC_CHECK(!state.Jobs&&state.IdleSignaled&&state.Returned&&state.Repairs==negative);
    /* No public/fixture call remains. The min-zero private pool never receives
     * a Work object or callback; CloseThreadpool is not claimed to join opaque
     * SDK worker threads. The actual caller and both shutdown OS threads joined. */
    OC_CHECK(FreeLibrary(fixture.Module)&&WaitForSingleObject(fixture.Config->Events[OC_DETACHED],0)==WAIT_OBJECT_0&&!GetModuleHandleW(path));
    for(DWORD i=0;i<OC_EVENTS;++i)CloseOwned(fixture.Config->Events[i]);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(fixture.State);PbTestGuardFree(fixture.Config);
#else
    free(fixture.State);free(fixture.Config);
#endif
    HostBlocks-=2;OC_CHECK(!HostHandles&&!HostBlocks);++Scenarios;
    printf("CASE phase=%s ambient=%lu gateSteps=4 failedSteps=5 jobsBefore=1 sdkEvents=2 sdkPools=1 jobWorkPinsUsers=0 initialShutdown=1460 allocationError=14 retry=1 joined=3 repairs=%lu detach=1 owners=0\n",phase,ambient,negative);
}
int wmain(int argc,WCHAR **argv)
{
    OC_CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"136")));BOOL control=argc==3;
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    OC_CHECK(control);
#endif
    const DWORD errors[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,ERROR_GEN_FAILURE,ERROR_NOT_ENOUGH_QUOTA};
    DWORD warm=control?2:4,measure=control?1:4;
    for(DWORD i=0;i<warm;++i)Scenario(argv[1],errors[i%_countof(errors)],"warm");
    DWORD before,after;OC_CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(DWORD i=0;i<measure;++i)Scenario(argv[1],errors[(i+warm)%_countof(errors)],"measure");
    OC_CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&Scenarios==warm+measure&&Joined==3*Scenarios&&!HostHandles&&!HostBlocks);
    printf("PASS: %lu operation construction cases warm%lu measure%lu; actual reservation/first job allocation/shutdown retry, joined%lu repairs%lu handles%lu/%lu; all SDK owners zero at actual DLL detach\n",Scenarios,warm,measure,Joined,Compensations,before,after);
#ifdef PB_OPERATION_CONSTRUCTION_NEGATIVE
    OC_CHECK(Compensations==Scenarios);puts("OPERATION_CONSTRUCTION_RESERVATION_NOT_RETURNED: actual caller returned OOM with one unowned reservation; original OperationUnreserve compensation and complete shutdown/thread/SDK/DLL cleanup precede136");return 136;
#else
    OC_CHECK(!Compensations);return 0;
#endif
}
#endif
