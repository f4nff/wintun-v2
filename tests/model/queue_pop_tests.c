/* SPDX-License-Identifier: GPL-2.0 */
/* Actual core, read-only guarded source and exact-sized guarded destinations.
 * An independent prefix oracle covers both ordinary and budgeted consumption. */
#include "../safety/guard_memory.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
static ULONG Publications;
static VOID (*BeforePublish)(VOID), (*AfterPublish)(VOID);
static __int64 Publish64(volatile __int64 *p, __int64 value)
{
    Publications++;
    if (BeforePublish) BeforePublish();
    __int64 old = _InterlockedExchange64(p, value);
    if (AfterPublish) AfterPublish();
    return old;
}
#define _InterlockedExchange64 Publish64
#ifndef PB_CORE_SOURCE
#define PB_CORE_SOURCE "../../common/v2_core.c"
#endif
#include PB_CORE_SOURCE
#undef _InterlockedExchange64

static PB_DESCRIPTOR Descriptor(ULONG serial)
{
    static const uint32_t lengths[] = { 0, 1, 20, PB_BLOCK_SIZE - 1, PB_BLOCK_SIZE, PB_BLOCK_SIZE + 1, UINT32_MAX };
    PB_DESCRIPTOR d;
    memset(&d, (int)(serial ^ 0xa5), sizeof(d));
    d.Token.Epoch = 42; d.Token.Generation = serial + 1; d.Length = lengths[serial % _countof(lengths)];
    return d;
}
static PB_QUEUE_VIEW View(PB_QUEUE_SHARED *shared, PB_DESCRIPTOR *entries, ULONG capacity, ULONG64 base, ULONG available)
{
    memset(shared, 0, sizeof(*shared)); shared->Producer = base + available; shared->Consumer = base;
    PB_QUEUE_VIEW v = { shared, entries, base, base, capacity, 0 }; return v;
}
static ULONG Expected(PB_DESCRIPTOR *entries, ULONG capacity, ULONG64 base, ULONG available, ULONG requested, ULONG budget)
{
    ULONG count = min(available, requested), taken = 0, bytes = 0;
    for (; taken < count; ++taken) {
        ULONG charge = min(entries[(ULONG)(base + taken) & (capacity - 1)].Length, PB_BLOCK_SIZE);
        if (taken && charge > budget - bytes) break;
        bytes += charge;
    }
    return taken;
}
static VOID Matrix(VOID)
{
    const ULONG capacities[] = { 1, 2, 4, 8, 32, 64, 4096 };
    const ULONG budgets[] = { PB_BLOCK_SIZE, 2 * PB_BLOCK_SIZE, UINT32_MAX };
    ULONG cases = 0;
    PB_TEST_GUARD destination = PbTestGuardCreate(PB_MAX_BATCH * sizeof(PB_DESCRIPTOR));
    for (ULONG ci = 0; ci < _countof(capacities); ++ci) {
        ULONG capacity = capacities[ci]; PB_TEST_GUARD source = PbTestGuardCreate(capacity * sizeof(PB_DESCRIPTOR));
        PB_DESCRIPTOR *entries = (PB_DESCRIPTOR *)PbTestGuardSpan(&source, capacity * sizeof(*entries));
        for (ULONG i = 0; i < capacity; ++i) entries[i] = Descriptor(i);
        DWORD old; CHECK(VirtualProtect(source.End - source.Capacity, source.Capacity, PAGE_READONLY, &old));
        const ULONG64 bases[] = { 0, capacity - 1, UINT64_MAX - 15 };
        for (ULONG bi = 0; bi < _countof(bases); ++bi) for (ULONG available = 0; available <= min(capacity, PB_MAX_BATCH); ++available)
            for (ULONG requested = 1; requested <= PB_MAX_BATCH; ++requested) for (ULONG mode = 0; mode <= _countof(budgets); ++mode) {
                ULONG budget = mode ? budgets[mode - 1] : UINT32_MAX;
                PB_QUEUE_SHARED shared; PB_QUEUE_VIEW v = View(&shared, entries, capacity, bases[bi], available);
                PB_DESCRIPTOR *output = (PB_DESCRIPTOR *)PbTestGuardSpan(&destination, requested * sizeof(*output));
                memset(output, 0x5a, requested * sizeof(*output));
                uint32_t count = 99; Publications = 0;
                PB_RESULT result = mode ? PbQueuePopBudget(&v, output, requested, budget, &count) : PbQueuePop(&v, output, requested, &count);
                ULONG expected = Expected(entries, capacity, bases[bi], available, requested, budget);
                CHECK(result == (expected ? PB_OK : PB_EMPTY) && count == expected && Publications == (ULONG)(expected != 0));
                CHECK(v.Cursor == bases[bi] + expected && shared.Consumer == v.Cursor && v.PeerCursor == shared.Producer);
                for (ULONG i = 0; i < expected; ++i) CHECK(!memcmp(output + i, entries + ((ULONG)(bases[bi] + i) & (capacity - 1)), sizeof(*output)));
                for (SIZE_T i = expected * sizeof(*output); i < requested * sizeof(*output); ++i) CHECK(((UCHAR *)output)[i] == 0x5a);
                cases++;
            }
        DWORD ignored; CHECK(VirtualProtect(source.End - source.Capacity, source.Capacity, old, &ignored)); PbTestGuardDestroy(&source);
    }
    PbTestGuardDestroy(&destination);
    printf("PASS: %lu guarded ordinary/budgeted prefix cases: byte-exact descriptors, zero/oversized lengths, tiny/4096-entry rings, partial reads, uint64 wrap, one publication.\n", cases);
}
static VOID Rejections(VOID)
{
    PB_TEST_GUARD inaccessible = PbTestGuardCreate(1);
    /* Empty/corrupt/invalid inputs must not touch descriptor or output pages. */
    PB_DESCRIPTOR *blocked = (PB_DESCRIPTOR *)inaccessible.End;
    for (ULONG budgeted = 0; budgeted < 2; ++budgeted) {
        PB_QUEUE_SHARED shared; PB_QUEUE_VIEW v = View(&shared, blocked, 8, 8, 0);
        uint32_t n = 99; Publications = 0;
#define POP(view, output, count, received) (budgeted ? PbQueuePopBudget(view, output, count, PB_BLOCK_SIZE, received) : PbQueuePop(view, output, count, received))
        CHECK(POP(&v, blocked, 32, &n) == PB_EMPTY && !n && !Publications);
        CHECK(POP(NULL, blocked, 1, &n) == PB_INVALID && !n);
        CHECK(POP(&v, NULL, 1, &n) == PB_INVALID && !n);
        CHECK(POP(&v, blocked, 0, &n) == PB_INVALID && !n);
        CHECK(POP(&v, blocked, 33, &n) == PB_INVALID && !n);
        CHECK(POP(&v, blocked, 1, NULL) == PB_INVALID);
        v.Producer = 1; CHECK(POP(&v, blocked, 1, &n) == PB_INVALID && !n); v.Producer = 0;
        shared.Producer = v.Cursor + 9; CHECK(POP(&v, blocked, 1, &n) == PB_CORRUPT && !n);
        shared.Producer = v.Cursor - 1; CHECK(POP(&v, blocked, 1, &n) == PB_CORRUPT && !n);
        shared.Producer = v.Cursor + 1; v.PeerCursor = shared.Producer + 1;
        CHECK(POP(&v, blocked, 1, &n) == PB_CORRUPT && !n && !Publications && v.Cursor == 8 && shared.Consumer == 8);
#undef POP
    }
    PB_QUEUE_SHARED shared; PB_QUEUE_VIEW v = View(&shared, blocked, 8, 8, 1); uint32_t n;
    CHECK(PbQueuePopBudget(&v, blocked, 1, PB_BLOCK_SIZE - 1, &n) == PB_INVALID && !n && !Publications);
    PbTestGuardDestroy(&inaccessible);
    puts("PASS: invalid inputs, undersized byte budget, empty and corrupt cursors do not touch inaccessible descriptors/output or publish consumption.");
}
static PB_DESCRIPTOR *ObservedOutput, *ObservedEntries, Original[PB_MAX_BATCH];
static ULONG ObservedCount;
static VOID CheckBeforePublish(VOID)
{
    CHECK(!memcmp(ObservedOutput, Original, ObservedCount * sizeof(*Original)));
}
static VOID ReuseAfterPublish(VOID)
{
    memset(ObservedEntries, 0xcc, ObservedCount * sizeof(*Original));
}
static VOID PublicationBoundary(VOID)
{
    PB_TEST_GUARD source = PbTestGuardCreate(sizeof(Original)), destination = PbTestGuardCreate(sizeof(Original));
    ObservedEntries = (PB_DESCRIPTOR *)PbTestGuardSpan(&source, sizeof(Original));
    ObservedOutput = (PB_DESCRIPTOR *)PbTestGuardSpan(&destination, sizeof(Original)); ObservedCount = PB_MAX_BATCH;
    for (ULONG budgeted = 0; budgeted < 2; ++budgeted) {
        for (ULONG i = 0; i < ObservedCount; ++i) Original[i] = Descriptor(i);
        memcpy(ObservedEntries, Original, sizeof(Original)); PB_QUEUE_SHARED shared;
        PB_QUEUE_VIEW v = View(&shared, ObservedEntries, ObservedCount, UINT64_MAX - 15, ObservedCount);
        /* Arrange the wrapped physical source to match the logical output. */
        for (ULONG i = 0; i < ObservedCount; ++i) ObservedEntries[(ULONG)(v.Cursor + i) & (ObservedCount - 1)] = Original[i];
        BeforePublish = CheckBeforePublish; AfterPublish = ReuseAfterPublish; Publications = 0; uint32_t n;
        PB_RESULT result = budgeted ? PbQueuePopBudget(&v, ObservedOutput, ObservedCount, UINT32_MAX, &n) : PbQueuePop(&v, ObservedOutput, ObservedCount, &n);
        BeforePublish = AfterPublish = NULL;
        CHECK(result == PB_OK && n == ObservedCount && Publications == 1 && !memcmp(ObservedOutput, Original, sizeof(Original)));
    }
    PbTestGuardDestroy(&source); PbTestGuardDestroy(&destination);
    puts("PASS: output is complete before Consumer publication; immediate peer reuse cannot change returned descriptors, including at uint64 wrap.");
}
int main(VOID)
{
    setvbuf(stdout, NULL, _IONBF, 0); Matrix(); Rejections(); PublicationBoundary(); return 0;
}
