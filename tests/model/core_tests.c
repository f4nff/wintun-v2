/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#include "../../common/offload_policy.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
_Static_assert(sizeof(PB_TOKEN) == 16, "token ABI");
_Static_assert(sizeof(PB_DESCRIPTOR) == 64, "descriptor ABI");
_Static_assert(sizeof(PB_QUEUE_SHARED) == 128, "queue ABI");
_Static_assert(offsetof(PB_QUEUE_SHARED, Consumer) == 64, "cache line separation");
_Static_assert(sizeof(PB_LAYOUT) == 296, "layout ABI v2.1");
_Static_assert(sizeof(PB_REGISTER_V2) == 248, "register ABI v2.1");

static void LayoutTests(void)
{
    PB_LAYOUT l;
    CHECK(PbCalculateLayout(1, 32, 64, &l) == PB_OK);
    CHECK(l.RegionSize < PB_MAX_REGION && l.BlockSize == 65536);
    for (unsigned q = 1; q <= 8; q++) {
        CHECK(PbCalculateLayout(q, 32, 64, &l) == PB_OK);
        for (unsigned i = 0; i < q; i++) {
            CHECK(!(l.Queues[i].TxData & 4095));
            CHECK(l.Queues[i].RxData == l.Queues[i].TxData + 32 * 65536);
            for (unsigned r = 0; r < 4; r++) CHECK(!(l.Queues[i].Rings[r] & 63));
            if (i) CHECK(l.Queues[i].Rings[0] >= l.Queues[i - 1].RxData + 32 * 65536);
        }
    }
    CHECK(PbCalculateLayout(0, 32, 64, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(9, 32, 64, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(1, 0, 64, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(1, 513, 1024, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(1, 32, 63, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(1, 32, 16, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(8, 512, 4096, &l) == PB_INVALID);
    CHECK(PbCalculateLayout(0xffffffffu, 0xffffffffu, 0xffffffffu, &l) == PB_INVALID);
}

static void QueueTests(void)
{
    PB_LAYOUT l;
    CHECK(PbCalculateLayout(1, 4, 4, &l) == PB_OK);
    void *region = VirtualAlloc(NULL, l.RegionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(region != NULL);
    PB_QUEUE_VIEW p, c;
    CHECK(PbAttachQueue(&p, region, &l, 0, 0, 1) == PB_OK);
    CHECK(PbAttachQueue(&c, region, &l, 0, 0, 0) == PB_OK);
    PB_DESCRIPTOR input[PB_MAX_BATCH] = { 0 }, output[PB_MAX_BATCH];
    for (unsigned i = 0; i < PB_MAX_BATCH; i++) input[i].Length = i;
    uint32_t n, notify, ready;
    CHECK(PbQueuePop(&c, output, 4, &n) == PB_EMPTY && n == 0);
    CHECK(PbQueueArm(&c, &ready) == PB_OK && !ready);
    CHECK(PbQueuePush(&p, input, 6, &n, &notify) == PB_OK && n == 4 && notify == 1);
    CHECK(PbQueuePush(&p, input, 1, &n, &notify) == PB_FULL && !n && !notify);
    CHECK(PbQueuePop(&c, output, 2, &n) == PB_OK && n == 2 && output[0].Length == 0 && output[1].Length == 1);
    CHECK(PbQueuePush(&p, input + 4, 2, &n, &notify) == PB_OK && n == 2);
    CHECK(PbQueuePop(&c, output, 4, &n) == PB_OK && n == 4);
    for (unsigned i = 0; i < 4; i++) CHECK(output[i].Length == i + 2);
    PB_DESCRIPTOR large[4] = { 0 };
    for (unsigned i = 0; i < 4; ++i) large[i].Length = 40000;
    CHECK(PbQueuePush(&p, large, 4, &n, &notify) == PB_OK && n == 4);
    CHECK(PbQueuePopBudget(&c, output, 4, 128 * 1024, &n) == PB_OK && n == 3);
    CHECK(PbQueuePop(&c, output, 4, &n) == PB_OK && n == 1);
    /* uint64 sequence wrap is valid; the ring uses bounded unsigned differences. */
    p.Cursor = p.PeerCursor = c.Cursor = c.PeerCursor = UINT64_MAX - 1;
    p.Shared->Producer = p.Shared->Consumer = p.Cursor;
    CHECK(PbQueuePush(&p, input, 4, &n, &notify) == PB_OK && p.Cursor == 2);
    CHECK(PbQueuePop(&c, output, 4, &n) == PB_OK && c.Cursor == 2);
    for (unsigned i = 0; i < 4; i++) CHECK(output[i].Length == i);
    /* Shared counters owned by this endpoint are not trusted as private state. */
    p.Shared->Producer = UINT64_MAX;
    CHECK(PbQueuePush(&p, input, 1, &n, &notify) == PB_OK);
    CHECK(PbQueueArm(&c, &ready) == PB_OK && ready == 1);
    CHECK(PbQueuePop(&c, output, 1, &n) == PB_OK);
    uint32_t space;
    CHECK(PbQueueSpace(&p, &space) == PB_OK && space == 4);
    p.Shared->Consumer = p.Cursor + 1;
    CHECK(PbQueuePush(&p, input, 1, &n, &notify) == PB_CORRUPT && n == 0);
    p.Shared->Consumer = p.Cursor - 1;
    CHECK(PbQueuePush(&p, input, 1, &n, &notify) == PB_CORRUPT && n == 0);
    p.Shared->Consumer = p.Cursor;
    p.Shared->Producer = c.Cursor + 5;
    CHECK(PbQueuePop(&c, output, 1, &n) == PB_CORRUPT && n == 0);
    p.Shared->Producer = c.Cursor - 1;
    CHECK(PbQueueArm(&c, &ready) == PB_CORRUPT && !p.Shared->Armed);
    CHECK(PbQueuePush(&p, input, PB_MAX_BATCH + 1, &n, &notify) == PB_INVALID);
    PB_LAYOUT bad = l; bad.Queues[0].TxData++;
    CHECK(PbAttachQueue(&p, region, &bad, 0, 0, 1) == PB_INVALID);
    VirtualFree(region, 0, MEM_RELEASE);
}

static void PoolTests(void)
{
    PB_POOL odd; PB_SLOT oddSlots[3]; PB_TOKEN oddToken;
    CHECK(PbPoolInitialize(&odd, oddSlots, 3, 1, 0) == PB_OK);
    PB_TOKEN held[3];
    for (unsigned i = 0; i < 3; ++i) CHECK(PbPoolReserve(&odd, held + i) == PB_OK);
    CHECK(PbPoolTransition(&odd, held + 1, PB_SLOT_RESERVED, PB_SLOT_FREE) == PB_OK);
    odd.Next = UINT32_MAX - 1;
    CHECK(PbPoolReserve(&odd, &oddToken) == PB_OK && oddToken.Slot == 1 && odd.Next < 3);
    for (unsigned i = 0; i < 100000; ++i) {
        CHECK(PbPoolTransition(&odd, &oddToken, PB_SLOT_RESERVED, PB_SLOT_FREE) == PB_OK);
        CHECK(PbPoolReserve(&odd, &oddToken) == PB_OK && oddToken.Slot == 1 && odd.Next < 3);
    }
    PB_POOL pool; PB_SLOT slots[4]; PB_TOKEN t[4], saved;
    CHECK(PbPoolInitialize(&pool, slots, 4, 42, 2) == PB_OK);
    for (unsigned i = 0; i < 4; i++) CHECK(PbPoolReserve(&pool, &t[i]) == PB_OK);
    saved = t[0];
    PB_TOKEN spare;
    CHECK(PbPoolReserve(&pool, &spare) == PB_FULL);
    CHECK(PbValidateTokens(&pool, t, 4, PB_SLOT_RESERVED) == PB_OK);
    PB_TOKEN duplicate[2] = { t[0], t[0] };
    CHECK(PbValidateTokens(&pool, duplicate, 2, PB_SLOT_RESERVED) == PB_INVALID);
    for (unsigned i = 0; i < 4; i++) CHECK(PbPoolTransition(&pool, &t[i], PB_SLOT_RESERVED, PB_SLOT_PUBLISHED) == PB_OK);
    CHECK(PbPoolTransition(&pool, &t[0], PB_SLOT_PUBLISHED, PB_SLOT_NDIS) == PB_OK);
    CHECK(PbPoolTransition(&pool, &t[0], PB_SLOT_NDIS, PB_SLOT_FREE) == PB_INVALID);
    CHECK(PbPoolTransition(&pool, &t[3], PB_SLOT_PUBLISHED, PB_SLOT_COMPLETE) == PB_OK);
    CHECK(PbPoolTransition(&pool, &t[3], PB_SLOT_COMPLETE, PB_SLOT_FREE) == PB_OK);
    CHECK(PbPoolReserve(&pool, &spare) == PB_OK && spare.Slot == 3 && spare.Generation == 2);
    CHECK(PbPoolTransition(&pool, &t[3], PB_SLOT_PUBLISHED, PB_SLOT_FREE) == PB_STALE);
    CHECK(PbPoolTransition(&pool, &t[0], PB_SLOT_NDIS, PB_SLOT_COMPLETE) == PB_OK);
    CHECK(PbPoolTransition(&pool, &t[0], PB_SLOT_COMPLETE, PB_SLOT_FREE) == PB_OK);
    CHECK(PbPoolTransition(&pool, &t[0], PB_SLOT_COMPLETE, PB_SLOT_FREE) == PB_BUSY);
    saved.Epoch++; CHECK(PbPoolTransition(&pool, &saved, PB_SLOT_FREE, PB_SLOT_RESERVED) == PB_INVALID);
    CHECK(PbValidateTokens(&pool, &saved, 1, PB_SLOT_FREE) == PB_STALE);
    saved = t[1]; saved.Queue++; CHECK(PbValidateTokens(&pool, &saved, 1, PB_SLOT_PUBLISHED) == PB_STALE);
    saved = t[1]; saved.Slot = 4; CHECK(PbValidateTokens(&pool, &saved, 1, PB_SLOT_PUBLISHED) == PB_STALE);
    slots[0].Generation = UINT32_MAX;
    CHECK(PbPoolReserve(&pool, &spare) == PB_FULL && slots[0].State == PB_SLOT_RETIRED);
    /* Randomized independent state model, with out-of-order release and reuse. */
    CHECK(PbPoolInitialize(&pool, slots, 4, 57, 0) == PB_OK);
    uint32_t model[4] = { 0 }, seed = 101;
    for (unsigned step = 0; step < 100000; step++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned index = (seed >> 20) % 4;
        if (!model[index]) {
            PB_RESULT result = PbPoolReserve(&pool, &spare);
            if (result == PB_OK) { CHECK(!model[spare.Slot]); t[spare.Slot] = spare; model[spare.Slot] = PB_SLOT_RESERVED; }
        } else {
            unsigned before = model[index], after = before == PB_SLOT_RESERVED ? PB_SLOT_PUBLISHED :
                before == PB_SLOT_PUBLISHED ? PB_SLOT_NDIS : before == PB_SLOT_NDIS ? PB_SLOT_COMPLETE : PB_SLOT_FREE;
            CHECK(PbPoolTransition(&pool, &t[index], before, after) == PB_OK); model[index] = after;
        }
        for (unsigned i = 0; i < 4; i++) CHECK(slots[i].State == model[i]);
    }
}

static void DescriptorTests(void)
{
    PB_LAYOUT l; CHECK(PbCalculateLayout(2, 4, 8, &l) == PB_OK);
    PB_DESCRIPTOR good = { .Token = { 7, 1, 3, 1 }, .Length = 40, .DataOffset = l.Queues[1].RxData + 3 * PB_BLOCK_SIZE };
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &good, 1) == PB_OK);
    PB_DESCRIPTOR d = good; d.Length = 65536;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.DataOffset = UINT32_MAX;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.Token.Slot = 4;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.Reserved64[1] = 1;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.Mss = 1200;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.Token.Epoch++;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 1) == PB_INVALID);
    d = good; d.Type = 32;
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 7) == PB_INVALID);
    d = good; d.DataOffset = l.Queues[1].TxData + 3 * PB_BLOCK_SIZE;
    d.Type = PB_DESC_TCP_GSO; d.Mss = 1200; d.Length = 5000; d.L4Offset = 20; d.HeaderLength = 40; d.IpVersion = 4; d.Protocol = 6;
    d.Flags = PB_CSUM_IPV4 | PB_CSUM_TCP;
    CHECK(PbValidateDescriptor(&l, 7, 1, 0, &d, 7) == PB_OK);
    CHECK(PbValidateDescriptor(&l, 7, 1, 1, &d, 7) == PB_INVALID);
    d.HeaderLength = 39; CHECK(PbValidateDescriptor(&l, 7, 1, 0, &d, 7) == PB_INVALID);
}

static void LifetimeTests(void)
{
    PB_LIFETIME life; PbLifetimeInitialize(&life);
    CHECK(PbLifetimeAcquire(&life) == PB_OK);
    CHECK(PbLifetimeAcquire(&life) == PB_OK);
    CHECK(PbLifetimeDrain(&life, 1000, 2000) == PB_BUSY);
    CHECK(PbLifetimeAcquire(&life) == PB_STOPPED);
    CHECK(PbLifetimeObserve(&life, 2999) == PB_BUSY);
    CHECK(PbLifetimeObserve(&life, 3000) == PB_QUARANTINED && life.References == 2);
    CHECK(PbLifetimeRelease(&life) == PB_OK && life.State == PB_SESSION_QUARANTINED);
    CHECK(PbLifetimeRelease(&life) == PB_OK && life.State == PB_SESSION_STOPPED);
    CHECK(PbLifetimeRelease(&life) == PB_INVALID);
    CHECK(PbLifetimeObserve(&life, 4000) == PB_OK);
    PbLifetimeInitialize(&life);
    CHECK(PbLifetimeDrain(&life, 0, 2000) == PB_OK);
    CHECK(PbLifetimeAcquire(&life) == PB_STOPPED);
    PbLifetimeInitialize(&life); CHECK(PbLifetimeAcquire(&life) == PB_OK);
    CHECK(PbLifetimeDrain(&life, UINT64_MAX - 10, 20) == PB_BUSY);
    CHECK(PbLifetimeObserve(&life, 8) == PB_BUSY);
    CHECK(PbLifetimeObserve(&life, 9) == PB_QUARANTINED);
}

static void ReceiveOwnershipTests(void)
{
    PB_LAYOUT l; CHECK(PbCalculateLayout(1, 4, 4, &l) == PB_OK);
    PB_RX_SLOT slots[4] = { 0 }; uint32_t complete;
    PB_DESCRIPTOR d = { 0 }; d.Token.Epoch = 123; d.Token.Generation = 1;
    d.Length = 20; d.DataOffset = l.Queues[0].RxData;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_OK && slots[0].Active);
    /* A duplicate cannot publish a failure completion that would release the NDIS-held original. */
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_STALE && !complete && slots[0].Active);
    d.Length = 0;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_STALE && !complete);
    d.Token.Generation = 2; d.Length = 20;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_BUSY && !complete && slots[0].Generation == 1);
    slots[0].Active = 0;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_OK && slots[0].Generation == 2);
    slots[0].Active = 0;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_STALE && !complete);
    d.Token.Generation = 3;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 1, &complete) == PB_STOPPED && complete && !slots[0].Active);
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_STALE && !complete);
    d.Token.Generation++;
    CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_OK);
    for (unsigned i = 0; i < 100000; ++i) {
        CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_STALE && !complete);
        CHECK(slots[0].Active && slots[0].Generation == d.Token.Generation);
        slots[0].Active = 0; d.Token.Generation++;
        CHECK(PbAdmitReceive(&l, 123, 0, slots, &d, 0, &complete) == PB_OK);
    }
}

