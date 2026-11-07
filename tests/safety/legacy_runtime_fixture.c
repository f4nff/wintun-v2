/* SPDX-License-Identifier: GPL-2.0 */
/* Real runtime, delay imports and helper transport; only package extraction and
 * process target are replaced. The child runs this test's executable, never SetupAPI. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
static HANDLE Detached;
#define DllMain ProductionDllMain
#ifndef PB_LEGACY_RUNTIME_MAIN_SOURCE
#define PB_LEGACY_RUNTIME_MAIN_SOURCE "../../api/main.c"
#endif
#include PB_LEGACY_RUNTIME_MAIN_SOURCE
#undef DllMain
VOID AdapterCleanupDetach(VOID) {}
VOID NamespaceInit(VOID) {}
VOID NamespaceDone(VOID) {}
static WCHAR ChildExecutable[MAX_PATH], TestRoot[MAX_PATH];
static LONG Sequence, FailAt, Acquisitions;
static LONG OwnedHandles, OwnedAllocations, OwnedWorks;
static BOOL Failure(VOID)
{
    if (InterlockedIncrement(&Acquisitions) != InterlockedCompareExchange(&FailAt, 0, 0)) return FALSE;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY); return TRUE;
}
static VOID *TrackAllocation(VOID *allocation) { if (allocation) InterlockedIncrement(&OwnedAllocations); return allocation; }
static HANDLE TrackHandle(HANDLE handle) { if (handle && handle != INVALID_HANDLE_VALUE) InterlockedIncrement(&OwnedHandles); return handle; }
static VOID *TestAlloc(SIZE_T bytes) { return Failure() ? NULL : TrackAllocation(Alloc(bytes)); }
static VOID *TestZalloc(SIZE_T bytes) { return Failure() ? NULL : TrackAllocation(Zalloc(bytes)); }
static VOID *TestArray(SIZE_T count, SIZE_T bytes) { return Failure() ? NULL : TrackAllocation(AllocArray(count, bytes)); }
static VOID *TestReZalloc(VOID *memory, SIZE_T bytes)
{
    if (Failure()) return NULL;
    VOID *allocation = ReZalloc(memory, bytes);
    if (allocation && !memory) InterlockedIncrement(&OwnedAllocations);
    return allocation;
}
static VOID TestFree(VOID *allocation) { if (allocation) { assert(OwnedAllocations > 0); InterlockedDecrement(&OwnedAllocations); } Free(allocation); }
#ifndef PB_FIXTURE_CLOSE_OBSERVER
#define PB_FIXTURE_CLOSE_OBSERVER(handle) ((void)(handle))
#endif
static BOOL WINAPI TestClose(HANDLE handle) { PB_FIXTURE_CLOSE_OBSERVER(handle); assert(OwnedHandles > 0); InterlockedDecrement(&OwnedHandles); return CloseHandle(handle); }
#ifndef PB_FIXTURE_EVENT_RESULT
#define PB_FIXTURE_EVENT_RESULT(handle) (handle)
#endif
static HANDLE WINAPI TestEvent(LPSECURITY_ATTRIBUTES security, BOOL manual, BOOL initial, LPCWSTR name)
{ return PB_FIXTURE_EVENT_RESULT(Failure() ? NULL : TrackHandle(CreateEventW(security, manual, initial, name))); }
static BOOL WINAPI TestModule(DWORD flags, LPCWSTR address, HMODULE *module)
{ return Failure() ? FALSE : GetModuleHandleExW(flags, address, module); }
static PTP_WORK WINAPI TestWork(PTP_WORK_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment)
{
    if (Failure()) return NULL;
    PTP_WORK work = CreateThreadpoolWork(callback, context, environment); if (work) InterlockedIncrement(&OwnedWorks); return work;
}
static VOID WINAPI TestCloseWork(PTP_WORK work) { assert(OwnedWorks > 0); InterlockedDecrement(&OwnedWorks); CloseThreadpoolWork(work); }
static HANDLE WINAPI TestPipe(LPCWSTR name, DWORD open, DWORD mode, DWORD instances, DWORD output, DWORD input, DWORD timeout, LPSECURITY_ATTRIBUTES security)
{ return Failure() ? INVALID_HANDLE_VALUE : TrackHandle(CreateNamedPipeW(name, open, mode, instances, output, input, timeout, security)); }
static BOOL WINAPI TestConnect(HANDLE pipe, LPOVERLAPPED io)
{ return Failure() ? FALSE : ConnectNamedPipe(pipe, io); }
static HANDLE WINAPI TestFile(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{ return Failure() ? INVALID_HANDLE_VALUE : TrackHandle(CreateFileW(name, access, share, security, disposition, flags, templateFile)); }
static BOOL WINAPI TestInitializeAttributes(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags, PSIZE_T size)
{ return list && Failure() ? FALSE : InitializeProcThreadAttributeList(list, count, flags, size); }
static BOOL WINAPI TestUpdateAttributes(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attribute, PVOID value, SIZE_T size, PVOID previous, PSIZE_T returned)
{ return Failure() ? FALSE : UpdateProcThreadAttribute(list, flags, attribute, value, size, previous, returned); }
static BOOL WINAPI TestProcess(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES processSecurity,
    LPSECURITY_ATTRIBUTES threadSecurity, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    (void)application;
    if (Failure()) return FALSE;
    WCHAR replacement[2048]; const WCHAR *function = wcsrchr(command, L',');
    if (!function || swprintf_s(replacement, _countof(replacement), L"\"%s\" child %s", ChildExecutable, function + 1) < 0) return FALSE;
    BOOL okay = CreateProcessW(ChildExecutable, replacement, processSecurity, threadSecurity, inherit, flags, environment, directory, startup, process);
    if (okay) { TrackHandle(process->hProcess); TrackHandle(process->hThread); }
    return okay;
}
#undef Alloc
#undef Zalloc
#undef AllocArray
#undef ReZalloc
#define Alloc TestAlloc
#define Zalloc TestZalloc
#define AllocArray TestArray
#define ReZalloc TestReZalloc
#define Free TestFree
#define CloseHandle TestClose
#define CreateEventW TestEvent
#define GetModuleHandleExW TestModule
#define CreateThreadpoolWork TestWork
#define CloseThreadpoolWork TestCloseWork
#define CreateNamedPipeW TestPipe
#define ConnectNamedPipe TestConnect
#define CreateFileW TestFile
#define InitializeProcThreadAttributeList TestInitializeAttributes
#define UpdateProcThreadAttribute TestUpdateAttributes
#define CreateProcessW TestProcess
#define RUN_DLL32_TIMEOUT 2000u
#ifndef PB_LEGACY_HELPER_SOURCE
#define PB_LEGACY_HELPER_SOURCE "../../api/rundll32.c"
#endif
#include PB_LEGACY_HELPER_SOURCE
#undef Alloc
#undef Zalloc
#undef AllocArray
#undef ReZalloc
#undef Free
#undef CloseHandle
#undef CreateEventW
#undef GetModuleHandleExW
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef CreateNamedPipeW
#undef ConnectNamedPipe
#undef CreateFileW
#undef InitializeProcThreadAttributeList
#undef UpdateProcThreadAttribute
#undef CreateProcessW
BOOL ResourceCreateTemporaryDirectory(LPWSTR directory)
{
    if (Failure()) return FALSE;
    if (swprintf_s(directory, MAX_PATH, L"%s\\helper-%lu-%ld", TestRoot, GetCurrentProcessId(), InterlockedIncrement(&Sequence)) < 0) return FALSE;
    return CreateDirectoryW(directory, NULL);
}
BOOL ResourceCopyToFile(LPCWSTR destination, LPCWSTR resource)
{
    (void)resource;
    if (Failure()) return FALSE;
    HANDLE file = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    return file != INVALID_HANDLE_VALUE && CloseHandle(file);
}
__declspec(dllexport) BOOL WINAPI Configure(LPCWSTR executable, LPCWSTR root, HANDLE detached)
{
    Detached = detached;
    return !wcscpy_s(ChildExecutable, _countof(ChildExecutable), executable) && !wcscpy_s(TestRoot, _countof(TestRoot), root);
}
__declspec(dllexport) BOOL WINAPI Acquire(VOID) { return MainRuntimeAcquire(); }
__declspec(dllexport) VOID WINAPI SetTestLogger(WINTUN_LOGGER_CALLBACK logger) { WintunSetLogger(logger); }
__declspec(dllexport) VOID WINAPI Release(VOID) { MainRuntimeRelease(); }
__declspec(dllexport) VOID WINAPI Retain(VOID) { MainRuntimeRetain(); }
__declspec(dllexport) BOOL WINAPI Touch(VOID)
{
    HANDLE token; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    CloseHandle(token);
    HDEVINFO devices = SetupDiCreateDeviceInfoList(NULL, NULL);
    return devices != INVALID_HANDLE_VALUE && SetupDiDestroyDeviceInfoList(devices);
}
__declspec(dllexport) DWORD WINAPI Loaded(VOID)
{
    const BYTE *base = (const BYTE *)ResourceModule;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const ImgDelayDescr *descriptor = (const ImgDelayDescr *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
    DWORD count = 0;
    for (; descriptor->rvaDLLName; ++descriptor) if (*(HMODULE *)(base + descriptor->rvaHmod)) ++count;
    return count;
}
__declspec(dllexport) BOOL WINAPI Execute(LPCWSTR function, LPCWSTR arguments, LPWSTR response, DWORD capacity)
{
    if (!MainRuntimeAcquire()) return FALSE;
    BOOL result = ExecuteRunDll32(function, arguments, response, capacity);
    MainRuntimeRelease(); return result;
}
__declspec(dllexport) VOID WINAPI SetFailure(LONG failure) { InterlockedExchange(&Acquisitions, 0); InterlockedExchange(&FailAt, failure); }
__declspec(dllexport) DWORD WINAPI Steps(VOID) { return (DWORD)InterlockedCompareExchange(&Acquisitions, 0, 0); }
__declspec(dllexport) DWORD WINAPI Owned(VOID)
{
    return (DWORD)(InterlockedCompareExchange(&OwnedAllocations, 0, 0) + InterlockedCompareExchange(&OwnedWorks, 0, 0) +
        InterlockedCompareExchange(&OwnedHandles, 0, 0) - (HelperIdle ? 2 : 0));
}
__declspec(dllexport) DWORD WINAPI Jobs(VOID)
{
    AcquireSRWLockShared(&HelperLock); DWORD count = HelperCount; ReleaseSRWLockShared(&HelperLock); return count;
}
__declspec(dllexport) VOID WINAPI Stop(VOID) { MainRuntimeBeginShutdown(); Rundll32RequestShutdown(); }
__declspec(dllexport) BOOL WINAPI Shutdown(DWORD timeout)
{
    ULONGLONG deadline = GetTickCount64() + timeout; MainRuntimeBeginShutdown();
    return Rundll32Shutdown(MainRemaining(deadline, timeout)) && MainRuntimeShutdown(MainRemaining(deadline, timeout));
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, PVOID reserved)
{
    BOOL result = ProductionDllMain(module, reason, reserved);
    if (reason == DLL_PROCESS_DETACH && !reserved) {
        assert(!OwnedHandles && !OwnedAllocations && !OwnedWorks);
        if (Detached) SetEvent(Detached);
    }
    return result;
}
