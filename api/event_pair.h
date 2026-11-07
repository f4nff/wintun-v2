/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <Windows.h>

/* Construct privately, then let the caller publish under its state lock. A
 * failed first acquisition must not run a later API that can erase its error.
 * Both outputs are unowned on failure; no half-initialized pair can escape. */
static BOOL CreateManualEventPair(BOOL firstSignaled, HANDLE *first, HANDLE *second)
{
    *first = *second = NULL;
    HANDLE idle = CreateEventW(NULL, TRUE, firstSignaled, NULL);
    if (!idle) {
        DWORD error = GetLastError(); SetLastError(error ? error : ERROR_GEN_FAILURE); return FALSE;
    }
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop) {
        DWORD error = GetLastError();
        CloseHandle(idle); SetLastError(error ? error : ERROR_GEN_FAILURE); return FALSE;
    }
    *first = idle; *second = stop;
    return TRUE;
}
