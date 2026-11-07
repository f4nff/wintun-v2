/* SPDX-License-Identifier: GPL-2.0 */
#include "v2_core.h"
#include <stddef.h>
#include <string.h>
#include <intrin.h>

/* Also compiled in the WDK's default C mode, without C11 static assertions. */
typedef char PB_QUEUE_CURSOR_ALIGNMENT_REQUIRED[
    offsetof(PB_QUEUE_SHARED, Producer) % 8 == 0 && offsetof(PB_QUEUE_SHARED, Consumer) % 8 == 0 &&
    sizeof(PB_QUEUE_SHARED) % 8 == 0 && sizeof(PB_DESCRIPTOR) % 8 == 0 ? 1 : -1];
/* Ordinary Pop previously passed UINT32_MAX as its byte budget. Specializing
 * that path is equivalent only while even maximum per-item charges fit. */
typedef char PB_QUEUE_UNBUDGETED_BATCH_FITS[
    (uint64_t)PB_MAX_BATCH * PB_BLOCK_SIZE <= 0xffffffffu ? 1 : -1];

static uint64_t Load64(volatile uint64_t *p)
{
#if defined(_M_AMD64) && !defined(_M_ARM64EC)
    /* Attached layouts start at 4096 and ring strides preserve 8-byte cursor
     * alignment, even with tiny rings. On native x64 an aligned
     * 64-bit load is atomic and the CPU preserves load-acquire order. Keep the
     * compiler acquire barrier explicit even with /volatile:iso. A read must
     * not request write ownership of the peer's cursor cache line.
     * Cursor publication and the Armed exchange retain their full barriers. */
    uint64_t value = *p;
    _ReadWriteBarrier();
    return value;
#else
    /* Do not extend the native x64 memory-ordering proof to other targets. */
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0);
#endif
}
static void Store64(volatile uint64_t *p, uint64_t v) { _InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
static uint32_t Exchange32(volatile uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v); }
static int PowerOfTwo(uint32_t n) { return n && !(n & (n - 1)); }

PB_RESULT PbValidateRegionAddress(uint64_t address, uint32_t size)
{
    /* Keep the one-past-end address representable before calling any MDL/page API. */
    return address && !(address & 63u) && size && size <= PB_MAX_REGION &&
        address <= UINT64_MAX - size ? PB_OK : PB_INVALID;
}

PB_RESULT PbCalculateLayout(uint32_t queues, uint32_t slots, uint32_t capacity, PB_LAYOUT *layout)
{
    if (!layout || !queues || queues > PB_MAX_QUEUES || !slots || slots > PB_MAX_SLOTS ||
        !PowerOfTwo(capacity) || capacity < slots || capacity > 4096) return PB_INVALID;
    PB_LAYOUT result;
    memset(&result, 0, sizeof(result));
    result.Size = sizeof(result); result.Magic = PB_ABI_MAGIC; result.Major = PB_ABI_MAJOR;
    result.Minor = PB_ABI_MINOR; result.QueueCount = queues; result.SlotCount = slots;
    result.RingCapacity = capacity; result.BlockSize = PB_BLOCK_SIZE;
    result.LifecycleOffset = (sizeof(result) + 63u) & ~63u;
    result.LifecycleSize = sizeof(PB_SHARED_LIFECYCLE);
    uint64_t offset = 4096;
    for (uint32_t q = 0; q < queues; ++q) {
        for (uint32_t r = 0; r < PB_QUEUE_COUNT; ++r) {
            result.Queues[q].Rings[r] = (uint32_t)offset;
            offset += sizeof(PB_QUEUE_SHARED) + (uint64_t)capacity * sizeof(PB_DESCRIPTOR);
        }
        offset = (offset + 4095u) & ~(uint64_t)4095u;
        result.Queues[q].TxData = (uint32_t)offset;
        offset += (uint64_t)slots * PB_BLOCK_SIZE;
        result.Queues[q].RxData = (uint32_t)offset;
        offset += (uint64_t)slots * PB_BLOCK_SIZE;
        if (offset > PB_MAX_REGION) return PB_INVALID;
    }
    result.RegionSize = (uint32_t)offset;
    *layout = result;
    return PB_OK;
}

