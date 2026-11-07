/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once

_Must_inspect_result_
static _Return_type_success_(return != FALSE)
BOOL
PopulateAdapterData(_Inout_ WINTUN_ADAPTER *Adapter)
{
    DWORD LastError = ERROR_SUCCESS;

    /* Open HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\<class>\<id> registry key. */
    HKEY Key =
        SetupDiOpenDevRegKey(Adapter->DevInfo, &Adapter->DevInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (Key == INVALID_HANDLE_VALUE)
    {
        LOG_LAST_ERROR(L"Failed to open adapter device registry key");
        return FALSE;
    }

    LPWSTR ValueStr = RegistryQueryString(Key, L"NetCfgInstanceId", TRUE);
    if (!ValueStr)
    {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LastError = LOG(WINTUN_LOG_ERR, L"Failed to get %.*s\\NetCfgInstanceId", MAX_REG_PATH, RegPath);
        goto cleanupKey;
    }
    HRESULT HRet = CLSIDFromString(ValueStr, &Adapter->CfgInstanceID);
    if (FAILED(HRet))
    {
        /* COM reports failure through HRESULT, independently of thread last
         * error. Never let a stale zero turn invalid metadata into success. */
        LastError = HRESULT_FACILITY(HRet) == FACILITY_WIN32 ? HRESULT_CODE(HRet) : ERROR_INVALID_DATA;
        if (!LastError) LastError = ERROR_INVALID_DATA;
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LOG(WINTUN_LOG_ERR, L"%.*s\\NetCfgInstanceId is not a GUID: %s", MAX_REG_PATH, RegPath, ValueStr);
        Free(ValueStr);
        goto cleanupKey;
    }
    Free(ValueStr);

    if (!RegistryQueryDWORD(Key, L"NetLuidIndex", &Adapter->LuidIndex, TRUE))
    {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LastError = LOG(WINTUN_LOG_ERR, L"Failed to get %.*s\\NetLuidIndex", MAX_REG_PATH, RegPath);
        goto cleanupKey;
    }

    if (!RegistryQueryDWORD(Key, L"*IfType", &Adapter->IfType, TRUE))
    {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LastError = LOG(WINTUN_LOG_ERR, L"Failed to get %.*s\\*IfType", MAX_REG_PATH, RegPath);
        goto cleanupKey;
    }

    Adapter->InterfaceFilename = AdapterGetDeviceObjectFileName(Adapter->DevInstanceID);
    if (!Adapter->InterfaceFilename)
    {
        LastError = LOG_LAST_ERROR(L"Unable to determine device object file name");
        goto cleanupKey;
    }

cleanupKey:
    RegCloseKey(Key);
    return RET_ERROR(TRUE, LastError);
}
