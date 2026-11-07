/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once
#include "adapter_name_log.h"

static _Return_type_success_(return != FALSE)
BOOL
DisableAllOurAdapters(_In_ HDEVINFO DevInfo, _Inout_ SP_DEVINFO_DATA_LIST **DisabledAdapters)
{
    DWORD LastError = ERROR_SUCCESS;
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        if (!MainOperationSleep(0)) return FALSE;
        SP_DEVINFO_DATA_LIST *DeviceNode = Zalloc(sizeof(*DeviceNode));
        if (!DeviceNode)
            return FALSE;
        DeviceNode->Data.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DeviceNode->Data))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
            {
                Free(DeviceNode);
                break;
            }
            goto cleanupDeviceNode;
        }

        WCHAR Name[PB_MAX_ADAPTER_NAME];
        AdapterNameForLog(DevInfo, &DeviceNode->Data, Name);

        ULONG Status, ProblemCode;
        if (CM_Get_DevNode_Status(&Status, &ProblemCode, DeviceNode->Data.DevInst, 0) != CR_SUCCESS ||
            ((Status & DN_HAS_PROBLEM) && ProblemCode == CM_PROB_DISABLED))
            goto cleanupDeviceNode;

        LOG(WINTUN_LOG_INFO, L"Disabling adapter \"%s\"", Name);
        if (!AdapterDisableInstance(DevInfo, &DeviceNode->Data))
        {
            LOG_LAST_ERROR(L"Failed to disable adapter \"%s\"", Name);
            LastError = LastError != ERROR_SUCCESS ? LastError : GetLastError();
            goto cleanupDeviceNode;
        }

        DeviceNode->Next = *DisabledAdapters;
        *DisabledAdapters = DeviceNode;
        continue;

    cleanupDeviceNode:
        Free(DeviceNode);
    }
    return RET_ERROR(TRUE, LastError);
}

static _Return_type_success_(return != FALSE)
BOOL
EnableAllOurAdapters(_In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA_LIST *AdaptersToEnable)
{
    DWORD LastError = ERROR_SUCCESS;
    for (SP_DEVINFO_DATA_LIST *DeviceNode = AdaptersToEnable; DeviceNode; DeviceNode = DeviceNode->Next)
    {
        WCHAR Name[PB_MAX_ADAPTER_NAME];
        AdapterNameForLog(DevInfo, &DeviceNode->Data, Name);

        LOG(WINTUN_LOG_INFO, L"Enabling adapter \"%s\"", Name);
        if (!AdapterEnableInstance(DevInfo, &DeviceNode->Data))
        {
            LOG_LAST_ERROR(L"Failed to enable adapter \"%s\"", Name);
            LastError = LastError != ERROR_SUCCESS ? LastError : GetLastError();
        }
    }
    return RET_ERROR(TRUE, LastError);
}

