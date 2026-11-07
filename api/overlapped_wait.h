/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <Windows.h>
/* Nonblocking prepare-to-wait transaction for one exclusively owned live file,
 * OVERLAPPED, buffer and manual event. Signals are hints, not completions.
 * Reset while still pending, then requery so completion before the reset cannot
 * lose its only wake. A failed reset retains ownership and reports pending;
 * the caller records ResetError and backs off instead of trusting that event. */
static BOOL PollOwnedIo(HANDLE File, OVERLAPPED *Io, DWORD *Bytes, DWORD *ResetError)
{
    *ResetError = ERROR_SUCCESS;
    BOOL okay = GetOverlappedResult(File, Io, Bytes, FALSE);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (okay || error != ERROR_IO_INCOMPLETE) { SetLastError(error); return okay; }
    if (!ResetEvent(Io->hEvent)) {
        error = GetLastError();
        *ResetError = error ? error : ERROR_GEN_FAILURE;
        SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
    }
    okay = GetOverlappedResult(File, Io, Bytes, FALSE);
    error = okay ? ERROR_SUCCESS : GetLastError();
    SetLastError(error); return okay;
}

/* Keep every owner until a nonwaiting query observes terminal I/O. Available
 * on the Win7 baseline; no GetOverlappedResultEx dependency. */
static BOOL WaitForOwnedIo(HANDLE File, OVERLAPPED *Io, DWORD *Bytes, DWORD *WaitError)
{
    *WaitError = ERROR_SUCCESS;
    for (;;) {
        DWORD resetError;
        BOOL okay = PollOwnedIo(File, Io, Bytes, &resetError);
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (okay || error != ERROR_IO_INCOMPLETE) { SetLastError(error); return okay; }
        if (resetError) {
            if (!*WaitError) *WaitError = resetError;
            Sleep(1000); continue;
        }
        DWORD waited = WaitForSingleObject(Io->hEvent, INFINITE);
        if (waited != WAIT_OBJECT_0) {
            error = waited == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            if (!*WaitError) *WaitError = error ? error : ERROR_GEN_FAILURE;
            /* A broken wait must retain the owner without a CPU retry loop. */
            Sleep(1000);
        }
    }
}
