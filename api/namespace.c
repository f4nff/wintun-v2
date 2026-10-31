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

static _Return_type_success_(return != FALSE)
BOOL NamespaceRuntimeInit(VOID)
{
    DWORD LastError;

    EnterCriticalSection(&Initializing);
    if (PrivateNamespace)
    {
        LeaveCriticalSection(&Initializing);
        return TRUE;
    }

    BYTE Sid[MAX_SID_SIZE];
    DWORD SidSize = sizeof(Sid);
    if (!CreateWellKnownSid(IsLocalSystem ? WinLocalSystemSid : WinBuiltinAdministratorsSid, NULL, Sid, &SidSize))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create SID");
        goto cleanupLeaveCriticalSection;
    }

    BoundaryDescriptor = CreateBoundaryDescriptorW(L"Wintun", 0);
    if (!BoundaryDescriptor)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create boundary descriptor");
        goto cleanupLeaveCriticalSection;
    }
    if (!AddSIDToBoundaryDescriptor(&BoundaryDescriptor, Sid))
    {
        LastError = LOG_LAST_ERROR(L"Failed to add SID to boundary descriptor");
        goto cleanupBoundaryDescriptor;
    }

    for (;;)
    {
        if (!MainOperationSleep(0)) { LastError = GetLastError(); goto cleanupBoundaryDescriptor; }
        if ((PrivateNamespace = CreatePrivateNamespaceW(&SecurityAttributes, BoundaryDescriptor, L"Wintun")) != NULL)
            break;
        if ((LastError = GetLastError()) == ERROR_ALREADY_EXISTS)
        {
            if ((PrivateNamespace = OpenPrivateNamespaceW(BoundaryDescriptor, L"Wintun")) != NULL)
                break;
            if ((LastError = GetLastError()) == ERROR_PATH_NOT_FOUND)
                continue;
            LOG_ERROR(LastError, L"Failed to open private namespace");
        }
        else
            LOG_ERROR(LastError, L"Failed to create private namespace");
        goto cleanupBoundaryDescriptor;
    }

    LeaveCriticalSection(&Initializing);
    return TRUE;

cleanupBoundaryDescriptor:
    DeleteBoundaryDescriptor(BoundaryDescriptor);
cleanupLeaveCriticalSection:
    LeaveCriticalSection(&Initializing);
    SetLastError(LastError);
    return FALSE;
}

static HANDLE NamespaceTakeInstallationMutex(LPCWSTR Name, HANDLE Stop)
{
    if (!NamespaceRuntimeInit())
        return NULL;
    HANDLE Mutex = CreateMutexW(&SecurityAttributes, FALSE, Name);
    if (!Mutex)
    {
        LOG_LAST_ERROR(L"Failed to create mutex");
        return NULL;
    }
    if (MainWaitForObject(Mutex, Stop, INFINITE)) return Mutex;
    DWORD error = GetLastError();
    CloseHandle(Mutex);
    SetLastError(error);
    return NULL;
}

_Use_decl_annotations_
HANDLE NamespaceTakeDriverInstallationMutex(VOID)
{
    return NamespaceTakeInstallationMutex(L"Wintun\\Wintun-Driver-Installation-Mutex", NULL);
}

_Use_decl_annotations_
HANDLE
NamespaceTakeDeviceInstallationMutex(VOID)
{
    return NamespaceTakeDeviceInstallationMutexCancelable(NULL);
}

HANDLE NamespaceTakeDeviceInstallationMutexCancelable(HANDLE Stop)
{
    return NamespaceTakeInstallationMutex(L"Wintun\\Wintun-Device-Installation-Mutex", Stop);
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
    }
    LeaveCriticalSection(&Initializing);
    DeleteCriticalSection(&Initializing);
}