PB_RESULT PbAttachQueue(PB_QUEUE_VIEW *view, void *region, const PB_LAYOUT *layout, uint32_t queue, uint32_t ring, int producer)
{
    PB_LAYOUT expected;
    if (!view || !region || ((uintptr_t)region & 63u) || !layout || queue >= layout->QueueCount || ring >= PB_QUEUE_COUNT ||
        PbCalculateLayout(layout->QueueCount, layout->SlotCount, layout->RingCapacity, &expected) != PB_OK ||
        memcmp(layout, &expected, sizeof(expected))) return PB_INVALID;
    memset(view, 0, sizeof(*view));
    view->Shared = (PB_QUEUE_SHARED *)((uint8_t *)region + layout->Queues[queue].Rings[ring]);
    view->Entries = (PB_DESCRIPTOR *)(view->Shared + 1);
    view->Capacity = layout->RingCapacity; view->Producer = !!producer;
    return PB_OK;
}

PB_RESULT PbQueueSpace(PB_QUEUE_VIEW *v, uint32_t *space)
{
    if (!v || !v->Shared || !space || !v->Producer) return PB_INVALID;
    uint64_t peer = Load64(&v->Shared->Consumer);
    if (v->Cursor - peer > v->Capacity || peer - v->PeerCursor > v->Capacity) return PB_CORRUPT;
    v->PeerCursor = peer;
    *space = v->Capacity - (uint32_t)(v->Cursor - peer);
    return PB_OK;
}

/* All producers validate space before touching the source or destination, then
 * publish the copied prefix once with the same arm exchange. */
static __forceinline PB_RESULT PushPrepare(PB_QUEUE_VIEW *v, const void *items,
    uint32_t *count, uint32_t *accepted, uint32_t *notify)
{
    if (accepted) *accepted = 0;
    if (notify) *notify = 0;
    if (!accepted || !notify || !items || !*count || *count > PB_MAX_BATCH) return PB_INVALID;
    uint32_t space;
    PB_RESULT status = PbQueueSpace(v, &space);
    if (status != PB_OK) return status;
    if (!space) return PB_FULL;
    if (*count > space) *count = space;
    return PB_OK;
}

static __forceinline PB_RESULT PushCommit(PB_QUEUE_VIEW *v, uint32_t count, uint32_t *accepted, uint32_t *notify)
{
    v->Cursor += count;
    Store64(&v->Shared->Producer, v->Cursor);
    *notify = Exchange32(&v->Shared->Armed, 0) != 0;
    *accepted = count;
    return PB_OK;
}

/* Specialize the contiguous caller with an all-ones source mask. */
static __forceinline PB_RESULT Push(PB_QUEUE_VIEW *v, const PB_DESCRIPTOR *items, uint32_t head, uint32_t mask,
    uint32_t count, uint32_t *accepted, uint32_t *notify)
{
    PB_RESULT status = PushPrepare(v, items, &count, accepted, notify);
    if (status != PB_OK) return status;
    for (uint32_t i = 0; i < count; ++i)
        v->Entries[(uint32_t)(v->Cursor + i) & (v->Capacity - 1)] = items[(head + i) & mask];
    return PushCommit(v, count, accepted, notify);
}

PB_RESULT PbQueuePush(PB_QUEUE_VIEW *v, const PB_DESCRIPTOR *items, uint32_t count, uint32_t *accepted, uint32_t *notify)
{
    return Push(v, items, 0, 0xffffffffu, count, accepted, notify);
}

PB_RESULT PbQueuePushRing(PB_QUEUE_VIEW *v, const PB_DESCRIPTOR *items, uint32_t capacity, uint32_t head,
    uint32_t count, uint32_t *accepted, uint32_t *notify)
{
    if (!PowerOfTwo(capacity) || capacity > 4096 || head >= capacity || count > capacity) {
        if (accepted) *accepted = 0;
        if (notify) *notify = 0;
        return PB_INVALID;
    }
    return Push(v, items, head, capacity - 1, count, accepted, notify);
}

PB_RESULT PbQueuePushTokens(PB_QUEUE_VIEW *v, const PB_TOKEN *tokens, uint32_t count, uint32_t *accepted, uint32_t *notify)
{
    PB_RESULT status = PushPrepare(v, tokens, &count, accepted, notify);
    if (status != PB_OK) return status;
    for (uint32_t i = 0; i < count; ++i) {
        /* Preserve the entire recycle descriptor's previous wire contents,
         * including zero unused metadata, without staging a descriptor batch. */
        PB_DESCRIPTOR item = { 0 };
        item.Token = tokens[i];
        v->Entries[(uint32_t)(v->Cursor + i) & (v->Capacity - 1)] = item;
    }
    return PushCommit(v, count, accepted, notify);
}