static void ReceiveTerminalTests(void)
{
    PB_LAYOUT l; CHECK(PbCalculateLayout(2, 32, 64, &l) == PB_OK);
    PB_RX_SLOT slots[32] = { 0 }; PB_DESCRIPTOR good[32], bad[32];
    unsigned retired = 0;
    /* Complete all 32 independent rejections before checking the ledger. A
     * failure completion transfers ownership just as a successful one does. */
    for (unsigned i = 0; i < 32; ++i) {
        good[i] = (PB_DESCRIPTOR){ .Token = {123, i == 31 ? UINT32_MAX : 1, (uint16_t)i, 1},
            .Length = 20, .DataOffset = l.Queues[1].RxData + i * PB_BLOCK_SIZE };
        bad[i] = good[i];
        switch (i % 8) {
        case 0: bad[i].Reserved16 = 1; break;
        case 1: bad[i].Reserved64[0] = UINT64_MAX; break;
        case 2: bad[i].Length = 0; break;
        case 3: bad[i].Length = PB_BLOCK_SIZE + 1; break;
        case 4: bad[i].DataOffset++; break;
        case 5: bad[i].Type = PB_DESC_TCP_GSO; break;
        case 6: bad[i].Flags = PB_CSUM_IPV4; break;
        default: break; /* The valid descriptor is terminally stopped. */
        }
        uint32_t complete = 99;
        CHECK(PbAdmitReceive(&l, 123, 1, slots, bad + i, i % 8 == 7, &complete) ==
            (i % 8 == 7 ? PB_STOPPED : PB_INVALID));
        CHECK(complete == 1 && !slots[i].Active);
        retired += slots[i].Generation == good[i].Token.Generation;
    }
    printf("TERMINAL_RECEIVE32 rejected=28 stopped=4 retired=%u\n", retired);
    fflush(stdout);
    CHECK(retired == 32);
    for (unsigned i = 0; i < 32; ++i) {
        PB_RX_SLOT before[32]; memcpy(before, slots, sizeof(slots)); uint32_t complete = 99;
        CHECK(PbAdmitReceive(&l, 123, 1, slots, bad + i, 0, &complete) == PB_STALE && !complete);
        CHECK(PbAdmitReceive(&l, 123, 1, slots, good + i, 0, &complete) == PB_STALE && !complete);
        CHECK(!memcmp(before, slots, sizeof(slots)));
        PB_DESCRIPTOR next = good[i]; next.Token.Generation++;
        if (!next.Token.Generation) {
            CHECK(PbAdmitReceive(&l, 123, 1, slots, &next, 0, &complete) == PB_INVALID && complete);
            CHECK(!memcmp(before, slots, sizeof(slots))); continue;
        }
        CHECK(PbAdmitReceive(&l, 123, 1, slots, &next, 0, &complete) == PB_OK && slots[i].Active);
        PB_DESCRIPTOR future = next; future.Token.Generation++;
        CHECK(PbAdmitReceive(&l, 123, 1, slots, &future, 0, &complete) == PB_BUSY && !complete);
        CHECK(slots[i].Generation == next.Token.Generation && slots[i].Active);
        slots[i].Active = 0;
        CHECK(PbAdmitReceive(&l, 123, 1, slots, &next, 0, &complete) == PB_STALE && !complete);
        CHECK(PbAdmitReceive(&l, 123, 1, slots, &future, 0, &complete) == PB_OK && slots[i].Active);
        slots[i].Active = 0;
    }
    /* A response for another ownership domain cannot retire our local slot. */
    for (unsigned shape = 0; shape < 5; ++shape) {
        PB_DESCRIPTOR d = good[0]; d.Token.Generation = UINT32_MAX;
        uint64_t epoch = 123;
        switch (shape) {
        case 0: d.Token.Epoch--; break;
        case 1: d.Token.Queue = 0; break;
        case 2: d.Token.Slot = 32; break;
        case 3: d.Token.Generation = 0; break;
        default: epoch = d.Token.Epoch = 0; break;
        }
        PB_RX_SLOT before[32]; memcpy(before, slots, sizeof(slots)); uint32_t complete = 99;
        CHECK(PbAdmitReceive(&l, epoch, 1, slots, &d, 0, &complete) == PB_INVALID && complete);
        CHECK(!memcmp(before, slots, sizeof(slots)));
    }
    puts("PASS: terminal receive generations; old/duplicate, active future, fresh reuse, maximum generation and foreign scope.");
}

