/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* One private one-thread pool per admitted session. The loaded-runtime budget bounds
 * permanently blocked I/O; no session can consume another session's work slot. */
#ifndef PB_RUNTIME_MAX_SESSIONS
#define PB_RUNTIME_MAX_SESSIONS 16u
#endif
#ifndef PB_RUNTIME_MAX_BYTES
#define PB_RUNTIME_MAX_BYTES (256ull * 1024ull * 1024ull)
#endif
#ifndef PB_RUNTIME_MAX_ADAPTERS
#define PB_RUNTIME_MAX_ADAPTERS 16u
#endif
static SRWLOCK RuntimeLock = SRWLOCK_INIT;
static PB_SESSION *RuntimeSessions;
static uint32_t RuntimeCount;
static uint32_t RuntimeAdapterCount;
static uint64_t RuntimeBytes;
static const BYTE SessionModuleAnchor = 0;
static void Destroy(PB_SESSION *);
static BOOL ReleaseResources(PB_SESSION *);
static PB_RESULT PollInternal(PB_USER_QUEUE *, PB_DESCRIPTOR *, uint32_t, uint32_t *);
static PB_RESULT StatusInternal(PB_SESSION *, PB_V2_STATUS *);
static PB_RESULT PolicyIo(PB_SESSION *, uint32_t);
static PB_RESULT DrainIo(PB_SESSION *);
static void FaultSession(PB_SESSION *);
static void CALLBACK LifecycleWorker(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK);

BOOL PbRuntimeReserveAdapter(void)
{
    AcquireSRWLockExclusive(&RuntimeLock);
    BOOL okay = RuntimeAdapterCount < PB_RUNTIME_MAX_ADAPTERS && RuntimeCount < PB_RUNTIME_MAX_SESSIONS && RuntimeBytes < PB_RUNTIME_MAX_BYTES;
    if (okay) RuntimeAdapterCount++;
    ReleaseSRWLockExclusive(&RuntimeLock); return okay;
}
void PbRuntimeReleaseAdapter(void)
{
    AcquireSRWLockExclusive(&RuntimeLock); RuntimeAdapterCount--; ReleaseSRWLockExclusive(&RuntimeLock);
}