static PB_RESULT Available(PB_QUEUE_VIEW *v, uint32_t *available)
{
    if (!v || !v->Shared || v->Producer) return PB_INVALID;
    uint64_t peer = Load64(&v->Shared->Producer);
    if (peer - v->Cursor > v->Capacity || peer - v->PeerCursor > v->Capacity) return PB_CORRUPT;
    v->PeerCursor = peer;
    *available = (uint32_t)(peer - v->Cursor);
    return PB_OK;
}

PB_RESULT PbQueueSnapshotProducer(PB_QUEUE_VIEW *v, uint64_t *producer)
{
    if (!producer) return PB_INVALID;
    uint32_t available;
    PB_RESULT result = Available(v, &available);
    if (result == PB_OK) *producer = v->Cursor + available;
    return result;
}

static __forceinline void PopCommit(PB_QUEUE_VIEW *v, uint32_t count, uint32_t *received)
{
    v->Cursor += count;
    Store64(&v->Shared->Consumer, v->Cursor);
    *received = count;
}

/* Specialize ordinary consumers without per-descriptor byte accounting. Keep
 * snapshot, validation and publication in one implementation; only the kernel
 * submission consumer needs to charge the length from the copied descriptor. */
static __forceinline PB_RESULT PopDescriptors(PB_QUEUE_VIEW *v, PB_DESCRIPTOR *items, uint32_t count,
    uint32_t byteBudget, int budgeted, uint32_t *received)
{
    if (received) *received = 0;
    if (!received || !items || !count || count > PB_MAX_BATCH || byteBudget < PB_BLOCK_SIZE) return PB_INVALID;
    uint32_t available;
    PB_RESULT status = Available(v, &available);
    if (status != PB_OK) return status;
    if (!available) return PB_EMPTY;
    if (count > available) count = available;
    uint32_t bytes = 0, taken = 0;
    for (; taken < count; ++taken) {
        PB_DESCRIPTOR item;
        memcpy(&item, v->Entries + ((uint32_t)(v->Cursor + taken) & (v->Capacity - 1)), sizeof(item));
        if (budgeted) {
            uint32_t charge = item.Length <= PB_BLOCK_SIZE ? item.Length : PB_BLOCK_SIZE;
            if (taken && charge > byteBudget - bytes) break;
            bytes += charge;
        }
        items[taken] = item;
    }
    PopCommit(v, taken, received);
    return PB_OK;
}

PB_RESULT PbQueuePop(PB_QUEUE_VIEW *v, PB_DESCRIPTOR *items, uint32_t count, uint32_t *received)
{
    return PopDescriptors(v, items, count, 0xffffffffu, 0, received);
}

PB_RESULT PbQueuePopBudget(PB_QUEUE_VIEW *v, PB_DESCRIPTOR *items, uint32_t count, uint32_t byteBudget, uint32_t *received)
{
    return PopDescriptors(v, items, count, byteBudget, 1, received);
}

PB_RESULT PbQueuePopTokens(PB_QUEUE_VIEW *v, PB_TOKEN *tokens, uint32_t count, uint32_t *received)
{
    if (received) *received = 0;
    if (!received || !tokens || !count || count > PB_MAX_BATCH) return PB_INVALID;
    uint32_t available;
    PB_RESULT status = Available(v, &available);
    if (status != PB_OK) return status;
    if (!available) return PB_EMPTY;
    if (count > available) count = available;
    for (uint32_t i = 0; i < count; ++i)
        memcpy(tokens + i, &v->Entries[(uint32_t)(v->Cursor + i) & (v->Capacity - 1)].Token, sizeof(*tokens));
    PopCommit(v, count, received);
    return PB_OK;
}

PB_RESULT PbQueueArm(PB_QUEUE_VIEW *v, uint32_t *ready)
{
    if (!ready || !v || !v->Shared || v->Producer) return PB_INVALID;
    *ready = 0;
    Exchange32(&v->Shared->Armed, 1);
    uint32_t available;
    PB_RESULT status = Available(v, &available);
    if (status != PB_OK || available) Exchange32(&v->Shared->Armed, 0);
    if (status == PB_OK) *ready = available != 0;
    return status;
}

