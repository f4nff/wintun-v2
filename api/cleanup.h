/* SPDX-License-Identifier: GPL-2.0 */
/* Included once by adapter.c; the fixture DLL uses this same scheduler.
 * A queued/running job owns a loader reference, including its return tail.
 * No shutdown wait is performed under the loader lock. */
#pragma once
#include <Windows.h>
static const BYTE CleanupModuleAnchor = 0;
static SRWLOCK CleanupLock = SRWLOCK_INIT;
static HANDLE CleanupIdle, CleanupStop;
static BOOL CleanupScheduled, CleanupRequested, CleanupStopping;
static VOID CleanupRun(VOID);

static BOOL CleanupInitializeLocked(VOID)
{
    if (CleanupIdle) return TRUE;
    HANDLE idle = CreateEventW(NULL, TRUE, TRUE, NULL);
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!idle || !stop) {
        DWORD error = GetLastError();
        if (idle) CloseHandle(idle);
        if (stop) CloseHandle(stop);
        SetLastError(error); return FALSE;
    }
    CleanupIdle = idle; CleanupStop = stop; return TRUE;
}
static VOID CALLBACK CleanupCallback(PTP_CALLBACK_INSTANCE instance, PVOID context)
{
    FreeLibraryWhenCallbackReturns(instance, (HMODULE)context);
    CallbackMayRunLong(instance);
    for (;;) {
        AcquireSRWLockExclusive(&CleanupLock);
        if (CleanupStopping || !CleanupRequested) {
            CleanupScheduled = FALSE; SetEvent(CleanupIdle);
            ReleaseSRWLockExclusive(&CleanupLock); return;
        }
        CleanupRequested = FALSE;
        ReleaseSRWLockExclusive(&CleanupLock);
        CleanupRun();
    }
}
static BOOL CleanupRequest(VOID)
{
    DWORD error = ERROR_SUCCESS; HMODULE module = NULL;
    AcquireSRWLockExclusive(&CleanupLock);
    if (CleanupStopping) { error = ERROR_SHUTDOWN_IN_PROGRESS; goto done; }
    if (!CleanupInitializeLocked()) { error = GetLastError(); goto done; }
    CleanupRequested = TRUE;
    if (CleanupScheduled) goto done;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&CleanupModuleAnchor, &module)) {
        error = GetLastError(); CleanupRequested = FALSE; goto done;
    }
    CleanupScheduled = TRUE; ResetEvent(CleanupIdle);
    if (!TrySubmitThreadpoolCallback(CleanupCallback, module, NULL)) {
        error = GetLastError(); CleanupScheduled = CleanupRequested = FALSE; SetEvent(CleanupIdle);
    } else module = NULL; /* Callback now owns this exact reference. */
done:
    ReleaseSRWLockExclusive(&CleanupLock);
    if (module) FreeLibrary(module); /* The caller still owns its ordinary DLL reference. */
    SetLastError(error); return error == ERROR_SUCCESS;
}
static BOOL CleanupShutdown(DWORD timeout)
{
    AcquireSRWLockExclusive(&CleanupLock);
    if (!CleanupInitializeLocked()) { ReleaseSRWLockExclusive(&CleanupLock); return FALSE; }
    CleanupStopping = TRUE; CleanupRequested = FALSE; SetEvent(CleanupStop);
    HANDLE idle = CleanupIdle;
    ReleaseSRWLockExclusive(&CleanupLock);
    DWORD result = WaitForSingleObject(idle, timeout);
    if (result == WAIT_OBJECT_0) return TRUE;
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return FALSE;
}
/* Called only at detach, when loader references prove there are no live callbacks. */
static VOID CleanupDetach(VOID)
{
    if (CleanupIdle) CloseHandle(CleanupIdle);
    if (CleanupStop) CloseHandle(CleanupStop);
}
