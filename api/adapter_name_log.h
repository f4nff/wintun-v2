/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once
#include "adapter.h"
#include <wchar.h>

/* Diagnostic metadata must neither become an unbounded string nor replace an
 * operation's original error. Keep the existing orphan-name fallback policy. */
static VOID AdapterNameForLog(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData, WCHAR Name[PB_MAX_ADAPTER_NAME])
{
    DWORD Error = GetLastError();
    DEVPROPTYPE Type = 0;
    DWORD Size = 0;
    ZeroMemory(Name, PB_MAX_ADAPTER_NAME * sizeof(*Name));
    if (!SetupDiGetDevicePropertyW(DevInfo, DevInfoData, &DEVPKEY_Wintun_Name, &Type, (PBYTE)Name,
            PB_MAX_ADAPTER_NAME * sizeof(*Name), &Size, 0) || Type != DEVPROP_TYPE_STRING ||
        Size < sizeof(*Name) || Size > PB_MAX_ADAPTER_NAME * sizeof(*Name) || Size % sizeof(*Name) ||
        Name[Size / sizeof(*Name) - 1])
        wcscpy_s(Name, PB_MAX_ADAPTER_NAME, L"<unknown>");
    Name[PB_MAX_ADAPTER_NAME - 1] = 0;
    SetLastError(Error);
}
