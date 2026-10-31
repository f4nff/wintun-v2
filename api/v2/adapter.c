/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include "adapter.h"
#include "driver_package.h"
#include "adapter_diagnostics.h"
#include <setupapi.h>
#include <swdevice.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <initguid.h>
#include <devguid.h>
#include <ndisguid.h>
#include <devpkey.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include "../../include/packetbridge_version.h"

struct PB_ADAPTER {
    volatile LONG References;
    HSWDEVICE Device;
    WCHAR *Path;
    NET_LUID Luid;
    BOOL Admitted;
};
#include "adapter_create_wait.h"
void PbAdapterRetain(PB_ADAPTER *adapter) { InterlockedIncrement(&adapter->References); }
void WINAPI PbAdapterRelease(PB_ADAPTER *adapter)
{
    if (!adapter || InterlockedDecrement(&adapter->References)) return;
    if (adapter->Device) SwDeviceClose(adapter->Device);
    if (adapter->Admitted) PbRuntimeReleaseAdapter();
    free(adapter->Path); free(adapter);
}
const wchar_t *PbAdapterPath(PB_ADAPTER *adapter) { return adapter->Path; }
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter, uint64_t *luid)
{
    if (!adapter || !luid) return PB_INVALID;
    *luid = adapter->Luid.Value; return PB_OK;
}
static BOOL MatchingDriver(const WCHAR *path, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) return FALSE;
    PB_PROBE_STATUS status = { 0 }; DWORD returned = 0;
    BOOL okay = DeviceIoControl(device, PB_PROBE_IOCTL_QUERY, NULL, 0, &status, sizeof(status), &returned, NULL);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);
    if (okay) {
        uint32_t mismatch = (returned != sizeof(status) ? 1u : 0) | (status.Size != sizeof(status) ? 2u : 0) |
            (status.Magic != PB_PROBE_MAGIC ? 4u : 0) | (status.Schema != PB_PROBE_SCHEMA ? 8u : 0) |
            (status.FileVersion != PB_VERSION_PACKED ? 16u : 0) | (status.DataAbi != PB_ABI_MAJOR ? 32u : 0) |
            (strlen(PB_SOURCE_STRING) != 64 || memcmp(status.SourceFingerprint, PB_SOURCE_STRING, 65) ? 64u : 0);
        PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
        if (details) {
            details->ProbeMismatchMask = mismatch; details->ObservedVersion = status.FileVersion;
            details->ObservedDataAbi = status.DataAbi; details->ObservedProbeSchema = status.Schema;
            memcpy(details->ObservedSource, status.SourceFingerprint, 64); details->ObservedSource[64] = 0;
        }
        if (mismatch) { okay = FALSE; error = ERROR_REVISION_MISMATCH; }
    }
    SetLastError(error); return okay;
}
static void ReadCreateProblem(const WCHAR *instance, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    if (!instance[0]) return;
    HDEVINFO devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (devices == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA device = { .cbSize = sizeof(device) };
    if (SetupDiOpenDeviceInfoW(devices, instance, NULL, 0, &device)) {
        DEVPROPTYPE type; DWORD value, bytes;
        if (SetupDiGetDevicePropertyW(devices, &device, &DEVPKEY_Device_ProblemCode, &type, (BYTE *)&value, sizeof(value), &bytes, 0) &&
            type == DEVPROP_TYPE_UINT32 && bytes == sizeof(value)) { diagnostics->ProblemCode = value; diagnostics->ProblemFieldsKnown |= 1; }
        if (SetupDiGetDevicePropertyW(devices, &device, &DEVPKEY_Device_ProblemStatus, &type, (BYTE *)&value, sizeof(value), &bytes, 0) &&
            type == DEVPROP_TYPE_NTSTATUS && bytes == sizeof(value)) { diagnostics->ProblemStatus = value; diagnostics->ProblemFieldsKnown |= 2; }
    }
    SetupDiDestroyDeviceInfoList(devices);
}
static BOOL FindInterface(PB_ADAPTER *adapter, const WCHAR *instance, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_NET, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE; DWORD error = ERROR_NOT_FOUND;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface = { .cbSize = sizeof(iface) };
        if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_NET, i, &iface)) {
            DWORD result = GetLastError(); if (result != ERROR_NO_MORE_ITEMS) error = result; break;
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, NULL, 0, &size, NULL);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(size);
        if (!detail) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info = { .cbSize = sizeof(info) };
        WCHAR id[1024], service[256] = { 0 };
        BOOL matched = SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, NULL, &info) &&
            SetupDiGetDeviceInstanceIdW(devices, &info, id, _countof(id), NULL) && !_wcsicmp(id, instance);
        if (matched) {
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_SERVICE, NULL, (BYTE *)service, sizeof(service) - sizeof(WCHAR), NULL))
                error = GetLastError();
            else if (_wcsicmp(service, L"packetbridge")) error = ERROR_REVISION_MISMATCH;
            else {
                HKEY key = SetupDiOpenDevRegKey(devices, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
                if (key == INVALID_HANDLE_VALUE) error = GetLastError();
                else {
                    WCHAR guid[64] = { 0 }; DWORD bytes = sizeof(guid);
                    error = RegGetValueW(key, NULL, L"NetCfgInstanceId", RRF_RT_REG_SZ, NULL, guid, &bytes);
                    if (!error) {
                        GUID parsed; HRESULT result = CLSIDFromString(guid, &parsed);
                        if (FAILED(result)) { diagnostics->HResult = (uint32_t)result; error = ERROR_INVALID_DATA; }
                        else error = ConvertInterfaceGuidToLuid(&parsed, &adapter->Luid);
                        if (!error && !MatchingDriver(detail->DevicePath, diagnostics)) error = GetLastError();
                        if (!error) {
                            adapter->Path = _wcsdup(detail->DevicePath);
                            found = adapter->Path != NULL; if (!found) error = ERROR_NOT_ENOUGH_MEMORY;
                        }
                    }
                    RegCloseKey(key);
                }
            }
        }
        free(detail);
        if (matched) break; /* Keep the matching device's original error. */
    }
    SetupDiDestroyDeviceInfoList(devices); SetLastError(found ? ERROR_SUCCESS : error); return found;
}
static PB_RESULT CreateAdapterOnce(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *diagnostics, PB_DRIVER_BINDING *binding)
{
    diagnostics->Stage = PB_ADAPTER_STAGE_DRIVER;
    if (!PbEnsureDriverPackage(diagnostics)) { diagnostics->Win32Error = GetLastError(); return PB_UNSUPPORTED; }
    PB_ADAPTER *adapter = calloc(1, sizeof(*adapter));
    if (!adapter) { diagnostics->Win32Error = ERROR_NOT_ENOUGH_MEMORY; return PB_FULL; }
    adapter->References = 1;
    PB_CREATE ctx = { .Event = CreateEventW(NULL, TRUE, FALSE, NULL), .Result = E_PENDING };
    if (!ctx.Event) { diagnostics->Win32Error = GetLastError(); PbAdapterRelease(adapter); return PB_FULL; }
    SW_DEVICE_CREATE_INFO info = { .cbSize = sizeof(info), .pszInstanceId = instance, .pszzHardwareIds = L"PacketBridgeV2\0",
        .CapabilityFlags = SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired, .pszDeviceDescription = name };
    DEVPROPERTY properties[] = {
        { .CompKey = { .Key = DEVPKEY_Device_ClassGuid, .Store = DEVPROP_STORE_SYSTEM }, .Type = DEVPROP_TYPE_GUID,
          .BufferSize = sizeof(GUID_DEVCLASS_NET), .Buffer = (PVOID)&GUID_DEVCLASS_NET },
        { .CompKey = { .Key = DEVPKEY_Device_FriendlyName, .Store = DEVPROP_STORE_SYSTEM }, .Type = DEVPROP_TYPE_STRING,
          .BufferSize = (ULONG)((wcslen(name) + 1) * sizeof(WCHAR)), .Buffer = (PVOID)name }
    };
    diagnostics->Stage = PB_ADAPTER_CREATE_DEVICE;
    HRESULT result = SwDeviceCreate(L"PacketBridgeV2", L"HTREE\\ROOT\\0", &info, _countof(properties), properties, Created, &ctx, &adapter->Device);
    if (SUCCEEDED(result)) { diagnostics->Stage = PB_ADAPTER_WAIT_DEVICE; result = PbWaitForAdapterCreate(adapter, &ctx); }
    diagnostics->HResult = (uint32_t)result;
    DWORD error = SUCCEEDED(result) ? ERROR_NOT_FOUND : HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : (DWORD)result;
    BOOL found = FALSE;
    if (SUCCEEDED(result)) {
        diagnostics->Stage = PB_ADAPTER_FIND_INTERFACE;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            SetLastError(ERROR_NOT_FOUND);
            found = FindInterface(adapter, ctx.Instance, diagnostics);
            if (found) break;
            error = GetLastError();
            if (error != ERROR_NOT_FOUND && error != ERROR_NO_MORE_ITEMS && error != ERROR_PATH_NOT_FOUND) break;
            Sleep(50);
        }
    }
    if (!found) {
        diagnostics->Win32Error = error;
        ReadCreateProblem(ctx.Instance, diagnostics);
        /* Retain the failed named device's actual INF before SwDeviceClose
         * can remove its devnode and erase the binding properties. */
        PbRememberAdapterBinding(name, ctx.Instance, binding);
        // The callback context and event remain live until the close barrier.
        PbAdapterRelease(adapter); CloseHandle(ctx.Event); SetLastError(error); return PB_UNSUPPORTED;
    }
    CloseHandle(ctx.Event); *out = adapter; return PB_OK;
}
#include "adapter_install.h"
static PB_RESULT CreateAdapterDetailed(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *diagnostics, uint32_t size, uint32_t schema)
{
    if (out) *out = NULL;
    if (!diagnostics || diagnostics->Size != size || diagnostics->Schema != schema) {
        SetLastError(ERROR_INVALID_PARAMETER); return PB_INVALID;
    }
    memset(diagnostics, 0, size);
    diagnostics->Size = size; diagnostics->Schema = schema;
    diagnostics->Win32Error = ERROR_INVALID_PARAMETER;
    if (!out || !name || !instance) return PB_INVALID;
    size_t nameLength = wcsnlen_s(name, 129), idLength = wcsnlen_s(instance, 129);
    if (!nameLength || nameLength > 128 || !idLength || idLength > 128) return PB_INVALID;
    for (size_t i = 0; i < idLength; ++i) {
        WCHAR c = instance[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')) return PB_INVALID;
    }
    if (!PbRuntimeReserveAdapter()) { diagnostics->Win32Error = ERROR_NO_SYSTEM_RESOURCES; return PB_FULL; }
    diagnostics->Stage = PB_ADAPTER_LOCK;
    HANDLE mutex = PbLockDriverOperations();
    if (!mutex) {
        DWORD error = GetLastError(); diagnostics->Win32Error = error; PbRuntimeReleaseAdapter(); SetLastError(error); return PB_UNSUPPORTED;
    }
    PB_DRIVER_BINDING binding = { 0 };
    PB_RESULT status = PbCreateWithRecovery(name, instance, out, diagnostics, CreateAdapterOnce, PbRepairDriverPackage, &binding);
    if (status == PB_OK) { (*out)->Admitted = TRUE; diagnostics->Win32Error = 0; }
    else PbRuntimeReleaseAdapter();
    SetLastError(diagnostics->Win32Error); PbUnlockDriverOperations(mutex); return status;
}
PB_RESULT WINAPI PbCreateAdapterEx(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    return CreateAdapterDetailed(name, instance, out, diagnostics, sizeof(*diagnostics), PB_ADAPTER_DIAG_SCHEMA);
}
PB_RESULT WINAPI PbCreateAdapterEx2(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS_EX *diagnostics)
{
    return CreateAdapterDetailed(name, instance, out, diagnostics ? &diagnostics->Base : NULL,
        sizeof(*diagnostics), PB_ADAPTER_DIAG_SCHEMA_EX);
}
PB_RESULT WINAPI PbAdapterCreate(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out)
{
    PB_ADAPTER_DIAGNOSTICS diagnostics = { .Size = sizeof(diagnostics), .Schema = PB_ADAPTER_DIAG_SCHEMA };
    return PbCreateAdapterEx(name, instance, out, &diagnostics);
}
