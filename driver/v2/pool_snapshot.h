/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "send_pressure.h"
/* Manual IOCTL only. TransitionLock pins V2 across queue reads; each TxLock
 * freezes private slot ownership. No sender releases TxLock with an unpublished
 * prefix, so a reserved slot observed here deserves investigation. History and
 * current queues are separate observations, not a global atomic snapshot. */
static VOID PbSnapshotCurrentTxPools(TUN_CTX *ctx, PB_POOL_PRESSURE_SNAPSHOT *out)
{
    out->CurrentQueueCount = 0; out->CurrentStatus = PB_OK;
    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
    PB_KERNEL_SESSION *s = ctx->V2;
    if (!s) { ExReleaseSpinLockShared(&ctx->TransitionLock, irql); return; }
    if (!s->Layout.QueueCount || s->Layout.QueueCount > PB_MAX_QUEUES ||
        !s->Layout.SlotCount || s->Layout.SlotCount > PB_MAX_SLOTS) {
        out->CurrentStatus = PB_CORRUPT;
        ExReleaseSpinLockShared(&ctx->TransitionLock, irql); return;
    }
    out->CurrentQueueCount = s->Layout.QueueCount;
    for (ULONG i = 0; i < s->Layout.QueueCount; ++i) {
        PB_KERNEL_QUEUE *q = s->Queues + i; PB_TX_POOL_STATE *state = out->Current + i;
        KLOCK_QUEUE_HANDLE lock; KeAcquireInStackQueuedSpinLock(&q->TxLock, &lock);
        state->Epoch = s->Epoch; state->Queue = i; state->Slots = s->Layout.SlotCount;
        state->PoolSlots = q->TxPool.Count; state->PoolExhausted = !!q->TxPoolExhausted;
        state->Running = !!ReadAcquire(&ctx->Running); state->Draining = !!ReadAcquire(&s->Draining);
        state->Stopping = !!ReadAcquire(&s->Stopping); state->Faulted = !!ReadAcquire(&s->Faulted);
        if (q->TxSlots && q->TxPool.Slots == q->TxSlots && q->TxPool.Count == s->Layout.SlotCount) {
            for (ULONG j = 0; j < s->Layout.SlotCount; ++j) {
                switch (q->TxSlots[j].State) {
                case PB_SLOT_FREE: state->Free++; break;
                case PB_SLOT_RESERVED: state->Reserved++; break;
                case PB_SLOT_PUBLISHED: state->Published++; break;
                case PB_SLOT_RETIRED: state->Retired++; break;
                default: state->Other++; break;
                }
            }
            state->CountsValid = TRUE;
        }
        PB_POOL_PRESSURE_EVENT cursors = { 0 }; cursors.Send.Epoch = s->Epoch; cursors.Send.Queue = i;
        PbV2CapturePoolPressure(q, 0, &cursors);
        state->ReadyProducer = cursors.ReadyProducer; state->ReadyConsumer = cursors.ReadyConsumer;
        state->ReturnProducer = cursors.ReturnProducer; state->ReturnConsumer = cursors.ReturnConsumer;
        state->ReadyStatus = cursors.ReadyStatus; state->ReturnStatus = cursors.ReturnStatus;
        state->CursorFlags = cursors.Flags & (PB_POOL_RETURN_VALID | PB_POOL_READY_VALID);
        KeReleaseInStackQueuedSpinLock(&lock);
    }
    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
}
