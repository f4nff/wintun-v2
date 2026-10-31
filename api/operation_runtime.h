/* SPDX-License-Identifier: GPL-2.0 */
/* Included by main.c and by the real-DLL ownership fixture through main.c. */
#pragma once
#ifndef MAIN_OPERATION_TIMEOUT
#define MAIN_OPERATION_TIMEOUT 120000u
#endif
#ifndef MAIN_OPERATION_MAX_JOBS
#define MAIN_OPERATION_MAX_JOBS 4u
#endif
typedef enum OPERATION_STATE { OperationPending, OperationReady, OperationClaimed, OperationAbandoned } OPERATION_STATE;
typedef struct MAIN_OPERATION {
    LONG References;
    SRWLOCK Lock;
    OPERATION_STATE State;
    ULONGLONG Deadline;
    MAIN_OPERATION_RUN Run;
    MAIN_OPERATION_DISPOSE Dispose;
    ULONG_PTR Result;
    DWORD Error;
    HANDLE Done, Decision;
    HMODULE Module;
    PTP_WORK Work;
    union { ULONGLONG Alignment; BYTE Bytes[4096]; } Input;
} MAIN_OPERATION;
static SRWLOCK OperationLock = SRWLOCK_INIT;
static HANDLE OperationIdle, OperationStop;
static PTP_POOL OperationPool;
static BOOL OperationStopping;
static DWORD OperationCount;
static __declspec(thread) MAIN_OPERATION *CurrentOperation;
static const BYTE OperationModuleAnchor = 0;

/* One total operation deadline also bounds nested mutex/event waits. The
 * explicit stop is used by background cleanup in addition to the runtime stop. */
