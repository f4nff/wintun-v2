/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
static PB_RESULT StatusInternal(PB_SESSION *s, PB_V2_STATUS *status)
{
    if (InterlockedCompareExchange(&s->ResourcesClosed, 0, 0)) {
        memset(status, 0, sizeof(*status)); status->Size = sizeof(*status); status->Epoch = s->Epoch;
        status->QueueCount = s->Layout.QueueCount; status->State = PB_SESSION_STOPPED; return PB_OK;
    }
#ifdef PB_API_TESTS
    if (s->Test) {
        InterlockedIncrement(&s->TestQueries);
        TestBoundary(s, PB_TEST_QUERY);
        if (TestFails(s, PB_TEST_QUERY)) return PB_CORRUPT;
        memset(status, 0, sizeof(*status)); status->Size = sizeof(*status); status->Epoch = s->Epoch;
        status->ActiveReferences = (uint32_t)InterlockedCompareExchange(&s->TestActive, 0, 0); status->QueueCount = s->Layout.QueueCount;
        status->State = (s->Draining || InterlockedCompareExchange(&s->TestFault, 0, 0)) ?
            (status->ActiveReferences ? PB_SESSION_QUARANTINED : PB_SESSION_STOPPED) : PB_SESSION_RUNNING;
        return PB_OK;
    }
#endif
    DWORD returned;
    if (!DeviceIoControl(s->Device, PB_V2_IOCTL_QUERY, NULL, 0, status, sizeof(*status), &returned, NULL)) return PB_CORRUPT;
    if (returned == sizeof(*status) && status->Size == sizeof(*status) && status->Epoch == s->Epoch) return PB_OK;
    /* Successful I/O has no useful failure code. Report the rejected reply,
     * rather than an unrelated thread error in lifecycle's close progress. */
    SetLastError(ERROR_INVALID_DATA); return PB_CORRUPT;
}
static PB_RESULT WINAPI GetStatus(PB_SESSION *s, PB_V2_STATUS *status)
{
    if (!s || !status) return PB_INVALID;
    if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_BUSY;
    PB_RESULT result = StatusInternal(s, status); ReleaseSRWLockShared(&s->Lifetime); return result;
}
static PB_RESULT WINAPI GetCapabilities(PB_SESSION *s, PB_PROBE_STATUS *status)
{
    if (!s || !status) return PB_INVALID;
    if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_BUSY;
    if (s->ResourcesClosed) { ReleaseSRWLockShared(&s->Lifetime); return PB_STOPPED; }
    DWORD returned;
    BOOL okay = DeviceIoControl(s->Device, PB_PROBE_IOCTL_QUERY, NULL, 0, status, sizeof(*status), &returned, NULL);
#ifdef PB_API_TESTS
    if (s->Test) {
        memset(status, 0, sizeof(*status)); status->Size = sizeof(*status); status->Magic = PB_PROBE_MAGIC;
        status->Schema = PB_PROBE_SCHEMA; status->FileVersion = PB_VERSION_PACKED; status->DataAbi = PB_ABI_MAJOR;
        status->ImplementedMask = PB_PROBE_SUPPORTED; status->RxOffloadsImplemented = 1; status->SessionRegistered = 1;
        memcpy(status->SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING)); returned = sizeof(*status); okay = TRUE;
    }
#endif
    PB_RESULT result = okay && returned == sizeof(*status) && status->Size == sizeof(*status) && status->Magic == PB_PROBE_MAGIC &&
        status->Schema == PB_PROBE_SCHEMA && status->FileVersion == PB_VERSION_PACKED && status->DataAbi == PB_ABI_MAJOR &&
        !memcmp(status->SourceFingerprint, PB_SOURCE_STRING, sizeof(status->SourceFingerprint)) ? PB_OK : PB_UNSUPPORTED;
    ReleaseSRWLockShared(&s->Lifetime); return result;
}