static void OffloadPolicyTests(void)
{
    CHECK(PbResolveOffloads(0, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) == 0);
    CHECK(PbResolveOffloads(PB_PROBE_SUPPORTED, 0, PB_PROBE_SUPPORTED, 1) == 0);
    CHECK(PbResolveOffloads(PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 0, 1) == 0);
    CHECK(PbResolveOffloads(PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 0) == 0);
    CHECK(PbResolveOffloads(PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) == PB_PROBE_SUPPORTED);
    CHECK(!(PbResolveOffloads(PB_RX_RSC4, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) & PB_RX_RSC4));
    CHECK(!(PbResolveOffloads(PB_RX_URO, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) & PB_RX_URO));
    CHECK(PbResolveOffloads(PB_TX_LSO4 | PB_TX_TCP4_CHECKSUM, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) & PB_TX_LSO4);
    CHECK(PbResolveOffloads(PB_TX_LSO6, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) & PB_TX_LSO6);
    CHECK(PbResolveOffloads(0xb6b3u, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) == 0xb6b3u);
    CHECK(PbResolveOffloads(0x1febfu, PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) == 0x1febfu);
    CHECK((PbResolveOffloads(0xb6b3u & ~(PB_TX_IP4_CHECKSUM | PB_TX_TCP4_CHECKSUM | PB_RX_TCP4_CHECKSUM),
        PB_PROBE_SUPPORTED, PB_PROBE_SUPPORTED, 1) & (PB_TX_LSO4 | PB_TX_LSO6 | PB_RX_RSC4 | PB_RX_RSC6)) == (PB_TX_LSO4 | PB_TX_LSO6 | PB_RX_RSC6));
    for (uint32_t parameter = 0; parameter <= 5; ++parameter) {
        uint32_t mask = PB_PROBE_SUPPORTED;
        PB_RESULT result = PbApplyChecksumParameter(parameter, PB_TX_UDP4_CHECKSUM, PB_RX_UDP4_CHECKSUM, &mask);
        CHECK(result == (parameter <= 4 ? PB_OK : PB_INVALID));
        if (parameter == 1) CHECK(!(mask & (PB_TX_UDP4_CHECKSUM | PB_RX_UDP4_CHECKSUM)));
        if (parameter == 2) CHECK((mask & PB_TX_UDP4_CHECKSUM) && !(mask & PB_RX_UDP4_CHECKSUM));
        if (parameter == 3) CHECK(!(mask & PB_TX_UDP4_CHECKSUM) && (mask & PB_RX_UDP4_CHECKSUM));
        CHECK(mask & PB_TX_USO4); /* Disabling UDP checksum does not disable USO. */
    }
    uint32_t mask = PB_PROBE_SUPPORTED;
    CHECK(PbApplyOffloadParameter(1, PB_RX_URO, &mask) == PB_OK && !(mask & PB_RX_URO));
    CHECK(PbApplyOffloadParameter(2, PB_RX_URO, &mask) == PB_OK && (mask & PB_RX_URO));
    CHECK(PbApplyOffloadParameter(3, PB_RX_URO, &mask) == PB_INVALID && mask == PB_PROBE_SUPPORTED);
}

