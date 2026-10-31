/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Status may be requested from a receive callback while unregister owns RegistrationLock. */
static NTSTATUS PbV2Query(TUN_CTX *ctx, PB_V2_STATUS *output)
{
    BOOLEAN exclusive = FALSE;
    KIRQL irql;
    PB_KERNEL_SESSION *s;
retry:
    irql = exclusive ? ExAcquireSpinLockExclusive(&ctx->TransitionLock) : ExAcquireSpinLockShared(&ctx->TransitionLock);
    s = ctx->V2;
    if (!s) {
        if (exclusive) ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
        else ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }
    PB_V2_STATUS status = { .Size = sizeof(status), .State = PB_SESSION_RUNNING, .QueueCount = s->Layout.QueueCount,
        .ActiveReferences = (ULONG)ReadAcquire(&s->Active), .Epoch = s->Epoch,
        .TxObjects = ReadULong64Acquire((ULONG64 *)&s->TxObjects), .RxObjects = ReadULong64Acquire((ULONG64 *)&s->RxObjects),
        .RxReturned = ReadULong64Acquire((ULONG64 *)&s->RxReturned), .Indications = ReadULong64Acquire((ULONG64 *)&s->Indications),
        .Completions = ReadULong64Acquire((ULONG64 *)&s->Completions) };
    if (ReadAcquire(&s->Draining)) {
        BOOLEAN pending = FALSE;
        BOOLEAN faulted = ReadAcquire(&s->Faulted) != 0;
        if (faulted && !exclusive) {
            /* Fault does not take TransitionLock. A send lookup may have read
             * admission before it, but not retained Active yet. Join that
             * short shared lookup before trusting zero references. Reacquire
             * and relook up ctx->V2: unregister may detach it in between. */
            ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
            exclusive = TRUE; goto retry;
        }
        for (ULONG i = 0; i < s->Layout.QueueCount; ++i) {
            PB_KERNEL_QUEUE *q = s->Queues + i; KLOCK_QUEUE_HANDLE lock;
            KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
            if (faulted) {
                /* A faulted producer can keep writing forever. Only private
                 * worker quiescence and returned ownership determine STOPPED. */
                pending |= !ReadAcquire(&q->FaultQuiesced) || q->Credits != 0 || q->PendingCount != 0;
            } else pending |= !ReadAcquire(&s->DrainSnapshotReady) || q->Credits != 0 ||
                q->DrainProducer != q->Rings[PB_QUEUE_RX_SUBMIT].Cursor;
            KeReleaseInStackQueuedSpinLock(&lock);
        }
        /* A worker may have retained a newly built chain before publishing its
         * quiescence above. Read references after observing every queue boundary. */
        status.ActiveReferences = (ULONG)ReadAcquire(&s->Active);
        /* Do not apply a normal-drain snapshot to a concurrent fault. The next
         * query must observe all private fault witnesses before acknowledging. */
        if (!faulted && ReadAcquire(&s->Faulted)) pending = TRUE;
        status.State = !status.ActiveReferences && !pending ? PB_SESSION_STOPPED :
            KeQueryInterruptTime() / 10000u >= ReadULong64Acquire(&s->Deadline) ? PB_SESSION_QUARANTINED : PB_SESSION_DRAINING;
    }
    *output = status;
    if (exclusive) ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
    else ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
    return STATUS_SUCCESS;
}
