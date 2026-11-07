/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once

_Must_inspect_result_
static _Return_type_success_(return != FALSE)
BOOL
WaitForInterfaceWin7(_In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA *DevInfoData, _In_ LPCWSTR DevInstanceId)
{
    ULONG Status, Number;
    DWORD ValType, Zero;
    WCHAR *FileName = NULL;
    HKEY Key = INVALID_HANDLE_VALUE;
    HANDLE FileHandle = INVALID_HANDLE_VALUE;
    BOOLEAN Ret = FALSE;
    for (DWORD Tries = 0; Tries < 1500; ++Tries)
    {
        if (Tries)
            if (!MainOperationSleep(10)) break;
        if (Key == INVALID_HANDLE_VALUE)
            Key = SetupDiOpenDevRegKey(DevInfo, DevInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (!FileName)
            FileName = AdapterGetDeviceObjectFileName(DevInstanceId);
        if (FileName && FileHandle == INVALID_HANDLE_VALUE)
            FileHandle = CreateFileW(
                FileName,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);
        Zero = 0;
        if (FileName && FileHandle != INVALID_HANDLE_VALUE && Key != INVALID_HANDLE_VALUE && Key &&
            RegQueryValueExW(Key, L"NetCfgInstanceId", NULL, &ValType, NULL, &Zero) == ERROR_SUCCESS &&
            CM_Get_DevNode_Status(&Status, &Number, DevInfoData->DevInst, 0) == CR_SUCCESS &&
            !(Status & DN_HAS_PROBLEM) && !Number)
        {
            Ret = TRUE;
            break;
        }
    }
    if (Key != INVALID_HANDLE_VALUE && Key)
        RegCloseKey(Key);
    if (FileHandle != INVALID_HANDLE_VALUE)
        CloseHandle(FileHandle);
    Free(FileName);
    return Ret;
}
