/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include "driver_package.h"
#include <setupapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
HANDLE PbLockDriverOperations(void)
{
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, NULL)) return NULL;
    SECURITY_ATTRIBUTES security = { sizeof(security), descriptor, FALSE };
    HANDLE mutex = CreateMutexW(&security, FALSE, L"Global\\PacketBridgeV2.DriverInstall.2");
    DWORD error = GetLastError(); LocalFree(descriptor);
    if (!mutex) { SetLastError(error); return NULL; }
    DWORD waited = WaitForSingleObject(mutex, 15000);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) return mutex;
    error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CloseHandle(mutex); SetLastError(error); return NULL;
}
void PbUnlockDriverOperations(HANDLE mutex)
{
    DWORD error = GetLastError(); ReleaseMutex(mutex); CloseHandle(mutex); SetLastError(error);
}
#ifdef PB_EMBEDDED_DRIVER
#include "driver_recovery.h"
static SRWLOCK PackageLock = SRWLOCK_INIT;
static BOOL Staged;
void PbRememberAdapterBinding(const WCHAR *name, const WCHAR *instance, PB_DRIVER_BINDING *binding)
{
    if (!instance[0]) return;
    DWORD error = GetLastError();
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA device = { .cbSize = sizeof(device) };
        if (SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device) && PbNamedAdapter(set, &device, name)) {
            PB_ADAPTER_DIAGNOSTICS ignored = { 0 }; PB_RECOVERY_INF *packages = NULL;
            PbSnapshotBoundInf(set, &device, &packages, &ignored);
            if (packages) {
                wcscpy_s(binding->Published, _countof(binding->Published), packages->Published);
                wcscpy_s(binding->Store, _countof(binding->Store), packages->Store);
                free(packages);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    SetLastError(error);
}
BOOL PbRepairDriverPackage(const WCHAR *name, const PB_DRIVER_BINDING *binding, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    AcquireSRWLockExclusive(&PackageLock);
    Staged = FALSE;
    BOOL okay = PbRemoveNamedAdapter(name, binding, diagnostics);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    ReleaseSRWLockExclusive(&PackageLock); SetLastError(error); return okay;
}
static const UCHAR ModuleAnchor = 0;
static BOOL WriteResource(HMODULE module, WORD id, const WCHAR *path)
{
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    DWORD size = resource ? SizeofResource(module, resource) : 0;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : NULL;
    const VOID *bytes = loaded ? LockResource(loaded) : NULL;
    if (!size || !bytes) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL okay = WriteFile(file, bytes, size, &written, NULL);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (okay && written != size) { okay = FALSE; error = ERROR_WRITE_FAULT; }
    if (okay && !FlushFileBuffers(file)) { okay = FALSE; error = GetLastError(); }
    CloseHandle(file); SetLastError(error); return okay;
}
BOOL PbEnsureDriverPackage(PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    AcquireSRWLockExclusive(&PackageLock);
    diagnostics->Stage = PB_ADAPTER_STAGE_DRIVER;
    if (Staged) { ReleaseSRWLockExclusive(&PackageLock); SetLastError(ERROR_SUCCESS); return TRUE; }
    diagnostics->Stage = PB_ADAPTER_PREPARE_PACKAGE;
    HMODULE module = NULL; PSECURITY_DESCRIPTOR descriptor = NULL;
    WCHAR root[MAX_PATH], directory[MAX_PATH] = { 0 }, files[3][MAX_PATH] = { 0 };
    UCHAR random[16]; BOOL okay = FALSE, created = FALSE;
    static const WCHAR *names[] = { L"packetbridge.sys", L"packetbridge.inf", L"packetbridge.cat" };
    UINT size = GetSystemWindowsDirectoryW(root, _countof(root));
    if (!size || size >= _countof(root) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&ModuleAnchor, &module) ||
        BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)", SDDL_REVISION_1, &descriptor, NULL)) goto done;
    WCHAR suffix[33];
    for (unsigned i = 0; i < sizeof(random); ++i) swprintf_s(suffix + 2 * i, 3, L"%02x", random[i]);
    if (swprintf_s(directory, _countof(directory), L"%s\\Temp\\PacketBridgeV2-%s", root, suffix) < 0) goto done;
    SECURITY_ATTRIBUTES security = { sizeof(security), descriptor, FALSE };
    if (!CreateDirectoryW(directory, &security)) goto done;
    created = TRUE;
    for (unsigned i = 0; i < 3; ++i) {
        if (swprintf_s(files[i], _countof(files[i]), L"%s\\%s", directory, names[i]) < 0 || !WriteResource(module, (WORD)(101 + i), files[i])) goto done;
    }
    /* SetupAPI validates the signed catalog when staging; kernel loading applies the target's policy. */
    diagnostics->Stage = PB_ADAPTER_STAGE_DRIVER;
    okay = SetupCopyOEMInfW(files[1], NULL, SPOST_PATH, 0, NULL, 0, NULL, NULL);
    Staged = okay;
done:;
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (!okay && !error) error = ERROR_GEN_FAILURE;
    if (created) {
        for (unsigned i = 0; i < 3; ++i) if (files[i][0]) DeleteFileW(files[i]);
        RemoveDirectoryW(directory);
    }
    if (descriptor) LocalFree(descriptor);
    ReleaseSRWLockExclusive(&PackageLock); SetLastError(error); return okay;
}
#else
BOOL PbEnsureDriverPackage(PB_ADAPTER_DIAGNOSTICS *diagnostics) { diagnostics->Stage = PB_ADAPTER_STAGE_DRIVER; return TRUE; }
void PbRememberAdapterBinding(const WCHAR *name, const WCHAR *instance, PB_DRIVER_BINDING *binding)
{ (void)name; (void)instance; (void)binding; }
BOOL PbRepairDriverPackage(const WCHAR *name, const PB_DRIVER_BINDING *binding, PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    (void)name; (void)binding; (void)diagnostics; SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
}
#endif
