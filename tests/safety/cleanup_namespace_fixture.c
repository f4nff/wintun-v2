/* SPDX-License-Identifier: GPL-2.0 */
/* Actual main/namespace/scheduler and actual CleanupRun/Shutdown. Only namespace
 * object naming/installation scans are hosted; no real namespace is created. */
#ifdef PB_API_TESTS
#error Cleanup/namespace cannot use PB_API_TESTS
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <delayimp.h>
#include <stdio.h>
#include "../model/cleanup_namespace_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (GLE %lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static CN_CONFIG *Host;
static volatile LONG NamespaceOwners,BoundaryOwners,MutexOwners,PairEvents,Pins,Callbacks,Returned,TakeError,LegacyCalls,OrphanCalls,UnloadCalls,MutexCreates,Unexpected,ReleaseHeld,Compensations;
static HANDLE LiveBoundary,LiveSpace;
static BOOL WINAPI ObserveUnload(LPCSTR name)
{BOOL unloaded=__FUnloadDelayLoadedDLL2(name);if(unloaded)InterlockedIncrement(&UnloadCalls);return unloaded;}
#define __FUnloadDelayLoadedDLL2 ObserveUnload
#define DllMain ActualMainDllMain
#include PB_CLEANUP_NAMESPACE_MAIN_SOURCE
#undef DllMain
#undef __FUnloadDelayLoadedDLL2
static DWORD DependenciesLoaded(VOID)
{
    const BYTE *base=(const BYTE *)ResourceModule;const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt=(const IMAGE_NT_HEADERS *)(base+dos->e_lfanew);DWORD offset=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress,count=0;
    if(offset)for(const ImgDelayDescr *d=(const ImgDelayDescr *)(base+offset);d->rvaDLLName;++d)if(*(HMODULE *)(base+d->rvaHmod))++count;return count;
}
static HANDLE WINAPI TrackedBoundary(LPCWSTR name,ULONG flags)
{CHECK(!BoundaryOwners);HANDLE value=CreateBoundaryDescriptorW(name,flags);CHECK(value);LiveBoundary=value;InterlockedIncrement(&BoundaryOwners);return value;}
static BOOL WINAPI TrackedAddSid(HANDLE *boundary,PSID sid)
{
    CHECK(*boundary==LiveBoundary&&BoundaryOwners==1);BOOL okay=AddSIDToBoundaryDescriptor(boundary,sid);CHECK(okay);
    /* The SDK may replace its allocation: the latest pointer owns deletion. */
    LiveBoundary=*boundary;return okay;
}
static VOID WINAPI TrackedDeleteBoundary(HANDLE boundary)
{CHECK(boundary==LiveBoundary&&BoundaryOwners==1);DeleteBoundaryDescriptor(boundary);LiveBoundary=NULL;InterlockedDecrement(&BoundaryOwners);}
static HANDLE WINAPI HostedCreateNamespace(LPSECURITY_ATTRIBUTES security,VOID *boundary,LPCWSTR name)
{
    CHECK(Host&&security==&SecurityAttributes&&boundary==LiveBoundary&&!wcscmp(name,L"PacketBridgeV2")&&!NamespaceOwners);
    LiveSpace=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(LiveSpace);InterlockedIncrement(&NamespaceOwners);
    CHECK(SetEvent(Host->Events[CN_CREATE_ENTERED])&&WaitForSingleObject(Host->Events[CN_CREATE_GATE],10000)==WAIT_OBJECT_0);return LiveSpace;
}
static HANDLE WINAPI HostedOpenNamespace(VOID *boundary,LPCWSTR name)
{(void)boundary;(void)name;InterlockedIncrement(&Unexpected);CHECK(!Unexpected);return NULL;}
static BOOLEAN WINAPI HostedCloseNamespace(HANDLE space,ULONG flags)
{CHECK(space==LiveSpace&&NamespaceOwners==1&&!flags&&CloseHandle(space));LiveSpace=NULL;InterlockedDecrement(&NamespaceOwners);return TRUE;}
static HANDLE WINAPI HostedNamespaceMutex(LPSECURITY_ATTRIBUTES security,BOOL owner,LPCWSTR name)
{
    CHECK(security==&SecurityAttributes&&!owner&&name&&NamespaceOwners==1);HANDLE mutex=CreateMutexW(NULL,FALSE,NULL);CHECK(mutex);
    InterlockedIncrement(&MutexOwners);InterlockedIncrement(&MutexCreates);return mutex;
}
static BOOL WINAPI CloseNamespaceMutex(HANDLE mutex)
{CHECK(MutexOwners>0&&CloseHandle(mutex));InterlockedDecrement(&MutexOwners);return TRUE;}
#define CreateBoundaryDescriptorW TrackedBoundary
#define AddSIDToBoundaryDescriptor TrackedAddSid
#define DeleteBoundaryDescriptor TrackedDeleteBoundary
#define CreatePrivateNamespaceW HostedCreateNamespace
#define OpenPrivateNamespaceW HostedOpenNamespace
#define ClosePrivateNamespace HostedCloseNamespace
#define CreateMutexW HostedNamespaceMutex
#define CloseHandle CloseNamespaceMutex
#include PB_CLEANUP_NAMESPACE_SOURCE
#undef CreateBoundaryDescriptorW
#undef AddSIDToBoundaryDescriptor
#undef DeleteBoundaryDescriptor
#undef CreatePrivateNamespaceW
#undef OpenPrivateNamespaceW
#undef ClosePrivateNamespace
#undef CreateMutexW
#undef CloseHandle
static PTP_SIMPLE_CALLBACK ActualCleanup;
static VOID CALLBACK ObservedCallback(PTP_CALLBACK_INSTANCE instance,VOID *context)
{
    CHECK(InterlockedIncrement(&Callbacks)==1);ActualCleanup(instance,context);InterlockedIncrement(&Returned);
    CHECK(SetEvent(Host->Events[CN_CALLBACK_RETURNED]));
}
static BOOL WINAPI ObservedSubmit(PTP_SIMPLE_CALLBACK callback,VOID *context,PTP_CALLBACK_ENVIRON environment)
{CHECK(!ActualCleanup&&!environment);ActualCleanup=callback;return TrySubmitThreadpoolCallback(ObservedCallback,context,environment);}
static BOOL WINAPI TrackedPin(DWORD flags,LPCWSTR address,HMODULE *module)
{CHECK(GetModuleHandleExW(flags,address,module));InterlockedIncrement(&Pins);return TRUE;}
static BOOL WINAPI TrackedUnpin(HMODULE module)
{CHECK(Pins==1);InterlockedDecrement(&Pins);return FreeLibrary(module);}
static VOID WINAPI TrackedTail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{CHECK(Pins==1);InterlockedDecrement(&Pins);FreeLibraryWhenCallbackReturns(instance,module);}
static HANDLE WINAPI TrackedPairEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL signaled,LPCWSTR name)
{HANDLE event=CreateEventW(security,manual,signaled,name);CHECK(event);InterlockedIncrement(&PairEvents);return event;}
static BOOL WINAPI TrackedPairClose(HANDLE event)
{CHECK(PairEvents>0&&CloseHandle(event));InterlockedDecrement(&PairEvents);return TRUE;}
#define TrySubmitThreadpoolCallback ObservedSubmit
#define GetModuleHandleExW TrackedPin
#define FreeLibrary TrackedUnpin
#define FreeLibraryWhenCallbackReturns TrackedTail
#define CreateEventW TrackedPairEvent
#define CloseHandle TrackedPairClose
#include PB_CLEANUP_NAMESPACE_SCHEDULER_SOURCE
#undef TrySubmitThreadpoolCallback
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef CreateEventW
#undef CloseHandle
static HANDLE ObserveTake(HANDLE stop)
{HANDLE mutex=NamespaceTakeDeviceInstallationMutexCancelable(stop);DWORD error=GetLastError();if(!mutex)InterlockedExchange(&TakeError,(LONG)error);SetLastError(error);return mutex;}
static VOID ObserveCleanupRelease(VOID)
{
    DWORD error=GetLastError();
    if(Host->Cancel){InterlockedExchange(&ReleaseHeld,1);CHECK(SetEvent(Host->Events[CN_RELEASE_ENTERED])&&WaitForSingleObject(Host->Events[CN_RELEASE_GATE],10000)==WAIT_OBJECT_0);InterlockedExchange(&ReleaseHeld,0);}
    SetLastError(error);MainRuntimeRelease();
}
BOOL AdapterCleanupLegacyDevices(VOID)
{InterlockedIncrement(&LegacyCalls);return TRUE;}
VOID AdapterCleanupOrphanedDevices(VOID)
{InterlockedIncrement(&OrphanCalls);HANDLE mutex=NamespaceTakeDeviceInstallationMutexCancelable(CleanupStop);CHECK(mutex);NamespaceReleaseMutex(mutex);}
#define NamespaceTakeDeviceInstallationMutexCancelable ObserveTake
#define MainRuntimeRelease ObserveCleanupRelease
#include PB_CLEANUP_NAMESPACE_RUN_SOURCE
#undef MainRuntimeRelease
#undef NamespaceTakeDeviceInstallationMutexCancelable
VOID AdapterCleanupDetach(VOID){CleanupDetach();}
VOID Rundll32Detach(VOID){}
VOID Rundll32RequestShutdown(VOID){}
BOOL Rundll32Shutdown(DWORD timeout){(void)timeout;return TRUE;}
#include PB_CLEANUP_NAMESPACE_SHUTDOWN_SOURCE
__declspec(dllexport) VOID WINAPI ConfigureCleanupNamespace(CN_CONFIG *config)
{CHECK(!Host&&config);Host=config;}
__declspec(dllexport) BOOL WINAPI QueueCleanupNamespace(VOID){return CleanupRequest();}
__declspec(dllexport) BOOL WINAPI ShutdownCleanupNamespace(DWORD timeout){return WintunShutdown(timeout);}
__declspec(dllexport) VOID WINAPI RepairCleanupNamespace(VOID)
{
    CHECK(Host->Cancel&&ReleaseHeld&&DependencyUsers==1&&!PrivateNamespace&&!BoundaryDescriptor&&!NamespaceOwners&&BoundaryOwners==1&&LiveBoundary&&TakeError==ERROR_CANCELLED);
    CHECK(InterlockedIncrement(&Compensations)==1&&TryEnterCriticalSection(&Initializing));LeaveCriticalSection(&Initializing);
    /* Only observer128: an actual final SDK pointer, while loader ownership
     * and the callback's real runtime lease still protect every global. */
    TrackedDeleteBoundary(LiveBoundary);CHECK(!BoundaryOwners&&!LiveBoundary);
}
__declspec(dllexport) VOID WINAPI ReadCleanupNamespace(CN_STATE *out)
{
    CHECK(out);memset(out,0,sizeof(*out));AcquireSRWLockShared(&DependencyLock);out->Users=DependencyUsers;out->Unloading=DependencyUnloading;out->Stopping=RuntimeStopping;ReleaseSRWLockShared(&DependencyLock);
    AcquireSRWLockShared(&CleanupLock);out->Scheduled=CleanupScheduled;out->Requested=CleanupRequested;out->CleanupStopping=CleanupStopping;ReleaseSRWLockShared(&CleanupLock);
    out->NamespaceCached=PrivateNamespace!=NULL;out->BoundaryCached=BoundaryDescriptor!=NULL;out->NamespaceOwners=NamespaceOwners;out->BoundaryOwners=BoundaryOwners;out->MutexOwners=MutexOwners;out->PairEvents=PairEvents;out->Pins=Pins;
    out->Callbacks=Callbacks;out->Returned=Returned;out->TakeError=TakeError;out->LegacyCalls=LegacyCalls;out->OrphanCalls=OrphanCalls;out->UnloadCalls=UnloadCalls;out->MutexCreates=MutexCreates;out->ReleaseHeld=ReleaseHeld;out->Compensations=Compensations;
    out->CSAvailable=TryEnterCriticalSection(&Initializing);if(out->CSAvailable)LeaveCriticalSection(&Initializing);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,VOID *reserved)
{
    if(reason==DLL_PROCESS_DETACH&&!reserved){
        CHECK(Host&&!CleanupScheduled&&!CleanupRequested&&!DependencyUsers&&!DependencyUnloading&&!Pins&&Callbacks==1&&Returned==1&&!MutexOwners&&PairEvents==2&&UnloadCalls>0&&!ReleaseHeld&&!Unexpected&&!DependenciesLoaded());
        if(Host->Cancel)CHECK(!PrivateNamespace&&!BoundaryDescriptor&&!NamespaceOwners&&!BoundaryOwners&&TakeError==ERROR_CANCELLED&&!LegacyCalls&&!OrphanCalls&&!MutexCreates);
        else CHECK(PrivateNamespace&&BoundaryDescriptor&&NamespaceOwners==1&&BoundaryOwners==1&&!TakeError&&LegacyCalls==1&&OrphanCalls==1&&MutexCreates==2&&!Compensations);
    }
    BOOL okay=ActualMainDllMain(module,reason,reserved);
    if(reason==DLL_PROCESS_DETACH&&!reserved){CHECK(!NamespaceOwners&&!BoundaryOwners&&!MutexOwners&&!PairEvents&&!Pins&&!PrivateNamespace&&!BoundaryDescriptor);CHECK(SetEvent(Host->Events[CN_DETACHED]));}
    return okay;
}
