/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once

static BOOL
IsNewer(
    _In_ const FILETIME *DriverDate1,
    _In_ DWORDLONG DriverVersion1,
    _In_ const FILETIME *DriverDate2,
    _In_ DWORDLONG DriverVersion2)
{
    if (DriverDate1->dwHighDateTime > DriverDate2->dwHighDateTime)
        return TRUE;
    if (DriverDate1->dwHighDateTime < DriverDate2->dwHighDateTime)
        return FALSE;

    if (DriverDate1->dwLowDateTime > DriverDate2->dwLowDateTime)
        return TRUE;
    if (DriverDate1->dwLowDateTime < DriverDate2->dwLowDateTime)
        return FALSE;

    if (DriverVersion1 > DriverVersion2)
        return TRUE;
    if (DriverVersion1 < DriverVersion2)
        return FALSE;

    return FALSE;
}

_Use_decl_annotations_
VOID
DriverInstallDeferredCleanup(HDEVINFO DevInfoExistingAdapters, SP_DEVINFO_DATA_LIST *ExistingAdapters)
{
    if (ExistingAdapters)
    {
        EnableAllOurAdapters(DevInfoExistingAdapters, ExistingAdapters);
        while (ExistingAdapters)
        {
            SP_DEVINFO_DATA_LIST *Next = ExistingAdapters->Next;
            Free(ExistingAdapters);
            ExistingAdapters = Next;
        }
    }
    if (DevInfoExistingAdapters != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(DevInfoExistingAdapters);
}

_Use_decl_annotations_
BOOL
DriverInstall(HDEVINFO *DevInfoExistingAdaptersForCleanup, SP_DEVINFO_DATA_LIST **ExistingAdaptersForCleanup)
{
    static const FILETIME OurDriverDate = WINTUN_INF_FILETIME;
    static const DWORDLONG OurDriverVersion = WINTUN_INF_VERSION;
    HANDLE DriverInstallationLock = NamespaceTakeDriverInstallationMutex();
    if (!DriverInstallationLock)
    {
        LOG(WINTUN_LOG_ERR, L"Failed to take driver installation mutex");
        return FALSE;
    }
    DWORD LastError = ERROR_SUCCESS;
    HDEVINFO DevInfo = SetupDiCreateDeviceInfoListExW(&GUID_DEVCLASS_NET, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create empty device information set");
        goto cleanupDriverInstallationLock;
    }
    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    if (!SetupDiCreateDeviceInfoW(DevInfo, WINTUN_HWID, &GUID_DEVCLASS_NET, NULL, NULL, DICD_GENERATE_ID, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create new device information element");
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
    FILETIME DriverDate = { 0 };
    DWORDLONG DriverVersion = 0;
    HDEVINFO DevInfoExistingAdapters = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA_LIST *ExistingAdapters = NULL;
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        SP_DRVINFO_DATA_W DrvInfoData = { .cbSize = sizeof(SP_DRVINFO_DATA_W) };
        if (!MainOperationSleep(0)) {
            LastError = GetLastError(); SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
            goto cleanupExistingAdapters;
        }
        if (!SetupDiEnumDriverInfoW(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER, EnumIndex, &DrvInfoData))
        {
            DWORD Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                break;
            /* An incomplete scan cannot establish which installed version
             * is newest or that no suitable driver exists. */
            LastError = Error;
            LOG_ERROR(Error, L"Failed to enumerate compatible drivers");
            SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
            goto cleanupExistingAdapters;
        }
        if (IsNewer(&OurDriverDate, OurDriverVersion, &DrvInfoData.DriverDate, DrvInfoData.DriverVersion))
        {
            if (DevInfoExistingAdapters == INVALID_HANDLE_VALUE)
            {
                DevInfoExistingAdapters = SetupDiGetClassDevsExW(
                    &GUID_DEVCLASS_NET, WINTUN_ENUMERATOR, NULL, DIGCF_PRESENT, NULL, NULL, NULL);
                if (DevInfoExistingAdapters == INVALID_HANDLE_VALUE)
                {
                    LastError = LOG_LAST_ERROR(L"Failed to get present adapters");
                    SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
                    goto cleanupExistingAdapters;
                }
                _Analysis_assume_(DevInfoExistingAdapters != NULL);
                DisableAllOurAdapters(DevInfoExistingAdapters, &ExistingAdapters);
                LOG(WINTUN_LOG_INFO, L"Waiting for existing driver to unload from kernel");
                if (!EnsureWintunUnloaded())
                    LOG(WINTUN_LOG_WARN,
                        L"Failed to unload existing driver, which means a reboot will likely be required");
                if (!MainOperationSleep(0)) {
                    LastError = GetLastError(); SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
                    goto cleanupExistingAdapters;
                }
            }
            LOG(WINTUN_LOG_INFO,
                L"Removing existing driver %u.%u",
                (DWORD)((DrvInfoData.DriverVersion & 0xffff000000000000) >> 48),
                (DWORD)((DrvInfoData.DriverVersion & 0x0000ffff00000000) >> 32));
            BYTE LargeBuffer[0x2000];
            DWORD Size = sizeof(LargeBuffer);
            SP_DRVINFO_DETAIL_DATA_W *DrvInfoDetailData = (SP_DRVINFO_DETAIL_DATA_W *)LargeBuffer;
            DrvInfoDetailData->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
            if (!SetupDiGetDriverInfoDetailW(DevInfo, &DevInfoData, &DrvInfoData, DrvInfoDetailData, Size, &Size))
            {
                /* Insufficient tail space for hardware IDs still guarantees
                 * all static fields, including InfFileName, are returned. */
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                {
                    LOG(WINTUN_LOG_WARN, L"Failed getting adapter driver info detail");
                    continue;
                }
            }
            LPWSTR InfFileName = PathFindFileNameW(DrvInfoDetailData->InfFileName);
            if (!SetupUninstallOEMInfW(InfFileName, SUOI_FORCEDELETE, NULL))
                LOG_LAST_ERROR(L"Unable to remove existing driver %s", InfFileName);
            continue;
        }
        if (!IsNewer(&DrvInfoData.DriverDate, DrvInfoData.DriverVersion, &DriverDate, DriverVersion))
            continue;
        DriverDate = DrvInfoData.DriverDate;
        DriverVersion = DrvInfoData.DriverVersion;
    }
    SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);

    if (DriverVersion)
    {
        LOG(WINTUN_LOG_INFO,
            L"Using existing driver %u.%u",
            (DWORD)((DriverVersion & 0xffff000000000000) >> 48),
            (DWORD)((DriverVersion & 0x0000ffff00000000) >> 32));
        LastError = ERROR_SUCCESS;
        goto cleanupExistingAdapters;
    }

    LOG(WINTUN_LOG_INFO,
        L"Installing driver %u.%u",
        (DWORD)((OurDriverVersion & 0xffff000000000000) >> 48),
        (DWORD)((OurDriverVersion & 0x0000ffff00000000) >> 32));
    WCHAR RandomTempSubDirectory[MAX_PATH];
    if (!ResourceCreateTemporaryDirectory(RandomTempSubDirectory))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create temporary folder %s", RandomTempSubDirectory);
        goto cleanupExistingAdapters;
    }

    WCHAR CatPath[MAX_PATH] = { 0 };
    WCHAR SysPath[MAX_PATH] = { 0 };
    WCHAR InfPath[MAX_PATH] = { 0 };
    if (!PathCombineW(CatPath, RandomTempSubDirectory, L"packetbridge.cat") ||
        !PathCombineW(SysPath, RandomTempSubDirectory, L"packetbridge.sys") ||
        !PathCombineW(InfPath, RandomTempSubDirectory, L"packetbridge.inf"))
    {
        LastError = ERROR_BUFFER_OVERFLOW;
        goto cleanupDirectory;
    }

    WCHAR *CatSource, *SysSource, *InfSource;
    if (NativeMachine == IMAGE_FILE_PROCESS)
    {
        CatSource = L"packetbridge.cat";
        SysSource = L"packetbridge.sys";
        InfSource = L"packetbridge.inf";
    }
    else if (NativeMachine == IMAGE_FILE_MACHINE_AMD64)
    {
        CatSource = L"wintun-amd64.cat";
        SysSource = L"wintun-amd64.sys";
        InfSource = L"wintun-amd64.inf";
    }
    else if (NativeMachine == IMAGE_FILE_MACHINE_ARM64)
    {
        CatSource = L"wintun-arm64.cat";
        SysSource = L"wintun-arm64.sys";
        InfSource = L"wintun-arm64.inf";
    }
    else
    {
        LastError = LOG_ERROR(ERROR_NOT_SUPPORTED, L"Unsupported platform 0x%x", NativeMachine);
        goto cleanupDirectory;
    }

    LOG(WINTUN_LOG_INFO, L"Extracting driver");
    if (!ResourceCopyToFile(CatPath, CatSource) || !ResourceCopyToFile(SysPath, SysSource) ||
        !ResourceCopyToFile(InfPath, InfSource))
    {
        LastError = LOG_LAST_ERROR(L"Failed to extract driver");
        goto cleanupDelete;
    }

    LOG(WINTUN_LOG_INFO, L"Installing driver");
    if (!SetupCopyOEMInfW(InfPath, NULL, SPOST_NONE, 0, NULL, 0, NULL, NULL))
        LastError = LOG_LAST_ERROR(L"Could not install driver %s to store", InfPath);

cleanupDelete:
    DeleteFileW(CatPath);
    DeleteFileW(SysPath);
    DeleteFileW(InfPath);
cleanupDirectory:
    RemoveDirectoryW(RandomTempSubDirectory);
cleanupExistingAdapters:
    if (LastError == ERROR_SUCCESS)
    {
        *DevInfoExistingAdaptersForCleanup = DevInfoExistingAdapters;
        *ExistingAdaptersForCleanup = ExistingAdapters;
    }
    else
        DriverInstallDeferredCleanup(DevInfoExistingAdapters, ExistingAdapters);
cleanupDevInfo:
    SetupDiDestroyDeviceInfoList(DevInfo);
cleanupDriverInstallationLock:
    NamespaceReleaseMutex(DriverInstallationLock);
    return RET_ERROR(TRUE, LastError);
}

