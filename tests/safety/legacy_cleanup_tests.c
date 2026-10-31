/* SPDX-License-Identifier: GPL-2.0 */
#include "guard_memory.h"
typedef BOOL (WINAPI *QUEUE)(HANDLE,HANDLE,HANDLE,BOOL);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
int wmain(int argc, WCHAR **argv)
{
    SAFETY_CHECK(argc == 2);
    for (ULONG i = 0; i < 64; ++i) {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL), gate = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE detached = CreateEventW(NULL, TRUE, FALSE, NULL); SAFETY_CHECK(entered && gate && detached);
        HMODULE library = LoadLibraryW(argv[1]); SAFETY_CHECK(library);
        QUEUE queue; SHUTDOWN shutdown;
        *(FARPROC *)&queue = GetProcAddress(library, "Queue");
        *(FARPROC *)&shutdown = GetProcAddress(library, "Shutdown"); SAFETY_CHECK(queue && shutdown);
        SAFETY_CHECK(!queue(entered, gate, detached, TRUE)); /* Failed submission releases its ref and resets scheduled. */
        SAFETY_CHECK(queue(entered, gate, detached, FALSE));
        SAFETY_CHECK(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0);
        if (i & 1) {
            SAFETY_CHECK(!shutdown(0) && GetLastError() == ERROR_TIMEOUT);
            SAFETY_CHECK(!queue(entered, gate, detached, FALSE) && GetLastError() == ERROR_SHUTDOWN_IN_PROGRESS);
            SAFETY_CHECK(SetEvent(gate) && shutdown(5000));
        }
        SAFETY_CHECK(FreeLibrary(library));
        if (!(i & 1)) SAFETY_CHECK(WaitForSingleObject(detached, 0) == WAIT_TIMEOUT && SetEvent(gate));
        SAFETY_CHECK(WaitForSingleObject(detached, 5000) == WAIT_OBJECT_0);
        SAFETY_CHECK(CloseHandle(entered) && CloseHandle(gate) && CloseHandle(detached));
    }
    puts("PASS: 64 real DLL load/unload cycles, failed submission retry, callback module pin, shutdown timeout and return-tail release.");
    return 0;
}
