/* SPDX-License-Identifier: GPL-2.0 */
/* Cold production initialization with real events and deterministic last-error
 * clobbering. No package installation, device mutation or child submission. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
static DWORD EventFailureError=ERROR_NOT_ENOUGH_MEMORY, EventSuccessError;
static LONG EventCalls, EventSignals;
static HANDLE ObserveEvent(HANDLE handle)
{
    LONG index=InterlockedIncrement(&EventCalls)-1;
    if(handle&&WaitForSingleObject(handle,0)==WAIT_OBJECT_0)InterlockedOr(&EventSignals,1L<<index);
    SetLastError(handle?EventSuccessError:EventFailureError);return handle;
}
#define PB_FIXTURE_EVENT_RESULT(handle) ObserveEvent(handle)
#include "legacy_runtime_fixture.c"
#define CreateEventW TestEvent
#define CloseHandle TestClose
#ifndef PB_CLEANUP_SOURCE
#define PB_CLEANUP_SOURCE "../../api/cleanup.h"
#endif
#include PB_CLEANUP_SOURCE
#undef CreateEventW
#undef CloseHandle
static VOID CleanupRun(VOID) { ExitProcess(80); } /* Never submitted by this fixture. */
__declspec(dllexport) VOID WINAPI InitConfigure(LONG failure,DWORD failureError,DWORD successError)
{SetFailure(failure);EventCalls=EventSignals=0;EventFailureError=failureError;EventSuccessError=successError;}
__declspec(dllexport) BOOL WINAPI InitProbe(DWORD target)
{
    if(target==0) {BOOL okay=HelperReserve();DWORD error=GetLastError();if(okay)HelperUnreserve(NULL);SetLastError(error);return okay;}
    if(target==1)return CleanupRequest();
    if(target==2)return CleanupShutdown(0);
    AcquireSRWLockExclusive(&CleanupLock);BOOL okay=CleanupInitializeLocked();DWORD error=GetLastError();
    ReleaseSRWLockExclusive(&CleanupLock);SetLastError(error);return okay;
}
__declspec(dllexport) DWORD WINAPI InitState(VOID)
{
    DWORD state=(HelperIdle?1u:0u)|(HelperStop?2u:0u)|(CleanupIdle?4u:0u)|(CleanupStop?8u:0u);
    return state;
}
__declspec(dllexport) DWORD WINAPI InitOwned(VOID)
{return (DWORD)(OwnedHandles+OwnedAllocations+OwnedWorks);}
__declspec(dllexport) DWORD WINAPI InitSignals(VOID) {return (DWORD)EventSignals;}
__declspec(dllexport) VOID WINAPI InitReset(VOID)
{
    assert(!HelperCount&&!HelperJobs&&!CleanupScheduled&&!OwnedAllocations&&!OwnedWorks);
    CleanupDetach();CleanupIdle=CleanupStop=NULL;CleanupRequested=CleanupStopping=FALSE;
    Rundll32Detach();HelperStopping=FALSE;
    assert(!OwnedHandles);SetFailure(0);
}
__declspec(dllexport) BOOL WINAPI InitReserveHeld(HANDLE entered,HANDLE gate,LONG *arrived)
{
    BOOL okay=HelperReserve();DWORD error=GetLastError();
    if(InterlockedIncrement(arrived)==8)SetEvent(entered);
    if(WaitForSingleObject(gate,5000)!=WAIT_OBJECT_0)ExitProcess(81);
    if(okay)HelperUnreserve(NULL);SetLastError(error);return okay;
}
