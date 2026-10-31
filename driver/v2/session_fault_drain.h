/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* The queue's sole worker calls this between transactions. Its private NBL chain
 * has already been published with references or returned locally. Never infer
 * this boundary from a shared terminal bit, a timeout, or Active == 0 alone. */
static VOID PbV2QuiesceFaultedQueue(PB_KERNEL_QUEUE *q)
{
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
    PbV2FlushLocked(q);
    ULONG active = 0;
    for (ULONG i = 0; i < q->Session->Layout.SlotCount; ++i) active += !!q->RxSlots[i].Active;
    BOOLEAN valid = !q->PendingCount && q->Credits == active;
    NT_ASSERT(valid);
    if (valid) WriteRelease(&q->FaultQuiesced, 1);
    KeReleaseInStackQueuedSpinLock(&lock);
    PbV2Progress(q->Session, FALSE);
}
