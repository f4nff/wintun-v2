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

/* Output counts must remain caller-owned while publishing packet ownership.
 * Ordinary stack/heap outputs take this constant-time fast path. Only a pointer
 * inside the mapping needs the bounded token walk; other held loans stay valid. */
static BOOL CountInTransferredPacket(PB_USER_QUEUE *q, const PB_TOKEN *tokens, uint32_t count,
    const uint32_t *output, BOOL rx)
{
    uintptr_t address = (uintptr_t)output, region = (uintptr_t)q->Session->Region;
    if (address - region >= q->Session->Layout.RegionSize) return FALSE;
    uintptr_t data = region + (rx ? q->Session->Layout.Queues[q->Index].RxData : q->Session->Layout.Queues[q->Index].TxData);
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t packet = data + (uintptr_t)tokens[i].Slot * PB_BLOCK_SIZE;
        if (address >= packet ? address - packet < PB_BLOCK_SIZE : packet - address < sizeof(*output)) return TRUE;
    }
    return FALSE;
}
static PB_RESULT WINAPI ReleaseBatch(PB_USER_QUEUE *q, const PB_TOKEN *tokens, uint32_t count, uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!accepted) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_RETIREMENT); if (result != PB_OK) return result;
    if (!tokens || !count || count > PB_MAX_BATCH) return QueueLeave(q, PB_INVALID);
    /* Input may reside in a loan being returned. The peer can overwrite that
     * payload as soon as Producer is published, before this call retires local
     * bookkeeping. Keep every post-publication read in private storage. */
    PB_TOKEN snapshot[PB_MAX_BATCH];
    memcpy(snapshot, tokens, count * sizeof(*tokens));
    result = PbValidateTokens(&q->Tx, snapshot, count, PB_SLOT_PUBLISHED);
    if (result == PB_OK) {
        /* Reject before publishing or dropping any loan. Otherwise a native
         * producer can overwrite the output, or our final store can overwrite
         * its next packet. A private count alone cannot make that storage live. */
        if (CountInTransferredPacket(q, snapshot, count, accepted, FALSE)) return QueueLeave(q, PB_INVALID);
        uint32_t notify = 0, retired = 0;
        BOOL local = ReadAcquire(&q->Session->Fatal) != 0;
        if (!local) {
            result = PbQueuePushTokens(&q->Rings[PB_QUEUE_TX_RECYCLE], snapshot, count, &retired, &notify);
            if (result == PB_CORRUPT) { FaultSession(q->Session); local = TRUE; }
            else if (result == PB_FULL && ReadAcquire(&q->Session->CloseRequested)) local = TRUE;
        }
        /* With admission permanently stopped, a full noncooperating recycle
         * ring must not pin the user view, including a native-only fault. No
         * slot may be reused; physical reclamation still requires the native
         * close barrier. Running sessions retain ordinary PB_FULL backpressure. */
        if (local) { retired = count; result = PB_OK; }
        if (result == PB_OK) {
            for (uint32_t i = 0; i < retired; ++i) PbPoolTransition(&q->Tx, snapshot + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
            WriteRelease(&q->Loans, ReadAcquire(&q->Loans) - (LONG)retired);
            *accepted = retired;
            if (!local && notify) SetEvent(q->KernelEvent);
        }
    }
    return QueueLeave(q, result);
}

