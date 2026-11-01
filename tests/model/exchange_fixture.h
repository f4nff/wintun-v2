/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Production DLL queue bodies and shared core, with an immediate hosted peer.
 * Windows events/locks/threads are real. No NDIS, driver, payload copy or crypto
 * timing is claimed. Only two 8-byte payload markers are touched per object. */
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/session_state.h"
#include <stdlib.h>
#include <string.h>
#define EX_CHECK SAFETY_CHECK
typedef struct EX_METRICS {
    ULONG64 Sets, Waits, Calls, Empty, Objects, Batches, Cycles, Cpu100ns;
    LARGE_INTEGER End;
} EX_METRICS;
static __declspec(thread) EX_METRICS *ExCurrent;
static BOOL WINAPI ExSetEvent(HANDLE event)
{ if (ExCurrent) ExCurrent->Sets++; return SetEvent(event); }
static DWORD WINAPI ExWaitMany(DWORD n, const HANDLE *events, BOOL all, DWORD timeout)
{ if (ExCurrent) ExCurrent->Waits++; return WaitForMultipleObjects(n, events, all, timeout); }
static DWORD ExWaitOne(HANDLE event)
{ if (ExCurrent) ExCurrent->Waits++; return WaitForSingleObject(event, 10000); }
#define SetEvent ExSetEvent
#define WaitForMultipleObjects ExWaitMany
#include "../../api/v2/session_queue.h"
#undef SetEvent
#undef WaitForMultipleObjects
static PB_RESULT StatusInternal(PB_SESSION *s, PB_V2_STATUS *status)
{ (void)s; (void)status; return PB_UNSUPPORTED; }

