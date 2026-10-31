/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#define PB_TX_ATTEMPT_BUDGET PB_MAX_BATCH
#define PB_TX_BYTE_BUDGET (128u * 1024u)

static VOID PbV2PublishTx(PB_KERNEL_QUEUE *q, PB_DESCRIPTOR *items, NET_BUFFER_LIST **owners, uint32_t count)
{
    if (!count) return;
    uint32_t accepted, notify;
    PB_RESULT result = PbQueuePush(&q->Rings[PB_QUEUE_TX_READY], items, count, &accepted, &notify);
    if (result != PB_OK) accepted = 0;
    if (result != PB_OK && result != PB_FULL) PbV2Fault(q->Session);
    ULONG64 totalPackets = 0, totalBytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i >= accepted) {
            (VOID)PbPoolTransition(&q->TxPool, &items[i].Token, PB_SLOT_RESERVED, PB_SLOT_FREE);
            if (NET_BUFFER_LIST_STATUS(owners[i]) == NDIS_STATUS_SUCCESS) {
                PB_SEND_FAILURE failure = { PB_SEND_PUBLICATION, 0, result, 0 };
                NET_BUFFER_LIST_STATUS(owners[i]) = NDIS_STATUS_RESOURCES;
                PbV2RejectSend(q->Session->Ctx, q->Session->Epoch, owners[i], NET_BUFFER_LIST_FIRST_NB(owners[i]),
                    NDIS_STATUS_RESOURCES, NDIS_STATUS_SUCCESS, q->TxPool.Queue, 0, &failure, accepted, count, 1, NULL, 0);
            }
            continue;
        }
        (VOID)PbPoolTransition(&q->TxPool, &items[i].Token, PB_SLOT_RESERVED, PB_SLOT_PUBLISHED);
        ULONG64 packets = items[i].Type ? 1u + (items[i].Length - items[i].HeaderLength - 1u) / items[i].Mss : 1;
        ULONG64 bytes = items[i].Length + (packets - 1u) * items[i].HeaderLength;
        totalPackets += packets; totalBytes += bytes;
    }
    /* Unpublished reservations were released above, without a user recycle. */
    if (accepted < count) q->TxPoolExhausted = FALSE;
    if (accepted) {
        InterlockedAddNoFence64((LONG64 *)&q->Session->Ctx->Statistics.ifHCOutOctets, totalBytes);
        InterlockedAddNoFence64((LONG64 *)&q->Session->Ctx->Statistics.ifHCOutUcastOctets, totalBytes);
        InterlockedAddNoFence64((LONG64 *)&q->Session->Ctx->Statistics.ifHCOutUcastPkts, totalPackets);
        InterlockedAdd64(&q->Session->TxObjects, accepted);
    }
    if (notify) KeSetEvent(q->UserEvent, IO_NO_INCREMENT, FALSE);
}

typedef struct PB_TX_WINDOW {
    TUN_CTX *Ctx;
    PB_KERNEL_SESSION *Session;
    PB_KERNEL_QUEUE *Queue;
    KLOCK_QUEUE_HANDLE QueueLock;
    KIRQL Irql;
    BOOLEAN Held;
    ULONG Attempts, WorkBytes;
    uint64_t Epoch;
    PB_RESULT QueueStatus;
    uint32_t Count, Space;
    PB_DESCRIPTOR *Items;
    NET_BUFFER_LIST **Owners;
} PB_TX_WINDOW;

