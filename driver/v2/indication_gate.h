/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Begin/switch run under TransitionLock. End never takes it: NDIS callbacks may reenter sends. */
static ULONG PbBeginIndication(TUN_CTX *ctx)
{
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&ctx->Probe.IndicationLock, &lock);
    ULONG bank = ctx->Probe.IndicationBank;
    /* Only the first owner changes idle to busy; the last exit sets idle under
     * this same lock. Nested/concurrent owners need no dispatcher event write. */
    if (!ctx->Probe.IndicationCount[bank]++) KeClearEvent(&ctx->Probe.IndicationIdle[bank]);
    KeReleaseInStackQueuedSpinLock(&lock);
    return bank;
}
static VOID PbEndIndication(TUN_CTX *ctx, ULONG bank)
{
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&ctx->Probe.IndicationLock, &lock);
    NT_ASSERT(ctx->Probe.IndicationCount[bank] > 0);
    if (!--ctx->Probe.IndicationCount[bank]) KeSetEvent(&ctx->Probe.IndicationIdle[bank], IO_NO_INCREMENT, FALSE);
    KeReleaseInStackQueuedSpinLock(&lock);
}
/* OffloadMutex serializes rotations; the previous rotation has already emptied the next bank. */
static ULONG PbRotateIndications(TUN_CTX *ctx)
{
    ULONG old = ctx->Probe.IndicationBank;
    NT_ASSERT(ctx->Probe.IndicationCount[old ^ 1] == 0);
    ctx->Probe.IndicationBank ^= 1;
    return old;
}
static VOID PbWaitIndications(TUN_CTX *ctx, ULONG bank)
{
    KeWaitForSingleObject(&ctx->Probe.IndicationIdle[bank], Executive, KernelMode, FALSE, NULL);
}
