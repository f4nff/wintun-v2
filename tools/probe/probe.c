/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <swdevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <initguid.h>
#include <devguid.h>
#include <ndisguid.h>
#include <devpkey.h>
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_version.h"
#include "return_diagnostics.h"
#include "send_diagnostics.h"
#include "pool_pressure.h"

_Static_assert(sizeof(PB_PROBE_STATUS) == 232, "probe status layout");
_Static_assert(sizeof(PB_PROBE_POLICY) == 16, "probe policy layout");

static HANDLE StopEvent;
static int InspectPoolPressure(const wchar_t *path)
{
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) { fprintf(stderr, "Open pool pressure device failed: %lu\n", GetLastError()); return 1; }
    PB_POOL_PRESSURE_SNAPSHOT snapshot; DWORD returned = 0;
    BOOL okay = DeviceIoControl(device, PB_POOL_PRESSURE_IOCTL, NULL, 0, &snapshot, sizeof(snapshot), &returned, NULL);
    DWORD error = okay ? ERROR_INVALID_DATA : GetLastError(); CloseHandle(device);
    if (!okay || returned != sizeof(snapshot)) { fprintf(stderr, "Pool pressure diagnostics unavailable/truncated: %lu; use matching SYS.\n", error); return 1; }
    int result = PbPrintPoolPressure(&snapshot);
    if (result == 1) fputs("Pool pressure diagnostic ABI validation failed.\n", stderr);
    return result;
}
static int InspectSends(const wchar_t *path)
{
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) { fprintf(stderr, "Open send diagnostic device failed: %lu\n", GetLastError()); return 1; }
    PB_SEND_SNAPSHOT snapshot; DWORD returned = 0;
    BOOL okay = DeviceIoControl(device, PB_SEND_DIAG_IOCTL, NULL, 0, &snapshot, sizeof(snapshot), &returned, NULL);
    DWORD error = okay ? ERROR_INVALID_DATA : GetLastError(); CloseHandle(device);
    if (!okay || returned != sizeof(snapshot)) { fprintf(stderr, "Send diagnostics unavailable/truncated: %lu; use matching SYS.\n", error); return 1; }
    int result = PbPrintSendSnapshot(&snapshot);
    if (result == 1) fputs("Send diagnostic ABI validation failed.\n", stderr);
    return result;
}
static int InspectReturns(const wchar_t *path)
{
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) { fprintf(stderr, "Open diagnostic device failed: %lu\n", GetLastError()); return 1; }
    PB_RETURN_SNAPSHOT snapshot;
    DWORD returned = 0;
    BOOL okay = DeviceIoControl(device, PB_RETURN_DIAG_IOCTL, NULL, 0, &snapshot, sizeof(snapshot), &returned, NULL);
    DWORD error = okay ? ERROR_INVALID_DATA : GetLastError();
    CloseHandle(device);
    if (!okay || returned != sizeof(snapshot)) {
        fprintf(stderr, "Receive return diagnostics unavailable/truncated: %lu; use the matching diagnostic SYS.\n", error);
        return 1;
    }
    int result = PbPrintReturnSnapshot(&snapshot);
    if (result == 1) fputs("Receive return diagnostic ABI validation failed.\n", stderr);
    return result;
}
static BOOL WINAPI StopHandler(DWORD reason)
{
    if (reason == CTRL_C_EVENT || reason == CTRL_BREAK_EVENT || reason == CTRL_CLOSE_EVENT) {
        if (StopEvent) SetEvent(StopEvent);
        return TRUE;
    }
    return FALSE;
}

static int ListDevices(void)
{
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_NET, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return 1;
    unsigned count = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA iface = { .cbSize = sizeof(iface) };
        if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_NET, i, &iface)) break;
        DWORD size = 0;
        SP_DEVINFO_DATA info = { .cbSize = sizeof(info) };
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, NULL, 0, &size, NULL);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(size);
        if (!detail) { SetupDiDestroyDeviceInfoList(devices); return 1; }
        detail->cbSize = sizeof(*detail);
        WCHAR service[256] = { 0 };
        if (SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, NULL, &info) &&
            SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_SERVICE, NULL, (BYTE *)service, sizeof(service), NULL) &&
            !_wcsicmp(service, L"packetbridge")) {
            wprintf(L"%ls\n", detail->DevicePath);
            count++;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (!count) puts("No PacketBridgeV2 adapter is installed. No system changes were made.");
    return 0;
}