#define STRESS_COUNT 1000000u
typedef struct STRESS { PB_QUEUE_VIEW Producer, Consumer; HANDLE Event; } STRESS;
static DWORD WINAPI Produce(void *arg)
{
    STRESS *s = arg;
    PB_DESCRIPTOR items[PB_MAX_BATCH] = { 0 };
    uint32_t next = 0;
    while (next < STRESS_COUNT) {
        uint32_t count = 1 + next % PB_MAX_BATCH;
        if (count > STRESS_COUNT - next) count = STRESS_COUNT - next;
        for (uint32_t i = 0; i < count; i++) { items[i].Token.Epoch = 123; items[i].Length = next + i; items[i].DataOffset = ~(next + i); }
        uint32_t n, notify;
        PB_RESULT result = PbQueuePush(&s->Producer, items, count, &n, &notify);
        CHECK(result == PB_OK || result == PB_FULL);
        next += n;
        if (notify) SetEvent(s->Event);
        if (!n) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI Consume(void *arg)
{
    STRESS *s = arg;
    PB_DESCRIPTOR items[PB_MAX_BATCH];
    uint32_t next = 0;
    while (next < STRESS_COUNT) {
        uint32_t n;
        PB_RESULT result = PbQueuePop(&s->Consumer, items, PB_MAX_BATCH, &n);
        CHECK(result == PB_OK || result == PB_EMPTY);
        for (uint32_t i = 0; i < n; i++, next++) CHECK(items[i].Length == next && items[i].DataOffset == ~next && items[i].Token.Epoch == 123);
        if (!n) {
            uint32_t ready;
            CHECK(PbQueueArm(&s->Consumer, &ready) == PB_OK);
            if (!ready) CHECK(WaitForSingleObject(s->Event, 5000) == WAIT_OBJECT_0);
        }
    }
    return 0;
}
static void ConcurrentQueueTests(void)
{
    PB_LAYOUT l; CHECK(PbCalculateLayout(1, 32, 64, &l) == PB_OK);
    void *region = VirtualAlloc(NULL, l.RegionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(region);
    STRESS s;
    CHECK(PbAttachQueue(&s.Producer, region, &l, 0, 0, 1) == PB_OK);
    CHECK(PbAttachQueue(&s.Consumer, region, &l, 0, 0, 0) == PB_OK);
    s.Event = CreateEventW(NULL, FALSE, FALSE, NULL); CHECK(s.Event);
    HANDLE threads[] = { CreateThread(NULL, 0, Produce, &s, 0, NULL), CreateThread(NULL, 0, Consume, &s, 0, NULL) };
    CHECK(threads[0] && threads[1]);
    CHECK(WaitForMultipleObjects(2, threads, TRUE, 20000) == WAIT_OBJECT_0);
    CloseHandle(threads[0]); CloseHandle(threads[1]); CloseHandle(s.Event); VirtualFree(region, 0, MEM_RELEASE);
}

int main(void)
{
    LayoutTests(); QueueTests(); PoolTests(); DescriptorTests(); LifetimeTests(); ReceiveOwnershipTests(); ReceiveTerminalTests(); OffloadPolicyTests(); ConcurrentQueueTests();
    puts("PASS: layout, queue, pool, descriptor, drain, offload policy; 100000 ownership transitions; 1000000 concurrent descriptors.");
    return 0;
}
