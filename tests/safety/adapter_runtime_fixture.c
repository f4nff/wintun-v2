/* SPDX-License-Identifier: GPL-2.0 */
/* Shipping unified adapter and runtime, actual discovery lease and Close.
 * Creation is hosted by real OpenAdapterInternal discovery, not device install.
 * No counter/Retain substitute is used for the runtime lease. */
#ifdef PB_API_TESTS
#error Adapter/runtime tests must execute the shipping native session
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <delayimp.h>
#include <stdio.h>
#include <stdlib.h>
#include "../model/adapter_runtime_fixture.h"
#include "../../api/ntdll.h"
#include "../../api/main.h"
static AR_CONFIG *OwnerConfig;
static volatile LONG UnloadArmed,UnloadHeld,UnloadCalls,ShutdownThreadId,UnexpectedCalls,Repairs;
/* A forward declaration of the actual main.c object lets reused host assertions
 * inspect it without inventing a second lease count. main.c defines it below. */
static ULONG DependencyUsers;
#define Leases DependencyUsers
#define PB_ADAPTER_OPEN_SOURCE PB_ADAPTER_RUNTIME_OPEN_SOURCE
#define main UnusedOpenHostMain
#include PB_ADAPTER_RUNTIME_OPEN_HOST
#undef main
static BOOL WINAPI ObserveUnload(LPCSTR name)
{
    if(InterlockedCompareExchange(&UnloadArmed,0,1)){
        InterlockedExchange(&UnloadHeld,1);CHECK(SetEvent(OwnerConfig->Events[AR_UNLOAD_ENTERED]));
        CHECK(WaitForSingleObject(OwnerConfig->Events[AR_UNLOAD_GATE],10000)==WAIT_OBJECT_0);InterlockedExchange(&UnloadHeld,0);
    }
    InterlockedIncrement(&UnloadCalls);BOOL unloaded=__FUnloadDelayLoadedDLL2(name);CHECK(unloaded);return unloaded;
}
static BOOL WINAPI ObserveCv(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{if(GetCurrentThreadId()==(DWORD)ShutdownThreadId)CHECK(SetEvent(OwnerConfig->Events[AR_SHUTDOWN_ENTERED]));return SleepConditionVariableSRW(cv,lock,timeout,flags);}
#define __FUnloadDelayLoadedDLL2 ObserveUnload
#define SleepConditionVariableSRW ObserveCv
#define DllMain ActualMainDllMain
#include PB_ADAPTER_RUNTIME_MAIN_SOURCE
#undef DllMain
#undef __FUnloadDelayLoadedDLL2
#undef SleepConditionVariableSRW
/* No namespace/installer/cleanup jobs are needed to discover the hosted set.
 * The reused discovery host supplies a real unnamed installation mutex. */
VOID NamespaceInit(VOID){}
VOID NamespaceDone(VOID){}
VOID AdapterCleanupDetach(VOID){}
VOID Rundll32Detach(VOID){}
VOID Rundll32RequestShutdown(VOID){}
BOOL Rundll32Shutdown(DWORD timeout){(void)timeout;return TRUE;}
BOOL AdapterRemoveInstance(HDEVINFO info,SP_DEVINFO_DATA *data)
{(void)info;(void)data;InterlockedIncrement(&UnexpectedCalls);CHECK(!UnexpectedCalls);return FALSE;}
static VOID WINAPI NoSwClose(VOID *device){(void)device;InterlockedIncrement(&UnexpectedCalls);CHECK(!UnexpectedCalls);}
#define SwDeviceClose NoSwClose
#define SetupDiDestroyDeviceInfoList TestDestroy
#include PB_ADAPTER_RUNTIME_CLOSE_SOURCE
#undef SwDeviceClose
#undef SetupDiDestroyDeviceInfoList
VOID WINAPI WintunGetAdapterLUID(WINTUN_ADAPTER *adapter,NET_LUID *luid)
{(void)adapter;(void)luid;InterlockedIncrement(&UnexpectedCalls);CHECK(!UnexpectedCalls);}
WINTUN_ADAPTER_HANDLE WINAPI WintunCreateAdapter(LPCWSTR name,LPCWSTR type,const GUID *requested)
{CHECK(!wcscmp(name,L"wanted")&&!wcscmp(type,L"PacketBridge")&&requested);return OpenAdapterInternal(name);}
static BOOL UnpublishedCleanup=TRUE;
static VOID *PendingCore;
static VOID *CombinedCalloc(SIZE_T,SIZE_T);
static HANDLE WINAPI CombinedFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL WINAPI CombinedClose(HANDLE);
static BOOL WINAPI CombinedIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
#define PB_NATIVE_POLICY_SESSION_SOURCE PB_ADAPTER_RUNTIME_SESSION_ENTRY
#define DllMain NativeLedgerDllMain
#undef CHECK
#include PB_ADAPTER_RUNTIME_NATIVE_HOST
#undef DllMain
static LONG UnifiedAllocations;
static VOID *UnifiedCalloc(SIZE_T n,SIZE_T bytes){VOID *p=calloc(n,bytes);CHECK(p);++UnifiedAllocations;return p;}
static VOID UnifiedFree(VOID *p){CHECK(p&&--UnifiedAllocations==0);free(p);}
#define calloc UnifiedCalloc
#define free UnifiedFree
#include "../../api/v2/adapter_unified.c"
#undef calloc
#undef free
static VOID *CombinedCalloc(SIZE_T n,SIZE_T bytes)
{VOID *p=TrackedCalloc(n,bytes);if(!PendingCore&&n==1)PendingCore=p;return p;}
static HANDLE WINAPI CombinedFile(LPCWSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sec,DWORD d,DWORD f,HANDLE t)
{CHECK(PendingCore&&!Current);Current=PendingCore;Progress=Current->ProgressEvent;HANDLE h=HostedFile(p,a,s,sec,d,f,t);CHECK(SetHandleInformation(h,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE));return h;}
static BOOL WINAPI CombinedClose(HANDLE h)
{
    if(h==Device){DWORD flags;CHECK(GetHandleInformation(h,&flags));if(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE){BOOL closed=CloseHandle(h);DWORD error=GetLastError();CHECK(!closed&&error);SetLastError(error);return FALSE;}}
    return TrackedClose(h);
}
static BOOL WINAPI CombinedIo(HANDLE h,DWORD code,VOID *input,DWORD inSize,VOID *output,DWORD outSize,DWORD *returned,LPOVERLAPPED overlap)
{
    if((code==PB_PROBE_IOCTL_QUERY&&OwnerConfig->Failure==AR_FAIL_PROBE)||(code==PB_V2_IOCTL_REGISTER&&OwnerConfig->Failure==AR_FAIL_REGISTER)){
        CHECK(h==Device&&returned&&!overlap);*returned=0;SetLastError(code==PB_PROBE_IOCTL_QUERY?ERROR_NOT_READY:ERROR_INVALID_DATA);return FALSE;
    }
    return HostedIo(h,code,input,inSize,output,outSize,returned,overlap);
}
static DWORD DependenciesLoaded(VOID)
{
    const BYTE *base=(const BYTE *)ResourceModule;const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt=(const IMAGE_NT_HEADERS *)(base+dos->e_lfanew);DWORD offset=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress,count=0;
    if(offset)for(const ImgDelayDescr *d=(const ImgDelayDescr *)(base+offset);d->rvaDLLName;++d)if(*(HMODULE *)(base+d->rvaHmod))++count;return count;
}
__declspec(dllexport) VOID *WINAPI CreateCombined(AR_CONFIG *config)
{
    CHECK(config&&!OwnerConfig&&!DependencyUsers&&!RuntimeReady&&!PendingCore&&config->Failure>=AR_FAIL_PROBE&&config->Failure<=AR_FAIL_POLICY);
    OwnerConfig=config;Begin();Initialize(&config->Native);if(config->Failure!=AR_FAIL_POLICY)InterlockedExchange(&NativeActive,0);
    PB_ADAPTER *adapter=NULL;CHECK(PbAdapterCreate(L"wanted",L"runtime-owner",&adapter)==PB_OK&&adapter&&DependencyUsers==1&&adapter->Native->RuntimeOwned);return adapter;
}
__declspec(dllexport) VOID WINAPI StartCombinedFailure(VOID *opaque)
{
    PB_ADAPTER *adapter=opaque;PB_CONFIG config={sizeof(config),1,4,4,PB_TX_IP4_CHECKSUM,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS};PB_SESSION *session=(PB_SESSION *)(ULONG_PTR)1;
    DWORD expected=OwnerConfig->Failure==AR_FAIL_PROBE?ERROR_NOT_READY:OwnerConfig->Failure==AR_FAIL_REGISTER?ERROR_INVALID_DATA:ERROR_NOT_SUPPORTED;
    CHECK(StartAdapterSession(adapter,&config,&session)==PB_UNSUPPORTED&&!session&&GetLastError()==expected);
}
__declspec(dllexport) VOID WINAPI CloseCombinedPublic(VOID *adapter){PbAdapterRelease(adapter);}
__declspec(dllexport) BOOL WINAPI StopCombined(VOID){MainRuntimeBeginShutdown();UnloadArmed=1;return MainRuntimeShutdown(0);}
__declspec(dllexport) BOOL WINAPI WaitCombinedShutdown(VOID)
{InterlockedExchange(&ShutdownThreadId,(LONG)GetCurrentThreadId());return MainRuntimeShutdown(5000);}
__declspec(dllexport) VOID WINAPI ReadCombined(AR_STATE *state)
{
    CHECK(state);memset(state,0,sizeof(*state));AcquireSRWLockShared(&DependencyLock);state->Users=DependencyUsers;state->Unloading=DependencyUnloading;state->Stopping=RuntimeStopping;ReleaseSRWLockShared(&DependencyLock);
    state->Wrappers=UnifiedAllocations;state->LegacyCloses=Closes;state->LegacyBlocks=Live;state->NativeSessions=RuntimeCount;state->NativePins=Pins;state->NativeMaps=Regions;state->NativeClosed=DeviceCloses;state->Parked=ParkHeld;state->UnloadingHeld=UnloadHeld;state->UnloadCalls=UnloadCalls;state->LoadedDependencies=DependenciesLoaded();
    state->Core=Current!=NULL;state->ActiveCallbacks=ActiveCallbacks;state->Callbacks=CallbackCount;state->Finished=Finished;state->Repairs=Repairs;state->Bytes=RuntimeBytes;
    if(Current){CHECK(ParkHeld||FinalHeld||UnloadHeld);AcquireSRWLockShared(&Current->State);state->Phase=Current->Progress.Phase;state->ResourcesClosed=Current->ResourcesClosed;PB_ADAPTER *a=Current->Adapter;
        /* Unload is inside WintunCloseAdapter after freeing Native. Its stale
         * adapter->Native field must never be inspected during that gate. */
        if(a&&!UnloadHeld){state->WrapperReferences=(ULONG)ReadAcquire(&a->References);state->RuntimeOwned=a->Native->RuntimeOwned;}ReleaseSRWLockShared(&Current->State);
    }
}
__declspec(dllexport) VOID WINAPI RecoverCombined(VOID)
{CHECK(ParkHeld&&Current&&Device&&SetHandleInformation(Device,HANDLE_FLAG_PROTECT_FROM_CLOSE,0));SetNativeActive(0);}
__declspec(dllexport) VOID WINAPI CompensateCombined(VOID)
{
    CHECK(!Current&&!UnifiedAllocations&&!Live&&!RuntimeCount&&!RuntimeBytes&&!Device&&!Allocations&&!Regions&&!Events&&!Works&&!Pools&&!Pins&&DependencyUsers==1&&!DependencyUnloading);
    CHECK(InterlockedIncrement(&Repairs)==1);MainRuntimeRelease();
}
__declspec(dllexport) VOID WINAPI FinishCombined(VOID)
{
    CHECK(!Current&&!UnifiedAllocations&&!Live&&!DependencyUsers&&!DependencyUnloading&&!RuntimeCount&&!RuntimeBytes&&!Device&&!Allocations&&!Regions&&!Events&&!Works&&!Pools&&!Pins&&!UnexpectedCalls&&!DependenciesLoaded());
    CHECK(!DeviceSet&&!InstallMutex&&!LiveAdapter&&LegacyAllocations==LegacyFrees&&SetsCreated==SetsClosed&&MutexesCreated==MutexesClosed);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,VOID *reserved)
{if(reason==DLL_PROCESS_DETACH&&!reserved)CHECK(!Live&&!DependencyUsers&&!DependencyUnloading&&!UnifiedAllocations);BOOL result=NativeLedgerDllMain(module,reason,reserved);CHECK(ActualMainDllMain(module,reason,reserved));return result;}
