/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "orphan_owner.h"
#include "adapter_wait_win7.h"

_Must_inspect_result_
static _Return_type_success_(return != FALSE)
BOOL
CreateAdapterWin7(_Inout_ WINTUN_ADAPTER *Adapter, _In_z_ LPCWSTR Name, _In_z_ LPCWSTR TunnelTypeName)
{
    DWORD LastError = ERROR_SUCCESS;

    HDEVINFO DevInfo = SetupDiCreateDeviceInfoListExW(&GUID_DEVCLASS_NET, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create empty device information set");
        goto cleanup;
    }
    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };

#ifdef MAYBE_WOW64
    if (NativeMachine != IMAGE_FILE_PROCESS)
    {
        if (!CreateInstanceWin7ViaRundll32(Adapter->DevInstanceID))
        {
            LastError = LOG_LAST_ERROR(L"Failed to create device instance");
            goto cleanupDevInfo;
        }
        if (!SetupDiOpenDeviceInfoW(DevInfo, Adapter->DevInstanceID, NULL, DIOD_INHERIT_CLASSDRVS, &DevInfoData))
        {
            LastError = GetLastError();
            goto cleanupDevInfo;
        }
        goto resumeAfterInstance;
    }
#endif

    if (!SetupDiCreateDeviceInfoW(
            DevInfo, WINTUN_HWID, &GUID_DEVCLASS_NET, TunnelTypeName, NULL, DICD_GENERATE_ID, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create new device information element");
        goto cleanupDevInfo;
    }
    SP_DEVINSTALL_PARAMS_W DevInstallParams = { .cbSize = sizeof(DevInstallParams) };
    if (!SetupDiGetDeviceInstallParamsW(DevInfo, &DevInfoData, &DevInstallParams))
    {
        LastError = LOG_LAST_ERROR(L"Failed to retrieve adapter device installation parameters");
        goto cleanupDevInfo;
    }
    DevInstallParams.Flags |= DI_QUIETINSTALL;
    if (!SetupDiSetDeviceInstallParamsW(DevInfo, &DevInfoData, &DevInstallParams))
    {
        LastError = LOG_LAST_ERROR(L"Failed to set adapter device installation parameters");
        goto cleanupDevInfo;
    }
    if (!SetupDiSetSelectedDevice(DevInfo, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to select adapter device");
        goto cleanupDevInfo;
    }
    static const WCHAR Hwids[_countof(WINTUN_HWID) + 1 /*Multi-string terminator*/] = WINTUN_HWID;
    if (!SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_HARDWAREID, (const BYTE *)Hwids, sizeof(Hwids)))
    {
        LastError = LOG_LAST_ERROR(L"Failed to set adapter hardware ID");
        goto cleanupDevInfo;
    }
    if (!SetupDiBuildDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER))
    {
        LastError = LOG_LAST_ERROR(L"Failed building adapter driver info list");
        goto cleanupDevInfo;
    }
    SP_DRVINFO_DATA_W DrvInfoData = { .cbSize = sizeof(SP_DRVINFO_DATA_W) };
    if (!SetupDiEnumDriverInfoW(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER, 0, &DrvInfoData) ||
        !SetupDiSetSelectedDriverW(DevInfo, &DevInfoData, &DrvInfoData))
    {
        LastError = LOG_ERROR(ERROR_DRIVER_INSTALL_BLOCKED, L"Failed to select a driver");
        goto cleanupDriverInfo;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to register adapter device");
        goto cleanupDevInfo;
    }
    if (!SetupDiCallClassInstaller(DIF_REGISTER_COINSTALLERS, DevInfo, &DevInfoData))
        LOG_LAST_ERROR(L"Failed to register adapter coinstallers");
    if (!SetupDiCallClassInstaller(DIF_INSTALLINTERFACES, DevInfo, &DevInfoData))
        LOG_LAST_ERROR(L"Failed to install adapter interfaces");
    if (!SetupDiCallClassInstaller(DIF_INSTALLDEVICE, DevInfo, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to install adapter device");
        goto cleanupDevice;
    }

#ifdef MAYBE_WOW64
resumeAfterInstance:;
#endif

    OWNING_PROCESS OwningProcess = { .ProcessId = GetCurrentProcessId() };
    FILETIME Unused;
    if (!GetProcessTimes(GetCurrentProcess(), &OwningProcess.CreationTime, &Unused, &Unused, &Unused))
    {
        LastError = LOG_LAST_ERROR(L"Failed to get process creation time");
        goto cleanupDevice;
    }

    if (!SetupDiSetDeviceRegistryPropertyW(
            DevInfo,
            &DevInfoData,
            SPDRP_FRIENDLYNAME,
            (PBYTE)TunnelTypeName,
            (DWORD)((wcslen(TunnelTypeName) + 1) * sizeof(TunnelTypeName[0]))) ||
        !SetupDiSetDeviceRegistryPropertyW(
            DevInfo,
            &DevInfoData,
            SPDRP_DEVICEDESC,
            (PBYTE)TunnelTypeName,
            (DWORD)((wcslen(TunnelTypeName) + 1) * sizeof(TunnelTypeName[0]))) ||
        !SetupDiSetDevicePropertyW(
            DevInfo,
            &DevInfoData,
            &DEVPKEY_Wintun_Name,
            DEVPROP_TYPE_STRING,
            (PBYTE)Name,
            (DWORD)((wcslen(Name) + 1) * sizeof(Name[0])),
            0) ||
        !SetupDiSetDevicePropertyW(
            DevInfo,
            &DevInfoData,
            &DEVPKEY_Wintun_OwningProcess,
            DEVPROP_TYPE_BINARY,
            (PBYTE)&OwningProcess,
            sizeof(OwningProcess),
            0))
    {
        LastError = LOG_LAST_ERROR(L"Failed to set device properties");
        goto cleanupDevice;
    }

    DWORD RequiredChars = _countof(Adapter->DevInstanceID);
    if (!SetupDiGetDeviceInstanceIdW(DevInfo, &DevInfoData, Adapter->DevInstanceID, RequiredChars, &RequiredChars))
    {
        LastError = LOG_LAST_ERROR(L"Failed to get adapter instance ID");
        goto cleanupDevice;
    }

    if (!WaitForInterfaceWin7(DevInfo, &DevInfoData, Adapter->DevInstanceID))
    {
        DEVPROPTYPE PropertyType = 0;
        INT32 ProblemCode = 0;
        if (!SetupDiGetDevicePropertyW(
                DevInfo,
                &DevInfoData,
                &DEVPKEY_Device_ProblemCode,
                &PropertyType,
                (PBYTE)&ProblemCode,
                sizeof(ProblemCode),
                NULL,
                0) ||
            (PropertyType != DEVPROP_TYPE_INT32 && PropertyType != DEVPROP_TYPE_UINT32))
            ProblemCode = 0;
        LastError = LOG_ERROR(
            ERROR_DEVICE_REINITIALIZATION_NEEDED, L"Failed to setup adapter (problem code: 0x%x)", ProblemCode);
        goto cleanupDevice;
    }

cleanupDevice:
    if (LastError != ERROR_SUCCESS)
        AdapterRemoveInstance(DevInfo, &DevInfoData);
cleanupDriverInfo:
    SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
cleanupDevInfo:
    SetupDiDestroyDeviceInfoList(DevInfo);
cleanup:
    return RET_ERROR(TRUE, LastError);
}

static VOID
CreateAdapterPostWin7(_Inout_ WINTUN_ADAPTER *Adapter, _In_z_ LPCWSTR TunnelTypeName)
{
    SetupDiSetDeviceRegistryPropertyW(
        Adapter->DevInfo,
        &Adapter->DevInfoData,
        SPDRP_FRIENDLYNAME,
        (PBYTE)TunnelTypeName,
        (DWORD)((wcslen(TunnelTypeName) + 1) * sizeof(TunnelTypeName[0])));
    SetupDiSetDeviceRegistryPropertyW(
        Adapter->DevInfo,
        &Adapter->DevInfoData,
        SPDRP_DEVICEDESC,
        (PBYTE)TunnelTypeName,
        (DWORD)((wcslen(TunnelTypeName) + 1) * sizeof(TunnelTypeName[0])));
}
