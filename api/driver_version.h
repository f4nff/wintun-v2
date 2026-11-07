/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
static _Return_type_success_(return != 0)
DWORD
VersionOfFile(_In_z_ LPCWSTR Filename)
{
    DWORD Zero;
    DWORD Len = GetFileVersionInfoSizeW(Filename, &Zero);
    if (!Len)
    {
        LOG_LAST_ERROR(L"Failed to query %s version info size", Filename);
        return 0;
    }
    VOID *VersionInfo = Alloc(Len);
    if (!VersionInfo)
        return 0;
    DWORD LastError = ERROR_SUCCESS, Version = 0;
    VS_FIXEDFILEINFO *FixedInfo;
    UINT FixedInfoLen = sizeof(*FixedInfo);
    if (!GetFileVersionInfoW(Filename, 0, Len, VersionInfo))
    {
        LastError = LOG_LAST_ERROR(L"Failed to get %s version info", Filename);
        goto out;
    }
    if (!VerQueryValueW(VersionInfo, L"\\", &FixedInfo, &FixedInfoLen))
    {
        LastError = LOG_LAST_ERROR(L"Failed to get %s version info root", Filename);
        goto out;
    }
    Version = FixedInfo->dwFileVersionMS;
    if (!Version)
    {
        LOG(WINTUN_LOG_WARN, L"Determined version of %s, but was v0.0, so returning failure", Filename);
        LastError = ERROR_VERSION_PARSE_ERROR;
    }
out:
    Free(VersionInfo);
    return RET_ERROR(Version, LastError);
}

static DWORD WINAPI
QueryRunningDriverVersion(BOOL ReturnOneIfRunningInsteadOfVersion, BOOL *Absent)
{
    if (Absent) *Absent = FALSE;
    PRTL_PROCESS_MODULES Modules;
    ULONG BufferSize = 128 * 1024;
    for (;;)
    {
        if (!MainOperationSleep(0)) return 0;
        Modules = Alloc(BufferSize);
        if (!Modules)
            return 0;
        NTSTATUS Status = NtQuerySystemInformation(SystemModuleInformation, Modules, BufferSize, &BufferSize);
        if (NT_SUCCESS(Status))
            break;
        Free(Modules);
        if (Status == STATUS_INFO_LENGTH_MISMATCH)
            continue;
        LOG(WINTUN_LOG_ERR, L"Failed to enumerate drivers (status: 0x%x)", Status);
        SetLastError(RtlNtStatusToDosError(Status));
        return 0;
    }
    DWORD LastError = ERROR_SUCCESS, Version = 0;
    for (ULONG i = Modules->NumberOfModules; i-- > 0;)
    {
        LPCSTR NtPath = (LPCSTR)Modules->Modules[i].FullPathName;
        if (!_stricmp(&NtPath[Modules->Modules[i].OffsetToFileName], "packetbridge.sys"))
        {
            if (ReturnOneIfRunningInsteadOfVersion)
            {
                Version = 1;
                goto cleanupModules;
            }
            WCHAR FilePath[MAX_PATH * 3 + 15];
            if (_snwprintf_s(FilePath, _countof(FilePath), _TRUNCATE, L"\\\\?\\GLOBALROOT%S", NtPath) == -1)
                continue;
            Version = VersionOfFile(FilePath);
            if (!Version)
                LastError = GetLastError();
            goto cleanupModules;
        }
    }
    /* A failed query may map to ERROR_FILE_NOT_FOUND too. Only the completed
     * enumeration without a matching module establishes actual absence. */
    if (Absent) *Absent = TRUE;
    LastError = ERROR_FILE_NOT_FOUND;
cleanupModules:
    Free(Modules);
    return RET_ERROR(Version, LastError);
}

static DWORD WINAPI MaybeGetRunningDriverVersion(BOOL ReturnOneIfRunningInsteadOfVersion)
{
    return QueryRunningDriverVersion(ReturnOneIfRunningInsteadOfVersion, NULL);
}

_Use_decl_annotations_
DWORD WINAPI WintunGetRunningDriverVersion(VOID)
{
    if (!MainRuntimeAcquire()) return 0;
    DWORD version = MaybeGetRunningDriverVersion(FALSE);
    MainRuntimeRelease(); return version;
}

static BOOL EnsureWintunUnloaded(VOID)
{
    for (DWORD Tries = 0; Tries < 1500; ++Tries)
    {
        if (Tries)
            if (!MainOperationSleep(50)) return FALSE;
        BOOL Absent;
        if (!QueryRunningDriverVersion(TRUE, &Absent))
            return Absent;
    }
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
}