static int InspectDevice(const wchar_t *path, const wchar_t *maskString)
{
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
    if (device == INVALID_HANDLE_VALUE) { fprintf(stderr, "Open device failed: %lu\n", GetLastError()); return 1; }
    DWORD returned = 0;
    PB_PROBE_STATUS status;
    if (!DeviceIoControl(device, PB_PROBE_IOCTL_QUERY, NULL, 0, &status, sizeof(status), &returned, NULL) ||
        returned != sizeof(status) || status.Size != sizeof(status) || status.Magic != PB_PROBE_MAGIC ||
        status.Schema != PB_PROBE_SCHEMA) {
        fprintf(stderr, "Probe query/ABI validation failed: %lu\n", GetLastError()); CloseHandle(device); return 1;
    }
    BOOL matchingBuild = status.FileVersion == PB_VERSION_PACKED &&
        !strncmp(status.SourceFingerprint, PB_SOURCE_STRING, sizeof(status.SourceFingerprint));
    if (maskString) {
        if (!matchingBuild) { fputs("Build mismatch; refusing to change driver policy.\n", stderr); CloseHandle(device); return 3; }
        WCHAR *end;
        unsigned long mask = wcstoul(maskString, &end, 0);
        if (*end || end == maskString || mask & ~PB_PROBE_SUPPORTED) { CloseHandle(device); return 2; }
        PB_PROBE_POLICY policy = { sizeof(policy), PB_PROBE_SCHEMA, mask, 0 };
        if (!DeviceIoControl(device, PB_PROBE_IOCTL_POLICY, &policy, sizeof(policy), NULL, 0, &returned, NULL)) {
            fprintf(stderr, "Set policy failed: %lu\n", GetLastError()); CloseHandle(device); return 1;
        }
    }
    BOOL success = DeviceIoControl(device, PB_PROBE_IOCTL_QUERY, NULL, 0, &status, sizeof(status), &returned, NULL);
    DWORD error = GetLastError();
    CloseHandle(device);
    if (!success) { fprintf(stderr, "Query failed: %lu\n", error); return 1; }
    if (returned != sizeof(status) || status.Size != sizeof(status) || status.Magic != PB_PROBE_MAGIC || status.Schema != PB_PROBE_SCHEMA) {
        fputs("Probe ABI mismatch. Use the matching driver/tool pair.\n", stderr); return 1;
    }
    printf("{\n  \"tool_version\": \"%s\",\n  \"driver_version_packed\": %u,\n", PB_VERSION_STRING, status.FileVersion);
    printf("  \"matching_build\": %s,\n  \"driver_source_sha256\": \"%.64s\",\n", matchingBuild ? "true" : "false", status.SourceFingerprint);
    printf("  \"ndis_major\": %u,\n  \"ndis_minor\": %u,\n  \"medium\": %u,\n  \"data_abi\": %u,\n", status.NdisVersion >> 16, status.NdisVersion & 255, status.Medium, status.DataAbi);
    printf("  \"implemented_mask\": %u,\n  \"allowed_mask\": %u,\n  \"ndis_mask\": %u,\n  \"effective_mask\": %u,\n", status.ImplementedMask, status.AllowedMask, status.NdisMask, status.EffectiveMask);
    printf("  \"session_registered\": %s,\n  \"rx_offloads_implemented\": %s,\n", status.SessionRegistered ? "true" : "false", status.RxOffloadsImplemented ? "true" : "false");
    printf("  \"rsc_packets\": %llu,\n  \"rsc_events\": %llu,\n  \"uro_packets\": %llu,\n  \"uro_events\": %llu,\n", (unsigned long long)status.RscPackets, (unsigned long long)status.RscEvents, (unsigned long long)status.UroPackets, (unsigned long long)status.UroEvents);
    printf("  \"rsc_bytes\": %llu,\n  \"rsc_aborts\": %llu,\n", (unsigned long long)status.RscBytes, (unsigned long long)status.RscAborts);
    printf("  \"last_ipv4_encapsulation\": %u,\n  \"last_ipv6_encapsulation\": %u,\n", status.LastIpv4Encapsulation, status.LastIpv6Encapsulation);
    printf("  \"offload_registration_status\": %u,\n", status.OffloadRegistrationStatus);
    printf("  \"lso_nbls\": %llu,\n  \"uso_nbls\": %llu,\n  \"checksum_nbls\": %llu,\n", (unsigned long long)status.LsoNbls, (unsigned long long)status.UsoNbls, (unsigned long long)status.ChecksumNbls);
    printf("  \"offload_rejected\": %llu,\n  \"segments_copied\": %llu,\n  \"parameter_changes\": %llu\n}\n", (unsigned long long)status.OffloadRejected, (unsigned long long)status.SegmentsCopied, (unsigned long long)status.OffloadParameterChanges);
    return matchingBuild ? 0 : 3;
}

