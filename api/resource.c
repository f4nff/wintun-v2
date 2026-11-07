/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "logger.h"
#include "main.h"
#include "resource.h"
#include <Windows.h>
#include <Shlwapi.h>
#include <NTSecAPI.h>

/* Resource APIs accept both string names and integer identifiers. Keep the
 * caller's spelling for aliases, and never pass MAKEINTRESOURCE as a string. */
static LPCWSTR ResourceLogName(LPCWSTR Name, WCHAR NumericName[16])
{
    if (!IS_INTRESOURCE(Name)) return Name;
    swprintf_s(NumericName, 16, L"#%u", (UINT)(ULONG_PTR)Name);
    return NumericName;
}

static DWORD ResourceLogFailure(DWORD Error, LPCWSTR Operation, LPCWSTR Name)
{
    if (!Error) Error = ERROR_GEN_FAILURE;
    LOG_ERROR(Error, L"Failed to %s resource %s", Operation, Name);
    SetLastError(Error); return Error;
}

_Use_decl_annotations_
const VOID *
ResourceGetAddress(LPCWSTR ResourceName, DWORD *Size)
{
    *Size = 0;
    WCHAR NumericName[16];
    LPCWSTR DisplayName = ResourceLogName(ResourceName, NumericName);
    /* Numeric resources are shared by both public APIs; no second SYS copy. */
    if (!IS_INTRESOURCE(ResourceName)) {
        if (!wcscmp(ResourceName, L"packetbridge.sys")) ResourceName = MAKEINTRESOURCEW(101);
        else if (!wcscmp(ResourceName, L"packetbridge.inf")) ResourceName = MAKEINTRESOURCEW(102);
        else if (!wcscmp(ResourceName, L"packetbridge.cat")) ResourceName = MAKEINTRESOURCEW(103);
    }
    HRSRC FoundResource = FindResourceW(ResourceModule, ResourceName, MAKEINTRESOURCEW(10));
    if (!FoundResource)
    {
        ResourceLogFailure(GetLastError(), L"find", DisplayName);
        return NULL;
    }
    *Size = SizeofResource(ResourceModule, FoundResource);
    if (!*Size)
    {
        ResourceLogFailure(GetLastError(), L"query size of", DisplayName);
        return NULL;
    }
    HGLOBAL LoadedResource = LoadResource(ResourceModule, FoundResource);
    if (!LoadedResource)
    {
        ResourceLogFailure(GetLastError(), L"load", DisplayName);
        *Size = 0;
        return NULL;
    }
    BYTE *Address = LockResource(LoadedResource);
    if (!Address)
    {
        ResourceLogFailure(ERROR_LOCK_FAILED, L"lock", DisplayName);
        *Size = 0;
        return NULL;
    }
    return Address;
}

_Use_decl_annotations_
BOOL
ResourceCopyToFile(LPCWSTR DestinationPath, LPCWSTR ResourceName)
{
    DWORD SizeResource;
    const VOID *LockedResource = ResourceGetAddress(ResourceName, &SizeResource);
    if (!LockedResource)
    {
        DWORD Error = GetLastError();
        WCHAR NumericName[16];
        LPCWSTR DisplayName = ResourceLogName(ResourceName, NumericName);
        ResourceLogFailure(Error, L"locate", DisplayName);
        return FALSE;
    }
    HANDLE DestinationHandle = CreateFileW(
        DestinationPath,
        GENERIC_WRITE,
        0,
        &SecurityAttributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY,
        NULL);
    if (DestinationHandle == INVALID_HANDLE_VALUE)
    {
        DWORD Error = LOG_LAST_ERROR(L"Failed to create file %s", DestinationPath);
        SetLastError(Error ? Error : ERROR_GEN_FAILURE);
        return FALSE;
    }
    DWORD BytesWritten;
    DWORD LastError;
    if (!WriteFile(DestinationHandle, LockedResource, SizeResource, &BytesWritten, NULL))
    {
        LastError = LOG_LAST_ERROR(L"Failed to write file %s", DestinationPath);
        if (!LastError) LastError = ERROR_GEN_FAILURE;
        goto cleanupDestinationHandle;
    }
    if (BytesWritten != SizeResource)
    {
        LOG(WINTUN_LOG_ERR,
            L"Incomplete write to %s (written: %u, expected: %u)",
            DestinationPath,
            BytesWritten,
            SizeResource);
        LastError = ERROR_WRITE_FAULT;
        goto cleanupDestinationHandle;
    }
    LastError = ERROR_SUCCESS;
cleanupDestinationHandle:
    CloseHandle(DestinationHandle);
    return RET_ERROR(TRUE, LastError);
}

_Return_type_success_(return != FALSE)
BOOL
ResourceCreateTemporaryDirectory(_Out_writes_z_(MAX_PATH) LPWSTR RandomTempSubDirectory)
{
    /* The caller may clean up a nonempty output after any later failure. Do
     * not publish a candidate that this invocation did not actually create. */
    RandomTempSubDirectory[0] = 0;
    WCHAR WindowsDirectory[MAX_PATH];
    UINT WindowsLength = GetWindowsDirectoryW(WindowsDirectory, _countof(WindowsDirectory));
    if (!WindowsLength)
    {
        DWORD Error = LOG_LAST_ERROR(L"Failed to get Windows folder");
        SetLastError(Error ? Error : ERROR_GEN_FAILURE);
        return FALSE;
    }
    if (WindowsLength >= _countof(WindowsDirectory))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    WCHAR WindowsTempDirectory[MAX_PATH];
    if (!PathCombineW(WindowsTempDirectory, WindowsDirectory, L"Temp"))
    {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    UCHAR RandomBytes[32] = { 0 };
    if (!RtlGenRandom(RandomBytes, sizeof(RandomBytes)))
    {
        LOG(WINTUN_LOG_ERR, L"Failed to generate random");
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    WCHAR RandomSubDirectory[sizeof(RandomBytes) * 2 + 1];
    for (int i = 0; i < sizeof(RandomBytes); ++i)
        swprintf_s(&RandomSubDirectory[i * 2], 3, L"%02x", RandomBytes[i]);
    WCHAR Candidate[MAX_PATH];
    if (!PathCombineW(Candidate, WindowsTempDirectory, RandomSubDirectory))
    {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    if (!CreateDirectoryW(Candidate, &SecurityAttributes))
    {
        DWORD Error = LOG_LAST_ERROR(L"Failed to create temporary folder %s", Candidate);
        SetLastError(Error ? Error : ERROR_GEN_FAILURE);
        return FALSE;
    }
    wcscpy_s(RandomTempSubDirectory, MAX_PATH, Candidate);
    return TRUE;
}