PB_RESULT PbPoolInitialize(PB_POOL *pool, PB_SLOT *slots, uint32_t count, uint64_t epoch, uint16_t queue)
{
    if (!pool || !slots || !count || count > PB_MAX_SLOTS || !epoch || queue >= PB_MAX_QUEUES) return PB_INVALID;
    memset(slots, 0, count * sizeof(*slots)); memset(pool, 0, sizeof(*pool));
    pool->Slots = slots; pool->Count = count; pool->Epoch = epoch; pool->Queue = queue;
    for (uint32_t word = 0; word < sizeof(pool->FreeBits) / sizeof(pool->FreeBits[0]); ++word) {
        if (word * 32u >= count) break;
        uint32_t remaining = count - word * 32u;
        pool->FreeBits[word] = remaining >= 32u ? 0xffffffffu : (1u << remaining) - 1u;
    }
    return PB_OK;
}

/* The serialized caller already checked this slot and its index bit. Retire
 * exhausted generations permanently; never wrap a token generation to zero. */
static __forceinline int ReserveSlot(PB_POOL *pool, uint32_t slot, PB_TOKEN *token)
{
    PB_SLOT *s = pool->Slots + slot;
    pool->FreeBits[slot / 32u] &= ~(1u << (slot & 31u));
    if (s->Generation == 0xffffffffu) { s->State = PB_SLOT_RETIRED; return 0; }
    s->Generation++; s->State = PB_SLOT_RESERVED;
    pool->Next = slot + 1u == pool->Count ? 0 : slot + 1u;
    token->Epoch = pool->Epoch; token->Generation = s->Generation; token->Slot = (uint16_t)slot; token->Queue = pool->Queue;
    return 1;
}

/* Keep the sparse/full search out of the common free-Next path, including its
 * register/stack setup. This helper may inspect only private index candidates. */
static __declspec(noinline) PB_RESULT ReserveIndexed(PB_POOL *pool, PB_TOKEN *token, uint32_t cursor)
{
    uint32_t first = cursor / 32u, words = (pool->Count + 31u) / 32u;
    uint32_t low = (1u << (cursor & 31u)) - 1u, word = first;
    uint32_t visits = words + (low != 0);
    uint32_t candidates = pool->FreeBits[word] & ~low;
    /* Visit the high part of the first word, later words around the pool, then
     * its low part. At most 17 word probes for 512 slots, independent of how
     * many busy slots separate free ones. Candidate slots remain validated. */
    for (uint32_t visited = 0; visited < visits; ++visited) {
        while (candidates) {
            unsigned long bit;
            (void)_BitScanForward(&bit, candidates);
            uint32_t slot = word * 32u + bit;
            if (slot >= pool->Count || pool->Slots[slot].State != PB_SLOT_FREE) return PB_CORRUPT;
            if (ReserveSlot(pool, slot, token)) return PB_OK;
            candidates &= candidates - 1u;
        }
        if (visited + 1u == visits) break;
        if (++word == words) word = 0;
        candidates = pool->FreeBits[word];
        if (word == first) candidates &= low;
    }
    pool->Next = cursor;
    return PB_FULL;
}

PB_RESULT PbPoolReserve(PB_POOL *pool, PB_TOKEN *token)
{
    if (!pool || !token || !pool->Slots || !pool->Count || pool->Count > PB_MAX_SLOTS) return PB_INVALID;
    /* Keep Next bounded: uint32 wrap followed by modulo can revisit a slot and skip a free one. */
    uint32_t cursor = pool->Next;
    if (cursor >= pool->Count) cursor %= pool->Count;
    if (pool->Slots[cursor].State == PB_SLOT_FREE) {
        if (!(pool->FreeBits[cursor / 32u] & (1u << (cursor & 31u)))) return PB_CORRUPT;
        if (ReserveSlot(pool, cursor, token)) return PB_OK;
    }
    return ReserveIndexed(pool, token, cursor);
}

static PB_RESULT ValidateToken(PB_POOL *pool, const PB_TOKEN *token, uint32_t state)
{
    if (!pool || !pool->Slots || !token || token->Slot >= pool->Count || token->Queue != pool->Queue || token->Epoch != pool->Epoch ||
        !token->Generation || pool->Slots[token->Slot].Generation != token->Generation) return PB_STALE;
    return pool->Slots[token->Slot].State == state ? PB_OK : PB_BUSY;
}