typedef struct CREATE_CONTEXT { HANDLE Event; HRESULT Result; WCHAR InstanceId[1024]; } CREATE_CONTEXT;
static VOID WINAPI DeviceCreated(HSWDEVICE device, HRESULT result, PVOID opaque, PCWSTR instanceId)
{
    (void)device;
    CREATE_CONTEXT *ctx = opaque;
    ctx->Result = result;
    if (SUCCEEDED(result) && instanceId) wcscpy_s(ctx->InstanceId, _countof(ctx->InstanceId), instanceId);
    SetEvent(ctx->Event);
}

static int HoldDevice(void)
{
    CREATE_CONTEXT ctx = { .Event = CreateEventW(NULL, TRUE, FALSE, NULL), .Result = E_PENDING };
    if (!ctx.Event) return 1;
    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!StopEvent) { CloseHandle(ctx.Event); return 1; }
    SetConsoleCtrlHandler(StopHandler, TRUE);
    SW_DEVICE_CREATE_INFO info = { .cbSize = sizeof(info), .pszInstanceId = L"PacketBridgeV2-Diagnostic",
        .pszzHardwareIds = L"PacketBridgeV2\0", .CapabilityFlags = SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired,
        .pszDeviceDescription = L"PacketBridgeV2 Offload Probe" };
    DEVPROPERTY property = { .CompKey = { .Key = DEVPKEY_Device_ClassGuid, .Store = DEVPROP_STORE_SYSTEM },
        .Type = DEVPROP_TYPE_GUID, .BufferSize = sizeof(GUID_DEVCLASS_NET), .Buffer = (PVOID)&GUID_DEVCLASS_NET };
    HSWDEVICE device = NULL;
    HRESULT result = SwDeviceCreate(L"PacketBridgeV2", L"HTREE\\ROOT\\0", &info, 1, &property, DeviceCreated, &ctx, &device);
    if (SUCCEEDED(result)) {
        /* The callback owns this context until signaled; never free it on a timeout. */
        WaitForSingleObject(ctx.Event, INFINITE);
        result = ctx.Result;
    }
    if (SUCCEEDED(result)) {
        wprintf(L"Created %ls\nKeep this process running; Ctrl+C removes its software device.\n", ctx.InstanceId);
        ListDevices();
        WaitForSingleObject(StopEvent, INFINITE);
    } else fprintf(stderr, "Software device creation failed: 0x%08lx\n", (unsigned long)result);
    if (device) SwDeviceClose(device);
    SetConsoleCtrlHandler(StopHandler, FALSE);
    CloseHandle(StopEvent); StopEvent = NULL;
    CloseHandle(ctx.Event);
    return FAILED(result) ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 1 || (argc == 2 && !wcscmp(argv[1], L"--list"))) return ListDevices();
    if (argc == 2 && !wcscmp(argv[1], L"--self-test")) {
        printf("PacketBridgeV2 %s: probe ABI %u, status=%zu, policy=%zu\n", PB_VERSION_STRING, PB_PROBE_SCHEMA, sizeof(PB_PROBE_STATUS), sizeof(PB_PROBE_POLICY));
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--hold-device")) return HoldDevice();
    if (argc == 3 && !wcscmp(argv[1], L"--inspect")) return InspectDevice(argv[2], NULL);
    if (argc == 3 && !wcscmp(argv[1], L"--rx-returns")) return InspectReturns(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--tx-rejects")) return InspectSends(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--tx-pressure")) return InspectPoolPressure(argv[2]);
    if (argc == 4 && !wcscmp(argv[1], L"--policy")) return InspectDevice(argv[2], argv[3]);
    if (argc == 6 && !wcscmp(argv[1], L"--relay")) {
        fputs("The historical M1 relay uses ABI 1 and is not valid for the Ethernet/v2 driver. Use the integrated Rust client.\n", stderr);
        return 2;
    }
    fputs("Usage: pbprobe --list | --self-test | --hold-device | --inspect DEVICE | --rx-returns DEVICE | --tx-rejects DEVICE | --tx-pressure DEVICE | --policy DEVICE MASK\n", stderr);
    return 2;
}
