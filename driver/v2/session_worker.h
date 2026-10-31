/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "session_fault_drain.h"
#include "session_drain.h"
static KSTART_ROUTINE PbV2Worker;
_Use_decl_annotations_
static VOID PbV2Worker(PVOID opaque)
{
    PB_KERNEL_QUEUE *q = opaque; PB_KERNEL_SESSION *s = q->Session;
    KeWaitForSingleObject(&s->StartEvent, Executive, KernelMode, FALSE, NULL);
    VOID *events[] = { &s->StopEvent, q->KernelEvent, &s->Ctx->Probe.RunningEvent };
    while (!ReadAcquire(&s->Stopping)) {
        /* Coalesce notifications before draining/rechecking ring counters, including manual-reset events. */
        KeClearEvent(q->KernelEvent);
        /* Clear before observing terminal state: never erase the only fault wake after checking it. */
        if (ReadAcquire(&s->Faulted)) { PbV2QuiesceFaultedQueue(q); break; }
        /* Pause stops packet admission, not retirement. Recycle before choosing
         * either wait path, and arm/recheck it below even while paused. */
        KLOCK_QUEUE_HANDLE lock;
        KeAcquireInStackQueuedSpinLock(&q->TxLock, &lock);
        PB_RESULT recycle = PbV2RecycleLocked(q);
        KeReleaseInStackQueuedSpinLock(&lock);
        /* Recycle failures already faulted the session. Publish the private
         * quiescence witness at the top, without admitting another RX batch. */
        if (recycle != PB_OK) continue;
        BOOLEAN paused = !ReadAcquire(&s->Ctx->Running) && !ReadAcquire(&s->Draining);
        uint32_t ready = 0, recycled = 0;
        BOOLEAN room = FALSE;
        if (paused) {
            KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
            PbV2FlushLocked(q);
            KeReleaseInStackQueuedSpinLock(&lock);
        } else {
            if (PbV2ReceiveBatch(q)) continue;
            KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
            if (PbV2ArmSubmissions(q, &ready) != PB_OK) PbV2Fault(s);
            room = q->Credits < s->Layout.RingCapacity;
            KeReleaseInStackQueuedSpinLock(&lock);
        }
        KeAcquireInStackQueuedSpinLock(&q->TxLock, &lock);
        if (PbQueueArm(&q->Rings[PB_QUEUE_TX_RECYCLE], &recycled) != PB_OK) PbV2Fault(s);
        KeReleaseInStackQueuedSpinLock(&lock);
        if ((ready && room) || recycled) continue;
        KeWaitForMultipleObjects(paused ? 3 : 2, events, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
