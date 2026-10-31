/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
typedef enum PB_QUEUE_ACCESS { PB_QUEUE_ADMISSION, PB_QUEUE_RETIREMENT } PB_QUEUE_ACCESS;
static void FaultSession(PB_SESSION *s)
{
    BOOL changed = !InterlockedExchange(&s->Fatal, 1);
    if (!InterlockedExchange(&s->CloseRequested, 1)) { SetEvent(s->ClosingEvent); changed = TRUE; }
    if (changed) SetEvent(s->ProgressEvent);
}
static PB_RESULT QueueEnter(PB_USER_QUEUE *q, PB_QUEUE_ACCESS access)
{
    if (!q || !q->Session) return PB_INVALID;
    if (!TryAcquireSRWLockShared(&q->Session->Lifetime)) return PB_STOPPED;
    AcquireSRWLockShared(&q->Session->DataGate);
    if (ReadAcquire(&q->Session->ResourcesClosed)) {
        ReleaseSRWLockShared(&q->Session->DataGate); ReleaseSRWLockShared(&q->Session->Lifetime); return PB_STOPPED;
    }
    if (ReadAcquire((volatile LONG *)&q->Session->SharedLifecycle->Terminal) &&
        !InterlockedExchange(&q->Session->CloseRequested, 1)) {
        SetEvent(q->Session->ClosingEvent); SetEvent(q->Session->ProgressEvent);
    }
    LONG thread = (LONG)GetCurrentThreadId();
    LONG previous = ReadAcquire(&q->Owner);
    if (!previous) previous = InterlockedCompareExchange(&q->Owner, thread, 0);
    if (previous && previous != thread) {
        ReleaseSRWLockShared(&q->Session->DataGate); ReleaseSRWLockShared(&q->Session->Lifetime); return PB_BUSY;
    }
    /* Stopping admission must never revoke the owner's ability to return a loan.
     * Retirement still requires the lifetime pin, queue owner and valid tokens. */
    if (access == PB_QUEUE_ADMISSION && ReadAcquire(&q->Session->Fatal)) {
        ReleaseSRWLockShared(&q->Session->DataGate); ReleaseSRWLockShared(&q->Session->Lifetime); return PB_CORRUPT;
    }
    return PB_OK;
}
static PB_RESULT FinishQueueCall(PB_USER_QUEUE *q, PB_RESULT result)
{
    if (result == PB_CORRUPT) {
        FaultSession(q->Session);
    }
    /* Successful finite calls transfer ownership. Empty/stopped/invalid calls
     * cannot unblock lifecycle; FaultSession owns the first corruption wake.
     * A final closer waiting on Lifetime is woken by the SRW release below. */
    if (result == PB_OK && ReadAcquire(&q->Session->CloseRequested)) SetEvent(q->Session->ProgressEvent);
    ReleaseSRWLockShared(&q->Session->Lifetime); return result;
}
static PB_RESULT QueueLeave(PB_USER_QUEUE *q, PB_RESULT result)
{
    ReleaseSRWLockShared(&q->Session->DataGate);
    return FinishQueueCall(q, result);
}
static PB_RESULT WINAPI OpenQueue(PB_SESSION *s, uint32_t index, PB_USER_QUEUE **out)
{
    if (out) *out = NULL;
    if (!s || !out || index >= s->Layout.QueueCount) return PB_INVALID;
    if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_STOPPED;
    PB_RESULT result = ReadAcquire(&s->CloseRequested) ? PB_STOPPED : ReadAcquire(&s->Fatal) ? PB_CORRUPT : PB_OK;
    if (result == PB_OK) *out = s->Queues + index;
    ReleaseSRWLockShared(&s->Lifetime); return result;
}

static PB_RESULT WINAPI ReceiveBatch(PB_USER_QUEUE *q, PB_LEASE *leases, uint32_t capacity, uint32_t *count)
{
    if (count) *count = 0;
    if (!leases || !count || !capacity || capacity > PB_MAX_BATCH) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    PB_DESCRIPTOR items[PB_MAX_BATCH]; uint32_t n;
    result = PbQueuePop(&q->Rings[PB_QUEUE_TX_READY], items, capacity, &n);
    if (result == PB_OK) {
        for (uint32_t i = 0; i < n; ++i) {
            PB_DESCRIPTOR *d = items + i;
            if (PbValidateDescriptor(&q->Session->Layout, q->Session->Epoch, q->Index, FALSE, d, 7) != PB_OK) {
                result = PB_CORRUPT; break;
            }
        }
        if (result == PB_OK && PbPoolImport(&q->Tx, items, n) != PB_OK) result = PB_CORRUPT;
        if (result == PB_OK) for (uint32_t i = 0; i < n; ++i) {
            PB_DESCRIPTOR *d = items + i;
            leases[i].Descriptor = *d; leases[i].Data = q->Session->Region + d->DataOffset;
            leases[i].Capacity = d->Length; leases[i].Reserved = 0;
        }
        if (result == PB_OK) { *count = n; WriteRelease(&q->Loans, ReadAcquire(&q->Loans) + (LONG)n); }
    }
    return QueueLeave(q, result);
}