PB_RESULT PbPoolTransition(PB_POOL *pool, const PB_TOKEN *token, uint32_t before, uint32_t after)
{
    if (before > PB_SLOT_COMPLETE || after > PB_SLOT_COMPLETE) return PB_INVALID;
    /* Reuse must pass through FREE -> RESERVED, which increments generation. */
    if (!((before == PB_SLOT_RESERVED && (after == PB_SLOT_FREE || after == PB_SLOT_PUBLISHED)) ||
          (before == PB_SLOT_PUBLISHED && (after == PB_SLOT_FREE || after == PB_SLOT_NDIS || after == PB_SLOT_COMPLETE)) ||
          (before == PB_SLOT_NDIS && after == PB_SLOT_COMPLETE) || (before == PB_SLOT_COMPLETE && after == PB_SLOT_FREE))) return PB_INVALID;
    PB_RESULT result = ValidateToken(pool, token, before);
    if (result == PB_OK) {
        pool->Slots[token->Slot].State = after;
        if (after == PB_SLOT_FREE) pool->FreeBits[token->Slot / 32u] |= 1u << (token->Slot & 31u);
    }
    return result;
}

/* Short batches keep the cheap exact scan. Larger batches use a local one-word
 * filter to skip rescanning when the low six slot bits differ. A hit still
 * requires exact comparison; this value never authorizes/rejects ownership. */
static __forceinline int ShouldScanEarlierSlots(uint64_t *seen, uint32_t slot, uint32_t count)
{
    if (count <= 8u) return 1;
    uint64_t bit = (uint64_t)1 << (slot & 63u);
    int hit = (*seen & bit) != 0;
    *seen |= bit;
    return hit;
}

PB_RESULT PbPoolImport(PB_POOL *pool, const PB_DESCRIPTOR *items, uint32_t count)
{
    if (!pool || !pool->Slots || !pool->Count || pool->Count > PB_MAX_SLOTS || !items || !count || count > PB_MAX_BATCH)
        return PB_INVALID;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PB_TOKEN *t = &items[i].Token;
        if (!t->Generation || t->Slot >= pool->Count || t->Epoch != pool->Epoch || t->Queue != pool->Queue ||
            t->Generation <= pool->Slots[t->Slot].Generation) return PB_STALE;
        if (pool->Slots[t->Slot].State != PB_SLOT_FREE) return PB_BUSY;
        if (!(pool->FreeBits[t->Slot / 32u] & (1u << (t->Slot & 31u)))) return PB_CORRUPT;
        if (ShouldScanEarlierSlots(&seen, t->Slot, count))
            for (uint32_t j = 0; j < i; ++j) if (items[j].Token.Slot == t->Slot) return PB_INVALID;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const PB_TOKEN *t = &items[i].Token;
        pool->Slots[t->Slot].Generation = t->Generation; pool->Slots[t->Slot].State = PB_SLOT_PUBLISHED;
        pool->FreeBits[t->Slot / 32u] &= ~(1u << (t->Slot & 31u));
    }
    return PB_OK;
}

PB_RESULT PbValidateTokens(PB_POOL *pool, const PB_TOKEN *tokens, uint32_t count, uint32_t state)
{
    if (!tokens || !count || count > PB_MAX_BATCH) return PB_INVALID;
    if (count == 1) return ValidateToken(pool, tokens, state);
    uint64_t seen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        PB_RESULT status = ValidateToken(pool, tokens + i, state);
        if (status != PB_OK) return status;
        if (ShouldScanEarlierSlots(&seen, tokens[i].Slot, count))
            for (uint32_t j = 0; j < i; ++j) if (tokens[j].Slot == tokens[i].Slot) return PB_INVALID;
    }
    return PB_OK;
}

