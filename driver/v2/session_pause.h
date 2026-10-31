/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
static VOID PbV2Pause(TUN_CTX *ctx)
{
    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
    PB_KERNEL_SESSION *s = ctx->V2;
    BOOLEAN held = s && ExAcquireRundownProtection(&s->Returns);
    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
    /* A worker may have sampled Running before pause, then skipped receive
     * after it. Its old wait set excludes RunningEvent. Recheck via the queue
     * event so paused retirement and the later restart cannot lose progress.
     * The existing pin keeps every queue/event alive after dropping the lock. */
    if (held) PbV2Wake(s);
    PbWaitIndications(ctx, 0); PbWaitIndications(ctx, 1);
    if (held) {
        KeWaitForSingleObject(&s->AllReturned, Executive, KernelMode, FALSE, NULL);
        ExReleaseRundownProtection(&s->Returns);
    }
}