static PB_RESULT WINAPI ReleaseBatch(PB_USER_QUEUE *q, const PB_TOKEN *tokens, uint32_t count, uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!accepted) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_RETIREMENT); if (result != PB_OK) return result;
    result = PbValidateTokens(&q->Tx, tokens, count, PB_SLOT_PUBLISHED);
    if (result == PB_OK) {
        uint32_t notify = 0;
        BOOL local = ReadAcquire(&q->Session->Fatal) != 0;
        if (!local) {
            result = PbQueuePushTokens(&q->Rings[PB_QUEUE_TX_RECYCLE], tokens, count, accepted, &notify);
            if (result == PB_CORRUPT) { FaultSession(q->Session); local = TRUE; }
            else if (result == PB_FULL && ReadAcquire(&q->Session->CloseRequested)) local = TRUE;
        }
        /* With admission permanently stopped, a full noncooperating recycle
         * ring must not pin the user view, including a native-only fault. No
         * slot may be reused; physical reclamation still requires the native
         * close barrier. Running sessions retain ordinary PB_FULL backpressure. */
        if (local) { *accepted = count; result = PB_OK; }
        if (result == PB_OK) {
            for (uint32_t i = 0; i < *accepted; ++i) PbPoolTransition(&q->Tx, tokens + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
            WriteRelease(&q->Loans, ReadAcquire(&q->Loans) - (LONG)*accepted);
            if (!local && notify) SetEvent(q->KernelEvent);
        }
    }
    return QueueLeave(q, result);
}

static PB_RESULT WINAPI ReserveBatch(PB_USER_QUEUE *q, PB_LEASE *leases, uint32_t requested, uint32_t *count)
{
    if (count) *count = 0;
    if (!leases || !count || !requested || requested > PB_MAX_BATCH) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->Draining) || ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    for (uint32_t i = 0; i < requested; ++i) {
        PB_TOKEN token; result = PbPoolReserve(&q->Rx, &token); if (result != PB_OK) break;
        memset(leases + i, 0, sizeof(*leases)); leases[i].Descriptor.Token = token;
        leases[i].Descriptor.DataOffset = q->Session->Layout.Queues[q->Index].RxData + token.Slot * PB_BLOCK_SIZE;
        leases[i].Data = q->Session->Region + leases[i].Descriptor.DataOffset; leases[i].Capacity = 65535;
        (*count)++;
    }
    WriteRelease(&q->Loans, ReadAcquire(&q->Loans) + (LONG)*count);
    return QueueLeave(q, *count ? PB_OK : result);
}
static PB_RESULT WINAPI CancelBatch(PB_USER_QUEUE *q, const PB_TOKEN *tokens, uint32_t count)
{
    PB_RESULT result = QueueEnter(q, PB_QUEUE_RETIREMENT); if (result != PB_OK) return result;
    result = PbValidateTokens(&q->Rx, tokens, count, PB_SLOT_RESERVED);
    if (result == PB_OK) {
        for (uint32_t i = 0; i < count; ++i) PbPoolTransition(&q->Rx, tokens + i, PB_SLOT_RESERVED, PB_SLOT_FREE);
        WriteRelease(&q->Loans, ReadAcquire(&q->Loans) - (LONG)count);
    }
    return QueueLeave(q, result);
}
static PB_RESULT WINAPI CommitBatch(PB_USER_QUEUE *q, const PB_LEASE *leases, uint32_t count, uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!leases || !accepted || !count || count > PB_MAX_BATCH) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->Draining) || ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    PB_TOKEN tokens[PB_MAX_BATCH]; PB_DESCRIPTOR items[PB_MAX_BATCH];
    for (uint32_t i = 0; i < count; ++i) {
        items[i] = leases[i].Descriptor; tokens[i] = items[i].Token;
        if (leases[i].Reserved || PbValidateDescriptor(&q->Session->Layout, q->Session->Epoch, q->Index, TRUE, items + i, 1) != PB_OK ||
            leases[i].Data != q->Session->Region + items[i].DataOffset) return QueueLeave(q, PB_INVALID);
    }
    result = PbValidateTokens(&q->Rx, tokens, count, PB_SLOT_RESERVED);
    if (result == PB_OK) {
        uint32_t notify;
        result = PbQueuePush(&q->Rings[PB_QUEUE_RX_SUBMIT], items, count, accepted, &notify);
        if (result == PB_OK) {
            for (uint32_t i = 0; i < *accepted; ++i) PbPoolTransition(&q->Rx, tokens + i, PB_SLOT_RESERVED, PB_SLOT_PUBLISHED);
            WriteRelease(&q->Loans, ReadAcquire(&q->Loans) - (LONG)*accepted);
            WriteRelease(&q->Published, ReadAcquire(&q->Published) + (LONG)*accepted);
            if (notify) SetEvent(q->KernelEvent);
        }
    }
    return QueueLeave(q, result);
}

