/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "logger.h"
#include "adapter.h"
#include "main.h"
#include "namespace.h"
#include "registry.h"
#include "ntdll.h"
#include "rundll32.h"

#include <Windows.h>
#include <delayimp.h>
#include <sddl.h>
#include <winefs.h>
#include <stdlib.h>
#include <assert.h>

HINSTANCE ResourceModule;
HANDLE ModuleHeap;
SECURITY_ATTRIBUTES SecurityAttributes = { .nLength = sizeof(SECURITY_ATTRIBUTES) };
SECURITY_ATTRIBUTES PipeSecurityAttributes = { .nLength = sizeof(SECURITY_ATTRIBUTES) };
BOOL IsLocalSystem;
USHORT NativeMachine = IMAGE_FILE_PROCESS;

#if NTDDI_VERSION == NTDDI_WIN7
BOOL IsWindows7;
#endif
#if NTDDI_VERSION < NTDDI_WIN10
BOOL IsWindows10;
#endif

static FARPROC WINAPI
DelayedLoadLibraryHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify != dliNotePreLoadLibrary)
        return NULL;
    HMODULE Library = LoadLibraryExA(pdli->szDll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Library)
        abort();
    return (FARPROC)Library;
}

const PfnDliHook __pfnDliNotifyHook2 = DelayedLoadLibraryHook;

static BOOL InitializeSecurityObjects(VOID)
{
    BYTE LocalSystemSid[MAX_SID_SIZE];
    DWORD RequiredBytes = sizeof(LocalSystemSid);
    HANDLE CurrentProcessToken;
    struct
    {
        TOKEN_USER MaybeLocalSystem;
        CHAR LargeEnoughForLocalSystem[MAX_SID_SIZE];
    } TokenUserBuffer;
    BOOL Ret = FALSE;

    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, LocalSystemSid, &RequiredBytes))
        return FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &CurrentProcessToken))
        return FALSE;

    if (!GetTokenInformation(CurrentProcessToken, TokenUser, &TokenUserBuffer, sizeof(TokenUserBuffer), &RequiredBytes))
        goto cleanupProcessToken;

    IsLocalSystem = EqualSid(TokenUserBuffer.MaybeLocalSystem.User.Sid, LocalSystemSid);
    Ret = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        IsLocalSystem ? L"O:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NWNRNX;;;HI)"
                      : L"O:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NWNRNX;;;HI)",
        SDDL_REVISION_1,
        &SecurityAttributes.lpSecurityDescriptor,
        NULL);
    if (Ret) {
        /* Local pipe clients run as this user. Give that SID and SYSTEM access,
         * without copying a file/namespace SACL that would require an enabled
         * SeSecurityPrivilege on CreateNamedPipe. The default integrity label
         * follows the creating token; remote clients are rejected separately. */
        LPWSTR sid = NULL; WCHAR pipeSddl[256];
        Ret = ConvertSidToStringSidW(TokenUserBuffer.MaybeLocalSystem.User.Sid, &sid);
        if (Ret) {
            if (swprintf_s(pipeSddl, _countof(pipeSddl), L"D:P(A;;GRGW;;;%s)(A;;GA;;;SY)", sid) < 0) {
                SetLastError(ERROR_BUFFER_OVERFLOW); Ret = FALSE;
            } else Ret = ConvertStringSecurityDescriptorToSecurityDescriptorW(pipeSddl, SDDL_REVISION_1,
                &PipeSecurityAttributes.lpSecurityDescriptor, NULL);
            LocalFree(sid);
        }
        if (!Ret) {
            DWORD error = GetLastError(); LocalFree(SecurityAttributes.lpSecurityDescriptor);
            SecurityAttributes.lpSecurityDescriptor = NULL; SetLastError(error);
        }
    }

cleanupProcessToken:
    DWORD error = Ret ? ERROR_SUCCESS : GetLastError();
    CloseHandle(CurrentProcessToken);
    if (!Ret) SetLastError(error);
    return Ret;
}

static void EnvInit(VOID)
{
    DWORD MajorVersion, MinorVersion;
    RtlGetNtVersionNumbers(&MajorVersion, &MinorVersion, NULL);

#if NTDDI_VERSION == NTDDI_WIN7
    IsWindows7 = MajorVersion == 6 && MinorVersion == 1;
#endif
#if NTDDI_VERSION < NTDDI_WIN10
    IsWindows10 = MajorVersion >= 10;
#endif

#ifdef MAYBE_WOW64
    HANDLE Kernel32;
    BOOL(WINAPI * IsWow64Process2)
    (_In_ HANDLE Process, _Out_ USHORT * ProcessMachine, _Out_opt_ USHORT * NativeMachine);
    USHORT ProcessMachine;
    if ((Kernel32 = GetModuleHandleW(L"kernel32.dll")) == NULL ||
        (*(FARPROC *)&IsWow64Process2 = GetProcAddress(Kernel32, "IsWow64Process2")) == NULL ||
        !IsWow64Process2(GetCurrentProcess(), &ProcessMachine, &NativeMachine))
    {
        BOOL IsWoW64;
        NativeMachine =
            IsWow64Process(GetCurrentProcess(), &IsWoW64) && IsWoW64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_PROCESS;
    }
#endif
}

