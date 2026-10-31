/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Called with TransitionLock exclusive: no receive transaction or query can
 * observe a partially captured boundary. A concurrent fault uses its own worker
 * witnesses; it never authorizes normal drain from an incomplete snapshot. */
static __inline VOID PbV2BeginDrain(PB_KERNEL_SESSION *s, ULONG64 deadline)
{
    if (ReadAcquire(&s->Draining)) return;
    for (ULONG i = 0; i < s->Layout.QueueCount; ++i) {
        PB_KERNEL_QUEUE *q = s->Queues + i; KLOCK_QUEUE_HANDLE lock;
        /* ArmSubmissions also observes this consumer view outside TransitionLock. */
        KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
        PB_RESULT result = PbQueueSnapshotProducer(&q->Rings[PB_QUEUE_RX_SUBMIT], &q->DrainProducer);
        KeReleaseInStackQueuedSpinLock(&lock);
        if (result != PB_OK) {
            PbV2Fault(s); return;
        }
    }
    s->Deadline = deadline;
    WriteRelease(&s->DrainSnapshotReady, 1);
    WriteRelease(&s->Draining, 1);
}

/* ReceiveBatch owns TransitionLock shared and RxLock here. New writes after
 * DRAIN cannot extend the finite work set, including uint64 cursor wrap. */
static __inline ULONG PbV2SubmissionRoom(PB_KERNEL_QUEUE *q, ULONG room)
{
    PB_KERNEL_SESSION *s = q->Session;
    if (!ReadAcquire(&s->Draining)) return room;
    if (!ReadAcquire(&s->DrainSnapshotReady)) return 0;
    ULONG64 remaining = q->DrainProducer - q->Rings[PB_QUEUE_RX_SUBMIT].Cursor;
    NT_ASSERT(remaining <= s->Layout.RingCapacity);
    return (ULONG)min(room, remaining);
}

/* Do not arm a stopped producer or spin on its post-boundary tail. If credits
 * returned since ReceiveBatch, the worker still rechecks room for finite work. */
static __inline PB_RESULT PbV2ArmSubmissions(PB_KERNEL_QUEUE *q, uint32_t *ready)
{
    if (!ReadAcquire(&q->Session->Draining)) return PbQueueArm(&q->Rings[PB_QUEUE_RX_SUBMIT], ready);
    *ready = ReadAcquire(&q->Session->DrainSnapshotReady) &&
        q->Rings[PB_QUEUE_RX_SUBMIT].Cursor != q->DrainProducer;
    return PB_OK;
}