static DWORD OperationWait(HANDLE object, HANDLE stop, DWORD timeout)
{
    DWORD remaining = CurrentOperation ? MainRemaining(CurrentOperation->Deadline, MAIN_OPERATION_TIMEOUT) : INFINITE;
    if (timeout > remaining) timeout = remaining;
    HANDLE events[3]; DWORD count = 0;
    AcquireSRWLockShared(&OperationLock); HANDLE runtimeStop = OperationStop; BOOL stopping = OperationStopping;
    ReleaseSRWLockShared(&OperationLock);
    if (stopping) { SetLastError(ERROR_CANCELLED); return WAIT_FAILED; }
    if (runtimeStop) events[count++] = runtimeStop;
    if (stop && stop != runtimeStop) events[count++] = stop;
    DWORD objectIndex = count;
    if (object) events[count++] = object;
    DWORD waited;
    if (count) waited = WaitForMultipleObjects(count, events, FALSE, timeout);
    else { Sleep(timeout); waited = WAIT_TIMEOUT; }
    if (object && (waited == WAIT_OBJECT_0 + objectIndex || waited == WAIT_ABANDONED_0 + objectIndex)) return WAIT_OBJECT_0;
    if (waited == WAIT_TIMEOUT) { SetLastError(ERROR_TIMEOUT); return WAIT_TIMEOUT; }
    if (waited != WAIT_FAILED) SetLastError(ERROR_CANCELLED);
    return WAIT_FAILED;
}
BOOL MainWaitForObject(HANDLE object, HANDLE stop, DWORD timeout)
{
    /* Unowned callers also have a finite installation wait. */
    if (!CurrentOperation && timeout == INFINITE && !stop) timeout = MAIN_OPERATION_TIMEOUT;
    return OperationWait(object, stop, timeout) == WAIT_OBJECT_0;
}
BOOL MainOperationSleep(DWORD delay)
{
    DWORD error = GetLastError();
    DWORD waited = OperationWait(NULL, NULL, delay);
    if (waited != WAIT_TIMEOUT) return FALSE;
    if (CurrentOperation && !MainRemaining(CurrentOperation->Deadline, MAIN_OPERATION_TIMEOUT)) return FALSE;
    SetLastError(error); return TRUE;
}
VOID MainOperationRequestShutdown(VOID)
{
    AcquireSRWLockExclusive(&OperationLock); OperationStopping = TRUE;
    if (OperationStop) SetEvent(OperationStop);
    ReleaseSRWLockExclusive(&OperationLock);
}
static VOID OperationUnreserve(VOID)
{
    AcquireSRWLockExclusive(&OperationLock); assert(OperationCount);
    if (!--OperationCount) SetEvent(OperationIdle);
    ReleaseSRWLockExclusive(&OperationLock);
}
static VOID OperationDrop(MAIN_OPERATION *job)
{
    if (InterlockedDecrement(&job->References)) return;
    if (job->Work) CloseThreadpoolWork(job->Work);
    if (job->Done) CloseHandle(job->Done);
    if (job->Decision) CloseHandle(job->Decision);
    HMODULE module = job->Module;
    HeapFree(GetProcessHeap(), 0, job);
    if (module) FreeLibrary(module); /* Construction failure, before submission. */
}
static VOID CALLBACK OperationWorker(PTP_CALLBACK_INSTANCE instance, PVOID opaque, PTP_WORK work)
{
    (void)work; MAIN_OPERATION *job = opaque;
    FreeLibraryWhenCallbackReturns(instance, job->Module); job->Module = NULL;
    CallbackMayRunLong(instance); CurrentOperation = job;
    ULONG_PTR result = 0;
    BOOL admitted = MainRuntimeAcquire(); DWORD error = admitted ? ERROR_SUCCESS : GetLastError();
    if (admitted) {
        if (MainOperationSleep(0)) { result = job->Run(job->Input.Bytes); error = result ? ERROR_SUCCESS : GetLastError(); }
        else error = GetLastError();
    }
    AcquireSRWLockExclusive(&job->Lock);
    if (job->State == OperationPending) {
        job->Result = result; job->Error = error ? error : result ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
        job->State = OperationReady; SetEvent(job->Done);
    }
    ReleaseSRWLockExclusive(&job->Lock);
    /* Keep the result on its owning thread until the caller claims it. A late
     * adapter is closed here, including potentially blocking callback barriers. */
    HANDLE decisions[] = { OperationStop, job->Decision };
    DWORD decision = WaitForMultipleObjects(2, decisions, FALSE, MainRemaining(job->Deadline, MAIN_OPERATION_TIMEOUT));
    if (decision == WAIT_TIMEOUT || decision == WAIT_FAILED) { MainRuntimeBeginShutdown(); Rundll32RequestShutdown(); }
    AcquireSRWLockExclusive(&job->Lock);
    BOOL claimed = job->State == OperationClaimed;
    if (!claimed) job->State = OperationAbandoned;
    ReleaseSRWLockExclusive(&job->Lock);
    if (!claimed && result && job->Dispose) job->Dispose(result);
    CurrentOperation = NULL;
    if (admitted) MainRuntimeRelease();
    OperationUnreserve(); OperationDrop(job);
}
ULONG_PTR MainRunOperation(MAIN_OPERATION_RUN run, MAIN_OPERATION_DISPOSE dispose, const VOID *input, SIZE_T size)
{
    ULONGLONG deadline = GetTickCount64() + MAIN_OPERATION_TIMEOUT;
    if (CurrentOperation) { SetLastError(ERROR_BUSY); return 0; }
    if (!run || size > sizeof(((MAIN_OPERATION *)0)->Input.Bytes) || (size && !input)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    DWORD error = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&OperationLock);
    if (OperationStopping) error = ERROR_SHUTDOWN_IN_PROGRESS;
    else if (OperationCount >= MAIN_OPERATION_MAX_JOBS) error = ERROR_BUSY;
    else if (!OperationIdle) {
        HANDLE idle = CreateEventW(NULL, TRUE, TRUE, NULL);
        HANDLE stop = idle ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
        PTP_POOL pool = stop ? CreateThreadpool(NULL) : NULL;
        BOOL initialized = FALSE;
        if (pool) { SetThreadpoolThreadMaximum(pool, MAIN_OPERATION_MAX_JOBS); initialized = SetThreadpoolThreadMinimum(pool, 0); }
        if (!initialized) {
            error = GetLastError(); if (!error) error = ERROR_NOT_ENOUGH_MEMORY;
            if (pool) CloseThreadpool(pool);
            if (idle) CloseHandle(idle); if (stop) CloseHandle(stop);
        } else { OperationIdle = idle; OperationStop = stop; OperationPool = pool; }
    }
    if (!error) { ++OperationCount; ResetEvent(OperationIdle); }
    ReleaseSRWLockExclusive(&OperationLock);
    if (error) { SetLastError(error); return 0; }
    MAIN_OPERATION *job = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*job));
    if (!job) { OperationUnreserve(); SetLastError(ERROR_OUTOFMEMORY); return 0; }
    job->References = 1; job->Deadline = deadline; job->Run = run; job->Dispose = dispose;
    if (size) memcpy(job->Input.Bytes, input, size);
    job->Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (job->Done) job->Decision = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (job->Done && job->Decision && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&OperationModuleAnchor, &job->Module)) {
        TP_CALLBACK_ENVIRON environment; InitializeThreadpoolEnvironment(&environment);
        SetThreadpoolCallbackPool(&environment, OperationPool);
        job->Work = CreateThreadpoolWork(OperationWorker, job, &environment);
        DestroyThreadpoolEnvironment(&environment);
    }
    if (!job->Done || !job->Decision || !job->Work) {
        error = GetLastError(); if (!error) error = ERROR_NOT_ENOUGH_MEMORY;
        OperationUnreserve(); OperationDrop(job); SetLastError(error); return 0;
    }
    InterlockedIncrement(&job->References); SubmitThreadpoolWork(job->Work);
    HANDLE events[] = { OperationStop, job->Done };
    DWORD waited = WaitForMultipleObjects(2, events, FALSE, MainRemaining(deadline, MAIN_OPERATION_TIMEOUT));
    DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    ULONG_PTR result = 0;
    AcquireSRWLockExclusive(&job->Lock);
    if (waited == WAIT_OBJECT_0 + 1 && job->State == OperationReady) {
        result = job->Result; error = job->Error; job->State = OperationClaimed;
    } else {
        error = waited == WAIT_OBJECT_0 ? ERROR_CANCELLED : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT :
            waited == WAIT_FAILED ? waitError : ERROR_CANCELLED;
        job->State = OperationAbandoned;
    }
    SetEvent(job->Decision); ReleaseSRWLockExclusive(&job->Lock);
    if (waited == WAIT_TIMEOUT || waited == WAIT_FAILED) {
        /* Late installation work may still mutate devices. Stop new control
         * admission until shutdown and reload, retaining every real owner. */
        MainRuntimeBeginShutdown(); Rundll32RequestShutdown();
    }
    OperationDrop(job); SetLastError(error); return result;
}
BOOL MainOperationShutdown(DWORD timeout)
{
    MainOperationRequestShutdown();
    AcquireSRWLockShared(&OperationLock); HANDLE idle = OperationIdle; ReleaseSRWLockShared(&OperationLock);
    if (!idle) return TRUE;
    DWORD result = WaitForSingleObject(idle, timeout);
    if (result == WAIT_OBJECT_0) return TRUE;
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return FALSE;
}
VOID MainOperationDetach(VOID)
{
    if (OperationPool) CloseThreadpool(OperationPool);
    if (OperationIdle) CloseHandle(OperationIdle);
    if (OperationStop) CloseHandle(OperationStop);
}
