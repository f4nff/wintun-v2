/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Called with RegistrationLock exclusively held, after the common owner/list cleanup. */
static VOID PbV2Unregister(TUN_CTX *ctx)
{
    PB_KERNEL_SESSION *s = ctx->V2;
    KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    InterlockedExchange(&s->Draining, 1); KeSetEvent(&ctx->Device.Disconnected, IO_NO_INCREMENT, FALSE);
    ctx->V2 = NULL; WriteRelease(&ctx->Probe.V2SessionActive, 0); WriteRelease(&ctx->Probe.AllowedMask, 0);
    ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
    PbV2Progress(s, TRUE);
    PbReportOffload(ctx);
    PbV2Free(s);
}
