/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "logger.h"
#include "main.h"
#include "namespace.h"

#include <Windows.h>
#include <winternl.h>
#include <winefs.h>
#include <wchar.h>
#include <stdlib.h>

static HANDLE PrivateNamespace = NULL;
static HANDLE BoundaryDescriptor = NULL;
static CRITICAL_SECTION Initializing;

#define NAMESPACE_INIT_TIMEOUT_MS 15000u
#define NAMESPACE_RETRY_DELAY_MS 50u

static DWORD NamespaceInitRemaining(ULONGLONG Started)
{
    ULONGLONG Elapsed = GetTickCount64() - Started;
    return Elapsed >= NAMESPACE_INIT_TIMEOUT_MS ? 0 : NAMESPACE_INIT_TIMEOUT_MS - (DWORD)Elapsed;
}

/* MainOperationSleep observes the enclosing operation/runtime deadline. The
 * optional cleanup stop must also interrupt initialization, before mutex wait.
 * A timeout here is the requested delay, not a completion/ownership witness. */
static BOOL NamespaceInitPause(HANDLE Stop, DWORD Delay)
{
    if (!Stop) return MainOperationSleep(Delay);
    DWORD Error = GetLastError();
    if (!MainOperationSleep(0)) return FALSE;
    if (MainWaitForObject(Stop, NULL, Delay)) { SetLastError(ERROR_CANCELLED); return FALSE; }
    DWORD WaitError = GetLastError();
    if (WaitError != ERROR_TIMEOUT) { SetLastError(WaitError ? WaitError : ERROR_GEN_FAILURE); return FALSE; }
    if (!MainOperationSleep(0)) return FALSE;
    SetLastError(Error); return TRUE;
}

static _Return_type_success_(return != FALSE)
BOOL NamespaceRuntimeInit(HANDLE Stop)
{
    DWORD LastError = ERROR_SUCCESS;
    LPCWSTR ErrorContext = NULL;
    HANDLE PendingBoundary = NULL, PendingNamespace = NULL;
    ULONGLONG Started = GetTickCount64();

    EnterCriticalSection(&Initializing);
    if (!NamespaceInitPause(Stop, 0)) { LastError = GetLastError(); goto cleanup; }
    if (PrivateNamespace)
    {
        LeaveCriticalSection(&Initializing);
        return TRUE;
    }

    BYTE Sid[MAX_SID_SIZE];
    DWORD SidSize = sizeof(Sid);
    if (!CreateWellKnownSid(IsLocalSystem ? WinLocalSystemSid : WinBuiltinAdministratorsSid, NULL, Sid, &SidSize))
    {
        LastError = GetLastError(); ErrorContext = L"Failed to create SID";
        goto cleanup;
    }

    PendingBoundary = CreateBoundaryDescriptorW(L"PacketBridgeV2", 0);
    if (!PendingBoundary)
    {
        LastError = GetLastError(); ErrorContext = L"Failed to create boundary descriptor";
        goto cleanup;
    }
    if (!AddSIDToBoundaryDescriptor(&PendingBoundary, Sid))
    {
        LastError = GetLastError(); ErrorContext = L"Failed to add SID to boundary descriptor";
        goto cleanup;
    }

    for (;;)
    {
        if (!NamespaceInitPause(Stop, 0)) { LastError = GetLastError(); goto cleanup; }
        if (!NamespaceInitRemaining(Started)) { LastError = ERROR_TIMEOUT; goto cleanup; }
        if ((PendingNamespace = CreatePrivateNamespaceW(&SecurityAttributes, PendingBoundary, L"PacketBridgeV2")) != NULL)
            break;
        if ((LastError = GetLastError()) == ERROR_ALREADY_EXISTS)
        {
            /* Create can return after cancellation or the local deadline.
             * Do not enter the next OS call once either is already known. */
            if (!NamespaceInitPause(Stop, 0)) { LastError = GetLastError(); goto cleanup; }
            if (!NamespaceInitRemaining(Started)) { LastError = ERROR_TIMEOUT; goto cleanup; }
            if ((PendingNamespace = OpenPrivateNamespaceW(PendingBoundary, L"PacketBridgeV2")) != NULL)
                break;
            if ((LastError = GetLastError()) == ERROR_PATH_NOT_FOUND)
            {
                /* The creator can disappear between create and open. Keep
                 * recovery finite even for an unowned background caller. */
                DWORD Remaining = NamespaceInitRemaining(Started);
                if (!Remaining) { LastError = ERROR_TIMEOUT; goto cleanup; }
                if (!NamespaceInitPause(Stop, min(Remaining, NAMESPACE_RETRY_DELAY_MS))) {
                    LastError = GetLastError(); goto cleanup;
                }
                continue;
            }
            ErrorContext = L"Failed to open private namespace";
        }
        else
            ErrorContext = L"Failed to create private namespace";
        goto cleanup;
    }

    /* A stop or deadline crossed inside an OS call must not publish partial
     * module state. Existing cached namespaces keep their original lifetime. */
    if (!NamespaceInitPause(Stop, 0)) { LastError = GetLastError(); goto cleanup; }
    if (!NamespaceInitRemaining(Started)) { LastError = ERROR_TIMEOUT; goto cleanup; }
    BoundaryDescriptor = PendingBoundary; PrivateNamespace = PendingNamespace;
    LeaveCriticalSection(&Initializing);
    return TRUE;

cleanup:
    if (PendingNamespace) ClosePrivateNamespace(PendingNamespace, 0);
    if (PendingBoundary) DeleteBoundaryDescriptor(PendingBoundary);
    LeaveCriticalSection(&Initializing);
    if (!LastError) LastError = ERROR_GEN_FAILURE;
    /* User loggers may dispatch work to another thread. Do not call them
     * while the initialization lock or private resource owners are held. */
    if (ErrorContext) LOG_ERROR(LastError, L"%s", ErrorContext);
    SetLastError(LastError);
    return FALSE;
}