static INIT_ONCE RuntimeOnce = INIT_ONCE_STATIC_INIT;
static BOOL RuntimeReady;
static SRWLOCK DependencyLock = SRWLOCK_INIT;
static CONDITION_VARIABLE DependencyChanged = CONDITION_VARIABLE_INIT;
static ULONG DependencyUsers;
static BOOL DependencyUnloading, RuntimeStopping;
static BOOL CALLBACK InitializeRuntime(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    ModuleHeap = HeapCreate(0, 0, 0);
    if (!ModuleHeap) return FALSE;
    if (!InitializeSecurityObjects()) {
        DWORD error = GetLastError(); HeapDestroy(ModuleHeap); ModuleHeap = NULL;
        SetLastError(error); return FALSE;
    }
    EnvInit(); NamespaceInit(); RuntimeReady = TRUE; return TRUE;
}
static VOID UnloadDependencies(VOID)
{
    /* Enumerate the linker's own descriptors; no second hand-maintained DLL list.
     * /DELAY:UNLOAD restores the thunks before dropping their loader references.
     * DependencyUnloading excludes every caller while IATs are being restored.
     * Neither the state lock nor the loader lock is held by this function. */
    const BYTE *base = (const BYTE *)ResourceModule;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD offset = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
    if (!offset) return;
    const ImgDelayDescr *descriptor = (const ImgDelayDescr *)(base + offset);
    for (; descriptor->rvaDLLName; ++descriptor)
        __FUnloadDelayLoadedDLL2((const CHAR *)(base + descriptor->rvaDLLName));
}
BOOL MainRuntimeAcquire(VOID)
{
    AcquireSRWLockExclusive(&DependencyLock);
    while (DependencyUnloading)
        SleepConditionVariableSRW(&DependencyChanged, &DependencyLock, INFINITE, 0);
    if (RuntimeStopping) {
        ReleaseSRWLockExclusive(&DependencyLock);
        SetLastError(ERROR_SHUTDOWN_IN_PROGRESS); return FALSE;
    }
    ++DependencyUsers;
    ReleaseSRWLockExclusive(&DependencyLock);
    if (InitOnceExecuteOnce(&RuntimeOnce, InitializeRuntime, NULL, NULL)) return TRUE;
    DWORD error = GetLastError(); MainRuntimeRelease(); SetLastError(error); return FALSE;
}
VOID MainRuntimeRetain(VOID)
{
    AcquireSRWLockExclusive(&DependencyLock);
    assert(DependencyUsers && !DependencyUnloading);
    ++DependencyUsers;
    ReleaseSRWLockExclusive(&DependencyLock);
}
VOID MainRuntimeRelease(VOID)
{
    DWORD error = GetLastError();
    AcquireSRWLockExclusive(&DependencyLock);
    assert(DependencyUsers && !DependencyUnloading);
    BOOL unload = !--DependencyUsers;
    if (unload) DependencyUnloading = TRUE;
    ReleaseSRWLockExclusive(&DependencyLock);
    if (unload) {
        UnloadDependencies();
        AcquireSRWLockExclusive(&DependencyLock);
        DependencyUnloading = FALSE; WakeAllConditionVariable(&DependencyChanged);
        ReleaseSRWLockExclusive(&DependencyLock);
    }
    SetLastError(error);
}
VOID MainRuntimeBeginShutdown(VOID)
{
    AcquireSRWLockExclusive(&DependencyLock); RuntimeStopping = TRUE;
    ReleaseSRWLockExclusive(&DependencyLock);
    MainOperationRequestShutdown();
}
BOOL MainRuntimeShutdown(DWORD Timeout)
{
    ULONGLONG deadline = GetTickCount64() + Timeout;
    AcquireSRWLockExclusive(&DependencyLock);
    while (DependencyUsers || DependencyUnloading) {
        DWORD remaining = MainRemaining(deadline, Timeout);
        if (!remaining) { ReleaseSRWLockExclusive(&DependencyLock); SetLastError(ERROR_TIMEOUT); return FALSE; }
        if (!SleepConditionVariableSRW(&DependencyChanged, &DependencyLock, remaining, 0) && GetLastError() != ERROR_TIMEOUT) {
            DWORD error = GetLastError(); ReleaseSRWLockExclusive(&DependencyLock); SetLastError(error); return FALSE;
        }
    }
    ReleaseSRWLockExclusive(&DependencyLock); return TRUE;
}

#include "operation_runtime.h"

BOOL APIENTRY
DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        ResourceModule = hinstDLL;
        break;

    case DLL_PROCESS_DETACH:
        if (!lpvReserved) {
            AdapterCleanupDetach();
            Rundll32Detach();
            MainOperationDetach();
            if (RuntimeReady) {
                NamespaceDone();
                LocalFree(SecurityAttributes.lpSecurityDescriptor);
                LocalFree(PipeSecurityAttributes.lpSecurityDescriptor);
                HeapDestroy(ModuleHeap);
            }
        }
        break;
    }
    return TRUE;
}
