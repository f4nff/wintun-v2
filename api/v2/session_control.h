/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
static PB_RESULT PolicyIo(PB_SESSION *s, uint32_t mask)
{
    if (!s || mask & ~PB_PROBE_SUPPORTED || (mask && !s->Experimental)) return PB_INVALID;
    AcquireSRWLockShared(&s->Lifetime);
    AcquireSRWLockExclusive(&s->Control);
    if (mask && InterlockedCompareExchange(&s->CloseRequested, 0, 0)) {
        ReleaseSRWLockExclusive(&s->Control); ReleaseSRWLockShared(&s->Lifetime); return PB_STOPPED;
    }
    PB_PROBE_POLICY policy = { sizeof(policy), PB_PROBE_SCHEMA, mask, 0 }; DWORD returned;
#ifdef PB_API_TESTS
    TestBoundary(s, PB_TEST_POLICY);
#endif
    BOOL okay = DeviceIoControl(s->Device, PB_PROBE_IOCTL_POLICY, &policy, sizeof(policy), NULL, 0, &returned, NULL);
#ifdef PB_API_TESTS
    if (s->Test) okay = !TestFails(s, PB_TEST_POLICY);
#endif
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    ReleaseSRWLockExclusive(&s->Control); ReleaseSRWLockShared(&s->Lifetime); SetLastError(error); return okay ? PB_OK : PB_UNSUPPORTED;
}
static PB_RESULT WINAPI SetPolicy(PB_SESSION *s, uint32_t mask)
{
    uint64_t ticket; PB_RESULT result = BeginSetPolicy(s, mask, &ticket);
    if (result != PB_OK) return result;
    PB_RESULT applied; result = PollPolicy(s, ticket, INFINITE, &applied);
    if (result != PB_OK) {
        /* This synchronous caller cannot return its ticket. Abandon on wait
         * failure, retaining the slot until real I/O completes if still owned. */
        DWORD error = GetLastError(); ForgetPolicy(s, ticket); SetLastError(error);
    }
    return result == PB_OK ? applied : result;
}

static PB_RESULT DrainIo(PB_SESSION *s)
{
    if (!s) return PB_INVALID;
    InterlockedExchange(&s->CloseRequested, 1); SetEvent(s->ClosingEvent);
    AcquireSRWLockShared(&s->Lifetime);
    AcquireSRWLockExclusive(&s->Control);
#ifdef PB_API_TESTS
    TestBoundary(s, PB_TEST_DRAIN);
#endif
    DWORD returned; BOOL okay = TRUE;
    if (!s->Draining) {
        okay = DeviceIoControl(s->Device, PB_V2_IOCTL_DRAIN, NULL, 0, NULL, 0, &returned, NULL);
#ifdef PB_API_TESTS
        if (s->Test) okay = !TestFails(s, PB_TEST_DRAIN);
#endif
        if (okay) InterlockedExchange(&s->Draining, 1);
    }
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    ReleaseSRWLockExclusive(&s->Control); ReleaseSRWLockShared(&s->Lifetime); SetLastError(error); return okay ? PB_OK : PB_CORRUPT;
}
static PB_RESULT WINAPI Drain(PB_SESSION *s)
{
    if (!s) return PB_INVALID;
    PB_RESULT result = BeginStop(s); if (result != PB_OK) return result;
    AcquireSRWLockExclusive(&s->State);
    while (!InterlockedCompareExchange(&s->Draining, 0, 0) && s->Progress.Result != PB_CORRUPT) {
        /* A failed wait is not a wake. Retrying while it keeps failing can
         * spin with State held; the native stop worker must retain ownership. */
        if (!SleepConditionVariableSRW(&s->Changed, &s->State, INFINITE, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; break; }
    }
    if (result == PB_OK) result = s->Draining ? PB_OK : PB_CORRUPT;
    ReleaseSRWLockExclusive(&s->State); return result;
}