static BOOL RuntimeAdmit(PB_SESSION *s)
{
    AcquireSRWLockExclusive(&RuntimeLock);
    BOOL okay = RuntimeCount < PB_RUNTIME_MAX_SESSIONS && s->Layout.RegionSize <= PB_RUNTIME_MAX_BYTES - RuntimeBytes;
    if (okay) {
        s->Admitted = TRUE;
        RuntimeCount++; RuntimeBytes += s->Layout.RegionSize;
    }
    ReleaseSRWLockExclusive(&RuntimeLock); return okay;
}
static void RuntimeRemove(PB_SESSION *s)
{
    if (!s->Admitted) return;
    AcquireSRWLockExclusive(&RuntimeLock);
    PB_SESSION **link = &RuntimeSessions;
    while (*link && *link != s) link = &(*link)->RuntimeNext;
    if (*link) *link = s->RuntimeNext;
    RuntimeCount--; RuntimeBytes -= s->Layout.RegionSize;
    ReleaseSRWLockExclusive(&RuntimeLock); s->Admitted = FALSE;
}
static void Retain(PB_SESSION *s) { InterlockedIncrement(&s->References); }
static void DropReference(PB_SESSION *s)
{
    if (InterlockedDecrement(&s->References)) return;
    HMODULE module = s->Module; s->Module = NULL;
    Destroy(s);
    /* Runtime retains the core until physical close. Its final callback transfers
     * the pre-acquired tail pin before releasing any core references. */
    if (module) FreeLibrary(module);
}
static void NotifyLocked(PB_SESSION *s)
{
    WakeAllConditionVariable(&s->Changed);
}
static void Publish(PB_SESSION *s, uint32_t phase, uint32_t block, PB_RESULT result, DWORD error)
{
    AcquireSRWLockExclusive(&s->State);
    BOOL changed = phase > s->Progress.Phase || block != s->Progress.BlockStage ||
        (uint32_t)result != s->Progress.Result || error != s->Progress.LastError;
    if (phase > s->Progress.Phase) { s->Progress.Phase = phase; s->Progress.LastProgress = GetTickCount64(); }
    s->Progress.BlockStage = block; s->Progress.Result = result; s->Progress.LastError = error;
    if (result == PB_CORRUPT || result == PB_FULL) s->Progress.Flags |= PB_LIFE_QUARANTINED;
    if (phase >= PB_LIFE_PREPARED && result == PB_OK) s->Progress.Flags &= ~PB_LIFE_QUARANTINED;
    if (changed) NotifyLocked(s);
    ReleaseSRWLockExclusive(&s->State);
}
static void ScheduleLocked(PB_SESSION *s)
{
    if (s->Scheduled || s->Progress.Phase == PB_LIFE_CLOSED) return;
    /* No loader calls or fallible allocation on stop/Drop/Close. The registry
     * owns the core pin across every queued, running and returning callback. */
    s->Scheduled = TRUE; Retain(s); SubmitThreadpoolWork(s->Work);
}
static BOOL InitializeLifecycle(PB_SESSION *s)
{
    s->References = 1; s->Progress.Size = sizeof(s->Progress); s->Progress.Phase = PB_LIFE_RUNNING;
    s->Progress.SessionId = s->Epoch; s->Progress.RetainedBytes = s->Layout.RegionSize;
    s->Progress.LastProgress = GetTickCount64();
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&SessionModuleAnchor, &s->Module)) return FALSE;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&SessionModuleAnchor, &s->TailModule)) return FALSE;
    s->Pool = CreateThreadpool(NULL); if (!s->Pool) return FALSE;
    SetThreadpoolThreadMaximum(s->Pool, 1);
    if (!SetThreadpoolThreadMinimum(s->Pool, 0)) return FALSE;
    TP_CALLBACK_ENVIRON environment; InitializeThreadpoolEnvironment(&environment);
    SetThreadpoolCallbackPool(&environment, s->Pool);
    s->Work = CreateThreadpoolWork(LifecycleWorker, s, &environment);
    DestroyThreadpoolEnvironment(&environment);
    if (!s->Work) return FALSE;
    return TRUE;
}
static void PublishSession(PB_SESSION *s)
{
    /* Publish only after registration / hosted fixture construction succeeded. */
    AcquireSRWLockExclusive(&RuntimeLock);
    s->RuntimeNext = RuntimeSessions; RuntimeSessions = s; s->Registered = TRUE; Retain(s);
    ReleaseSRWLockExclusive(&RuntimeLock);
}
static uint32_t Loans(PB_SESSION *s)
{
    uint32_t loans = 0;
    for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) loans += (uint32_t)ReadAcquire(&s->Queues[i].Loans);
    return loans;
}
/* Called under exclusive DataGate. Preserve token checks alongside counters. */
static BOOL ValidOwnership(PB_USER_QUEUE *q)
{
    LONG loans = ReadAcquire(&q->Loans), published = ReadAcquire(&q->Published);
    if (loans < 0 || (uint32_t)loans > 2 * q->Session->Layout.SlotCount ||
        published < 0 || (uint32_t)published > q->Session->Layout.SlotCount) return FALSE;
#if !defined(NDEBUG) || defined(PB_API_TESTS)
    uint32_t actualLoans = 0, actualPublished = 0;
    for (uint32_t i = 0; i < q->Session->Layout.SlotCount; ++i) {
        actualLoans += q->TxSlots[i].State == PB_SLOT_PUBLISHED;
        actualLoans += q->RxSlots[i].State == PB_SLOT_RESERVED;
        actualPublished += q->RxSlots[i].State == PB_SLOT_PUBLISHED;
    }
    if (actualLoans != (uint32_t)loans || actualPublished != (uint32_t)published) return FALSE;
#endif
    return TRUE;
}
static PB_RESULT WINAPI BeginStop(PB_SESSION *s)
{
    if (!s) return PB_INVALID;
    InterlockedExchange(&s->CloseRequested, 1); SetEvent(s->ClosingEvent); SetEvent(s->ProgressEvent);
    AcquireSRWLockExclusive(&s->State);
    BOOL changed = s->Progress.Phase < PB_LIFE_STOP_REQUESTED;
    /* Failed unpublished cleanup may already be RELEASING before this first
     * stop request. Initialize its timestamps without regressing the phase. */
    if (changed || !s->Progress.Started) {
        s->Progress.LastProgress = GetTickCount64();
        if (!s->Progress.Started) s->Progress.Started = s->Progress.LastProgress;
        if (changed) s->Progress.Phase = PB_LIFE_STOP_REQUESTED;
        changed = TRUE;
    }
    ScheduleLocked(s); if (changed) NotifyLocked(s);
    ReleaseSRWLockExclusive(&s->State); return PB_OK;
}
static PB_RESULT WINAPI BeginSetPolicy(PB_SESSION *s, uint32_t mask, uint64_t *ticket)
{
    if (ticket) *ticket = 0;
    if (!s || !ticket || mask & ~PB_PROBE_SUPPORTED || (mask && !s->Experimental)) return PB_INVALID;
    AcquireSRWLockExclusive(&s->State);
    PB_RESULT result = PB_OK;
    if (InterlockedCompareExchange(&s->CloseRequested, 0, 0)) result = PB_STOPPED;
    else if (s->PolicyOccupied || s->Progress.PolicyRequested == UINT64_MAX) result = PB_BUSY;
    else {
        s->PolicyOccupied = s->PolicyQueued = TRUE; s->PolicyAbandoned = FALSE; s->PolicyMask = mask;
        *ticket = ++s->Progress.PolicyRequested;
        ScheduleLocked(s);
    }
    ReleaseSRWLockExclusive(&s->State); return result;
}
static DWORD Remaining(ULONGLONG deadline, uint32_t timeout)
{
    if (timeout == INFINITE) return INFINITE;
    ULONGLONG now = GetTickCount64(); return now >= deadline ? 0 : (DWORD)(deadline - now);
}
static PB_RESULT WINAPI PollPolicy(PB_SESSION *s, uint64_t ticket, uint32_t timeout, PB_RESULT *operation)
{
    if (!s || !ticket || !operation || (timeout != INFINITE && timeout > 60000)) return PB_INVALID;
    ULONGLONG deadline = GetTickCount64() + timeout;
    AcquireSRWLockExclusive(&s->State);
    PB_RESULT result = PB_OK;
    for (;;) {
        /* Sleep releases State. Another poller/ForgetPolicy may retire this
         * ticket and start a new request before we reacquire it. Completion
         * alone cannot authorize releasing that replacement's occupied slot. */
        if (!s->PolicyOccupied || ticket != s->Progress.PolicyRequested) { result = PB_STALE; break; }
        if (s->Progress.PolicyCompleted == ticket) break;
        DWORD remaining = Remaining(deadline, timeout);
        if (!remaining) { result = PB_EMPTY; break; }
        if (!SleepConditionVariableSRW(&s->Changed, &s->State, remaining, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; break; }
    }
    if (result == PB_OK) { *operation = s->PolicyResult; s->PolicyOccupied = FALSE; }
    ReleaseSRWLockExclusive(&s->State); return result;
}
static PB_RESULT WINAPI GetCloseProgress(PB_SESSION *s, PB_CLOSE_PROGRESS *out)
{
    if (!s || !out) return PB_INVALID;
    AcquireSRWLockShared(&s->State); *out = s->Progress; ReleaseSRWLockShared(&s->State);
    out->UserLeases = Loans(s); out->Published = 0;
    for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) out->Published += (uint32_t)ReadAcquire(&s->Queues[i].Published);
    if (InterlockedCompareExchange(&s->ResourcesClosed, 0, 0)) out->RetainedBytes = 0;
    return PB_OK;
}
static void WINAPI ForgetPolicy(PB_SESSION *s, uint64_t ticket)
{
    if (!s) return;
    AcquireSRWLockExclusive(&s->State);
    if (s->PolicyOccupied && ticket == s->Progress.PolicyRequested) {
        s->PolicyAbandoned = TRUE;
        if (s->Progress.PolicyCompleted == ticket) s->PolicyOccupied = FALSE;
    }
    ReleaseSRWLockExclusive(&s->State);
}
static PB_RESULT WINAPI GetRuntimeStatus(PB_RUNTIME_STATUS *out)
{
    if (!out) return PB_INVALID;
    AcquireSRWLockShared(&RuntimeLock);
    *out = (PB_RUNTIME_STATUS){sizeof(*out), RuntimeCount, RuntimeBytes, PB_RUNTIME_MAX_SESSIONS, 0, PB_RUNTIME_MAX_BYTES, RuntimeAdapterCount, PB_RUNTIME_MAX_ADAPTERS};
    ReleaseSRWLockShared(&RuntimeLock); return PB_OK;
}
static PB_RESULT WINAPI GetRetained(uint32_t index, PB_CLOSE_PROGRESS *out)
{
    if (!out) return PB_INVALID;
    AcquireSRWLockShared(&RuntimeLock); PB_SESSION *s = RuntimeSessions;
    while (s && index--) s = s->RuntimeNext;
    if (s) Retain(s);
    ReleaseSRWLockShared(&RuntimeLock);
    if (!s) return PB_EMPTY;
    PB_RESULT result = GetCloseProgress(s, out); DropReference(s); return result;
}
static void CALLBACK LifecycleWorker(PTP_CALLBACK_INSTANCE instance, PVOID opaque, PTP_WORK work)
{
    (void)work; PB_SESSION *s = opaque;
    CallbackMayRunLong(instance);
#ifdef PB_API_TESTS
    TestBoundary(s, PB_TEST_WORKER_START);
#endif
    for (;;) {
        AcquireSRWLockExclusive(&s->State);
        BOOL policy = s->PolicyQueued, stop = InterlockedCompareExchange(&s->CloseRequested, 0, 0) != 0;
        BOOL releasing = s->Progress.Phase == PB_LIFE_RELEASING;
        uint32_t mask = s->PolicyMask; uint64_t ticket = s->Progress.PolicyRequested;
        s->PolicyQueued = FALSE;
        if (!policy && !stop) {
            s->Scheduled = FALSE; ReleaseSRWLockExclusive(&s->State); break;
        }
        ReleaseSRWLockExclusive(&s->State);
        if (policy) {
            Publish(s, 0, PB_BLOCK_POLICY, PB_OK, ERROR_SUCCESS);
            PB_RESULT applied = stop ? PB_STOPPED : PolicyIo(s, mask); DWORD error = applied == PB_OK ? 0 : GetLastError();
            AcquireSRWLockExclusive(&s->State);
            s->PolicyResult = applied; s->Progress.PolicyCompleted = ticket; s->Progress.LastError = error;
            s->Progress.LastProgress = GetTickCount64();
            if (s->PolicyAbandoned) s->PolicyOccupied = FALSE;
            s->Progress.BlockStage = PB_BLOCK_NONE; NotifyLocked(s); ReleaseSRWLockExclusive(&s->State);
            continue;
        }
        /* Release was already authorized, or attempted for an unpublished
         * Open failure with no user views. Retry its real device-close barrier;
         * an unavailable/unregistered control plane cannot authorize it again. */
        if (releasing) goto release_resources;
        if (!InterlockedCompareExchange(&s->Draining, 0, 0)) {
            Publish(s, PB_LIFE_DRAINING, PB_BLOCK_DRAIN, PB_QUARANTINED, 0);
            PB_RESULT drained = DrainIo(s);
            if (drained != PB_OK) { Publish(s, 0, PB_BLOCK_DRAIN, drained, GetLastError()); goto parked; }
            AcquireSRWLockExclusive(&s->State); s->Progress.LastProgress = GetTickCount64(); NotifyLocked(s); ReleaseSRWLockExclusive(&s->State);
        }
        uint32_t published = 0, failedQueues = 0; PB_RESULT result = PB_OK; BOOL ownershipValid = TRUE;
        AcquireSRWLockExclusive(&s->DataGate);
        for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) {
            PB_USER_QUEUE *q = s->Queues + i; PB_DESCRIPTOR items[PB_MAX_BATCH]; uint32_t n;
            for (uint32_t batch = 0; batch < 4; ++batch) {
                PB_RESULT polled = PollInternal(q, items, PB_MAX_BATCH, &n);
                if (polled == PB_EMPTY) break;
                if (polled != PB_OK) { result = polled; failedQueues |= 1u << i; break; }
                if (batch == 3) SetEvent(s->ProgressEvent);
            }
            published += (uint32_t)ReadAcquire(&q->Published);
            if (!ValidOwnership(q)) ownershipValid = FALSE;
        }
        uint32_t loans = Loans(s);
        uint64_t sequence = (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&s->SharedLifecycle->Sequence, 0, 0);
        ReleaseSRWLockExclusive(&s->DataGate);
        AcquireSRWLockExclusive(&s->State);
        if (sequence != s->LastKernelSequence || loans != s->Progress.UserLeases || published != s->Progress.Published) {
            s->LastKernelSequence = sequence; s->Progress.UserLeases = loans; s->Progress.Published = published;
            s->Progress.LastProgress = GetTickCount64(); NotifyLocked(s);
        }
        ReleaseSRWLockExclusive(&s->State);
        if (!ownershipValid) { Publish(s, 0, PB_BLOCK_KERNEL, PB_CORRUPT, ERROR_INVALID_DATA); goto parked; }
        if (result != PB_OK) FaultSession(s);
        if (loans) { Publish(s, 0, PB_BLOCK_LEASES, PB_BUSY, 0); goto parked; }
        Publish(s, 0, PB_BLOCK_KERNEL, PB_QUARANTINED, 0);
        PB_V2_STATUS status; result = StatusInternal(s, &status);
        if (result != PB_OK) { Publish(s, 0, PB_BLOCK_KERNEL, result, GetLastError()); goto parked; }
        AcquireSRWLockExclusive(&s->State);
        if (s->Progress.NativeReferences != status.ActiveReferences) {
            s->Progress.NativeReferences = status.ActiveReferences; s->Progress.LastProgress = GetTickCount64(); NotifyLocked(s);
        }
        BOOL final; ReleaseSRWLockExclusive(&s->State);
        if (status.State != PB_SESSION_STOPPED || status.ActiveReferences) {
            Publish(s, 0, PB_BLOCK_KERNEL, PB_QUARANTINED, 0); goto parked;
        }
        if (published) {
            /* STOPPED settles native ownership/publication, not DLL consumption.
             * Recheck after QUERY: the poll budget may leave valid completions,
             * or publication may race the earlier empty poll. Consume those on
             * another bounded turn before treating bookkeeping as abandoned.
             * A damaged queue still needs the trusted STOPPED fallback below. */
            AcquireSRWLockExclusive(&s->DataGate);
            BOOL pending = FALSE;
            for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) {
                PB_USER_QUEUE *q = s->Queues + i;
                if ((failedQueues & (1u << i)) || !ReadAcquire(&q->Published)) continue;
                PB_QUEUE_VIEW *complete = &q->Rings[PB_QUEUE_RX_COMPLETE]; uint64_t producer;
                PB_RESULT snapshot = PbQueueSnapshotProducer(complete, &producer);
                if (snapshot != PB_OK) FaultSession(s);
                else if (producer != complete->Cursor) pending = TRUE;
            }
            if (pending) {
                ReleaseSRWLockExclusive(&s->DataGate);
                SetEvent(s->ProgressEvent); goto parked;
            }
            /* No readable completion remains in an undamaged queue. These
             * buffers have no user views and native ownership has ended. Only
             * the real IOCTL acknowledgement authorizes this reconciliation. */
            PB_RESULT retired = PB_OK;
            for (uint32_t i = 0; i < s->Layout.QueueCount; ++i) {
                PB_USER_QUEUE *q = s->Queues + i;
                for (uint32_t j = 0; j < s->Layout.SlotCount; ++j) {
                    if (q->RxSlots[j].State != PB_SLOT_PUBLISHED) continue;
                    PB_TOKEN token = { s->Epoch, q->RxSlots[j].Generation, (uint16_t)j, (uint16_t)i };
                    retired = PbPoolTransition(&q->Rx, &token, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
                    if (retired != PB_OK) break;
                }
                if (retired != PB_OK) break;
                WriteRelease(&q->Published, 0);
            }
            ReleaseSRWLockExclusive(&s->DataGate);
            if (retired != PB_OK) { Publish(s, 0, PB_BLOCK_KERNEL, PB_CORRUPT, ERROR_INVALID_DATA); goto parked; }
            AcquireSRWLockExclusive(&s->State);
            s->Progress.Published = 0; s->Progress.LastProgress = GetTickCount64(); NotifyLocked(s);
            ReleaseSRWLockExclusive(&s->State);
        }
        Publish(s, PB_LIFE_PREPARED, PB_BLOCK_NONE, PB_OK, 0);
        AcquireSRWLockExclusive(&s->State); final = s->FinalRequested;
        if (!final) { s->Scheduled = FALSE; ReleaseSRWLockExclusive(&s->State); break; }
        ReleaseSRWLockExclusive(&s->State);
release_resources:
        /* ClosingEvent was published before requesting this exclusive lifetime pin. */
        Publish(s, PB_LIFE_RELEASING, PB_BLOCK_API_USERS, PB_QUARANTINED, 0);
        AcquireSRWLockExclusive(&s->Lifetime);
        Publish(s, PB_LIFE_RELEASING, PB_BLOCK_DEVICE_CLOSE, PB_QUARANTINED, 0);
        BOOL released = ReleaseResources(s); DWORD releaseError = released ? 0 : GetLastError();
        ReleaseSRWLockExclusive(&s->Lifetime);
        if (!released) { Publish(s, PB_LIFE_RELEASING, PB_BLOCK_DEVICE_CLOSE, PB_CORRUPT, releaseError); goto parked; }
        AcquireSRWLockExclusive(&s->State);
        s->Progress.Phase = PB_LIFE_CLOSED; s->Progress.BlockStage = PB_BLOCK_NONE; s->Progress.Result = PB_OK;
        s->Progress.Flags &= ~PB_LIFE_QUARANTINED;
        s->Progress.LastProgress = GetTickCount64();
        s->Scheduled = FALSE; NotifyLocked(s); ReleaseSRWLockExclusive(&s->State); break;
parked:
        /* ProgressEvent belongs to lifecycle, never steal a queue owner's UserEvent.
         * The diagnostic fallback is low frequency, including old driver failure paths. */
        WaitForSingleObject(s->ProgressEvent, 1000);
        AcquireSRWLockExclusive(&s->State); s->Progress.Rechecks++; ReleaseSRWLockExclusive(&s->State);
    }
    if (InterlockedCompareExchange(&s->ResourcesClosed, 0, 0)) {
        HMODULE tail = s->TailModule; s->TailModule = NULL;
        if (tail) FreeLibraryWhenCallbackReturns(instance, tail);
        if (s->Registered) { RuntimeRemove(s); s->Registered = FALSE; DropReference(s); }
    }
    DropReference(s);
}
static PB_RESULT CloseInternal(PB_SESSION *s, uint32_t timeout, BOOL destroy)
{
    if (!s || timeout > 60000) return PB_INVALID;
    ULONGLONG deadline = GetTickCount64() + timeout; Retain(s);
    if (destroy) { AcquireSRWLockExclusive(&s->State); s->FinalRequested = TRUE; ReleaseSRWLockExclusive(&s->State); }
    BeginStop(s);
    PB_RESULT result = PB_QUARANTINED;
    AcquireSRWLockExclusive(&s->State);
    for (;;) {
        if (s->Progress.Phase == PB_LIFE_CLOSED || (!destroy && s->Progress.Phase >= PB_LIFE_PREPARED)) { result = PB_OK; break; }
        if (Loans(s)) { result = PB_BUSY; break; }
        if (s->Progress.Result == PB_CORRUPT) { result = PB_CORRUPT; break; }
        DWORD remaining = Remaining(deadline, timeout);
        if (!remaining) { s->Progress.Flags |= PB_LIFE_QUARANTINED; break; }
        if (!SleepConditionVariableSRW(&s->Changed, &s->State, remaining, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; break; }
    }
    ReleaseSRWLockExclusive(&s->State);
    if (result == PB_OK && destroy && !InterlockedExchange(&s->PublicReleased, 1)) DropReference(s);
    DropReference(s); return result;
}
static void WINAPI ReleaseSession(PB_SESSION *s)
{
    if (!s) return;
    Retain(s);
    AcquireSRWLockExclusive(&s->State); s->FinalRequested = TRUE; ReleaseSRWLockExclusive(&s->State);
    BeginStop(s);
    if (!InterlockedExchange(&s->PublicReleased, 1)) DropReference(s);
    DropReference(s);
}
PB_RESULT WINAPI PbGetLifecycleInterface(uint32_t major, uint32_t size, PB_LIFECYCLE_INTERFACE *out)
{
    if (!out || major != PB_LIFECYCLE_ABI || size < 16 || size % sizeof(void *)) return PB_UNSUPPORTED;
    PB_LIFECYCLE_INTERFACE table = {sizeof(table), PB_LIFECYCLE_ABI, 0, PB_LIFECYCLE_CAPS,
        BeginStop, BeginSetPolicy, PollPolicy, GetCloseProgress, ReleaseSession, GetRuntimeStatus, GetRetained, ForgetPolicy};
    if (size < table.Size) table.Size = size;
    memcpy(out, &table, table.Size); return PB_OK;
}
