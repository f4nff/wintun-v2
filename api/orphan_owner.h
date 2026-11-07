/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once
#include <Windows.h>
#include <devpropdef.h>
#include <string.h>
static const DEVPROPKEY DEVPKEY_Wintun_OwningProcess = {
    { 0x3361c968, 0x2f2e, 0x4660, { 0xb4, 0x7e, 0x69, 0x9c, 0xdc, 0x4c, 0x32, 0xb9 } },
    DEVPROPID_FIRST_USABLE + 3
};

typedef struct _OWNING_PROCESS
{
    DWORD ProcessId;
    FILETIME CreationTime;
} OWNING_PROCESS;

static BOOL
ProcessIsStale(_In_ OWNING_PROCESS *OwningProcess)
{
    /* Failure to inspect a process is not proof that it exited. The access mask
     * is fixed and supported on Win7; INVALID_PARAMETER means no such user PID.
     * Keep denied/temporarily unavailable owners until another scan can prove it. */
    HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, OwningProcess->ProcessId);
    if (!Process)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    DWORD Wait = WaitForSingleObject(Process, 0);
    BOOL Stale = Wait == WAIT_OBJECT_0;
    if (Wait == WAIT_TIMEOUT)
    {
        FILETIME CreationTime, Unused;
        /* A PID can already belong to a different live process. Compare the
         * identity on the same opened object, not a second PID lookup. */
        if (GetProcessTimes(Process, &CreationTime, &Unused, &Unused, &Unused))
            Stale = !!memcmp(&CreationTime, &OwningProcess->CreationTime, sizeof(CreationTime));
    }
    CloseHandle(Process);
    return Stale;
}

