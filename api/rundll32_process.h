/* SPDX-License-Identifier: GPL-2.0 */
/* Included by rundll32.c. One admitted object owns the process, asynchronous pipe
 * reads, output buffers, temporary package and loader/dependency references.
 * The public waiter owns only a reference, never a worker's stack or I/O buffer. */
#pragma once
#include "../include/packetbridge_helper_protocol.h"
#include "termination_wait.h"
#include "overlapped_wait.h"
#include "event_pair.h"
#ifndef RUN_DLL32_MAX_JOBS
#define RUN_DLL32_MAX_JOBS 4u
#endif
#ifndef RUN_DLL32_TIMEOUT
#define RUN_DLL32_TIMEOUT 120000u
#endif
#define RUN_DLL32_OUTPUT_LIMIT (1024u * 1024u)
typedef struct _HELPER_STREAM {
    HANDLE Read, Write;
    OVERLAPPED Io;
    BOOL Pending, Closed, LowByteValid, DiscardLine;
    BYTE Bytes[4096], LowByte;
    WCHAR Line[1024];
    DWORD LineLength, Total;
} HELPER_STREAM;
typedef struct _HELPER_JOB {
    LONG References;
    HMODULE Module;
    PTP_WORK Work;
    HANDLE Done, Cancel, Process;
    HELPER_STREAM Streams[2];
    WCHAR *Function, *Arguments, *CommandLine;
    WCHAR Directory[MAX_PATH], DllPath[MAX_PATH];
    WCHAR Response[PB_HELPER_CREATE_RESPONSE_CAPACITY];
    DWORD ResponseCapacity, ResponseBytes, Error;
    struct _HELPER_JOB *Next;
} HELPER_JOB;
static const BYTE HelperModuleAnchor = 0;
static SRWLOCK HelperLock = SRWLOCK_INIT;
static HANDLE HelperIdle, HelperStop;
static BOOL HelperStopping;
static DWORD HelperCount;
static HELPER_JOB *HelperJobs;

