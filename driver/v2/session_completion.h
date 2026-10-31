/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "session_progress.h"
/* Fault retirement never trusts a shared consumer cursor and never clears an
 * active slot. Those credits remain owned by their actual NDIS return callbacks. */
static BOOLEAN PbV2RetirePendingLocked(PB_KERNEL_QUEUE *q)
{
    if (q->PendingCount > q->Credits || q->Credits > q->Session->Layout.RingCapacity) {
        NT_ASSERT(FALSE); return FALSE;
    }
    if (q->PendingCount) {
        q->Credits -= q->PendingCount; q->PendingCount = q->PendingHead = 0;
        PbV2Progress(q->Session, FALSE);
    }
    return TRUE;
}
static VOID PbV2FlushLocked(PB_KERNEL_QUEUE *q)
{
    ULONG completed = 0, batches = 0;
    uint32_t notifyUser = 0;
    /* Bound DISPATCH-level hold time, including when the consumer keeps making space. */
    for (; batches < 4 && q->PendingCount; ++batches) {
        if (ReadAcquire(&q->Session->Faulted)) { PbV2RetirePendingLocked(q); break; }
        uint32_t count = min(q->PendingCount, PB_MAX_BATCH), accepted, notify;
        /* RxLock keeps the source stable. The core copies directly from this
         * private ledger and publishes once; no intermediate batch is needed. */
        PB_RESULT result = PbQueuePushRing(&q->Rings[PB_QUEUE_RX_COMPLETE], q->Pending,
            q->Session->Layout.RingCapacity, q->PendingHead, count, &accepted, &notify);
        if (result == PB_FULL) break;
        if (result != PB_OK) { PbV2Fault(q->Session); PbV2RetirePendingLocked(q); break; }
        q->PendingHead = (q->PendingHead + accepted) & (q->Session->Layout.RingCapacity - 1);
        q->PendingCount -= accepted; q->Credits -= accepted;
        completed += accepted;
        notifyUser |= notify;
    }
    if (completed) {
        InterlockedAdd64(&q->Session->Completions, completed);
        if (ReadAcquire(&q->Session->Draining)) PbV2Progress(q->Session, FALSE);
    }
    if (notifyUser) KeSetEvent(q->UserEvent, IO_NO_INCREMENT, FALSE);
    /* A full ring parks until the consumer wakes us. Only budget exhaustion schedules more work. */
    if (batches == 4 && q->PendingCount && !ReadAcquire(&q->Session->Faulted))
        KeSetEvent(q->KernelEvent, IO_NO_INCREMENT, FALSE);
}

static VOID PbV2CompleteLocked(PB_KERNEL_QUEUE *q, const PB_DESCRIPTOR *d, ULONG status)
{
    NT_ASSERT(q->PendingCount < q->Session->Layout.RingCapacity);
    PB_DESCRIPTOR *out = q->Pending + ((q->PendingHead + q->PendingCount) & (q->Session->Layout.RingCapacity - 1));
    *out = *d; out->Status = status; q->PendingCount++;
}