static VOID PbV2FlushTxWindow(PB_TX_WINDOW *w)
{
    if (!w->Queue) return;
    PbV2PublishTx(w->Queue, w->Items, w->Owners, w->Count);
    KeReleaseInStackQueuedSpinLock(&w->QueueLock);
    w->Queue = NULL; w->Count = w->Space = 0;
    /* Queue switches and failed publications do not renew the work budget. */
}
static VOID PbV2EndTxWindow(PB_TX_WINDOW *w)
{
    PbV2FlushTxWindow(w);
    if (w->Held) { ExReleaseSpinLockShared(&w->Ctx->TransitionLock, w->Irql); w->Held = FALSE; }
    w->Attempts = w->WorkBytes = 0;
}
static NDIS_STATUS PbV2EnterTxWindow(PB_TX_WINDOW *w, ULONG length)
{
    ULONG bytes = min(length, PB_BLOCK_SIZE);
    if (w->Held && (w->Attempts == PB_TX_ATTEMPT_BUDGET || bytes > PB_TX_BYTE_BUDGET - w->WorkBytes))
        PbV2EndTxWindow(w);
    if (!w->Held) { w->Irql = ExAcquireSpinLockShared(&w->Ctx->TransitionLock); w->Held = TRUE; }
    NDIS_STATUS result = w->Ctx->V2 != w->Session ? NDIS_STATUS_MEDIA_DISCONNECTED :
        !ReadAcquire(&w->Ctx->Running) || ReadAcquire(&w->Session->Draining) || ReadAcquire(&w->Session->Stopping) ?
        NDIS_STATUS_PAUSED : NDIS_STATUS_SUCCESS;
    if (result != NDIS_STATUS_SUCCESS) { PbV2EndTxWindow(w); return result; }
    /* Charge every attempt, including drops, malformed packets and full pools. */
    w->Attempts++; w->WorkBytes += bytes; return NDIS_STATUS_SUCCESS;
}

