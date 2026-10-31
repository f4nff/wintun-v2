/* SPDX-License-Identifier: GPL-2.0 */
#include <Windows.h>
static BOOL FailSubmission;
static HANDLE Entered, Gate, Detached;
static BOOL WINAPI Submit(PTP_SIMPLE_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment)
{
    if (FailSubmission) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return TrySubmitThreadpoolCallback(callback, context, environment);
}
#define TrySubmitThreadpoolCallback Submit
#include "../../api/cleanup.h"
#undef TrySubmitThreadpoolCallback
static VOID CleanupRun(VOID)
{
    SetEvent(Entered);
    if (WaitForSingleObject(Gate, 5000) != WAIT_OBJECT_0) ExitProcess(30);
}
__declspec(dllexport) BOOL WINAPI Queue(HANDLE entered, HANDLE gate, HANDLE detached, BOOL fail)
{
    Entered = entered; Gate = gate; Detached = detached; FailSubmission = fail; return CleanupRequest();
}
__declspec(dllexport) BOOL WINAPI Shutdown(DWORD timeout) { return CleanupShutdown(timeout); }
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, PVOID reserved)
{
    (void)module;
    if (reason == DLL_PROCESS_DETACH && !reserved) { CleanupDetach(); if (Detached) SetEvent(Detached); }
    return TRUE;
}