typedef struct EX_PEER {
    PB_QUEUE_VIEW Rings[PB_QUEUE_COUNT];
    PB_POOL Tx;
    PB_SLOT *TxSlots;
    PB_RX_SLOT *RxSlots;
} EX_PEER;
typedef struct EX_FIXTURE { PB_SESSION S; EX_PEER Peer[PB_MAX_QUEUES]; } EX_FIXTURE;
static EX_FIXTURE *ExCreate(ULONG queues, ULONG slots, ULONG capacity)
{
    EX_FIXTURE *f = calloc(1, sizeof(*f)); EX_CHECK(f);
    PB_SESSION *s = &f->S;
    EX_CHECK(PbCalculateLayout(queues, slots, capacity, &s->Layout) == PB_OK);
    s->Epoch = 42;
    s->Region = VirtualAlloc(NULL, s->Layout.RegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EX_CHECK(s->Region);
    s->SharedLifecycle = (PB_SHARED_LIFECYCLE *)(s->Region + s->Layout.LifecycleOffset);
    s->ClosingEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->ProgressEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    EX_CHECK(s->ClosingEvent && s->ProgressEvent);
    for (ULONG qi = 0; qi < queues; ++qi) {
        PB_USER_QUEUE *q = s->Queues + qi; EX_PEER *p = f->Peer + qi;
        q->Session = s; q->Index = qi;
        q->KernelEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        q->UserEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        EX_CHECK(q->KernelEvent && q->UserEvent);
        q->TxSlots = calloc(slots, sizeof(*q->TxSlots)); q->RxSlots = calloc(slots, sizeof(*q->RxSlots));
        p->TxSlots = calloc(slots, sizeof(*p->TxSlots)); p->RxSlots = calloc(slots, sizeof(*p->RxSlots));
        EX_CHECK(q->TxSlots && q->RxSlots && p->TxSlots && p->RxSlots);
        EX_CHECK(PbPoolInitialize(&q->Tx, q->TxSlots, slots, s->Epoch, (uint16_t)qi) == PB_OK);
        EX_CHECK(PbPoolInitialize(&q->Rx, q->RxSlots, slots, s->Epoch, (uint16_t)qi) == PB_OK);
        EX_CHECK(PbPoolInitialize(&p->Tx, p->TxSlots, slots, s->Epoch, (uint16_t)qi) == PB_OK);
        for (ULONG r = 0; r < PB_QUEUE_COUNT; ++r) {
            int userProducer = r == PB_QUEUE_TX_RECYCLE || r == PB_QUEUE_RX_SUBMIT;
            EX_CHECK(PbAttachQueue(q->Rings + r, s->Region, &s->Layout, qi, r, userProducer) == PB_OK);
            EX_CHECK(PbAttachQueue(p->Rings + r, s->Region, &s->Layout, qi, r, !userProducer) == PB_OK);
        }
        /* Fault in every data page that will be touched, before timing. */
        for (ULONG slot = 0; slot < slots; ++slot) for (ULONG page = 0; page < PB_BLOCK_SIZE; page += 4096) {
            s->Region[s->Layout.Queues[qi].TxData + slot * PB_BLOCK_SIZE + page] = 0;
            s->Region[s->Layout.Queues[qi].RxData + slot * PB_BLOCK_SIZE + page] = 0;
        }
    }
    return f;
}
static VOID ExDestroy(EX_FIXTURE *f)
{
    PB_SESSION *s = &f->S;
    for (ULONG qi = 0; qi < s->Layout.QueueCount; ++qi) {
        PB_USER_QUEUE *q = s->Queues + qi; EX_PEER *p = f->Peer + qi;
        EX_CHECK(!q->Loans && !q->Published && !s->Fatal);
        for (ULONG slot = 0; slot < s->Layout.SlotCount; ++slot)
            EX_CHECK(q->RxSlots[slot].State == PB_SLOT_FREE && q->TxSlots[slot].State == PB_SLOT_FREE &&
                p->TxSlots[slot].State == PB_SLOT_FREE && !p->RxSlots[slot].Active);
        for (ULONG r = 0; r < PB_QUEUE_COUNT; ++r)
            EX_CHECK(q->Rings[r].Shared->Consumer == q->Rings[r].Shared->Producer);
        EX_CHECK(CloseHandle(q->KernelEvent) && CloseHandle(q->UserEvent));
        free(q->TxSlots); free(q->RxSlots); free(p->TxSlots); free(p->RxSlots);
    }
    EX_CHECK(CloseHandle(s->ClosingEvent) && CloseHandle(s->ProgressEvent));
    EX_CHECK(VirtualFree(s->Region, 0, MEM_RELEASE)); free(f);
}
static VOID ExMark(UCHAR *data, ULONG length, ULONG qi, ULONG sequence)
{
    ULONG64 marker = ((ULONG64)qi << 48) | sequence, tail = ~marker;
    memcpy(data, &marker, sizeof(marker)); memcpy(data + length - sizeof(tail), &tail, sizeof(tail));
}
static VOID ExCheckMark(const UCHAR *data, ULONG length, ULONG qi, ULONG sequence)
{
    ULONG64 marker, tail, expected = ((ULONG64)qi << 48) | sequence;
    memcpy(&marker, data, sizeof(marker)); memcpy(&tail, data + length - sizeof(tail), sizeof(tail));
    EX_CHECK(marker == expected && tail == ~expected);
}
#pragma warning(push)
#pragma warning(disable:4324) /* Intentional per-thread cache-line padding. */
typedef struct __declspec(align(64)) EX_WORK {
    EX_FIXTURE *F;
    HANDLE Start;
    volatile LONG *Ready;
    ULONG Qi, Total, Batch, Budget, Length;
    BOOL Tx, User;
    DWORD_PTR Cpu;
    EX_METRICS M;
} EX_WORK;
#pragma warning(pop)
static VOID ExUserRx(EX_WORK *w)
{
    PB_USER_QUEUE *q = w->F->S.Queues + w->Qi;
    PB_LEASE leases[PB_MAX_BATCH]; PB_DESCRIPTOR done[PB_MAX_BATCH];
    ULONG sent = 0, received = 0;
    while (received < w->Total) {
        uint32_t n; BOOL progress = FALSE;
        w->M.Calls++; PB_RESULT r = PollCompletions(q, done, w->Batch, &n);
        EX_CHECK(r == PB_OK || r == PB_EMPTY);
        for (ULONG i = 0; i < n; ++i) EX_CHECK(done[i].Status == PB_OK && done[i].Length == w->Length);
        if (n) { received += n; w->M.Batches++; progress = TRUE; } else w->M.Empty++;
        if (sent < w->Total) {
            w->M.Calls++; r = ReserveBatch(q, leases, min(w->Batch, w->Total - sent), &n);
            EX_CHECK(r == PB_OK || r == PB_FULL);
            if (n) {
                for (ULONG i = 0; i < n; ++i) {
                    leases[i].Descriptor.Length = w->Length;
                    ExMark(leases[i].Data, w->Length, w->Qi, sent + i);
                }
                uint32_t accepted; w->M.Calls++;
                EX_CHECK(CommitBatch(q, leases, n, &accepted) == PB_OK && accepted == n);
                sent += n; progress = TRUE;
            }
        }
        if (!progress && received < w->Total) { EX_CHECK(Wait(q, 10000) == PB_OK); }
    }
    EX_CHECK(sent == w->Total && received == w->Total); w->M.Objects = received;
}
static VOID ExPeerRx(EX_WORK *w)
{
    PB_SESSION *s = &w->F->S; PB_USER_QUEUE *q = s->Queues + w->Qi; EX_PEER *p = w->F->Peer + w->Qi;
    PB_DESCRIPTOR items[PB_MAX_BATCH]; ULONG received = 0;
    while (received < w->Total) {
        uint32_t n, notify; w->M.Calls++;
        PB_RESULT r = PbQueuePopBudget(p->Rings + PB_QUEUE_RX_SUBMIT, items, w->Batch, w->Budget, &n);
        EX_CHECK(r == PB_OK || r == PB_EMPTY);
        if (n) {
            for (ULONG i = 0; i < n; ++i) {
                uint32_t complete;
                EX_CHECK(items[i].Length == w->Length &&
                    PbAdmitReceive(&s->Layout, s->Epoch, w->Qi, p->RxSlots, items + i, 0, &complete) == PB_OK);
                ExCheckMark(s->Region + items[i].DataOffset, w->Length, w->Qi, received + i);
                p->RxSlots[items[i].Token.Slot].Active = FALSE;
            }
            uint32_t accepted; w->M.Calls++;
            EX_CHECK(PbQueuePush(p->Rings + PB_QUEUE_RX_COMPLETE, items, n, &accepted, &notify) == PB_OK && accepted == n);
            if (notify) EX_CHECK(ExSetEvent(q->UserEvent));
            received += n; w->M.Batches++;
        } else {
            uint32_t ready; w->M.Empty++;
            EX_CHECK(PbQueueArm(p->Rings + PB_QUEUE_RX_SUBMIT, &ready) == PB_OK);
            if (!ready) EX_CHECK(ExWaitOne(q->KernelEvent) == WAIT_OBJECT_0);
        }
    }
    w->M.Objects = received;
}
static VOID ExUserTx(EX_WORK *w)
{
    PB_USER_QUEUE *q = w->F->S.Queues + w->Qi;
    PB_LEASE leases[PB_MAX_BATCH]; PB_TOKEN tokens[PB_MAX_BATCH]; ULONG received = 0;
    while (received < w->Total) {
        uint32_t n; w->M.Calls++; PB_RESULT r = ReceiveBatch(q, leases, w->Batch, &n);
        EX_CHECK(r == PB_OK || r == PB_EMPTY);
        if (n) {
            for (ULONG i = 0; i < n; ++i) {
                EX_CHECK(leases[i].Descriptor.Length == w->Length);
                ExCheckMark(leases[i].Data, w->Length, w->Qi, received + i);
                tokens[i] = leases[i].Descriptor.Token;
            }
            uint32_t accepted; w->M.Calls++;
            EX_CHECK(ReleaseBatch(q, tokens, n, &accepted) == PB_OK && accepted == n);
            received += n; w->M.Batches++;
        } else { w->M.Empty++; EX_CHECK(Wait(q, 10000) == PB_OK); }
    }
    w->M.Objects = received;
}
static VOID ExPeerTx(EX_WORK *w)
{
    PB_SESSION *s = &w->F->S; PB_USER_QUEUE *q = s->Queues + w->Qi; EX_PEER *p = w->F->Peer + w->Qi;
    PB_DESCRIPTOR items[PB_MAX_BATCH]; PB_TOKEN tokens[PB_MAX_BATCH]; ULONG sent = 0, returned = 0;
    ULONG limit = min(w->Batch, max(1u, w->Budget / w->Length));
    while (returned < w->Total) {
        uint32_t n, notify; BOOL progress = FALSE; w->M.Calls++;
        PB_RESULT r = PbQueuePopTokens(p->Rings + PB_QUEUE_TX_RECYCLE, tokens, w->Batch, &n);
        EX_CHECK(r == PB_OK || r == PB_EMPTY);
        if (n) {
            /* Native session_recycle.h validates and retires each token with
             * PbPoolTransition; it does not run another batch validator. */
            for (ULONG i = 0; i < n; ++i)
                EX_CHECK(PbPoolTransition(&p->Tx, tokens + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE) == PB_OK);
            returned += n; progress = TRUE;
        }
        n = 0;
        while (n < limit && sent + n < w->Total) {
            PB_TOKEN token; r = PbPoolReserve(&p->Tx, &token); EX_CHECK(r == PB_OK || r == PB_FULL);
            if (r == PB_FULL) break;
            PB_DESCRIPTOR d = { 0 }; d.Token = token; d.Length = w->Length;
            d.DataOffset = s->Layout.Queues[w->Qi].TxData + token.Slot * PB_BLOCK_SIZE;
            ExMark(s->Region + d.DataOffset, w->Length, w->Qi, sent + n);
            EX_CHECK(PbPoolTransition(&p->Tx, &token, PB_SLOT_RESERVED, PB_SLOT_PUBLISHED) == PB_OK);
            items[n++] = d;
        }
        if (n) {
            uint32_t accepted; w->M.Calls++;
            EX_CHECK(PbQueuePush(p->Rings + PB_QUEUE_TX_READY, items, n, &accepted, &notify) == PB_OK && accepted == n);
            if (notify) EX_CHECK(ExSetEvent(q->UserEvent));
            sent += n; w->M.Batches++; progress = TRUE;
        }
        if (!progress && returned < w->Total) {
            uint32_t ready; w->M.Empty++;
            EX_CHECK(PbQueueArm(p->Rings + PB_QUEUE_TX_RECYCLE, &ready) == PB_OK);
            if (!ready) EX_CHECK(ExWaitOne(q->KernelEvent) == WAIT_OBJECT_0);
        }
    }
    EX_CHECK(sent == w->Total); w->M.Objects = returned;
}
static ULONG64 ExFileTime(FILETIME time)
{ return ((ULONG64)time.dwHighDateTime << 32) | time.dwLowDateTime; }
static DWORD WINAPI ExThread(VOID *opaque)
{
    EX_WORK *w = opaque; if (w->Cpu) EX_CHECK(SetThreadAffinityMask(GetCurrentThread(), w->Cpu));
    InterlockedIncrement(w->Ready); EX_CHECK(WaitForSingleObject(w->Start, 10000) == WAIT_OBJECT_0);
    ULONG64 first, last; FILETIME created, exited, kernel0, user0, kernel1, user1;
    EX_CHECK(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel0, &user0));
    EX_CHECK(QueryThreadCycleTime(GetCurrentThread(), &first)); ExCurrent = &w->M;
    if (w->Tx) { if (w->User) ExUserTx(w); else ExPeerTx(w); }
    else { if (w->User) ExUserRx(w); else ExPeerRx(w); }
    ExCurrent = NULL; EX_CHECK(QueryPerformanceCounter(&w->M.End));
    EX_CHECK(QueryThreadCycleTime(GetCurrentThread(), &last));
    EX_CHECK(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel1, &user1));
    w->M.Cycles = last - first;
    w->M.Cpu100ns = ExFileTime(kernel1) + ExFileTime(user1) - ExFileTime(kernel0) - ExFileTime(user0);
    return 0;
}
typedef struct EX_RESULT {
    EX_METRICS User, Peer;
    double Seconds;
    ULONG RegionBytes;
    DWORD_PTR Affinity;
} EX_RESULT;
static EX_RESULT ExRun(BOOL tx, ULONG queues, ULONG slots, ULONG capacity, ULONG batch,
    ULONG budget, ULONG length, ULONG total, BOOL pin)
{
    EX_CHECK(queues && queues <= PB_MAX_QUEUES && batch && batch <= PB_MAX_BATCH &&
        slots >= batch && budget >= PB_BLOCK_SIZE && length >= 64 && length <= 65535 && total >= queues);
    EX_FIXTURE *f = ExCreate(queues, slots, capacity); EX_RESULT result = { 0 };
    result.RegionBytes = f->S.Layout.RegionSize;
    DWORD_PTR allowed, system, cpus[64]; ULONG cpuCount = 0;
    EX_CHECK(GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)); result.Affinity = allowed;
    for (ULONG i = 0; i < sizeof(DWORD_PTR) * 8; ++i) if (allowed & ((DWORD_PTR)1 << i)) cpus[cpuCount++] = (DWORD_PTR)1 << i;
    EX_CHECK(!pin || cpuCount >= 2 * queues);
    EX_WORK *work = _aligned_malloc(2 * queues * sizeof(*work), 64); HANDLE threads[2 * PB_MAX_QUEUES];
    HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL); volatile LONG ready = 0; EX_CHECK(work && start);
    memset(work, 0, 2 * queues * sizeof(*work));
    for (ULONG qi = 0; qi < queues; ++qi) for (ULONG role = 0; role < 2; ++role) {
        ULONG i = 2 * qi + role; EX_WORK *w = work + i;
        w->F = f; w->Qi = qi; w->Total = total / queues + (qi < total % queues);
        w->Batch = batch; w->Budget = budget; w->Length = length; w->Tx = tx; w->User = role == 0;
        w->Ready = &ready; w->Start = start; w->Cpu = pin ? cpus[i] : 0;
        threads[i] = CreateThread(NULL, 0, ExThread, w, 0, NULL); EX_CHECK(threads[i]);
    }
    ULONGLONG deadline = GetTickCount64() + 10000;
    while (ReadAcquire(&ready) != (LONG)(2 * queues)) { EX_CHECK(GetTickCount64() < deadline); SwitchToThread(); }
    LARGE_INTEGER begin, frequency; EX_CHECK(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&begin));
    EX_CHECK(SetEvent(start));
    EX_CHECK(WaitForMultipleObjects(2 * queues, threads, TRUE, 30000) == WAIT_OBJECT_0);
    LARGE_INTEGER end = begin;
    for (ULONG i = 0; i < 2 * queues; ++i) {
        EX_WORK *w = work + i; EX_METRICS *sum = w->User ? &result.User : &result.Peer;
        EX_CHECK(w->M.Objects == w->Total);
        sum->Sets += w->M.Sets; sum->Waits += w->M.Waits; sum->Calls += w->M.Calls;
        sum->Empty += w->M.Empty; sum->Objects += w->M.Objects; sum->Batches += w->M.Batches;
        sum->Cycles += w->M.Cycles; sum->Cpu100ns += w->M.Cpu100ns;
        if (w->M.End.QuadPart > end.QuadPart) end = w->M.End;
        EX_CHECK(CloseHandle(threads[i]));
    }
    result.Seconds = (double)(end.QuadPart - begin.QuadPart) / frequency.QuadPart;
    EX_CHECK(result.User.Objects == total && result.Peer.Objects == total && CloseHandle(start));
    ExDestroy(f); _aligned_free(work); return result;
}
