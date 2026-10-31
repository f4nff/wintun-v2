/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include "packetbridge.h"
#include "adapter.h"
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_version.h"
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#ifdef PB_API_TESTS
#include "test.h"
#endif

#include "session_state.h"
#ifdef PB_API_TESTS
static void TestBoundary(PB_SESSION *s, uint32_t stage)
{
    if (s->Test) InterlockedExchange(s->TestBoundaryThreads + stage, (LONG)GetCurrentThreadId());
    if (s->Test && s->TestGate[stage]) {
        SetEvent(s->TestEntered[stage]);
        if (WaitForSingleObject(s->TestGate[stage], 10000) != WAIT_OBJECT_0) ExitProcess(70);
    }
}
static BOOL TestFails(PB_SESSION *s, uint32_t stage)
{
    if (s->Test && (InterlockedCompareExchange(&s->TestFailures, 0, 0) & (1L << stage))) {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED); return TRUE;
    }
    return FALSE;
}
#endif

#include "session_lifecycle.h"
#include "session_control.h"
#include "session_status.h"
#include "session_queue.h"

static BOOL ReleaseResources(PB_SESSION *s)
{
#ifdef PB_API_TESTS
    TestBoundary(s, PB_TEST_DEVICE_CLOSE);
    if (TestFails(s, PB_TEST_DEVICE_CLOSE)) return FALSE;
#endif
    /* A failed close does not establish the driver barrier. Retain the mapping. */
    if (s->Device != INVALID_HANDLE_VALUE && !CloseHandle(s->Device)) return FALSE;
    s->Device = INVALID_HANDLE_VALUE;
    for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) {
        PB_USER_QUEUE *q = s->Queues + i;
        if (q->KernelEvent) CloseHandle(q->KernelEvent);
        if (q->UserEvent) CloseHandle(q->UserEvent);
        free(q->TxSlots); free(q->RxSlots);
        q->KernelEvent = q->UserEvent = NULL; q->TxSlots = q->RxSlots = NULL;
#ifdef PB_API_TESTS
        free(s->TestTxSlots[i]);
        s->TestTxSlots[i] = NULL;
#endif
    }
    if (s->Region) VirtualFree(s->Region, 0, MEM_RELEASE);
    s->Region = NULL;
    AcquireSRWLockExclusive(&s->State);
    BOOL final = s->FinalRequested;
    if (final) { s->Progress.RetainedBytes = 0; s->Progress.LastProgress = GetTickCount64(); }
    ReleaseSRWLockExclusive(&s->State);
    if (final) Publish(s, PB_LIFE_RELEASING, PB_BLOCK_ADAPTER_CLOSE, PB_QUARANTINED, 0);
#ifdef PB_API_TESTS
    TestBoundary(s, PB_TEST_ADAPTER_CLOSE);