static BOOL DeleteDriverInternal(VOID)
{
    if (!MainRuntimeAcquire()) return FALSE;
    DWORD LastError = ERROR_SUCCESS;

    AdapterCleanupOrphanedDevices();

    HANDLE DriverInstallationLock = NamespaceTakeDriverInstallationMutex();
    if (!DriverInstallationLock)
    {
        LastError = LOG(WINTUN_LOG_ERR, L"Failed to take driver installation mutex");
        goto cleanup;
    }

    HDEVINFO DevInfo = SetupDiCreateDeviceInfoListExW(&GUID_DEVCLASS_NET, NULL, NULL, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create empty device information set");
        goto cleanupDriverInstallationLock;
    }
    SP_DEVINFO_DATA DevInfoData = { .cbSize = sizeof(DevInfoData) };
    if (!SetupDiCreateDeviceInfoW(DevInfo, WINTUN_HWID, &GUID_DEVCLASS_NET, NULL, NULL, DICD_GENERATE_ID, &DevInfoData))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create new device information element");
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
    for (DWORD EnumIndex = 0;; ++EnumIndex)
    {
        SP_DRVINFO_DATA_W DrvInfoData = { .cbSize = sizeof(SP_DRVINFO_DATA_W) };
        if (!MainOperationSleep(0)) { LastError = GetLastError(); break; }
        if (!SetupDiEnumDriverInfoW(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER, EnumIndex, &DrvInfoData))
        {
            DWORD Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                break;
            if (LastError == ERROR_SUCCESS) LastError = Error;
            LOG_ERROR(Error, L"Failed to enumerate compatible drivers");
            break;
        }
        BYTE LargeBuffer[0x2000];
        DWORD Size = sizeof(LargeBuffer);
        SP_DRVINFO_DETAIL_DATA_W *DrvInfoDetailData = (SP_DRVINFO_DETAIL_DATA_W *)LargeBuffer;
        DrvInfoDetailData->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        if (!SetupDiGetDriverInfoDetailW(DevInfo, &DevInfoData, &DrvInfoData, DrvInfoDetailData, Size, &Size))
        {
            DWORD Error = GetLastError();
            if (Error != ERROR_INSUFFICIENT_BUFFER)
            {
                if (LastError == ERROR_SUCCESS) LastError = Error;
                LOG_ERROR(Error, L"Failed getting adapter driver info detail");
                continue;
            }
        }
        LPCWSTR Path = PathFindFileNameW(DrvInfoDetailData->InfFileName);
        LOG(WINTUN_LOG_INFO, L"Removing driver %s", Path);
        if (!SetupUninstallOEMInfW(Path, 0, NULL))
        {
            LOG_LAST_ERROR(L"Unable to remove driver %s", Path);
            LastError = LastError != ERROR_SUCCESS ? LastError : GetLastError();
        }
    }
    SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
cleanupDevInfo:
    SetupDiDestroyDeviceInfoList(DevInfo);
cleanupDriverInstallationLock:
    NamespaceReleaseMutex(DriverInstallationLock);
cleanup:
    MainRuntimeRelease();
    return RET_ERROR(TRUE, LastError);
}

