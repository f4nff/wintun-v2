/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifndef PB_ADAPTER_CREATE_TIMEOUT_MS
#define PB_ADAPTER_CREATE_TIMEOUT_MS 15000u
#endif
typedef struct PB_CREATE { HANDLE Event; HRESULT Result; WCHAR Instance[1024]; } PB_CREATE;
static VOID WINAPI Created(HSWDEVICE device, HRESULT result, PVOID opaque, PCWSTR instance)
{
    (void)device; PB_CREATE *ctx=opaque; ctx->Result=result;
    if (instance || SUCCEEDED(result)) {
        size_t length=instance?wcsnlen_s(instance,_countof(ctx->Instance)):_countof(ctx->Instance);
        if (length>=_countof(ctx->Instance)) {
            if (SUCCEEDED(result)) ctx->Result=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        else memcpy(ctx->Instance,instance,(length+1)*sizeof(WCHAR));
    }
    SetEvent(ctx->Event);
}
static HRESULT PbWaitForAdapterCreate(PB_ADAPTER *adapter, PB_CREATE *ctx)
{
    DWORD waited=WaitForSingleObject(ctx->Event,PB_ADAPTER_CREATE_TIMEOUT_MS);
    if (waited==WAIT_OBJECT_0) return ctx->Result;
    DWORD error=waited==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
    /* SwDeviceClose is the callback barrier even if creation has not called back yet.
     * Keep stack context/event alive until it returns; never abandon a live callback. */
    if (adapter->Device) { SwDeviceClose(adapter->Device); adapter->Device=NULL; }
    return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
}
