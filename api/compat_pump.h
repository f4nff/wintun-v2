/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* One queue owner, finite object/byte work per direction. Every accepted prefix
 * has an explicit owner; an unaccepted native reservation is always cancelled. */
typedef struct COMPAT_PENDING {
    PB_LEASE Items[PB_MAX_BATCH];
    uint32_t Count, Copied, Retired;
} COMPAT_PENDING;

static PB_RESULT CompatSendBatch(TUN_SESSION *s, PB_USER_QUEUE *q, BOOL *progress)
{
    TUN_RING *ring = s->Descriptor.Receive.Ring;
    ULONG head = ReadULongAcquire(&ring->Head), tail = ReadULongAcquire(&ring->Tail);
    if (head >= s->Capacity || tail >= s->Capacity) return PB_CORRUPT;
    TUN_PACKET *packets[PB_MAX_BATCH]; ULONG after[PB_MAX_BATCH];
    uint32_t count = 0, bytes = 0;
    for (uint32_t attempts = 0; attempts < PB_MAX_BATCH && head != tail; ++attempts) {
        TUN_PACKET *p = (TUN_PACKET *)(ring->Data + head); ULONG length = p->Size;
        ULONG span = TUN_ALIGN(sizeof(*p) + length);
        if (length > WINTUN_MAX_IP_PACKET_SIZE || span > TUN_RING_WRAP(tail-head, s->Capacity)) return PB_CORRUPT;
        if (length > PB_RX_BYTE_BUDGET - bytes) break;
        head = TUN_RING_WRAP(head + span, s->Capacity);
        if (length) { packets[count] = p; after[count++] = head; bytes += length; }
        else if (count) after[count-1] = head;
        else { WriteULongRelease(&ring->Head, head); *progress = TRUE; }
    }
    if (!count) return PB_OK;
    PB_LEASE leases[PB_MAX_BATCH]; PB_TOKEN tokens[PB_MAX_BATCH]; uint32_t reserved = 0, accepted = 0;
    PB_RESULT result = s->Api.ReserveBatch(q, leases, count, &reserved);
    if (result == PB_FULL || result == PB_EMPTY) return PB_OK;
    if (result != PB_OK || !reserved || reserved > count) return result == PB_OK ? PB_CORRUPT : result;
    BOOL valid = TRUE;
    for (uint32_t i = 0; i < reserved; ++i) {
        tokens[i] = leases[i].Descriptor.Token;
        if (!leases[i].Data || leases[i].Capacity < packets[i]->Size) valid = FALSE;
    }
    if (valid) {
        for (uint32_t i = 0; i < reserved; ++i) {
            memcpy(leases[i].Data, packets[i]->Data, packets[i]->Size);
            leases[i].Descriptor.Length = packets[i]->Size;
        }
        result = s->Api.CommitBatch(q, leases, reserved, &accepted);
    } else result = PB_CORRUPT;
    if (accepted > reserved) return PB_CORRUPT; /* A native implementation contract violation. */
    if (accepted < reserved) {
        PB_RESULT cancelled = s->Api.CancelBatch(q, tokens + accepted, reserved - accepted);
        if (cancelled != PB_OK) return cancelled;
    }
    if (accepted) { WriteULongRelease(&ring->Head, after[accepted-1]); *progress = TRUE; }
    return result == PB_FULL || result == PB_EMPTY ? PB_OK : result;
}

static PB_RESULT CompatRetire(TUN_SESSION *s, PB_USER_QUEUE *q, COMPAT_PENDING *p)
{
    if (p->Retired == p->Copied) return PB_OK;
    PB_TOKEN tokens[PB_MAX_BATCH]; uint32_t count = p->Copied - p->Retired, accepted = 0;
    for (uint32_t i = 0; i < count; ++i) tokens[i] = p->Items[p->Retired+i].Descriptor.Token;
    PB_RESULT result = s->Api.ReleaseBatch(q, tokens, count, &accepted);
    if (accepted > count) return PB_CORRUPT;
    p->Retired += accepted;
    if (p->Retired == p->Count) p->Count = p->Copied = p->Retired = 0;
    return result;
}

static PB_RESULT CompatReceiveBatch(TUN_SESSION *s, PB_USER_QUEUE *q, COMPAT_PENDING *p, BOOL *progress)
{
    uint32_t before = p->Copied - p->Retired;
    PB_RESULT result = CompatRetire(s, q, p);
    if (p->Copied - p->Retired < before) *progress = TRUE;
    if (result == PB_FULL) return PB_OK;
    if (result != PB_OK || p->Retired != p->Copied) return result;
    if (!p->Count) {
        uint32_t received = 0;
        result = s->Api.ReceiveBatch(q, p->Items, PB_MAX_BATCH, &received);
        if (result == PB_EMPTY) return PB_OK;
        if (result != PB_OK || !received || received > PB_MAX_BATCH) return result == PB_OK ? PB_CORRUPT : result;
        p->Count = received;
    }
    TUN_RING *ring = s->Descriptor.Send.Ring; ULONG tail = ReadULongAcquire(&ring->Tail);
    uint32_t first = p->Copied, bytes = 0;
    while (p->Copied < p->Count) {
        PB_LEASE *lease = p->Items + p->Copied;
        ULONG length = lease->Descriptor.Length, head = ReadULongAcquire(&ring->Head);
        if (!lease->Data || lease->Capacity < length || length > WINTUN_MAX_IP_PACKET_SIZE ||
            lease->Descriptor.Type != PB_DESC_PLAIN || head >= s->Capacity || tail >= s->Capacity) return PB_CORRUPT;
        ULONG span = TUN_ALIGN(sizeof(TUN_PACKET) + length);
        if (length > PB_RX_BYTE_BUDGET - bytes || span > TUN_RING_WRAP(head-tail-TUN_ALIGNMENT, s->Capacity)) break;
        TUN_PACKET *packet = (TUN_PACKET *)(ring->Data + tail);
        packet->Size = length; memcpy(packet->Data, lease->Data, length);
        tail = TUN_RING_WRAP(tail + span, s->Capacity); p->Copied++; bytes += length;
    }
    if (p->Copied != first) {
        /* A readable event is a condition hint, not one counted signal/packet. */
        WriteULongRelease(&ring->Tail, tail); SetEvent(s->Descriptor.Send.TailMoved); *progress = TRUE;
    }
    result = CompatRetire(s, q, p);
    return result == PB_FULL ? PB_OK : result;
}

static void CompatReturnPending(TUN_SESSION *s, PB_USER_QUEUE *q, COMPAT_PENDING *p)
{
    /* BeginStop has permanently disabled reuse/admission. Even a full or
     * corrupt recycle ring permits local retirement before native handoff. */
    p->Copied = p->Count;
    for (uint32_t attempt = 0; attempt < PB_MAX_BATCH && p->Count; ++attempt) {
        uint32_t remaining = p->Count - p->Retired;
        PB_RESULT result = CompatRetire(s, q, p);
        if (!p->Count || result != PB_OK || p->Count - p->Retired >= remaining) break;
    }
}