static VOID HelperClose(HANDLE *Handle)
{
    if (*Handle && *Handle != INVALID_HANDLE_VALUE) CloseHandle(*Handle);
    *Handle = NULL;
}
static VOID HelperError(HELPER_JOB *Job, DWORD Error)
{
    if (!Job->Error) Job->Error = Error ? Error : ERROR_GEN_FAILURE;
}
static BOOL HelperCancelled(HELPER_JOB *Job)
{
    return WaitForSingleObject(Job->Cancel, 0) == WAIT_OBJECT_0;
}
static BOOL HelperReserve(VOID)
{
    DWORD error = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&HelperLock);
    if (HelperStopping) error = ERROR_SHUTDOWN_IN_PROGRESS;
    else if (HelperCount >= RUN_DLL32_MAX_JOBS) error = ERROR_BUSY;
    else {
        if (!HelperIdle) {
            HANDLE idle, stop;
            if (!CreateManualEventPair(TRUE, &idle, &stop)) error = GetLastError();
            else { HelperIdle = idle; HelperStop = stop; }
        }
        if (!error) { ++HelperCount; ResetEvent(HelperIdle); }
    }
    ReleaseSRWLockExclusive(&HelperLock); SetLastError(error); return !error;
}
static VOID HelperUnreserve(HELPER_JOB *Job)
{
    AcquireSRWLockExclusive(&HelperLock);
    HELPER_JOB **link = &HelperJobs;
    while (*link && *link != Job) link = &(*link)->Next;
    if (*link) *link = Job->Next;
    assert(HelperCount);
    if (!--HelperCount) SetEvent(HelperIdle);
    ReleaseSRWLockExclusive(&HelperLock);
}
VOID Rundll32RequestShutdown(VOID)
{
    AcquireSRWLockExclusive(&HelperLock);
    HelperStopping = TRUE;
    if (HelperStop) SetEvent(HelperStop);
    for (HELPER_JOB *job = HelperJobs; job; job = job->Next) SetEvent(job->Cancel);
    ReleaseSRWLockExclusive(&HelperLock);
}
BOOL Rundll32Shutdown(DWORD Timeout)
{
    Rundll32RequestShutdown();
    AcquireSRWLockShared(&HelperLock); HANDLE idle = HelperIdle; ReleaseSRWLockShared(&HelperLock);
    if (!idle) return TRUE;
    DWORD result = WaitForSingleObject(idle, Timeout);
    if (result == WAIT_OBJECT_0) return TRUE;
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return FALSE;
}
VOID Rundll32Detach(VOID)
{
    /* Only reached after the queued/running callbacks have dropped their pins. */
    HelperClose(&HelperIdle); HelperClose(&HelperStop);
}
static VOID HelperDrop(HELPER_JOB *Job)
{
    if (InterlockedDecrement(&Job->References)) return;
    if (Job->Work) CloseThreadpoolWork(Job->Work);
    HelperClose(&Job->Done); HelperClose(&Job->Cancel);
    Free(Job->Function); Free(Job->Arguments); Free(Job->CommandLine);
    HMODULE module = Job->Module; Free(Job);
    if (module) FreeLibrary(module); /* Unsubmitted construction failure only. */
}
static VOID HelperLogLine(HELPER_JOB *Job, HELPER_STREAM *Stream)
{
    Stream->Line[Stream->LineLength] = 0;
    if (Stream->LineLength && !Stream->DiscardLine && !HelperCancelled(Job)) {
        const WCHAR *position = Stream->Line + 3; DWORD64 timestamp = 0;
        WINTUN_LOGGER_LEVEL level;
        if (Stream->LineLength < 7 || Stream->Line[0] != L'[' || Stream->Line[2] != L' ') goto malformed;
        if (Stream->Line[1] == L'+') level = WINTUN_LOG_INFO;
        else if (Stream->Line[1] == L'-') level = WINTUN_LOG_WARN;
        else if (Stream->Line[1] == L'!') level = WINTUN_LOG_ERR;
        else goto malformed;
        if (*position < L'0' || *position > L'9') goto malformed;
        do {
            DWORD digit = *position++ - L'0';
            if (timestamp > (MAXUINT64 - digit) / 10) goto malformed;
            timestamp = timestamp * 10 + digit;
        } while (*position >= L'0' && *position <= L'9');
        if (position[0] != L']' || position[1] != L' ' || !position[2]) goto malformed;
        Logger(level, timestamp, position + 2);
        goto done;
malformed:
        HelperError(Job, ERROR_INVALID_DATA);
    }
done:
    Stream->LineLength = 0; Stream->DiscardLine = FALSE;
}
static VOID HelperConsume(HELPER_JOB *Job, HELPER_STREAM *Stream, DWORD Count)
{
    if (Count > RUN_DLL32_OUTPUT_LIMIT - Stream->Total) {
        Stream->Total = RUN_DLL32_OUTPUT_LIMIT; HelperError(Job, ERROR_BUFFER_OVERFLOW);
        SetEvent(Job->Cancel); return;
    }
    Stream->Total += Count;
    if (Stream == &Job->Streams[0]) {
        DWORD capacity = (Job->ResponseCapacity - 1) * sizeof(WCHAR);
        DWORD copied = min(Count, capacity - Job->ResponseBytes);
        memcpy((BYTE *)Job->Response + Job->ResponseBytes, Stream->Bytes, copied);
        Job->ResponseBytes += copied;
        if (copied != Count) HelperError(Job, ERROR_BUFFER_OVERFLOW);
        return;
    }
    for (DWORD i = 0; i < Count; ++i) {
        if (!Stream->LowByteValid) { Stream->LowByte = Stream->Bytes[i]; Stream->LowByteValid = TRUE; continue; }
        WCHAR character = (WCHAR)(Stream->LowByte | ((WORD)Stream->Bytes[i] << 8));
        Stream->LowByteValid = FALSE;
        if (character == L'\n' || !character) HelperLogLine(Job, Stream);
        else if (character != L'\r' && !Stream->DiscardLine) {
            if (Stream->LineLength == _countof(Stream->Line) - 1) {
                Stream->DiscardLine = TRUE; HelperError(Job, ERROR_BUFFER_OVERFLOW);
            } else Stream->Line[Stream->LineLength++] = character;
        }
    }
}
static VOID HelperReadResult(HELPER_JOB *Job, HELPER_STREAM *Stream, BOOL Okay, DWORD Count, DWORD Error)
{
    if (Okay && Count) HelperConsume(Job, Stream, Count);
    else {
        Stream->Closed = TRUE;
        if (!Okay && Error != ERROR_BROKEN_PIPE && Error != ERROR_OPERATION_ABORTED) HelperError(Job, Error);
    }
}
static VOID HelperPump(HELPER_JOB *Job, HELPER_STREAM *Stream)
{
    if (Stream->Closed) return;
    DWORD count = 0;
    if (Stream->Pending) {
        DWORD resetError;
        BOOL okay = PollOwnedIo(Stream->Read, &Stream->Io, &count, &resetError);
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (resetError) {
            HelperError(Job, resetError);
            Sleep(1000); return; /* Keep the live read and avoid a broken-event spin. */
        }
        if (!okay && error == ERROR_IO_INCOMPLETE) return;
        Stream->Pending = FALSE; HelperReadResult(Job, Stream, okay, count, error);
        return;
    }
    ResetEvent(Stream->Io.hEvent);
    BOOL okay = ReadFile(Stream->Read, Stream->Bytes, sizeof(Stream->Bytes), &count, &Stream->Io);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (!okay && error == ERROR_IO_PENDING) Stream->Pending = TRUE;
    else {
        HelperReadResult(Job, Stream, okay, count, error);
        /* Synchronous completions still get one finite pump turn each. */
        if (!Stream->Closed) SetEvent(Stream->Io.hEvent);
    }
}
static BOOL HelperAwaitIo(HELPER_JOB *Job, HELPER_STREAM *Stream, DWORD *Count)
{
    DWORD waitError;
    BOOL okay = WaitForOwnedIo(Stream->Read, &Stream->Io, Count, &waitError);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    Stream->Pending = FALSE; /* Only the terminal query retires this owner. */
    if (waitError) HelperError(Job, waitError);
    SetLastError(error); return okay;
}
static VOID HelperFinishStream(HELPER_JOB *Job, HELPER_STREAM *Stream, BOOL Drain)
{
    DWORD count = 0;
    if (Stream->Pending) {
        CancelIoEx(Stream->Read, &Stream->Io);
        BOOL okay = HelperAwaitIo(Job, Stream, &count);
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (Drain) HelperReadResult(Job, Stream, okay, count, error);
    }
    /* Own-process completion is the boundary, not EOF from every inherited copy.
     * Harvest bytes already written, then stop even if an unrelated child has a
     * duplicate writer. Each post-exit drain has an explicit byte/turn budget. */
    if (Drain) for (DWORD turn = 0; turn <= RUN_DLL32_OUTPUT_LIMIT / sizeof(Stream->Bytes); ++turn) {
        DWORD available = 0;
        if (!PeekNamedPipe(Stream->Read, NULL, 0, NULL, &available, NULL)) {
            DWORD error = GetLastError();
            HelperReadResult(Job, Stream, FALSE, 0, error); break;
        }
        if (!available) break;
        ResetEvent(Stream->Io.hEvent);
        BOOL okay = ReadFile(Stream->Read, Stream->Bytes, min(available, sizeof(Stream->Bytes)), &count, &Stream->Io);
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (!okay && error == ERROR_IO_PENDING) {
            Stream->Pending = TRUE;
            okay = HelperAwaitIo(Job, Stream, &count);
            error = okay ? ERROR_SUCCESS : GetLastError();
        }
        HelperReadResult(Job, Stream, okay, count, error);
        if (!okay || !count) break;
        if (turn == RUN_DLL32_OUTPUT_LIMIT / sizeof(Stream->Bytes)) HelperError(Job, ERROR_BUFFER_OVERFLOW);
    }
    if (Drain && Stream == &Job->Streams[1]) {
        if (Stream->LowByteValid) HelperError(Job, ERROR_INVALID_DATA);
        HelperLogLine(Job, Stream);
    }
    HelperClose(&Stream->Read); HelperClose(&Stream->Write); HelperClose(&Stream->Io.hEvent);
}
static BOOL HelperPipe(HELPER_JOB *Job, DWORD Index)
{
    HELPER_STREAM *stream = &Job->Streams[Index];
    WCHAR name[MAX_PATH];
    const WCHAR *suffix = wcsrchr(Job->Directory, L'\\');
    if (!suffix || swprintf_s(name, _countof(name), L"\\\\.\\pipe\\Wintun-%s-%lu", suffix + 1, Index) < 0) {
        SetLastError(ERROR_BUFFER_OVERFLOW); return FALSE;
    }
    stream->Io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stream->Io.hEvent) return FALSE;
    stream->Read = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &PipeSecurityAttributes);
    if (stream->Read == INVALID_HANDLE_VALUE) return FALSE;
    BOOL connected = ConnectNamedPipe(stream->Read, &stream->Io);
    DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED) return FALSE;
    stream->Pending = !connected && error == ERROR_IO_PENDING;
    SECURITY_ATTRIBUTES inherit = PipeSecurityAttributes; inherit.bInheritHandle = TRUE;
    stream->Write = CreateFileW(name, GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, 0, NULL);
    if (stream->Write == INVALID_HANDLE_VALUE) return FALSE;
    if (stream->Pending) {
        DWORD count;
        if (!HelperAwaitIo(Job, stream, &count)) return FALSE;
    }
    ResetEvent(stream->Io.hEvent); return TRUE;
}
static BOOL HelperStart(HELPER_JOB *Job)
{
    WCHAR windows[MAX_PATH], executable[MAX_PATH];
    UINT windowsLength = GetWindowsDirectoryW(windows, _countof(windows));
    if (!windowsLength) return FALSE;
    /* An insufficient-buffer result is a required capacity, not a valid path.
     * In particular, do not let PathCombine scan an untouched stack buffer. */
    if (windowsLength >= _countof(windows)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (!PathCombineW(executable, windows, L"Sysnative\\rundll32.exe") ||
        !ResourceCreateTemporaryDirectory(Job->Directory) ||
        !PathCombineW(Job->DllPath, Job->Directory, L"setupapihost.dll")) return FALSE;
    LPCWSTR resource;
    if (NativeMachine == IMAGE_FILE_MACHINE_AMD64) resource = L"setupapihost-amd64.dll";
    else if (NativeMachine == IMAGE_FILE_MACHINE_ARM64) resource = L"setupapihost-arm64.dll";
    else { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    if (!ResourceCopyToFile(Job->DllPath, resource)) return FALSE;
    size_t length = 16 + MAX_PATH + wcslen(Job->Arguments) + wcslen(Job->Function);
    Job->CommandLine = AllocArray(length, sizeof(WCHAR));
    if (!Job->CommandLine) return FALSE;
    if (_snwprintf_s(Job->CommandLine, length, _TRUNCATE, L"rundll32 \"%s\",%s %s", Job->DllPath, Job->Function, Job->Arguments) < 0) {
        SetLastError(ERROR_BUFFER_OVERFLOW); return FALSE;
    }
    if (!HelperPipe(Job, 0) || !HelperPipe(Job, 1)) return FALSE;
    SECURITY_ATTRIBUTES inherit = SecurityAttributes; inherit.bInheritHandle = TRUE;
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE) return FALSE;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = Alloc(bytes);
    if (!attributes) { HelperClose(&input); return FALSE; }
    BOOL initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes);
    HANDLE handles[] = { input, Job->Streams[0].Write, Job->Streams[1].Write };
    BOOL okay = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL);
    PROCESS_INFORMATION process = { 0 };
    STARTUPINFOEXW startup = { .StartupInfo = { .cb = sizeof(startup), .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
        .wShowWindow = SW_HIDE, .hStdInput = input, .hStdOutput = handles[1], .hStdError = handles[2] }, .lpAttributeList = attributes };
    if (okay && HelperCancelled(Job)) { SetLastError(ERROR_CANCELLED); okay = FALSE; }
    if (okay) okay = CreateProcessW(executable, Job->CommandLine, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        NULL, NULL, &startup.StartupInfo, &process);
    DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (initialized) DeleteProcThreadAttributeList(attributes);
    Free(attributes); HelperClose(&input);
    /* Never retain the parent's writer copies while waiting for process exit. */
    HelperClose(&Job->Streams[0].Write); HelperClose(&Job->Streams[1].Write);
    if (okay) { Job->Process = process.hProcess; HelperClose(&process.hThread); }
    SetLastError(error); return okay;
}
static VOID CALLBACK HelperWorker(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
    (void)Work; HELPER_JOB *job = Context;
    FreeLibraryWhenCallbackReturns(Instance, job->Module); job->Module = NULL;
    CallbackMayRunLong(Instance);
    BOOL exited = FALSE;
    if (HelperCancelled(job)) HelperError(job, ERROR_CANCELLED);
    else if (!HelperStart(job)) HelperError(job, GetLastError());
    else for (;;) {
        if (HelperCancelled(job)) { HelperError(job, ERROR_CANCELLED); break; }
        HelperPump(job, &job->Streams[0]); HelperPump(job, &job->Streams[1]);
        HANDLE events[4] = { job->Cancel, job->Process }; DWORD count = 2;
        for (DWORD i = 0; i < 2; ++i) if (!job->Streams[i].Closed) events[count++] = job->Streams[i].Io.hEvent;
        DWORD waited = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        if (waited == WAIT_OBJECT_0 + 1) { exited = TRUE; break; }
        if (waited == WAIT_FAILED) { HelperError(job, GetLastError()); break; }
    }
    for (DWORD i = 0; i < 2; ++i) HelperFinishStream(job, &job->Streams[i], exited && !HelperCancelled(job));
    /* Cancellation consumes no process ownership. A hung installer retains one
     * bounded slot, its package, dependencies and callback pin until actual exit. */
    if (job->Process) {
        DWORD waitError = WaitForOwnedTermination(job->Process);
        if (waitError) HelperError(job, waitError);
        DWORD code;
        if (!GetExitCodeProcess(job->Process, &code)) HelperError(job, GetLastError());
        else if (code) HelperError(job, ERROR_PROCESS_ABORTED);
        HelperClose(&job->Process);
    }
    if (!job->ResponseBytes || job->ResponseBytes % sizeof(WCHAR)) HelperError(job, ERROR_INVALID_DATA);
    job->Response[job->ResponseBytes / sizeof(WCHAR)] = 0;
    if (job->DllPath[0]) DeleteFileW(job->DllPath);
    if (job->Directory[0]) RemoveDirectoryW(job->Directory);
    MainRuntimeRelease();
    SetEvent(job->Done); HelperUnreserve(job); HelperDrop(job);
}
static BOOL ExecuteRunDll32(LPCWSTR Function, LPCWSTR Arguments, LPWSTR Response, DWORD ResponseCapacity)
{
    ULONGLONG deadline = GetTickCount64() + RUN_DLL32_TIMEOUT;
    if (!Response || !ResponseCapacity || ResponseCapacity > PB_HELPER_CREATE_RESPONSE_CAPACITY) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Response[0] = 0;
    if (!HelperReserve()) return FALSE;
    HELPER_JOB *job = Zalloc(sizeof(*job));
    if (!job) { DWORD error = GetLastError(); HelperUnreserve(NULL); SetLastError(error); return FALSE; }
    job->References = 1; job->ResponseCapacity = ResponseCapacity;
    if (!CreateManualEventPair(FALSE, &job->Done, &job->Cancel) ||
        !(job->Function = AllocArray(wcslen(Function) + 1, sizeof(WCHAR))) ||
        !(job->Arguments = AllocArray(wcslen(Arguments) + 1, sizeof(WCHAR))) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&HelperModuleAnchor, &job->Module) ||
        !(job->Work = CreateThreadpoolWork(HelperWorker, job, NULL))) {
        DWORD error = GetLastError(); HelperUnreserve(NULL); HelperDrop(job);
        SetLastError(error ? error : ERROR_GEN_FAILURE); return FALSE;
    }
    wcscpy_s(job->Function, wcslen(Function) + 1, Function);
    wcscpy_s(job->Arguments, wcslen(Arguments) + 1, Arguments);
    AcquireSRWLockExclusive(&HelperLock);
    if (HelperStopping) {
        ReleaseSRWLockExclusive(&HelperLock); HelperUnreserve(NULL); HelperDrop(job);
        SetLastError(ERROR_SHUTDOWN_IN_PROGRESS); return FALSE;
    }
    job->Next = HelperJobs; HelperJobs = job;
    MainRuntimeRetain(); InterlockedIncrement(&job->References);
    SubmitThreadpoolWork(job->Work);
    ReleaseSRWLockExclusive(&HelperLock);
    HANDLE events[] = { HelperStop, job->Done };
    DWORD waited = WaitForMultipleObjects(_countof(events), events, FALSE, MainRemaining(deadline, RUN_DLL32_TIMEOUT));
    DWORD error;
    if (waited == WAIT_OBJECT_0 + 1) {
        error = job->Error;
        if (!error) memcpy(Response, job->Response, (job->ResponseBytes / sizeof(WCHAR) + 1) * sizeof(WCHAR));
    } else {
        error = waited == WAIT_OBJECT_0 ? ERROR_CANCELLED : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        SetEvent(job->Cancel);
        if (waited != WAIT_OBJECT_0) {
            /* A timed-out installer may still mutate devices. Fail closed for
             * this instance rather than admit a new operation against late work.
             * Existing jobs keep their ownership; shutdown can still be retried. */
            MainRuntimeBeginShutdown(); Rundll32RequestShutdown();
        }
    }
    HelperDrop(job); SetLastError(error); return !error;
}