#endif
    if (s->Adapter) PbAdapterRelease(s->Adapter);
    s->Adapter = NULL; InterlockedExchange(&s->ResourcesClosed, 1); return TRUE;
}
static void Destroy(PB_SESSION *s)
{
    if (!s->ResourcesClosed && !ReleaseResources(s)) {
        /* An unpublished Open failure still needs an observable native owner. */
        DWORD error = GetLastError();
        Publish(s, PB_LIFE_RELEASING, PB_BLOCK_DEVICE_CLOSE, PB_CORRUPT, error);
        if (!s->Registered) PublishSession(s);
        ReleaseSession(s); return;
    }
    if (s->Work) CloseThreadpoolWork(s->Work);
    if (s->Pool) CloseThreadpool(s->Pool);
    if (s->ClosingEvent) CloseHandle(s->ClosingEvent);
    if (s->ProgressEvent) CloseHandle(s->ProgressEvent);
    RuntimeRemove(s);
    if (s->TailModule) FreeLibrary(s->TailModule); /* Unpublished construction failure, no callbacks exist. */
    if (s->Module) FreeLibrary(s->Module);
    free(s);
}
static PB_RESULT Allocate(const PB_CONFIG *config, PB_SESSION **out)
{
    if (out) *out = NULL;
    PB_LAYOUT layout;
    if (!config || !out || config->Size != sizeof(*config) || config->Flags & ~PB_OPEN_KNOWN_FLAGS ||
        config->Offloads & ~PB_PROBE_SUPPORTED || (config->Offloads && !(config->Flags & PB_OPEN_EXPERIMENTAL_OFFLOADS)) ||
        PbCalculateLayout(config->QueueCount, config->SlotCount, config->RingCapacity, &layout) != PB_OK) return PB_INVALID;
    if ((config->Flags & PB_OPEN_EXPERIMENTAL_OFFLOADS) && !(config->Flags & PB_OPEN_CHECKED_SEGMENTS)) return PB_UNSUPPORTED;
    PB_SESSION *s = calloc(1, sizeof(*s));
    if (!s) return PB_FULL;
    s->Device = INVALID_HANDLE_VALUE; s->Layout = layout; s->Experimental = !!(config->Flags & PB_OPEN_EXPERIMENTAL_OFFLOADS);
    /* The reservation covers construction as well as published/retained sessions.
     * Destroy owns rollback from this point, including every acquisition failure. */
    if (!RuntimeAdmit(s)) { free(s); return PB_FULL; }
    s->ClosingEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s->ClosingEvent) { Destroy(s); return PB_FULL; }
    s->ProgressEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s->ProgressEvent) { Destroy(s); return PB_FULL; }
    if (BCryptGenRandom(NULL, (PUCHAR)&s->Epoch, sizeof(s->Epoch), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !s->Epoch) { Destroy(s); return PB_FULL; }
    s->Region = VirtualAlloc(NULL, layout.RegionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s->Region) { Destroy(s); return PB_FULL; }
    memcpy(s->Region, &layout, sizeof(layout));
    s->SharedLifecycle = (PB_SHARED_LIFECYCLE *)(s->Region + layout.LifecycleOffset);
    for (uint32_t i = 0; i < layout.QueueCount; ++i) {
        PB_USER_QUEUE *q = s->Queues + i; q->Session = s; q->Index = i;
        q->KernelEvent = CreateEventW(NULL, FALSE, FALSE, NULL); q->UserEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        q->TxSlots = calloc(layout.SlotCount, sizeof(*q->TxSlots)); q->RxSlots = calloc(layout.SlotCount, sizeof(*q->RxSlots));
        if (!q->KernelEvent || !q->UserEvent || !q->TxSlots || !q->RxSlots) { Destroy(s); return PB_FULL; }
        PbPoolInitialize(&q->Tx, q->TxSlots, layout.SlotCount, s->Epoch, (uint16_t)i);
        PbPoolInitialize(&q->Rx, q->RxSlots, layout.SlotCount, s->Epoch, (uint16_t)i);
        for (uint32_t r = 0; r < PB_QUEUE_COUNT; ++r)
            PbAttachQueue(&q->Rings[r], s->Region, &layout, i, r, r == PB_QUEUE_TX_RECYCLE || r == PB_QUEUE_RX_SUBMIT);
    }
    if (!InitializeLifecycle(s)) { Destroy(s); return PB_FULL; }
    *out = s; return PB_OK;
}

/* Consumes StartAdapterSession's retained adapter on every outcome. Once a
 * session exists, even failed Open cleanup owns that reference until close. */