static HANDLE NamespaceTakeInstallationMutex(LPCWSTR Name, HANDLE Stop)
{
    if (!NamespaceRuntimeInit(Stop))
        return NULL;
    HANDLE Mutex = CreateMutexW(&SecurityAttributes, FALSE, Name);
    if (!Mutex)
    {
        DWORD Error = LOG_LAST_ERROR(L"Failed to create mutex");
        SetLastError(Error ? Error : ERROR_GEN_FAILURE);
        return NULL;
    }
    if (MainWaitForObject(Mutex, Stop, INFINITE)) return Mutex;
    DWORD error = GetLastError();
    CloseHandle(Mutex);
    SetLastError(error ? error : ERROR_GEN_FAILURE);
    return NULL;
}

_Use_decl_annotations_
HANDLE NamespaceTakeDriverInstallationMutex(VOID)
{
    return NamespaceTakeInstallationMutex(L"PacketBridgeV2\\PacketBridgeV2-Driver-Installation-Mutex", NULL);
}

_Use_decl_annotations_
HANDLE
NamespaceTakeDeviceInstallationMutex(VOID)
{
    return NamespaceTakeDeviceInstallationMutexCancelable(NULL);
}

_Use_decl_annotations_
HANDLE NamespaceTakeDeviceInstallationMutexCancelable(HANDLE Stop)
{
    return NamespaceTakeInstallationMutex(L"PacketBridgeV2\\PacketBridgeV2-Device-Installation-Mutex", Stop);
}

_Use_decl_annotations_
VOID
NamespaceReleaseMutex(HANDLE Mutex)
{
    ReleaseMutex(Mutex);
    CloseHandle(Mutex);
}

VOID NamespaceInit(VOID)
{
    InitializeCriticalSection(&Initializing);
}

VOID NamespaceDone(VOID)
{
    EnterCriticalSection(&Initializing);
    if (PrivateNamespace)
    {
        ClosePrivateNamespace(PrivateNamespace, 0);
        DeleteBoundaryDescriptor(BoundaryDescriptor);
        PrivateNamespace = NULL;
        BoundaryDescriptor = NULL;
    }
    LeaveCriticalSection(&Initializing);
    DeleteCriticalSection(&Initializing);
}
