/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <setupapi.h>
#include <devguid.h>
#include <devpkey.h>
#include <newdev.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include "adapter_diagnostics.h"

#define PB_RECOVERY_LIMIT 4096u
static const WCHAR *PbRecoveryOemName(const WCHAR *path)
{
    const WCHAR *name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    if (_wcsnicmp(name, L"oem", 3)) return NULL;
    const WCHAR *at = name + 3;
    if (*at < L'0' || *at > L'9') return NULL;
    while (*at >= L'0' && *at <= L'9') ++at;
    return !_wcsicmp(at, L".inf") ? name : NULL;
}
static BOOL PbNamedAdapter(HDEVINFO set, SP_DEVINFO_DATA *device, const WCHAR *name)
{
    if (!IsEqualGUID(&device->ClassGuid, &GUID_DEVCLASS_NET)) return FALSE;
    WCHAR value[256]; DWORD type, bytes;
    /* Device Manager displays FriendlyName, falling back to DeviceDesc. Do
     * not broaden a nonmatching friendly name to a shared driver description. */
    if (!SetupDiGetDeviceRegistryPropertyW(set, device, SPDRP_FRIENDLYNAME, &type, (BYTE *)value, sizeof(value), &bytes)) {
        if (GetLastError() != ERROR_INVALID_DATA ||
            !SetupDiGetDeviceRegistryPropertyW(set, device, SPDRP_DEVICEDESC, &type, (BYTE *)value, sizeof(value), &bytes)) return FALSE;
    }
    return type == REG_SZ && bytes >= sizeof(WCHAR) && bytes <= sizeof(value) && !(bytes % sizeof(WCHAR)) &&
        !value[bytes / sizeof(WCHAR) - 1] && !_wcsicmp(value, name);
}
typedef struct PB_RECOVERY_INF {
    struct PB_RECOVERY_INF *Next;
    WCHAR Published[MAX_PATH], Store[MAX_PATH];
} PB_RECOVERY_INF;
static void PbSnapshotBoundInf(HDEVINFO set, SP_DEVINFO_DATA *device, PB_RECOVERY_INF **packages,
    PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    WCHAR bound[MAX_PATH], root[MAX_PATH], published[MAX_PATH]; DEVPROPTYPE type; DWORD bytes;
    if (!SetupDiGetDevicePropertyW(set, device, &DEVPKEY_Device_DriverInfPath, &type,
        (BYTE *)bound, sizeof(bound), &bytes, 0)) {
        DWORD error = GetLastError();
        /* A failed, not-yet-bound device legitimately has no driver INF. */
        if (error != ERROR_INVALID_DATA && error != ERROR_NOT_FOUND)
            PbRecoveryFailure(diagnostics, PB_RECOVERY_READ_INF, error);
        return;
    }
    if (type != DEVPROP_TYPE_STRING || bytes < sizeof(WCHAR) || bytes > sizeof(bound) ||
        bytes % sizeof(WCHAR) || bound[bytes / sizeof(WCHAR) - 1]) {
        PbRecoveryFailure(diagnostics, PB_RECOVERY_READ_INF, ERROR_INVALID_DATA); return;
    }
    const WCHAR *oem = PbRecoveryOemName(bound);
    if (!oem) { PbRecoveryFailure(diagnostics, PB_RECOVERY_READ_INF, ERROR_INVALID_DATA); return; }
    UINT length = GetSystemWindowsDirectoryW(root, _countof(root));
    if (!length) { PbRecoveryFailure(diagnostics, PB_RECOVERY_INF_PATH, GetLastError()); return; }
    if (length >= _countof(root) || swprintf_s(published, _countof(published), L"%s\\INF\\%s", root, oem) < 0) {
        PbRecoveryFailure(diagnostics, PB_RECOVERY_INF_PATH, ERROR_FILENAME_EXCED_RANGE); return;
    }
    for (PB_RECOVERY_INF *item = *packages; item; item = item->Next)
        if (!_wcsicmp(item->Published, published)) return;
    PB_RECOVERY_INF *item = calloc(1, sizeof(*item));
    if (!item) { PbRecoveryFailure(diagnostics, PB_RECOVERY_INF_PATH, ERROR_NOT_ENOUGH_MEMORY); return; }
    wcscpy_s(item->Published, _countof(item->Published), published);
    /* Resolve while the device is still bound. A missing store mapping must
     * not block a native uninstall using the known published OEM path. */
    if (!SetupGetInfDriverStoreLocationW(published, NULL, NULL, item->Store, _countof(item->Store), NULL)) item->Store[0] = 0;
    item->Next = *packages; *packages = item;
    PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
    if (details) details->FoundPackages++;
}
static BOOL PbMissingInf(const WCHAR *path)
{
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return FALSE;
    DWORD error = GetLastError(); return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
static void PbRemoveBoundInf(PB_RECOVERY_INF *item, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    BOOL deferred = FALSE;
    const WCHAR *path = item->Store[0] ? item->Store : item->Published;
    BOOL okay = DiUninstallDriverW(NULL, path, 0, &deferred);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (deferred) diagnostics->NeedReboot = TRUE;
    /* A stale store mapping cannot suppress the direct OEM uninstall. */
    if (!okay && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && _wcsicmp(path, item->Published)) {
        deferred = FALSE; okay = DiUninstallDriverW(NULL, item->Published, 0, &deferred);
        error = okay ? ERROR_SUCCESS : GetLastError();
        if (deferred) diagnostics->NeedReboot = TRUE;
    }
    if (okay) diagnostics->RemovedPackages++;
    else if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && PbMissingInf(item->Published) &&
        (!item->Store[0] || PbMissingInf(item->Store))) {
        PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
        if (details) details->AlreadyAbsentPackages++;
    } else PbRecoveryFailure(diagnostics, PB_RECOVERY_REMOVE_DRIVER, error);
}
static BOOL PbRemoveNamedAdapter(const WCHAR *name, const PB_DRIVER_BINDING *binding, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    diagnostics->Stage = PB_ADAPTER_REMOVE_DEVICES;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    if (set == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError(); PbRecoveryFailure(diagnostics, PB_RECOVERY_OPEN_DEVICES, error);
        SetLastError(error); return FALSE;
    }
    SP_DEVINFO_DATA *targets = calloc(PB_RECOVERY_LIMIT, sizeof(*targets));
    PB_RECOVERY_INF *packages = NULL; DWORD count = 0;
    /* The failed devnode may already be gone after its callback close barrier.
     * Its name-validated bound package was captured before that barrier. */
    if (binding && binding->Published[0]) {
        packages = calloc(1, sizeof(*packages));
        if (!packages) { PbRecoveryFailure(diagnostics, PB_RECOVERY_INF_PATH, ERROR_NOT_ENOUGH_MEMORY); goto done; }
        wcscpy_s(packages->Published, _countof(packages->Published), binding->Published);
        wcscpy_s(packages->Store, _countof(packages->Store), binding->Store);
        PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
        if (details) details->FoundPackages++;
    }
    if (!targets) { PbRecoveryFailure(diagnostics, PB_RECOVERY_SCAN_DEVICES, ERROR_NOT_ENOUGH_MEMORY); goto done; }
    /* Capture the named devices and their actual bound INF before the first
     * removal. No compatible-driver ranking or global OEM package search. */
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA device = { .cbSize = sizeof(device) };
        if (!SetupDiEnumDeviceInfo(set, i, &device)) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) PbRecoveryFailure(diagnostics, PB_RECOVERY_SCAN_DEVICES, error);
            break;
        }
        if (i == PB_RECOVERY_LIMIT) { PbRecoveryFailure(diagnostics, PB_RECOVERY_SCAN_DEVICES, ERROR_NO_SYSTEM_RESOURCES); break; }
        if (!PbNamedAdapter(set, &device, name)) continue;
        targets[count++] = device;
        PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
        if (details) details->MatchedDevices++;
        PbSnapshotBoundInf(set, &device, &packages, diagnostics);
    }
    for (DWORD i = 0; i < count; ++i) {
        SP_DEVINFO_DATA *device = targets + i;
        if (!PbNamedAdapter(set, device, name)) continue;
        SP_DEVINSTALL_PARAMS_W parameters = { .cbSize = sizeof(parameters) };
        BOOL deferred = FALSE;
        BOOL okay = SetupDiGetDeviceInstallParamsW(set, device, &parameters);
        if (okay) { parameters.Flags |= DI_QUIETINSTALL; okay = SetupDiSetDeviceInstallParamsW(set, device, &parameters); }
        if (okay) okay = DiUninstallDevice(NULL, set, device, 0, &deferred);
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (deferred) diagnostics->NeedReboot = TRUE;
        if (okay) diagnostics->RemovedDevices++;
        else PbRecoveryFailure(diagnostics, PB_RECOVERY_REMOVE_DEVICE, error);
    }
    diagnostics->Stage = PB_ADAPTER_REMOVE_PACKAGES;
    for (PB_RECOVERY_INF *item = packages; item; item = item->Next) PbRemoveBoundInf(item, diagnostics);
done:
    while (packages) { PB_RECOVERY_INF *next = packages->Next; free(packages); packages = next; }
    free(targets); SetupDiDestroyDeviceInfoList(set);
    SetLastError(diagnostics->RecoveryError); return !diagnostics->RecoveryError;
}
