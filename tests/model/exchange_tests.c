/* SPDX-License-Identifier: GPL-2.0 */
#pragma warning(disable:4505) /* Shared fixture also exposes benchmark entry points. */
#include "exchange_fixture.h"
static VOID Notifications(VOID)
{
    const ULONG counts[] = { 1, 8, 16, 32, PB_MAX_BATCH };
    for (ULONG ci = 0; ci < _countof(counts); ++ci) {
        ULONG count = counts[ci]; EX_FIXTURE *f = ExCreate(1, count, 512);
        PB_USER_QUEUE *q = f->S.Queues; EX_PEER *p = f->Peer;
        for (ULONG r = 0; r < PB_QUEUE_COUNT; ++r) {
            ULONG64 base = UINT64_MAX - 17;
            q->Rings[r].Cursor = q->Rings[r].PeerCursor = base;
            p->Rings[r].Cursor = p->Rings[r].PeerCursor = base;
            q->Rings[r].Shared->Producer = q->Rings[r].Shared->Consumer = base;
        }
        PB_TEST_GUARD storage = PbTestGuardCreate(count * sizeof(PB_DESCRIPTOR));
        PB_DESCRIPTOR *out = (PB_DESCRIPTOR *)PbTestGuardSpan(&storage, count * sizeof(*out));
        PB_LEASE leases[PB_MAX_BATCH]; uint32_t n, notify, accepted; EX_METRICS m = { 0 };
        ExCurrent = &m;
        EX_CHECK(PollCompletions(q, out, count, &n) == PB_EMPTY && !n && !m.Sets);
        EX_CHECK(ReserveBatch(q, leases, count, &n) == PB_OK && n == count);
        for (ULONG i = 0; i < count; ++i) leases[i].Descriptor.Length = 1500;
        EX_CHECK(CommitBatch(q, leases, count, &n) == PB_OK && n == count && !m.Sets);
        EX_CHECK(PbQueuePopBudget(p->Rings + PB_QUEUE_RX_SUBMIT, out, count, 1024 * 1024, &n) == PB_OK && n == count);
        EX_CHECK(PbQueuePush(p->Rings + PB_QUEUE_RX_COMPLETE, out, n, &accepted, &notify) == PB_OK && accepted == n);
        EX_CHECK(PollCompletions(q, out, count, &n) == PB_OK && n == count && m.Sets == 1);
        EX_CHECK(PollCompletions(q, out, count, &n) == PB_EMPTY && !n && m.Sets == 1);
        PB_TEST_GUARD inaccessible = PbTestGuardCreate(0);
        EX_CHECK(ReceiveBatch(q, (PB_LEASE *)inaccessible.End, PB_MAX_BATCH + 1, &n) == PB_INVALID && !n);
        EX_CHECK(PollCompletions(q, (PB_DESCRIPTOR *)inaccessible.End, PB_MAX_BATCH + 1, &n) == PB_INVALID && !n && m.Sets == 1);
        ExCurrent = NULL; PbTestGuardDestroy(&inaccessible); PbTestGuardDestroy(&storage); ExDestroy(f);
    }
    puts("PASS: actual DLL completion notification: exactly one real SetEvent per nonempty poll; none for empty/oversized calls; guarded output bounds and uint64 cursor wrap.");
}
static VOID ByteBudgets(VOID)
{
    const ULONG lengths[] = { 64, 1500, 32768, 65535 }, budgets[] = { 65536, 131072, 524288 };
    for (ULONG li = 0; li < _countof(lengths); ++li) for (ULONG bi = 0; bi < _countof(budgets); ++bi) {
        ULONG count = PB_MAX_BATCH; EX_FIXTURE *f = ExCreate(1, count, 512);
        PB_USER_QUEUE *q = f->S.Queues; EX_PEER *p = f->Peer;
        PB_LEASE leases[PB_MAX_BATCH]; PB_DESCRIPTOR items[PB_MAX_BATCH]; uint32_t n, notify;
        EX_CHECK(ReserveBatch(q, leases, count, &n) == PB_OK && n == count);
        for (ULONG i = 0; i < count; ++i) leases[i].Descriptor.Length = lengths[li];
        EX_CHECK(CommitBatch(q, leases, count, &n) == PB_OK && n == count);
        ULONG received = 0;
        while (received < count) {
            EX_CHECK(PbQueuePopBudget(p->Rings + PB_QUEUE_RX_SUBMIT, items, count, budgets[bi], &n) == PB_OK);
            EX_CHECK(n == min(count - received, budgets[bi] / lengths[li])); received += n;
            uint32_t accepted; EX_CHECK(PbQueuePush(p->Rings + PB_QUEUE_RX_COMPLETE, items, n, &accepted, &notify) == PB_OK && accepted == n);
            EX_CHECK(PollCompletions(q, items, count, &accepted) == PB_OK && accepted == n);
        }
        ExDestroy(f);
    }
    puts("PASS: actual shared-core object/byte limits, partial batches and slot retirement at 64/1500/32768/65535 bytes.");
}
static VOID LayoutLimits(VOID)
{
    PB_LAYOUT layout; const ULONG queues[] = { 1, 2, 4, 8 };
#if PB_MAX_REGION == 64u * 1024u * 1024u
    const ULONG maxima[] = { 510, 254, 126, 62 };
#elif PB_MAX_REGION == 128u * 1024u * 1024u
    const ULONG maxima[] = { 512, 510, 254, 126 };
#elif PB_MAX_REGION == 256u * 1024u * 1024u
    const ULONG maxima[] = { 512, 512, 510, 254 };
#else
#error Add independently calculated layout boundaries for this test-only region size.
#endif
    for (ULONG i = 0; i < _countof(queues); ++i) {
        EX_CHECK(PbCalculateLayout(queues[i], maxima[i], 512, &layout) == PB_OK && layout.RegionSize <= PB_MAX_REGION);
        EX_CHECK(PbCalculateLayout(queues[i], maxima[i] + 1, 512, &layout) == PB_INVALID);
    }
    printf("PASS: actual layout bounds at ring capacity 512: region_limit=%u MiB, queues 1/2/4/8 allow %lu/%lu/%lu/%lu slots (ring storage also counts).\n",
        PB_MAX_REGION / 1024u / 1024u, maxima[0], maxima[1], maxima[2], maxima[3]);
}
static VOID TokenTransactions(VOID)
{
    const ULONG counts[] = { 1, 8, 32, PB_MAX_BATCH };
    PB_SLOT slots[PB_MAX_SLOTS], saved[PB_MAX_SLOTS]; PB_POOL pool;
    for (ULONG ci = 0; ci < _countof(counts); ++ci) {
        ULONG count = counts[ci];
        PB_TEST_GUARD descriptors = PbTestGuardCreate(count * sizeof(PB_DESCRIPTOR));
        PB_TEST_GUARD tokenMemory = PbTestGuardCreate(count * sizeof(PB_TOKEN));
        PB_DESCRIPTOR *items = (PB_DESCRIPTOR *)PbTestGuardSpan(&descriptors, count * sizeof(*items));
        PB_TOKEN *tokens = (PB_TOKEN *)PbTestGuardSpan(&tokenMemory, count * sizeof(*tokens));
        for (ULONG mode = 0; mode < 4; ++mode) {
            if (!mode && count == 1) continue;
            EX_CHECK(PbPoolInitialize(&pool, slots, PB_MAX_SLOTS, 42, 0) == PB_OK);
            memset(items, 0, count * sizeof(*items));
            for (ULONG i = 0; i < count; ++i) items[i].Token = (PB_TOKEN){ 42, 1, (uint16_t)i, 0 };
            PB_RESULT expected = PB_OK;
            if (!mode) { items[count - 1] = items[0]; expected = PB_INVALID; }
            if (mode == 1) { items[count - 1].Token.Generation = 0; expected = PB_STALE; }
            if (mode == 2) { slots[count - 1].State = PB_SLOT_PUBLISHED; expected = PB_BUSY; }
            PB_POOL snapshot = pool; memcpy(saved, slots, sizeof(saved));
            EX_CHECK(PbPoolImport(&pool, items, count) == expected);
            if (expected != PB_OK) EX_CHECK(!memcmp(&snapshot, &pool, sizeof(pool)) && !memcmp(saved, slots, sizeof(saved)));
            else {
                for (ULONG i = 0; i < count; ++i) tokens[i] = items[i].Token;
                EX_CHECK(PbValidateTokens(&pool, tokens, count, PB_SLOT_PUBLISHED) == PB_OK);
                snapshot = pool; memcpy(saved, slots, sizeof(saved));
                if (count > 1) {
                    tokens[count - 1] = tokens[0];
                    EX_CHECK(PbValidateTokens(&pool, tokens, count, PB_SLOT_PUBLISHED) == PB_INVALID);
                    tokens[count - 1].Generation = 0;
                    EX_CHECK(PbValidateTokens(&pool, tokens, count, PB_SLOT_PUBLISHED) == PB_STALE);
                }
                EX_CHECK(!memcmp(&snapshot, &pool, sizeof(pool)) && !memcmp(saved, slots, sizeof(saved)));
            }
        }
        PbTestGuardDestroy(&descriptors); PbTestGuardDestroy(&tokenMemory);
    }
    puts("PASS: guarded full-width token import/validation, duplicate slots, stale-before-duplicate error order, busy ownership and transactional rollback.");
}
int main(VOID)
{
    DWORD before, after; EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    Notifications(); ByteBudgets(); LayoutLimits(); TokenTransactions();
    ULONG batch = min(PB_MAX_BATCH, 64u), slots = batch * 2;
    for (ULONG tx = 0; tx < 2; ++tx) for (ULONG length = 64; length <= 32768; length = length == 64 ? 1500 : length == 1500 ? 32768 : 32769) {
        EX_RESULT r = ExRun((BOOL)tx, 2, slots, 256, batch, 131072, length, 16391, FALSE);
        EX_CHECK(r.Seconds > 0 && r.User.Objects == 16391);
    }
    EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after);
    printf("PASS: concurrent actual DLL/core HostTx and HostRx, six real-event runs, exact ordered payload markers/tokens, partial tails, zero residual loans/published objects and no handle leak; max_batch=%u.\n", PB_MAX_BATCH);
    return 0;
}
