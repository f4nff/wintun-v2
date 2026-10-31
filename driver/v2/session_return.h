/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#define PB_RETURN_DIAGNOSTICS_ENABLED 1
typedef struct PB_RETURN_ITEM {
    PB_RX_RECORD *Record;
    ULONG Result, NblStatus;
} PB_RETURN_ITEM;

/* Consume one same-queue window. A coalesced NBL charges every original
 * descriptor, so one RxLock hold never grows with an arbitrary return chain.
 * The first unconsumed NBL remains owned by the caller, including foreign tags. */
static BOOLEAN PbV2ReturnNbls(TUN_CTX *ctx, NET_BUFFER_LIST **lists, ULONG limit)
{
    NT_ASSERT(lists && *lists && limit && limit <= PB_MAX_BATCH);
    NET_BUFFER_LIST *nbl = *lists;
    ULONG_PTR tag = (ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl);
    if (!(tag & 1)) return FALSE;
    PB_KERNEL_QUEUE *q = ((PB_RX_RECORD *)(tag & ~(ULONG_PTR)1))->Queue;
    PB_KERNEL_SESSION *s = q->Session;
    /* Each unconsumed NBL still owns an Active reference. Pin the entire window
     * before freeing objects or releasing any of those references. */
    BOOLEAN acquired = ExAcquireRundownProtection(&s->Returns);
    NT_ASSERT(acquired);
    if (!acquired) { *lists = NET_BUFFER_LIST_NEXT_NBL(nbl); return TRUE; }
    PB_RETURN_ITEM items[PB_MAX_BATCH];
    ULONG count = 0, objects = 0;
    BOOLEAN diagnostic = FALSE;
    while (nbl && count < limit) {
        tag = (ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl);
        if (!(tag & 1)) break;
        PB_RX_RECORD *record = (PB_RX_RECORD *)(tag & ~(ULONG_PTR)1);
        if (record->Queue != q || record->Count > PB_MAX_BATCH - objects) break;
        NT_ASSERT(record->Count);
        /* Capture the raw word before NdisFreeNetBufferList can recycle the NBL.
         * Local cancellation is not evidence of a failed NDIS receive indication. */
        ULONG raw = NET_BUFFER_LIST_STATUS(nbl);
        /* Protocol-stack rejection is a packet result. The numeric ABI remains
         * 4; never confuse it with an API-level queue-corruption result. */
        ULONG result = record->LocallyCancelled ? PB_STOPPED : raw == NDIS_STATUS_SUCCESS ? PB_OK : PB_RX_REJECTED;
        items[count++] = (PB_RETURN_ITEM){ record, result, raw };
        diagnostic |= raw != NDIS_STATUS_SUCCESS || record->LocallyCancelled;
        objects += record->Count;
        NET_BUFFER_LIST *next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        IoFreeMdl(record->HeaderMdl);
        IoFreeMdl(record->Mdl);
        NdisFreeNetBufferList(nbl);
        /* No NBL/MDL can read this copied payload now. Release its lease before
         * publishing completions or waiting on diagnostic/queue locks. Only
         * private record metadata is used below; the tail must not clear a
         * buffer that the receive worker may already have acquired again. */
        if (record->CoalesceIndex < 2) InterlockedExchange(&q->CoalesceBusy[record->CoalesceIndex], 0);
        nbl = next;
    }
    NT_ASSERT(count && objects);
    *lists = nbl;
    if (diagnostic) {
        /* Exceptional path only, one independent lock per bounded window. Never
         * nest this with RxLock, a transition lock, or an NDIS upcall. No allocation,
         * formatting, clocks or payload reads take place in the return callback. */
        KIRQL irql;
        KeAcquireSpinLock(&ctx->Probe.ReturnDiagnosticsLock, &irql);
        for (ULONG i = 0; i < count; ++i) {
            const PB_RX_RECORD *record = items[i].Record;
            if (items[i].NblStatus == NDIS_STATUS_SUCCESS && !record->LocallyCancelled) continue;
            PB_RETURN_EVENT event = { .FirstToken = record->Descriptors[0].Token,
                .NblStatus = items[i].NblStatus, .CompletionStatus = items[i].Result,
                .Origin = record->LocallyCancelled ? PB_RETURN_ORIGIN_LOCAL_CANCEL : PB_RETURN_ORIGIN_NDIS,
                .OriginalCount = record->Count, .IndicationLength = record->IndicationLength,
                .IpVersion = record->IpVersion, .Protocol = record->Protocol, .ReceiveType = record->ReceiveType,
                .IpDeclaredLength = record->IpDeclaredLength };
            for (ULONG j = 0; j < record->Count; ++j) event.OriginalBytes += record->Descriptors[j].Length;
            PbRecordReturnEvent(&ctx->Probe.ReturnDiagnostics, &event);
        }
        KeReleaseSpinLock(&ctx->Probe.ReturnDiagnosticsLock, irql);
    }
    ULONG successful = 0, errors = 0, cancelled = 0;
    ULONG64 bytes = 0;
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&q->RxLock, &lock);
    BOOLEAN wasFull = q->Credits == s->Layout.RingCapacity;
    for (ULONG i = 0; i < count; ++i) {
        PB_RX_RECORD *record = items[i].Record;
        for (ULONG j = 0; j < record->Count; ++j) {
            PB_DESCRIPTOR *d = record->Descriptors + j;
            q->RxSlots[d->Token.Slot].Active = FALSE;
            PbV2CompleteLocked(q, d, items[i].Result);
            if (items[i].Result == PB_OK) bytes += d->Length;
        }
        if (record->LocallyCancelled) cancelled += record->Count;
        else if (items[i].Result == PB_OK) successful += record->Count;
        else errors++; /* Preserve the existing error-per-NBL statistic. */
    }
    PbV2FlushLocked(q);
    /* The worker checks credit room under this same lock before sleeping.
     * Only a full -> usable transition can unblock that wait. New submissions
     * have their own arm/recheck wake; flush owns budget continuations and the
     * completion consumer wakes after freeing space in a full completion ring. */
    BOOLEAN wake = wasFull && q->Credits < s->Layout.RingCapacity;
    KeReleaseInStackQueuedSpinLock(&lock);
    for (ULONG i = 0; i < count; ++i) {
        PB_RX_RECORD *record = items[i].Record;
        ExFreePoolWithTag(record, TUN_MEMORY_TAG);
    }
    InterlockedAdd64(&s->RxReturned, objects);
    if (successful) {
        InterlockedAddNoFence64((LONG64 *)&ctx->Statistics.ifHCInOctets, bytes);
        InterlockedAddNoFence64((LONG64 *)&ctx->Statistics.ifHCInUcastOctets, bytes);
        InterlockedAddNoFence64((LONG64 *)&ctx->Statistics.ifHCInUcastPkts, successful);
    }
    if (errors) InterlockedAddNoFence64((LONG64 *)&ctx->Statistics.ifInErrors, errors);
    if (cancelled) InterlockedAddNoFence64((LONG64 *)&ctx->Statistics.ifInDiscards, cancelled);
    PbV2ReleaseReferences(s, count);
    /* The private Returns pin covers notification and the last callback tail,
     * including when Active has reached zero and cleanup is already waiting. */
    if (wake) KeSetEvent(q->KernelEvent, IO_NO_INCREMENT, FALSE);
    ExReleaseRundownProtection(&s->Returns);
    return TRUE;
}

static BOOLEAN PbV2ReturnOne(TUN_CTX *ctx, NET_BUFFER_LIST *nbl)
{
    return PbV2ReturnNbls(ctx, &nbl, 1);
}
