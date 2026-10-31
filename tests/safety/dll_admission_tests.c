/* SPDX-License-Identifier: GPL-2.0 */
/* Hold real Allocate calls at their first mapping request. Both quota dimensions
 * must already include construction, before any heavy allocation is attempted. */
#define WIN32_LEAN_AND_MEAN
#define PB_API_TESTS 1
#define PB_RUNTIME_MAX_SESSIONS 4u
#define PB_RUNTIME_MAX_BYTES (3u * 1024u * 1024u)
#include "guard_memory.h"
#include "../../api/v2/lifecycle.h"
static HANDLE Start, MappingEntered, MappingGate, Rejected;
static LONG MappingCalls, Rejections, Expected, TargetRejections;
static LPVOID WINAPI CountMapping(LPVOID address, SIZE_T size, DWORD type, DWORD protection)
{
    LONG count = InterlockedIncrement(&MappingCalls);
    if (count == Expected) SetEvent(MappingEntered);
    SAFETY_CHECK(WaitForSingleObject(MappingGate, 5000) == WAIT_OBJECT_0);
    return VirtualAlloc(address, size, type, protection);
}
#define VirtualAlloc CountMapping
#include "../../api/v2/session.c"
#undef VirtualAlloc
typedef struct { PB_CONFIG Config; PB_SESSION *Session; PB_RESULT Result; } REQUEST;
static DWORD WINAPI CreateSession(PVOID opaque)
{
    REQUEST *request = opaque; void *region; PB_LAYOUT layout; uint64_t epoch;
    SAFETY_CHECK(WaitForSingleObject(Start, 5000) == WAIT_OBJECT_0);
    request->Result = PbTestOpen(&request->Config, &request->Session, &region, &layout, &epoch);
    if (request->Result == PB_FULL && InterlockedIncrement(&Rejections) == TargetRejections) SetEvent(Rejected);
    return 0;
}
static void CheckBudget(uint32_t slots, LONG expected)
{
    enum { THREADS = 8 }; REQUEST requests[THREADS]; HANDLE threads[THREADS];
    Expected = expected; TargetRejections = THREADS - expected; MappingCalls = Rejections = 0;
    Start = CreateEventW(NULL, TRUE, FALSE, NULL); MappingEntered = CreateEventW(NULL, TRUE, FALSE, NULL);
    MappingGate = CreateEventW(NULL, TRUE, FALSE, NULL); Rejected = CreateEventW(NULL, TRUE, FALSE, NULL);
    SAFETY_CHECK(Start && MappingEntered && MappingGate && Rejected);
    PB_LAYOUT layout; SAFETY_CHECK(PbCalculateLayout(1, slots, slots, &layout) == PB_OK);
    for (unsigned i = 0; i < THREADS; ++i) {
        requests[i] = (REQUEST){.Config = {sizeof(PB_CONFIG), 1, slots, slots, 0, 0}};
        threads[i] = CreateThread(NULL, 0, CreateSession, &requests[i], 0, NULL); SAFETY_CHECK(threads[i]);
    }
    SAFETY_CHECK(SetEvent(Start) && WaitForSingleObject(MappingEntered, 5000) == WAIT_OBJECT_0);
    SAFETY_CHECK(WaitForSingleObject(Rejected, 5000) == WAIT_OBJECT_0);
    PB_RUNTIME_STATUS status; SAFETY_CHECK(GetRuntimeStatus(&status) == PB_OK);
    SAFETY_CHECK(MappingCalls == expected && status.Sessions == (uint32_t)expected && status.RetainedBytes == (uint64_t)expected * layout.RegionSize);
    /* All admitted objects are still unpublished and have not allocated a mapping. */
    SAFETY_CHECK(!RuntimeSessions && SetEvent(MappingGate));
    SAFETY_CHECK(WaitForMultipleObjects(THREADS, threads, TRUE, 5000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < THREADS; ++i) {
        SAFETY_CHECK(requests[i].Result == PB_OK || (requests[i].Result == PB_FULL && !requests[i].Session));
        if (requests[i].Result == PB_OK) SAFETY_CHECK(Close(requests[i].Session, 5000) == PB_OK);
        SAFETY_CHECK(CloseHandle(threads[i]));
    }
    ULONGLONG deadline = GetTickCount64() + 5000;
    do { SAFETY_CHECK(GetRuntimeStatus(&status) == PB_OK && GetTickCount64() < deadline); SwitchToThread(); } while (status.Sessions);
    SAFETY_CHECK(!status.RetainedBytes);
    SAFETY_CHECK(CloseHandle(Start) && CloseHandle(MappingEntered) && CloseHandle(MappingGate) && CloseHandle(Rejected));
}
int main(void)
{
    CheckBudget(4, 4); /* Session count binds first. */
    CheckBudget(16, 1); /* Mapping bytes bind first. */
    puts("PASS: concurrent session construction reserves count/bytes before mappings; rejected requests allocate no mapping; full quota recovery.");
    return 0;
}