static PB_RESULT OpenSession(const wchar_t *path, const PB_CONFIG *config, PB_ADAPTER *adapter, PB_SESSION **out)
{
    if (out) *out = NULL;
    if (!path || !out) { PbAdapterRelease(adapter); return PB_INVALID; }
    PB_SESSION *s; PB_RESULT result = Allocate(config, &s);
    if (result != PB_OK) { PbAdapterRelease(adapter); return result; }
    s->Adapter = adapter;
    s->Device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (s->Device == INVALID_HANDLE_VALUE) { Destroy(s); return PB_UNSUPPORTED; }
    PB_PROBE_STATUS status; DWORD returned;
    if (!DeviceIoControl(s->Device, PB_PROBE_IOCTL_QUERY, NULL, 0, &status, sizeof(status), &returned, NULL) ||
        returned != sizeof(status) || status.Size != sizeof(status) || status.Magic != PB_PROBE_MAGIC || status.Schema != PB_PROBE_SCHEMA ||
        status.DataAbi != PB_ABI_MAJOR || status.FileVersion != PB_VERSION_PACKED ||
        memcmp(status.SourceFingerprint, PB_SOURCE_STRING, sizeof(status.SourceFingerprint))) { Destroy(s); return PB_UNSUPPORTED; }
    PB_REGISTER_V2 r = { .Size = sizeof(r), .Major = PB_ABI_MAJOR, .Minor = PB_ABI_MINOR, .Epoch = s->Epoch,
        .RegionAddress = (uint64_t)(uintptr_t)s->Region, .QueueCount = s->Layout.QueueCount, .SlotCount = s->Layout.SlotCount,
        .RingCapacity = s->Layout.RingCapacity, .RegionSize = s->Layout.RegionSize };
    r.ProgressEvent = (uint64_t)(uintptr_t)s->ProgressEvent;
    memcpy(r.SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING));
    for (uint32_t i = 0; i < r.QueueCount; ++i) {
        r.KernelEvents[i] = (uint64_t)(uintptr_t)s->Queues[i].KernelEvent;
        r.UserEvents[i] = (uint64_t)(uintptr_t)s->Queues[i].UserEvent;
    }
    if (!DeviceIoControl(s->Device, PB_V2_IOCTL_REGISTER, &r, sizeof(r), NULL, 0, &returned, NULL)) { Destroy(s); return PB_UNSUPPORTED; }
    if (config->Offloads && PolicyIo(s, config->Offloads) != PB_OK) { Destroy(s); return PB_UNSUPPORTED; }
    PublishSession(s); *out = s; return PB_OK;
}
static PB_RESULT WINAPI Open(const wchar_t *path, const PB_CONFIG *config, PB_SESSION **out)
{
    return OpenSession(path, config, NULL, out);
}

static PB_RESULT WINAPI Wake(PB_SESSION *s, uint32_t queue)
{
    if (!s || queue >= s->Layout.QueueCount) return PB_INVALID;
    if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_STOPPED;
    if (s->ResourcesClosed) { ReleaseSRWLockShared(&s->Lifetime); return PB_STOPPED; }
    BOOL okay = SetEvent(s->Queues[queue].UserEvent);
    ReleaseSRWLockShared(&s->Lifetime); return okay ? PB_OK : PB_CORRUPT;
}
static PB_RESULT WINAPI StartAdapterSession(PB_ADAPTER *adapter, const PB_CONFIG *config, PB_SESSION **out)
{
    if (!adapter || !out) return PB_INVALID;
    PbAdapterRetain(adapter);
    return OpenSession(PbAdapterPath(adapter), config, adapter, out);
}
static PB_RESULT WINAPI Close(PB_SESSION *s, uint32_t timeout) { return CloseInternal(s, timeout, TRUE); }
static PB_RESULT WINAPI PrepareClose(PB_SESSION *s, uint32_t timeout) { return CloseInternal(s, timeout, FALSE); }

PB_RESULT WINAPI PbGetInterfaceV2(uint32_t major, uint32_t size, PB_INTERFACE_V2 *table)
{
    if (!table || major != PB_INTERFACE_V2_MAJOR || size != sizeof(*table)) return PB_UNSUPPORTED;
    PB_INTERFACE_V2 result = { .Size = sizeof(result), .Major = PB_INTERFACE_V2_MAJOR, .Minor = PB_INTERFACE_V2_MINOR, .FileVersion = PB_VERSION_PACKED,
        .Open = Open, .Close = Close, .OpenQueue = OpenQueue, .ReceiveBatch = ReceiveBatch, .ReleaseBatch = ReleaseBatch,
        .ReserveBatch = ReserveBatch, .CancelBatch = CancelBatch, .CommitBatch = CommitBatch, .PollCompletions = PollCompletions,
        .Wait = Wait, .GetStatus = GetStatus, .SetPolicy = SetPolicy, .PlanSegments = PbPlanSegments, .WriteSegment = PbWriteSegment,
        .WriteSegmentBatch = PbWriteSegmentBatch, .SelectQueue = PbSelectQueue,
        .CreateAdapter = PbAdapterCreate, .CloseAdapter = PbAdapterRelease, .StartAdapterSession = StartAdapterSession,
        .GetAdapterLuid = PbAdapterLuid, .Drain = Drain, .Wake = Wake, .GetCapabilities = GetCapabilities, .PrepareClose = PrepareClose };
    memcpy(result.SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING)); *table = result; return PB_OK;
}