PB_RESULT PbValidateDescriptor(const PB_LAYOUT *layout, uint64_t epoch, uint32_t queue, int rx, const PB_DESCRIPTOR *d, uint32_t types)
{
    if (!layout || !d || !epoch || queue >= layout->QueueCount || d->Token.Epoch != epoch || d->Token.Queue != queue ||
        d->Token.Slot >= layout->SlotCount || !d->Token.Generation || d->Reserved16 || d->Reserved64[0] || d->Reserved64[1] ||
        d->Status || !d->Length || d->Length > PB_BLOCK_SIZE || d->Type > PB_DESC_UDP_GSO || !(types & (1u << d->Type)) ||
        (d->Flags & ~(PB_CSUM_IPV4 | PB_CSUM_TCP | PB_CSUM_UDP | PB_UDP_NO_CHECKSUM))) return PB_INVALID;
    uint64_t offset = (rx ? layout->Queues[queue].RxData : layout->Queues[queue].TxData) + (uint64_t)d->Token.Slot * PB_BLOCK_SIZE;
    if (d->DataOffset != offset || offset + d->Length > layout->RegionSize) return PB_INVALID;
    if (d->Type == PB_DESC_PLAIN) {
        if (d->Length > 65535 || d->Mss || d->L4Offset || d->HeaderLength || d->IpVersion || d->Protocol || d->Flags) return PB_INVALID;
    } else if (rx || !d->Mss || (d->IpVersion != 4 && d->IpVersion != 6) ||
        d->Flags != ((d->IpVersion == 4 ? PB_CSUM_IPV4 : 0u) | (d->Type == PB_DESC_TCP_GSO ? PB_CSUM_TCP :
            (d->Flags & PB_UDP_NO_CHECKSUM) ? PB_UDP_NO_CHECKSUM : PB_CSUM_UDP)) ||
        d->L4Offset < (d->IpVersion == 4 ? 20u : 40u) ||
        d->HeaderLength > d->Length || d->HeaderLength < (uint32_t)d->L4Offset + (d->Type == PB_DESC_TCP_GSO ? 20u : 8u) ||
        d->Protocol != (d->Type == PB_DESC_TCP_GSO ? 6 : 17)) return PB_INVALID;
    return PB_OK;
}

PB_RESULT PbAdmitReceive(const PB_LAYOUT *layout, uint64_t epoch, uint32_t queue, PB_RX_SLOT *slots,
    const PB_DESCRIPTOR *d, int draining, uint32_t *complete)
{
    if (!layout || !slots || !d || !complete || queue >= layout->QueueCount) return PB_INVALID;
    *complete = 1;
    int scoped = epoch && d->Token.Epoch == epoch && d->Token.Queue == queue && d->Token.Slot < layout->SlotCount;
    if (scoped) {
        PB_RX_SLOT *slot = slots + d->Token.Slot;
        /* Even an otherwise malformed duplicate must not release an NDIS-held original. */
        if (d->Token.Generation && d->Token.Generation <= slot->Generation) {
            *complete = 0;
            return PB_STALE;
        }
        if (slot->Active) { *complete = 0; return PB_BUSY; }
        /* A failure completion also ends this generation. Remember it before
         * validating the payload, so a later corrected/repeated descriptor
         * cannot acquire a second completion or an NDIS reference. Foreign
         * tokens and generation zero never retire a local slot. */
        if (d->Token.Generation) slot->Generation = d->Token.Generation;
    }
    PB_RESULT result = PbValidateDescriptor(layout, epoch, queue, 1, d, 1);
    if (result != PB_OK) return result;
    if (draining) return PB_STOPPED;
    slots[d->Token.Slot].Active = 1;
    return PB_OK;
}

void PbLifetimeInitialize(PB_LIFETIME *life) { memset(life, 0, sizeof(*life)); life->State = PB_SESSION_RUNNING; }
PB_RESULT PbLifetimeAcquire(PB_LIFETIME *life)
{
    if (!life || life->State != PB_SESSION_RUNNING) return PB_STOPPED;
    if (life->References == 0xffffffffu) return PB_FULL;
    life->References++; return PB_OK;
}
PB_RESULT PbLifetimeRelease(PB_LIFETIME *life)
{
    if (!life || !life->References) return PB_INVALID;
    life->References--;
    if (!life->References && life->State != PB_SESSION_RUNNING) life->State = PB_SESSION_STOPPED;
    return PB_OK;
}
PB_RESULT PbLifetimeDrain(PB_LIFETIME *life, uint64_t now, uint32_t timeout)
{
    if (!life || !timeout || timeout > 60000) return PB_INVALID;
    if (life->State == PB_SESSION_RUNNING) {
        life->State = life->References ? PB_SESSION_DRAINING : PB_SESSION_STOPPED;
        life->Deadline = now + timeout;
    }
    return PbLifetimeObserve(life, now);
}
PB_RESULT PbLifetimeObserve(PB_LIFETIME *life, uint64_t now)
{
    if (!life) return PB_INVALID;
    if (life->State == PB_SESSION_DRAINING && now - life->Deadline < ((uint64_t)1 << 63)) life->State = PB_SESSION_QUARANTINED;
    return life->State == PB_SESSION_STOPPED ? PB_OK : life->State == PB_SESSION_QUARANTINED ? PB_QUARANTINED : PB_BUSY;
}
