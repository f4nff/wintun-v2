/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "session_drain.h"
#include "session_coalesce.h"
#include "session_rx_stats.h"
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q)
{
    PB_KERNEL_SESSION *s = q->Session; TUN_CTX *ctx = s->Ctx;
    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
    if (!ReadAcquire(&ctx->Running) && !ReadAcquire(&s->Draining)) {
        ExReleaseSpinLockShared(&ctx->TransitionLock, irql); return FALSE;
    }
    PB_DESCRIPTOR *items = q->ReceiveItems; uint32_t count = 0;
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
    PbV2FlushLocked(q);
    ULONG room = PbV2SubmissionRoom(q, min(PB_MAX_BATCH, s->Layout.RingCapacity - q->Credits));
    PB_RESULT popped = room ? PbQueuePopBudget(&q->Rings[PB_QUEUE_RX_SUBMIT], items, room, PB_RX_BYTE_BUDGET, &count) : PB_EMPTY;
    if (popped != PB_OK && popped != PB_EMPTY) PbV2Fault(s);
    q->Credits += count;
    KeReleaseInStackQueuedSpinLock(&lock);
    /* The first flush owns its continuation wake. With no descriptors there
     * are no new completions, references, or indication metadata to process. */
    if (!count) { ExReleaseSpinLockShared(&ctx->TransitionLock, irql); return FALSE; }
    NET_BUFFER_LIST *head = NULL, *tail = NULL;
    ULONG indicated = 0, objects = 0;
    PB_RX_INDICATION_STATS stats = { 0 };
    /* Admission below writes every valid[0..count] before coalescing reads it. */
    BOOLEAN *valid = q->ReceiveValid;
    BOOLEAN fault = FALSE;
    /* One bounded ownership transaction, rather than reacquiring the same lock for every descriptor. */
    KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
    for (ULONG i = 0; i < count; ++i) {
        PB_DESCRIPTOR *d = items + i;
        uint32_t complete;
        PB_RESULT failure = PbAdmitReceive(&s->Layout, s->Epoch, q->Index, q->RxSlots, d,
            ReadAcquire(&s->Draining) || ReadAcquire(&s->Stopping), &complete);
        valid[i] = failure == PB_OK;
        if (failure != PB_OK) {
            if (complete) PbV2CompleteLocked(q, d, failure);
            else {
                /* No second completion for a live/old token. The original owns its one eventual completion. */
                NT_ASSERT(q->Credits); q->Credits--;
                fault = TRUE;
            }
        }
    }
    KeReleaseInStackQueuedSpinLock(&lock);
    if (fault) PbV2Fault(s);
    if (ReadAcquire(&s->Draining) || ReadAcquire(&s->Stopping)) {
        /* Admission is private to this worker until indication. Cancel only
         * this batch's admitted slots; rejected live tokens can still belong
         * to NDIS. No NBL, MDL, or native return reference exists for valid[]. */
        KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
        for (ULONG i = 0; i < count; ++i) {
            if (!valid[i]) continue;
            q->RxSlots[items[i].Token.Slot].Active = FALSE;
            PbV2CompleteLocked(q, items + i, PB_STOPPED);
        }
        PbV2FlushLocked(q);
        KeReleaseInStackQueuedSpinLock(&lock);
        ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
        return TRUE;
    }
    ULONG enabled = PbEffectiveMask(ctx);
    for (ULONG i = 0; i < count;) {
        if (!valid[i]) { i++; continue; }
        ULONG group = 1, copied = MAXULONG; PB_COALESCE coal = { 0 };
        if (enabled & (PB_RX_RSC4 | PB_RX_RSC6 | PB_RX_URO))
            group = PbV2TryCoalesce(q, items + i, valid + i, count - i, enabled, &copied, &coal);
        NET_BUFFER_LIST *nbl = PbV2Receive(q, items + i, group, copied, copied < 2 ? &coal : NULL);
        if (!nbl) {
            if (copied < 2) InterlockedExchange(&q->CoalesceBusy[copied], 0);
            KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
            for (ULONG j = 0; j < group; ++j) {
                q->RxSlots[items[i + j].Token.Slot].Active = FALSE;
                PbV2CompleteLocked(q, items + i + j, PB_FULL);
            }
            KeReleaseInStackQueuedSpinLock(&lock);
            i += group; continue;
        }
        /* Capture only successful coalesced objects while they are already
         * private here. Ordinary NBLs need no record walk for offload stats. */
        if (group > 1) PbV2AccumulateRxStats(&stats, nbl);
        if (tail) NET_BUFFER_LIST_NEXT_NBL(tail) = nbl; else head = nbl;
        tail = nbl; indicated++; objects += group; i += group;
    }
    if (indicated) PbV2RetainReceives(s, indicated, objects);
    BOOLEAN indicate = head && ReadAcquire(&ctx->Running) && !ReadAcquire(&s->Draining) && !ReadAcquire(&s->Stopping);
    ULONG bank = 0;
    if (indicate) {
            InterlockedIncrement64(&s->Indications);
            PbV2PublishRxStats(ctx, &stats);
            bank = PbBeginIndication(ctx);
    }
    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
    if (head) {
        if (indicate) {
            /* Upper protocol callbacks can reenter sends. No transition/queue lock may cross this call. */
            PbIndicateReceive(ctx->MiniportAdapterHandle, head, indicated, 0);
            PbEndIndication(ctx, bank);
        } else {
            for (NET_BUFFER_LIST *nbl = head; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
                PB_RX_RECORD *record = (PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl) & ~(ULONG_PTR)1);
                record->LocallyCancelled = TRUE;
                NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_PAUSED;
            }
            /* Every object belongs to this queue and the whole batch has at
             * most PB_MAX_BATCH original descriptors. Use the normal bounded
             * return window, preserving its Active/rundown tail protection. */
            while (head) {
                if (!PbV2ReturnNbls(ctx, &head, PB_MAX_BATCH)) { NT_ASSERT(FALSE); break; }
            }
        }
    }
    /* Every successfully built original belongs to its return callback, which
     * flushes completions itself. Only unbuilt originals can leave new local
     * completions here. Earlier pending work and budget continuations are also
     * flushed at the worker's next turn (this nonempty batch returns TRUE). */
    if (objects < count) {
        KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock); PbV2FlushLocked(q); KeReleaseInStackQueuedSpinLock(&lock);
    }
    return count != 0;
}

