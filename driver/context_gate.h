/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* CtxGuard protects pointer lookup only. Rundown protects blocking control work after lookup. */
static BOOLEAN TunReferenceCtxLocked(TUN_CTX *ctx)
{
    return ctx && !ReadAcquire(&ctx->Halting) && ExAcquireRundownProtection(&ctx->ControlRundown);
}
static TUN_CTX *TunReferenceDeviceCtx(DEVICE_OBJECT *device)
{
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&TunDispatchCtxGuard, TRUE);
#pragma warning(suppress : 28175)
    TUN_CTX *ctx = device->Reserved;
    if (!TunReferenceCtxLocked(ctx)) ctx = NULL;
    ExReleaseResourceLite(&TunDispatchCtxGuard);
    KeLeaveCriticalRegion();
    return ctx;
}
static VOID TunDereferenceCtx(TUN_CTX *ctx) { ExReleaseRundownProtection(&ctx->ControlRundown); }
static VOID TunRevokeContext(TUN_CTX *ctx)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&TunDispatchCtxGuard, TRUE);
    InterlockedExchange(&ctx->Halting, 1);
#pragma warning(suppress : 28175)
    ctx->FunctionalDeviceObject->Reserved = NULL;
    ExReleaseResourceLite(&TunDispatchCtxGuard);
    KeLeaveCriticalRegion();
    /* Never wait for a callback/thread while holding the global lookup lock. */
    ExWaitForRundownProtectionRelease(&ctx->ControlRundown);
}
