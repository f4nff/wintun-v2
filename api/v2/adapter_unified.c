/* SPDX-License-Identifier: GPL-2.0 */
/* Both exported APIs use the same downlevel-capable installer and identity. */
#include "../adapter.h"
#include "adapter.h"
#include "../../include/packetbridge_version.h"
#include <bcrypt.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

struct PB_ADAPTER { volatile LONG References; WINTUN_ADAPTER *Native; };
void PbAdapterRetain(PB_ADAPTER *adapter) { InterlockedIncrement(&adapter->References); }
void WINAPI PbAdapterRelease(PB_ADAPTER *adapter)
{
    if (!adapter || InterlockedDecrement(&adapter->References)) return;
    WintunCloseAdapter(adapter->Native); PbRuntimeReleaseAdapter(); free(adapter);
}
const wchar_t *PbAdapterPath(PB_ADAPTER *adapter) { return adapter->Native->InterfaceFilename; }
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter, uint64_t *out)
{
    if (!adapter || !out) return PB_INVALID;
    NET_LUID luid; WintunGetAdapterLUID(adapter->Native, &luid); *out = luid.Value; return PB_OK;
}
/* Stable v2 instance -> requested GUID, using a domain-separated SHA-256 digest.
 * BCrypt's provider/hash APIs are available on the minimum supported system. */
static BOOL InstanceGuid(const WCHAR *instance, GUID *guid)
{
    BCRYPT_ALG_HANDLE algorithm = NULL; BCRYPT_HASH_HANDLE hash = NULL;
    UCHAR digest[32]; static const UCHAR domain[] = "PacketBridgeV2 adapter instance";
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)domain, sizeof(domain), 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)instance, (ULONG)(wcslen(instance) * sizeof(WCHAR)), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    memcpy(guid, digest, sizeof(*guid)); guid->Data3 = (guid->Data3 & 0x0fff) | 0x8000;
    guid->Data4[0] = (guid->Data4[0] & 0x3f) | 0x80; return TRUE;
}
static PB_RESULT Detailed(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *d, uint32_t size, uint32_t schema)
{
    if (out) *out = NULL;
    if (!d || d->Size != size || d->Schema != schema) { SetLastError(ERROR_INVALID_PARAMETER); return PB_INVALID; }
    memset(d, 0, size); d->Size = size; d->Schema = schema; d->Win32Error = ERROR_INVALID_PARAMETER;
    if (!name || !instance || !out) goto invalid;
    size_t n = wcsnlen_s(name, 129), id = wcsnlen_s(instance, 129);
    if (!n || n > 128 || !id || id > 128) goto invalid;
    for (size_t i = 0; i < id; ++i) {
        WCHAR c = instance[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')) goto invalid;
    }
    if (!PbRuntimeReserveAdapter()) { d->Win32Error = ERROR_NO_SYSTEM_RESOURCES; SetLastError(d->Win32Error); return PB_FULL; }
    GUID guid; PB_ADAPTER *a = calloc(1, sizeof(*a));
    if (!a) { d->Win32Error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    d->Stage = PB_ADAPTER_CREATE_DEVICE; d->Attempts = 1;
    if (!InstanceGuid(instance, &guid)) { d->Win32Error = GetLastError(); goto failed; }
    a->Native = WintunCreateAdapter(name, L"PacketBridge", &guid);
    if (!a->Native) { d->Win32Error = GetLastError(); goto failed; }
    a->References = 1; *out = a; d->Win32Error = ERROR_SUCCESS; d->Stage = PB_ADAPTER_COMPLETE;
    SetLastError(ERROR_SUCCESS); return PB_OK;
failed:
    free(a); PbRuntimeReleaseAdapter(); SetLastError(d->Win32Error);
    return d->Win32Error == ERROR_NOT_ENOUGH_MEMORY ? PB_FULL : PB_UNSUPPORTED;
invalid:
    SetLastError(d->Win32Error); return PB_INVALID;
}
PB_RESULT WINAPI PbCreateAdapterEx(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out, PB_ADAPTER_DIAGNOSTICS *d)
{ return Detailed(name, instance, out, d, sizeof(*d), PB_ADAPTER_DIAG_SCHEMA); }
PB_RESULT WINAPI PbCreateAdapterEx2(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out, PB_ADAPTER_DIAGNOSTICS_EX *d)
{ return Detailed(name, instance, out, d ? &d->Base : NULL, sizeof(*d), PB_ADAPTER_DIAG_SCHEMA_EX); }
PB_RESULT WINAPI PbAdapterCreate(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out)
{
    PB_ADAPTER_DIAGNOSTICS d = { .Size = sizeof(d), .Schema = PB_ADAPTER_DIAG_SCHEMA };
    return PbCreateAdapterEx(name, instance, out, &d);
}