static BOOLEAN PbV2Send(TUN_CTX *ctx, NET_BUFFER_LIST *nbls)
{
    if (!nbls) return TRUE;
    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
    PB_KERNEL_SESSION *s = ctx->V2;
    if (!s) { ExReleaseSpinLockShared(&ctx->TransitionLock, irql); return FALSE; }
    NDIS_HANDLE adapter = ctx->MiniportAdapterHandle;
    BOOLEAN retained = ReadAcquire(&ctx->Running) && !ReadAcquire(&s->Draining) && !ReadAcquire(&s->Stopping) &&
        ExAcquireRundownProtection(&s->Returns);
    /* Increment before any second admission check. If Fault races this lookup,
     * a later query sees either this reference or a rejected, resource-free send. */
    if (retained) PbV2RetainReferences(s, 1);
    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
    /* Batch storage is written completely before Count exposes each entry.
     * Keep it separate so control initialization need not clear unused entries,
     * including sends rejected before any packet work. Both live on this call's stack. */
    PB_DESCRIPTOR items[PB_MAX_BATCH];
    NET_BUFFER_LIST *owners[PB_MAX_BATCH];
    PB_TX_WINDOW w = { .Ctx = ctx, .Session = s, .Epoch = retained ? s->Epoch : 0, .Items = items, .Owners = owners };
    for (NET_BUFFER_LIST *nbl = nbls; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        NDIS_STATUS status = retained ? NDIS_STATUS_SUCCESS : NDIS_STATUS_PAUSED;
        NET_BUFFER_LIST_STATUS(nbl) = status;
        if (NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo)) InterlockedIncrement64(&ctx->Probe.LsoNbls);
        if (NET_BUFFER_LIST_INFO(nbl, UdpSegmentationOffloadInfo)) InterlockedIncrement64(&ctx->Probe.UsoNbls);
        if (NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo)) InterlockedIncrement64(&ctx->Probe.ChecksumNbls);
        NET_BUFFER *first = NET_BUFFER_LIST_FIRST_NB(nbl);
        NET_BUFFER *failedNb = first;
        ULONG failedQueue = PB_SEND_QUEUE_UNKNOWN, failedOffset = 0;
        PB_SEND_FAILURE failure = { PB_SEND_ADMISSION, 0, 0, 0 };
        if (retained && !first) {
            status = PbV2EnterTxWindow(&w, 0);
            if (status == NDIS_STATUS_SUCCESS) { status = NDIS_STATUS_INVALID_LENGTH; failure.Reason = PB_SEND_EMPTY_NBL; }
        }
        for (NET_BUFFER *nb = first; nb && status == NDIS_STATUS_SUCCESS; nb = NET_BUFFER_NEXT_NB(nb)) {
            failedNb = nb;
            status = PbV2EnterTxWindow(&w, NET_BUFFER_DATA_LENGTH(nb));
            /* Ending the preceding window may have failed an earlier buffer
             * belonging to this same NBL. Preserve that publication error. */
            if (NET_BUFFER_LIST_STATUS(nbl) != NDIS_STATUS_SUCCESS) { status = NET_BUFFER_LIST_STATUS(nbl); failure.Reason = PB_SEND_PRIOR_FAILURE; }
            if (status != NDIS_STATUS_SUCCESS) break;
            UCHAR prefix[PB_ETH_CONTROL_MAX], reply[PB_ETH_CONTROL_MAX]; uint32_t offset = 0, replyLength = 0;
            ULONG peek = min(NET_BUFFER_DATA_LENGTH(nb), PB_ETH_CLASSIFY_MAX);
            if (!PbV2Read(nb, 0, prefix, peek)) { status = NDIS_STATUS_RESOURCES; failure.Reason = PB_SEND_PREFIX_READ; break; }
            PB_ETH_RESULT kind = PbEthernetInspect(prefix, peek, &offset);
            failedOffset = offset;
            BOOLEAN neighbor = kind == PB_ETH_IP && prefix[offset] >> 4 == 6 && peek >= offset + 41 &&
                prefix[offset + 6] == 58 && prefix[offset + 40] >= 133 && prefix[offset + 40] <= 137;
            /* The control parser can reply only to ARP or direct IPv6 neighbor
             * messages. Classify this stable prefix first; ordinary IP needs
             * neither a second Ethernet parse nor control-reply validation. */
            if (kind != PB_ETH_IP || neighbor) {
                /* Reuse the stable classification bytes and read the remaining
                 * control span only here. Ordinary IP keeps its normal complete
                 * Describe snapshot; no metadata is authorized by this prefix. */
                ULONG controlPeek = min(NET_BUFFER_DATA_LENGTH(nb), sizeof(prefix));
                if (controlPeek > peek && !PbV2Read(nb, peek, prefix + peek, controlPeek - peek)) {
                    status = NDIS_STATUS_RESOURCES; failure.Reason = PB_SEND_CONTROL_READ; break;
                }
                peek = controlPeek;
                PB_ETH_RESULT control = PbEthernetControl(prefix, peek, ctx->Probe.PeerMac, reply, &replyLength);
                PbV2FlushTxWindow(&w);
                if (NET_BUFFER_LIST_STATUS(nbl) != NDIS_STATUS_SUCCESS) { status = NET_BUFFER_LIST_STATUS(nbl); failure.Reason = PB_SEND_PRIOR_FAILURE; break; }
                if (control == PB_ETH_REPLY) {
                    ULONG bank = PbBeginIndication(ctx);
                    PbV2EndTxWindow(&w);
                    /* The send reference protects session, mapping and callback
                     * lifetime across this unlocked, possibly reentrant upcall. */
                    status = PbV2ControlReply(ctx, reply, replyLength);
                    if (status != NDIS_STATUS_SUCCESS) failure.Reason = PB_SEND_CONTROL_REPLY;
                    PbEndIndication(ctx, bank);
                }
                continue;
            }
            ULONG index = PbSelectQueue(prefix + offset, peek - offset, s->Layout.QueueCount);
            failedQueue = index;
            PB_KERNEL_QUEUE *q = s->Queues + index;
            if (w.Queue && (w.Queue != q || w.Count == PB_MAX_BATCH || (w.Count && w.Count == w.Space))) {
                PbV2FlushTxWindow(&w);
                if (NET_BUFFER_LIST_STATUS(nbl) != NDIS_STATUS_SUCCESS) { status = NET_BUFFER_LIST_STATUS(nbl); failure.Reason = PB_SEND_PRIOR_FAILURE; break; }
            }
            if (!w.Queue) {
                w.Queue = q; KeAcquireInStackQueuedSpinLock(&q->TxLock, &w.QueueLock);
            }
            /* A zero-space window has no prefix to publish. Keep its bounded
             * lock hold, but refresh recycling and peer space on every attempt:
             * the independent consumer may have freed room since the last one. */
            if (!w.Space) {
                PB_RESULT recycled = PbV2RecycleLocked(q);
                if (recycled != PB_OK) { status = NDIS_STATUS_PAUSED; failure.Reason = PB_SEND_RECYCLE; failure.CoreStatus = recycled; break; }
                w.QueueStatus = PbQueueSpace(&q->Rings[PB_QUEUE_TX_READY], &w.Space);
                if (w.QueueStatus != PB_OK) { w.Space = 0; PbV2Fault(s); }
            }
            PB_DESCRIPTOR d = { 0 };
            PB_RESULT reserved = !w.Space || q->TxPoolExhausted ? PB_FULL : PbPoolReserve(&q->TxPool, &d.Token);
            if (reserved == PB_FULL && w.Space) {
                /* TxLock excludes the worker, not the independent return-ring
                 * producer. A cached full result is valid only until new
                 * returns appear. The snapshot validates the shared cursor;
                 * no-progress pressure neither rescans slots nor drains again.
                 * One bounded recycle per attempt keeps the original work
                 * budgets; no waiting, prefix publication or NBL replay. */
                uint64_t returned;
                PB_RESULT recycled = PbQueueSnapshotProducer(&q->Rings[PB_QUEUE_TX_RECYCLE], &returned);
                if (recycled != PB_OK) {
                    PbV2Fault(s);
                    status = NDIS_STATUS_PAUSED; failure.Reason = PB_SEND_RECYCLE; failure.CoreStatus = recycled; break;
                }
                uint64_t before = q->Rings[PB_QUEUE_TX_RECYCLE].Cursor;
                if (returned != before) {
                    recycled = PbV2RecycleLocked(q);
                    if (recycled != PB_OK) {
                        status = NDIS_STATUS_PAUSED; failure.Reason = PB_SEND_RECYCLE; failure.CoreStatus = recycled; break;
                    }
                    if (q->Rings[PB_QUEUE_TX_RECYCLE].Cursor != before) {
                        reserved = PbPoolReserve(&q->TxPool, &d.Token);
                    }
                }
            }
            if (reserved != PB_OK) {
                status = NDIS_STATUS_RESOURCES;
                failure.CoreStatus = !w.Space && w.QueueStatus != PB_OK ? w.QueueStatus : reserved;
                failure.Reason = !w.Space ? (w.QueueStatus != PB_OK ? PB_SEND_QUEUE_RING : PB_SEND_QUEUE_FULL) :
                    reserved == PB_FULL ? PB_SEND_POOL_FULL : PB_SEND_POOL_ERROR;
                /* Keep a full free-index scan cached until validated return
                 * progress above makes private slots available again. */
                if (reserved == PB_FULL) {
                    if (w.Space) q->TxPoolExhausted = TRUE;
                } else PbV2Fault(s);
            } else {
                d.DataOffset = s->Layout.Queues[index].TxData + d.Token.Slot * PB_BLOCK_SIZE;
                status = PbV2Describe(ctx, nbl, nb, offset, &d, s->Region + d.DataOffset, &failure);
                if (status == NDIS_STATUS_SUCCESS) {
                    w.Items[w.Count] = d; w.Owners[w.Count] = nbl; w.Count++;
                } else (VOID)PbPoolTransition(&q->TxPool, &d.Token, PB_SLOT_RESERVED, PB_SLOT_FREE);
            }
        }
        if (status != NDIS_STATUS_SUCCESS) {
            ULONG prior = NET_BUFFER_LIST_STATUS(nbl);
            NET_BUFFER_LIST_STATUS(nbl) = status;
            PbV2RejectSend(ctx, w.Epoch, nbl, failedNb, status, prior, failedQueue, failedOffset, &failure, 0, 0, 2, w.Queue, w.Count);
            /* Pressure must not hide an already prepared prefix from the
             * consumer while later NBLs keep failing in this lock window.
             * Record this original's failure first, then publish and release
             * both locks. The next original rechecks admission and recycling;
             * no wait, retry or replay of this rejected original is introduced. */
            if (failure.Reason == PB_SEND_POOL_FULL && w.Count) PbV2EndTxWindow(&w);
        }
    }
    PbV2EndTxWindow(&w);
    PbSetLsoCompletion(nbls);
    PbCompleteSend(adapter, nbls);
    /* No NBL access after completion. AllReturned includes this operation until
     * the upcall returns; rundown additionally protects the final release tail. */
    if (retained) { PbV2ReleaseReference(s); ExReleaseRundownProtection(&s->Returns); }
    return TRUE;
}
