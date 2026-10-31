/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#pragma once

#include <Windows.h>
#include "operation.h"

#if defined(_M_IX86)
#    define IMAGE_FILE_PROCESS IMAGE_FILE_MACHINE_I386
#elif defined(_M_AMD64)
#    define IMAGE_FILE_PROCESS IMAGE_FILE_MACHINE_AMD64
#elif defined(_M_ARM)
#    define IMAGE_FILE_PROCESS IMAGE_FILE_MACHINE_ARMNT
#elif defined(_M_ARM64)
#    define IMAGE_FILE_PROCESS IMAGE_FILE_MACHINE_ARM64
#else
#    error Unsupported architecture
#endif

extern HINSTANCE ResourceModule;
extern HANDLE ModuleHeap;
extern SECURITY_ATTRIBUTES SecurityAttributes;
extern SECURITY_ATTRIBUTES PipeSecurityAttributes;
extern BOOL IsLocalSystem;
extern USHORT NativeMachine;
/* Each successful acquire owns a dependency lease. Adapters and asynchronous
 * jobs retain it across their entire lifetime, including callback barriers.
 * The last release resets delay imports and unloads them in ordinary context. */
BOOL MainRuntimeAcquire(VOID);
VOID MainRuntimeRetain(VOID);
VOID MainRuntimeRelease(VOID);
VOID MainRuntimeBeginShutdown(VOID);
BOOL MainRuntimeShutdown(DWORD Timeout);
static inline DWORD MainRemaining(ULONGLONG Deadline, DWORD Timeout)
{
    if (Timeout == INFINITE) return INFINITE;
    ULONGLONG Now = GetTickCount64();
    return Now >= Deadline ? 0 : (DWORD)(Deadline - Now);
}

#if NTDDI_VERSION > NTDDI_WIN7
#    define IsWindows7 FALSE
#else
extern BOOL IsWindows7;
#endif

#if NTDDI_VERSION >= NTDDI_WIN10
#    define IsWindows10 TRUE
#else
extern BOOL IsWindows10;
#endif