static PB_RESULT PollInternal(PB_USER_QUEUE *q, PB_DESCRIPTOR *items, uint32_t capacity, uint32_t *count)
{
    *count = 0;
    PB_RESULT result = PbQueuePop(&q->Rings[PB_QUEUE_RX_COMPLETE], items, capacity, count);
    if (result != PB_OK) return result;
    PB_TOKEN tokens[PB_MAX_BATCH];
    for (uint32_t i = 0; i < *count; ++i) tokens[i] = items[i].Token;
    if (PbValidateTokens(&q->Rx, tokens, *count, PB_SLOT_PUBLISHED) != PB_OK) {
        /* Pop already released ring space. A parked native producer must retry
         * even though this batch cannot retire any private ownership. */
        *count = 0; SetEvent(q->KernelEvent); return PB_CORRUPT;
    }
    /* The owner/DataGate exclude observers of an intermediate COMPLETE state.
     * Keep the checked transition and free-index maintenance after validating
     * the whole batch, without validating/writing an unobservable state twice. */
    for (uint32_t i = 0; i < *count; ++i)
        PbPoolTransition(&q->Rx, tokens + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
    WriteRelease(&q->Published, ReadAcquire(&q->Published) - (LONG)*count);
    /* Wake after every space release. A non-full producer snapshot before Pop
     * does not prove the producer stayed unblocked: it can fill the ring and
     * park before Pop publishes Consumer. Armed only requests data wakes. */
    SetEvent(q->KernelEvent); return PB_OK;
}
static PB_RESULT WINAPI PollCompletions(PB_USER_QUEUE *q, PB_DESCRIPTOR *items, uint32_t capacity, uint32_t *count)
{
    if (count) *count = 0;
    if (!items || !count || !capacity || capacity > PB_MAX_BATCH) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_RETIREMENT); if (result != PB_OK) return result;
    return QueueLeave(q, PollInternal(q, items, capacity, count));
}
static PB_RESULT StatusInternal(PB_SESSION *s, PB_V2_STATUS *status);
static PB_RESULT WINAPI Wait(PB_USER_QUEUE *q, uint32_t timeout)
{
    if (!q || !q->Session) return PB_INVALID;
    if (ReadAcquire(&q->Session->Fatal)) return PB_CORRUPT;
    if (ReadAcquire(&q->Session->CloseRequested)) return PB_STOPPED;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    uint32_t tx = 0, rx = 0;
    result = PbQueueArm(&q->Rings[PB_QUEUE_TX_READY], &tx);
    if (result == PB_OK) result = PbQueueArm(&q->Rings[PB_QUEUE_RX_COMPLETE], &rx);
    ReleaseSRWLockShared(&q->Session->DataGate);
    if (result == PB_OK && !tx && !rx) {
        HANDLE events[] = { q->Session->ClosingEvent, q->UserEvent };
#ifdef PB_API_TESTS
        InterlockedIncrement(&q->Session->TestWaiters);
#endif
        DWORD waited = WaitForMultipleObjects(2, events, FALSE, timeout);
#ifdef PB_API_TESTS
        InterlockedDecrement(&q->Session->TestWaiters);
#endif
        result = waited == WAIT_OBJECT_0 ? PB_STOPPED : waited == WAIT_OBJECT_0 + 1 ? PB_OK : waited == WAIT_TIMEOUT ? PB_EMPTY : PB_CORRUPT;
    }
    if (ReadAcquire(&q->Session->CloseRequested)) result = PB_STOPPED;
    if ((result == PB_OK || result == PB_EMPTY) &&
        ReadAcquire((volatile LONG *)&q->Session->SharedLifecycle->Terminal)) {
        BOOL changed = !InterlockedExchange(&q->Session->CloseRequested, 1);
        SetEvent(q->Session->ClosingEvent);
        if (changed) SetEvent(q->Session->ProgressEvent);
        result = PB_STOPPED;
    }
    return FinishQueueCall(q, result);
}
