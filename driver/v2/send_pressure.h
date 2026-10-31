/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Caller holds TxLock and a transition/session lifetime pin. Copy views so observation
 * cannot move validation cursors, authorize recycling, fault, or admit work.
 * Read returns before ready-consumer progress: returned tokens were consumed
 * before publication, so these ordered samples cannot double-count a token. */
static __declspec(noinline) VOID PbV2CapturePoolPressure(PB_KERNEL_QUEUE *q, ULONG pending, PB_POOL_PRESSURE_EVENT *event)
{
    if (!q || q->Session->Epoch != event->Send.Epoch || q->Index != event->Send.Queue) return;
    event->Slots = q->TxPool.Count; event->Pending = pending;
    if (q->TxPoolExhausted) event->Flags |= PB_POOL_FULL_PROVEN;
    PB_QUEUE_VIEW returned = q->Rings[PB_QUEUE_TX_RECYCLE], ready = q->Rings[PB_QUEUE_TX_READY];
    event->ReadyProducer = ready.Cursor; event->ReturnConsumer = returned.Cursor;
    event->ReturnStatus = PbQueueSnapshotProducer(&returned, &event->ReturnProducer);
    if (event->ReturnStatus == PB_OK) event->Flags |= PB_POOL_RETURN_VALID;
    uint32_t space;
    event->ReadyStatus = PbQueueSpace(&ready, &space);
    if (event->ReadyStatus == PB_OK) { event->ReadyConsumer = ready.PeerCursor; event->Flags |= PB_POOL_READY_VALID; }
    uint64_t outstanding = event->ReadyProducer - event->ReturnConsumer;
    uint64_t unread = event->ReadyProducer - event->ReadyConsumer;
    uint64_t queued = event->ReturnProducer - event->ReturnConsumer;
    if (event->Flags != PB_POOL_CURSOR_FLAGS || outstanding > event->Slots || pending > event->Slots - outstanding ||
        unread > outstanding || queued > outstanding - unread) return;
    event->Outstanding = (uint32_t)outstanding; event->ReadyUnread = (uint32_t)unread;
    event->ReturnQueued = (uint32_t)queued; event->UserHeld = (uint32_t)(outstanding - unread - queued);
    /* Residual slots may be retired, or expose an unaccounted private state.
     * Do not diagnose retirement or a leak without inspecting that state. */
    event->Unaccounted = event->Slots - pending - (uint32_t)outstanding;
    event->Flags |= PB_POOL_COUNTS_VALID;
}