static PB_RESULT WINAPI ReserveBatch(PB_USER_QUEUE *q, _Out_writes_to_(requested, *count) PB_LEASE *leases,
    uint32_t requested, _Out_ uint32_t *count)
{
    if (count) *count = 0;
    if (!leases || !count || !requested || requested > PB_MAX_BATCH) return PB_INVALID;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->Draining) || ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    uint32_t reserved = 0;
    for (uint32_t i = 0; i < requested; ++i) {
        PB_TOKEN token; result = PbPoolReserve(&q->Rx, &token); if (result != PB_OK) break;
        PB_LEASE lease = { 0 }; lease.Descriptor.Token = token;
        lease.Descriptor.DataOffset = q->Session->Layout.Queues[q->Index].RxData + token.Slot * PB_BLOCK_SIZE;
        lease.Data = q->Session->Region + lease.Descriptor.DataOffset; lease.Capacity = 65535;
        leases[i] = lease;
        reserved++;
    }
    WriteRelease(&q->Loans, ReadAcquire(&q->Loans) + (LONG)reserved);
    *count = reserved;
    return QueueLeave(q, reserved ? PB_OK : result);
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
        if (CountInTransferredPacket(q, tokens, count, accepted, TRUE)) return QueueLeave(q, PB_INVALID);
        uint32_t notify, published = 0;
        result = PbQueuePush(&q->Rings[PB_QUEUE_RX_SUBMIT], items, count, &published, &notify);
        if (result == PB_OK) {
            for (uint32_t i = 0; i < published; ++i) PbPoolTransition(&q->Rx, tokens + i, PB_SLOT_RESERVED, PB_SLOT_PUBLISHED);
            WriteRelease(&q->Loans, ReadAcquire(&q->Loans) - (LONG)published);
            WriteRelease(&q->Published, ReadAcquire(&q->Published) + (LONG)published);
            *accepted = published;
            if (notify) SetEvent(q->KernelEvent);
        }
    }
    return QueueLeave(q, result);
}

static PB_RESULT PollInternal(PB_USER_QUEUE *q, PB_DESCRIPTOR *items, uint32_t capacity, uint32_t *count)
{
    *count = 0;
    uint32_t completed = 0;
    PB_RESULT result = PbQueuePop(&q->Rings[PB_QUEUE_RX_COMPLETE], items, capacity, &completed);
    if (result != PB_OK) return result;
    PB_TOKEN tokens[PB_MAX_BATCH];
    for (uint32_t i = 0; i < completed; ++i) tokens[i] = items[i].Token;
    if (PbValidateTokens(&q->Rx, tokens, completed, PB_SLOT_PUBLISHED) != PB_OK) {
        /* Pop already released ring space. A parked native producer must retry
         * even though this batch cannot retire any private ownership. */
        *count = 0; SetEvent(q->KernelEvent); return PB_CORRUPT;
    }
    /* The owner/DataGate exclude observers of an intermediate COMPLETE state.
     * Keep the checked transition and free-index maintenance after validating
     * the whole batch, without validating/writing an unobservable state twice. */
    for (uint32_t i = 0; i < completed; ++i)
        PbPoolTransition(&q->Rx, tokens + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
    WriteRelease(&q->Published, ReadAcquire(&q->Published) - (LONG)completed);
    *count = completed;
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
PB_RESULT PbCompatWait(PB_USER_QUEUE *q, HANDLE external, BOOL receive, uint32_t timeout)
{
    if (!q || !q->Session) return PB_INVALID;
    if (ReadAcquire(&q->Session->Fatal)) return PB_CORRUPT;
    if (ReadAcquire(&q->Session->CloseRequested)) return PB_STOPPED;
    PB_RESULT result = QueueEnter(q, PB_QUEUE_ADMISSION); if (result != PB_OK) return result;
    if (ReadAcquire(&q->Session->CloseRequested)) return QueueLeave(q, PB_STOPPED);
    uint32_t tx = 0, rx = 0;
    result = receive ? PbQueueArm(&q->Rings[PB_QUEUE_TX_READY], &tx) : PB_OK;
    if (result == PB_OK) result = PbQueueArm(&q->Rings[PB_QUEUE_RX_COMPLETE], &rx);
    ReleaseSRWLockShared(&q->Session->DataGate);
    if (result == PB_OK && !tx && !rx) {
        HANDLE events[] = { q->Session->ClosingEvent, q->UserEvent, external };
#ifdef PB_API_TESTS
        InterlockedIncrement(&q->Session->TestWaiters);
#endif
        DWORD waited = WaitForMultipleObjects(external ? 3 : 2, events, FALSE, timeout);
#ifdef PB_API_TESTS
        InterlockedDecrement(&q->Session->TestWaiters);
#endif
        result = waited == WAIT_OBJECT_0 ? PB_STOPPED :
            (waited == WAIT_OBJECT_0 + 1 || (external && waited == WAIT_OBJECT_0 + 2)) ? PB_OK :
            waited == WAIT_TIMEOUT ? PB_EMPTY : PB_CORRUPT;
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
static PB_RESULT WINAPI Wait(PB_USER_QUEUE *q, uint32_t timeout)
{
    return PbCompatWait(q, NULL, TRUE, timeout);
}
