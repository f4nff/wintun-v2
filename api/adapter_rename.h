/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once

static _Return_type_success_(return != FALSE)
BOOL
RenameByNetGUID(_In_ GUID *Guid, _In_reads_or_z_(PB_MAX_ADAPTER_NAME) LPCWSTR Name)
{
    DWORD LastError = ERROR_NOT_FOUND;
    HDEVINFO DevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_NET, WINTUN_ENUMERATOR, NULL, 0, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        LastError = GetLastError();
        goto cleanup;
    }

    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        if (!MainOperationSleep(0)) { LastError = GetLastError(); break; }
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            continue;
        }

        HKEY Key = SetupDiOpenDevRegKey(DevInfo, &DevInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (Key == INVALID_HANDLE_VALUE)
            continue;
        LPWSTR ValueStr = RegistryQueryString(Key, L"NetCfgInstanceId", TRUE);
        RegCloseKey(Key);
        if (!ValueStr)
            continue;
        GUID Guid2;
        HRESULT HRet = CLSIDFromString(ValueStr, &Guid2);
        Free(ValueStr);
        if (FAILED(HRet) || memcmp(Guid, &Guid2, sizeof(*Guid)))
            continue;
        LastError = SetupDiSetDevicePropertyW(
                        DevInfo,
                        &DevInfoData,
                        &DEVPKEY_Wintun_Name,
                        DEVPROP_TYPE_STRING,
                        (PBYTE)Name,
                        (DWORD)((wcslen(Name) + 1) * sizeof(Name[0])),
                        0)
                        ? ERROR_SUCCESS
                        : GetLastError();
        break;
    }
    SetupDiDestroyDeviceInfoList(DevInfo);
cleanup:
    return RET_ERROR(TRUE, LastError);
}

_Must_inspect_result_
static _Return_type_success_(return != FALSE)
BOOL
ConvertInterfaceAliasToGuid(_In_z_ LPCWSTR Name, _Out_ GUID *Guid)
{
    NET_LUID Luid;
    DWORD LastError = ConvertInterfaceAliasToLuid(Name, &Luid);
    if (LastError != NO_ERROR)
    {
        SetLastError(LOG_ERROR(LastError, L"Failed convert interface %s name to the locally unique identifier", Name));
        return FALSE;
    }
    LastError = ConvertInterfaceLuidToGuid(&Luid, Guid);
    if (LastError != NO_ERROR)
    {
        SetLastError(LOG_ERROR(LastError, L"Failed to convert interface %s LUID (%I64u) to GUID", Name, Luid.Value));
        return FALSE;
    }
    return TRUE;
}

static _Return_type_success_(return != FALSE)
BOOL
NciSetAdapterName(_In_ GUID *Guid, _In_reads_or_z_(PB_MAX_ADAPTER_NAME) LPCWSTR Name)
{
    const int MaxSuffix = 1000;
    WCHAR AvailableName[PB_MAX_ADAPTER_NAME];
    if (wcsncpy_s(AvailableName, _countof(AvailableName), Name, _TRUNCATE) == STRUNCATE)
    {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    for (int i = 0;; ++i)
    {
        if (!MainOperationSleep(0)) return FALSE;
        DWORD LastError = NciSetConnectionName(Guid, AvailableName);
        if (LastError == ERROR_DUP_NAME)
        {
            GUID Guid2;
            if (ConvertInterfaceAliasToGuid(AvailableName, &Guid2))
            {
                for (int j = 0; j < MaxSuffix; ++j)
                {
                    WCHAR Proposal[PB_MAX_ADAPTER_NAME];
                    if (_snwprintf_s(Proposal, _countof(Proposal), _TRUNCATE, L"%s %d", Name, j + 1) == -1)
                    {
                        SetLastError(ERROR_BUFFER_OVERFLOW);
                        return FALSE;
                    }
                    if (_wcsnicmp(Proposal, AvailableName, PB_MAX_ADAPTER_NAME) == 0)
                        continue;
                    DWORD LastError2 = NciSetConnectionName(&Guid2, Proposal);
                    if (LastError2 == ERROR_DUP_NAME)
                        continue;
                    if (LastError2 == ERROR_SUCCESS)
                    {
                        /* A failed native rename does not authorize changing
                         * the other adapter's metadata to the proposal. */
                        if (!RenameByNetGUID(&Guid2, Proposal))
                            LOG_LAST_ERROR(L"Failed to set foreign adapter name to \"%s\"", Proposal);
                        LastError = NciSetConnectionName(Guid, AvailableName);
                        if (LastError == ERROR_SUCCESS)
                            break;
                    }
                    break;
                }
            }
        }
        if (LastError == ERROR_SUCCESS)
            break;
        if (i >= MaxSuffix || LastError != ERROR_DUP_NAME)
        {
            SetLastError(LastError);
            return FALSE;
        }
        if (_snwprintf_s(AvailableName, _countof(AvailableName), _TRUNCATE, L"%s %d", Name, i + 1) == -1)
        {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return FALSE;
        }
    }
    return TRUE;
}

