/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
/* Shared by the installer and hosted ownership tests. */
#pragma once
#include "orphan_owner.h"
#include "adapter_name_log.h"
static VOID OrphanDeviceName(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData, WCHAR Name[PB_MAX_ADAPTER_NAME])
{
    AdapterNameForLog(DevInfo, DevInfoData, Name);
}

VOID AdapterCleanupOrphanedDevicesWin7(VOID)
{
    HDEVINFO DevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_NET, WINTUN_ENUMERATOR, NULL, 0, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_INVALID_DATA)
            LOG_LAST_ERROR(L"Failed to get adapters");
        return;
    }

    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        if (AdapterCleanupCancelled()) break;
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
                LOG_LAST_ERROR(L"Failed to enumerate adapters");
            break;
        }

        OWNING_PROCESS OwningProcess = { 0 };
        DEVPROPTYPE PropType = 0;
        DWORD OwnerSize = 0;
        BOOL OwnerRead = SetupDiGetDevicePropertyW(
                DevInfo,
                &DevInfoData,
                &DEVPKEY_Wintun_OwningProcess,
                &PropType,
                (PBYTE)&OwningProcess,
                sizeof(OwningProcess),
                &OwnerSize,
                0);
        if (OwnerRead)
        {
            if (PropType != DEVPROP_TYPE_BINARY || OwnerSize != sizeof(OwningProcess) || !ProcessIsStale(&OwningProcess))
                continue;
        }
        else if (GetLastError() != ERROR_NOT_FOUND)
            continue; /* Unknown metadata is not an orphan. A truly absent owner
                       * still permits cleanup of an interrupted installation. */

        WCHAR Name[PB_MAX_ADAPTER_NAME];
        OrphanDeviceName(DevInfo, &DevInfoData, Name);
        if (AdapterCleanupCancelled()) break;
        if (!AdapterRemoveInstance(DevInfo, &DevInfoData))
        {
            LOG_LAST_ERROR(L"Failed to remove orphaned adapter \"%s\"", Name);
            continue;
        }
        LOG(WINTUN_LOG_INFO, L"Removed orphaned adapter \"%s\"", Name);
    }
    SetupDiDestroyDeviceInfoList(DevInfo);
}

BOOL AdapterCleanupLegacyDevices(VOID)
{
    HDEVINFO DevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_NET, L"ROOT\\NET", NULL, 0, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL Complete = TRUE;
    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        if (AdapterCleanupCancelled()) { Complete = FALSE; break; }
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
            {
                LOG_LAST_ERROR(L"Failed to enumerate legacy adapters");
                Complete = FALSE;
            }
            break;
        }
        WCHAR HardwareIDs[0x400] = { 0 };
        DWORD ValueType, Size = sizeof(HardwareIDs) - sizeof(HardwareIDs[0]);
        if (!SetupDiGetDeviceRegistryPropertyW(
                DevInfo, &DevInfoData, SPDRP_HARDWAREID, &ValueType, (PBYTE)HardwareIDs, Size, &Size) ||
            ValueType != REG_MULTI_SZ || Size > sizeof(HardwareIDs) - sizeof(HardwareIDs[0]) ||
            Size < 2 * sizeof(HardwareIDs[0]) || Size % sizeof(HardwareIDs[0]))
        { Complete = FALSE; continue; }
        Size /= sizeof(HardwareIDs[0]);
        if (HardwareIDs[Size - 1] || HardwareIDs[Size - 2]) { Complete = FALSE; continue; }
        BOOL Matches = FALSE, Valid = TRUE;
        DWORD Offset = 0;
        while (Offset < Size && HardwareIDs[Offset])
        {
            DWORD Start = Offset;
            while (Offset < Size && HardwareIDs[Offset]) ++Offset;
            if (Offset == Size) { Valid = FALSE; break; }
            if (Offset - Start == _countof(WINTUN_HWID) - 1 &&
                !_wcsnicmp(HardwareIDs + Start, WINTUN_HWID, Offset - Start)) Matches = TRUE;
            ++Offset;
        }
        /* Padding after the MULTI_SZ terminator may only be zero. Never let
         * unparsed trailing data authorize removing an unrelated device. */
        for (; Offset < Size; ++Offset)
            if (HardwareIDs[Offset]) { Valid = FALSE; break; }
        if (!Valid) { Complete = FALSE; continue; }
        if (Matches)
        {
            if (AdapterCleanupCancelled() || !AdapterRemoveInstance(DevInfo, &DevInfoData)) Complete = FALSE;
        }
    }
    SetupDiDestroyDeviceInfoList(DevInfo);
    return Complete;
}
VOID AdapterCleanupOrphanedDevices(VOID)
{
    HANDLE DeviceInstallationMutex = NamespaceTakeDeviceInstallationMutexCancelable(CleanupStop);
    if (!DeviceInstallationMutex)
    {
        LOG_LAST_ERROR(L"Failed to take device installation mutex");
        return;
    }

    if (IsWindows7)
    {
        AdapterCleanupOrphanedDevicesWin7();
        goto cleanupDeviceInstallationMutex;
    }

    HDEVINFO DevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_NET, WINTUN_ENUMERATOR, NULL, 0, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        LOG_LAST_ERROR(L"Failed to get adapters");
        goto cleanupDeviceInstallationMutex;
    }

    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        if (AdapterCleanupCancelled()) break;
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
                LOG_LAST_ERROR(L"Failed to enumerate adapters");
            break;
        }
        ULONG Status, Code;
        if (CM_Get_DevNode_Status(&Status, &Code, DevInfoData.DevInst, 0) != CR_SUCCESS || !(Status & DN_HAS_PROBLEM))
            continue;

        WCHAR Name[PB_MAX_ADAPTER_NAME];
        OrphanDeviceName(DevInfo, &DevInfoData, Name);
        if (AdapterCleanupCancelled()) break;
        if (!AdapterRemoveInstance(DevInfo, &DevInfoData))
        {
            LOG_LAST_ERROR(L"Failed to remove orphaned adapter \"%s\"", Name);
            continue;
        }
        LOG(WINTUN_LOG_INFO, L"Removed orphaned adapter \"%s\"", Name);
    }
    SetupDiDestroyDeviceInfoList(DevInfo);
cleanupDeviceInstallationMutex:
    NamespaceReleaseMutex(DeviceInstallationMutex);
}

