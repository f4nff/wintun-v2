/* SPDX-License-Identifier: GPL-2.0 */
/* Complete shipping Open/operation wrappers/Close and actual main/runtime.
 * Hash-locked SDK host performs discovery only; no device/registry is changed. */
#ifdef PB_API_TESTS
#error Operation/adapter must not use PB_API_TESTS
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <delayimp.h>
#include "../model/operation_adapter_fixture.h"
#include "../../api/adapter.h"
static ULONG DependencyUsers;
static OPA_CONFIG *Owner;
static volatile LONG Unexpected,Compensations,DependencyUnloads;
struct MAIN_OPERATION;
static struct MAIN_OPERATION *RetiringJob;
static BOOL PopulateAdapterData(WINTUN_ADAPTER *);
static VOID ObserveCallbackThread(VOID);
static VOID ObserveOperationRelease(struct MAIN_OPERATION *);
static VOID OperationAdapterFinalCheck(VOID);
static BOOL WINAPI ObserveDependencyUnload(LPCSTR);
#define Leases DependencyUsers
#define PB_ADAPTER_OPEN_SOURCE PB_OPERATION_ADAPTER_OPEN_SOURCE
#define main UnusedOpenHostMain
#include PB_OPERATION_ADAPTER_OPEN_HOST
#undef main
#define __FUnloadDelayLoadedDLL2 ObserveDependencyUnload
#include PB_OPERATION_ADAPTER_OPERATION_HOST
#undef __FUnloadDelayLoadedDLL2
static DWORD DependenciesLoaded(VOID)
{
    const BYTE *base=(const BYTE *)ResourceModule;const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt=(const IMAGE_NT_HEADERS *)(base+dos->e_lfanew);DWORD offset=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress,count=0;
    if(offset)for(const ImgDelayDescr *d=(const ImgDelayDescr *)(base+offset);d->rvaDLLName;++d)if(*(HMODULE *)(base+d->rvaHmod))++count;return count;
}
static BOOL WINAPI ObserveDependencyUnload(LPCSTR name)
{BOOL unloaded=__FUnloadDelayLoadedDLL2(name);if(unloaded)InterlockedIncrement(&DependencyUnloads);return unloaded;}
static BOOL PopulateAdapterData(WINTUN_ADAPTER *adapter)
{
    CHECK(HostedPopulate(adapter));CHECK(Owner&&SetEvent(Owner->Events[OPA_POPULATE_ENTERED]));
    CHECK(WaitForSingleObject(Owner->Events[OPA_POPULATE_GATE],10000)==WAIT_OBJECT_0);return TRUE;
}
static VOID ObserveCallbackThread(VOID)
{
    CHECK(Owner&&!Owner->CallbackThread);Owner->CallbackThreadId=GetCurrentThreadId();
    Owner->CallbackThread=OpenThread(SYNCHRONIZE,FALSE,Owner->CallbackThreadId);CHECK(Owner->CallbackThread);
}
static VOID ObserveOperationRelease(MAIN_OPERATION *job)
{
    CHECK(Owner&&job&&job->State==OperationAbandoned&&job->References==1&&!RetiringJob);
    /* Exact actual worker tail, after Dispose and before its actual release.
     * The wrapper retains no new reference and edits no runtime/job state. */
    RetiringJob=job;CHECK(SetEvent(Owner->Events[OPA_RETIRING_ENTERED]));
    CHECK(WaitForSingleObject(Owner->Events[OPA_RETIRING_GATE],10000)==WAIT_OBJECT_0);RetiringJob=NULL;MainRuntimeRelease();
}
BOOL AdapterRemoveInstance(HDEVINFO info,SP_DEVINFO_DATA *data)
{(void)info;(void)data;InterlockedIncrement(&Unexpected);CHECK(!Unexpected);return FALSE;}
static VOID WINAPI NoSwClose(VOID *device)
{(void)device;InterlockedIncrement(&Unexpected);CHECK(!Unexpected);}
#define SwDeviceClose NoSwClose
#define SetupDiDestroyDeviceInfoList TestDestroy
#include PB_OPERATION_ADAPTER_CLOSE_SOURCE
#undef SetupDiDestroyDeviceInfoList
#undef SwDeviceClose
static WINTUN_ADAPTER_HANDLE CreateAdapterInternal(LPCWSTR name,LPCWSTR tunnel,const GUID *guid)
{(void)name;(void)tunnel;(void)guid;InterlockedIncrement(&Unexpected);CHECK(!Unexpected);return NULL;}
#include PB_OPERATION_ADAPTER_PUBLIC_SOURCE
__declspec(dllexport) VOID WINAPI ConfigureOperationAdapter(OPA_CONFIG *config)
{CHECK(config&&!Owner);Owner=config;Detached=config->Events[OPA_DETACHED];Begin();}
__declspec(dllexport) VOID *WINAPI OpenOperationAdapter(VOID)
{CHECK(Owner);return WintunOpenAdapter(L"wanted");}
__declspec(dllexport) BOOL WINAPI StopOperationAdapter(DWORD timeout)
{ULONGLONG deadline=GetTickCount64()+timeout;MainRuntimeBeginShutdown();return MainOperationShutdown(MainRemaining(deadline,timeout))&&MainRuntimeShutdown(MainRemaining(deadline,timeout));}
__declspec(dllexport) BOOL WINAPI WaitOperationAdapterRuntime(DWORD timeout)
{return MainRuntimeShutdown(timeout);}
__declspec(dllexport) VOID WINAPI ReadOperationAdapter(OPA_STATE *state)
{
    CHECK(state);memset(state,0,sizeof(*state));AcquireSRWLockShared(&DependencyLock);state->Users=DependencyUsers;state->Unloading=DependencyUnloading;state->Stopping=RuntimeStopping;ReleaseSRWLockShared(&DependencyLock);
    AcquireSRWLockShared(&OperationLock);state->Jobs=OperationCount;ReleaseSRWLockShared(&OperationLock);
    state->NativeBlocks=Live;state->Closes=Closes;state->DeviceSet=DeviceSet!=NULL;state->InstallationMutex=InstallMutex!=NULL;state->RuntimeOwned=LiveAdapter&&LiveAdapter->RuntimeOwned;
    state->SetsCreated=SetsCreated;state->SetsClosed=SetsClosed;state->MutexesCreated=MutexesCreated;state->MutexesClosed=MutexesClosed;state->NativeAllocations=LegacyAllocations;state->NativeFrees=LegacyFrees;
    state->CoreHandles=Handles;state->CoreAllocations=Allocations;state->CoreWorks=Works;state->CorePins=Pins;state->CorePools=Pools;
    state->Retiring=RetiringJob!=NULL;if(RetiringJob){AcquireSRWLockShared(&RetiringJob->Lock);state->Abandoned=RetiringJob->State==OperationAbandoned;state->JobReferences=RetiringJob->References;ReleaseSRWLockShared(&RetiringJob->Lock);}
    state->Compensations=Compensations;state->UnloadCalls=DependencyUnloads;state->LoadedDependencies=DependenciesLoaded();
}
__declspec(dllexport) VOID WINAPI RepairOperationAdapter(VOID)
{
    CHECK(RetiringJob&&RetiringJob->State==OperationAbandoned&&RetiringJob->References==1&&OperationCount==1&&DependencyUsers==2&&Live==2&&LiveAdapter&&LiveAdapter->RuntimeOwned);
    CHECK(!InstallMutex&&!DeviceSet&&InterlockedIncrement(&Compensations)==1);WintunCloseAdapter(LiveAdapter);
    CHECK(!LiveAdapter&&!Live&&DependencyUsers==1&&Closes==1);
}
static VOID OperationAdapterFinalCheck(VOID)
{
    CHECK(!Unexpected&&!RetiringJob&&!Live&&!DeviceSet&&!InstallMutex&&!LiveAdapter&&Closes==1&&SetsCreated==1&&SetsClosed==1&&MutexesCreated==1&&MutexesClosed==1&&LegacyAllocations==LegacyFrees);
    CHECK(!DependencyUsers&&!DependencyUnloading&&DependencyUnloads>0&&!DependenciesLoaded());
}