PB_RESULT WINAPI PbGetPacketInterface(uint32_t major, uint32_t size, PB_PACKET_INTERFACE *out)
{
    if (!out || major != PB_PACKET_ABI_MAJOR || size < 16 || size % sizeof(void *)) return PB_UNSUPPORTED;
    PB_PACKET_INTERFACE table = {sizeof(table), PB_PACKET_ABI_MAJOR, PB_PACKET_ABI_MINOR, PB_PACKET_REQUIRED_CAPS,
        PbWriteSegmentChecked, PbWriteSegmentBatchChecked};
    if (size < table.Size) table.Size = size;
    memcpy(out, &table, table.Size); return PB_OK;
}

#ifdef PB_API_TESTS
PB_RESULT PbTestOpen(const PB_CONFIG *config, PB_SESSION **out, void **region, PB_LAYOUT *layout, uint64_t *epoch)
{
    PB_RESULT result = Allocate(config, out);
    if (result == PB_OK) {
        PB_SESSION *s = *out; s->Test = 1;
        for (uint32_t q = 0; q < s->Layout.QueueCount; ++q) {
            s->TestTxSlots[q] = calloc(s->Layout.SlotCount, sizeof(PB_SLOT));
            if (!s->TestTxSlots[q]) { Destroy(s); *out = NULL; return PB_FULL; }
            PbPoolInitialize(&s->TestTxPools[q], s->TestTxSlots[q], s->Layout.SlotCount, s->Epoch, (uint16_t)q);
            for (uint32_t r = 0; r < PB_QUEUE_COUNT; ++r)
                PbAttachQueue(&s->TestRings[q][r], s->Region, &s->Layout, q, r, r == PB_QUEUE_TX_READY || r == PB_QUEUE_RX_COMPLETE);
        }
        *region = s->Region; *layout = s->Layout; *epoch = s->Epoch;
        PublishSession(s);
    }
    return result;
}
void PbTestActive(PB_SESSION *s, uint32_t active) { InterlockedExchange(&s->TestActive, (LONG)active); SetEvent(s->ProgressEvent); }
void PbTestFault(PB_SESSION *s) { InterlockedExchange(&s->TestFault, 1); InterlockedExchange((volatile LONG *)&s->SharedLifecycle->Terminal, 1); SetEvent(s->ProgressEvent); for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) SetEvent(s->Queues[i].UserEvent); }
uint32_t PbTestWaiters(PB_SESSION *s) { return (uint32_t)InterlockedCompareExchange(&s->TestWaiters, 0, 0); }
uint32_t PbTestQueries(PB_SESSION *s) { return (uint32_t)InterlockedCompareExchange(&s->TestQueries, 0, 0); }
uint32_t PbTestRetained(uint64_t id)
{
    AcquireSRWLockShared(&RuntimeLock); PB_SESSION *s = RuntimeSessions;
    while (s && s->Epoch != id) s = s->RuntimeNext;
    uint32_t present = s != NULL; ReleaseSRWLockShared(&RuntimeLock); return present;
}
void PbTestBlock(PB_SESSION *s, uint32_t stage, HANDLE entered, HANDLE gate)
{
    if (stage < PB_TEST_HOOKS) { s->TestEntered[stage] = entered; s->TestGate[stage] = gate; }
}
void PbTestFail(PB_SESSION *s, uint32_t stage, BOOL fail)
{
    if (stage >= PB_TEST_HOOKS) return;
    if (fail) InterlockedOr(&s->TestFailures, 1L << stage);
    else InterlockedAnd(&s->TestFailures, ~(1L << stage));
    SetEvent(s->ProgressEvent);
}
uint32_t PbTestBoundaryThread(PB_SESSION *s, uint32_t stage)
{
    return stage < PB_TEST_HOOKS ? (uint32_t)InterlockedCompareExchange(s->TestBoundaryThreads + stage, 0, 0) : 0;
}
PB_RESULT PbTestSend(PB_SESSION *s, uint32_t queue, const uint8_t *bytes, uint32_t length, uint32_t type, uint32_t mss)
{
    if (!s || !s->Test || queue >= s->Layout.QueueCount || !bytes || !length || length > PB_BLOCK_SIZE) return PB_INVALID;
    PB_DESCRIPTOR recycled[PB_MAX_BATCH]; uint32_t count;
    while (PbQueuePop(&s->TestRings[queue][PB_QUEUE_TX_RECYCLE], recycled, PB_MAX_BATCH, &count) == PB_OK)
        for (uint32_t i = 0; i < count; ++i) PbPoolTransition(&s->TestTxPools[queue], &recycled[i].Token, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
    PB_DESCRIPTOR d = { 0 }; PB_RESULT result = PbPoolReserve(&s->TestTxPools[queue], &d.Token);
    if (result != PB_OK) return result;
    d.Length = length; d.Type = type; d.Mss = mss;
    d.DataOffset = s->Layout.Queues[queue].TxData + d.Token.Slot * PB_BLOCK_SIZE;
    if (type) {
        PB_SEGMENT_PLAN plan;
        if (PbPlanSegments(bytes, length, (PB_SEGMENT_TYPE)type, mss, &plan) != PB_PACKET_OK) {
            PbPoolTransition(&s->TestTxPools[queue], &d.Token, PB_SLOT_RESERVED, PB_SLOT_FREE); return PB_INVALID;
        }
        d.L4Offset = plan.TransportOffset; d.HeaderLength = plan.HeaderLength; d.IpVersion = plan.IpVersion; d.Protocol = plan.Protocol;
        d.Flags = (plan.IpVersion == 4 ? PB_CSUM_IPV4 : 0) | (plan.Protocol == 6 ? PB_CSUM_TCP : plan.UdpChecksumDisabled ? PB_UDP_NO_CHECKSUM : PB_CSUM_UDP);
    }
    memcpy(s->Region + d.DataOffset, bytes, length);
    uint32_t notify;
    result = PbQueuePush(&s->TestRings[queue][PB_QUEUE_TX_READY], &d, 1, &count, &notify);
    PbPoolTransition(&s->TestTxPools[queue], &d.Token, PB_SLOT_RESERVED, result == PB_OK ? PB_SLOT_PUBLISHED : PB_SLOT_FREE);
    if (notify) SetEvent(s->Queues[queue].UserEvent);
    return result;
}
PB_RESULT PbTestTake(PB_SESSION *s, uint32_t q, PB_DESCRIPTOR *out, uint32_t capacity, uint32_t *count)
{
    if (!s || !s->Test || q >= s->Layout.QueueCount) return PB_INVALID;
    return PbQueuePop(&s->TestRings[q][PB_QUEUE_RX_SUBMIT], out, capacity, count);
}
PB_RESULT PbTestComplete(PB_SESSION *s, uint32_t q, const PB_DESCRIPTOR *items, uint32_t count)
{
    if (!s || !s->Test || q >= s->Layout.QueueCount) return PB_INVALID;
    uint32_t accepted, notify;
    PB_RESULT result = PbQueuePush(&s->TestRings[q][PB_QUEUE_RX_COMPLETE], items, count, &accepted, &notify);
    if (notify) SetEvent(s->Queues[q].UserEvent);
    return result == PB_OK && accepted != count ? PB_FULL : result;
}
#endif
