/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
static DWORD WINAPI TerminalWait(HANDLE,DWORD);
static VOID WINAPI TerminalSleep(DWORD);
static BOOL WINAPI TerminalExitCode(HANDLE,DWORD *);
#define WaitForSingleObject TerminalWait
#define Sleep TerminalSleep
#define GetExitCodeProcess TerminalExitCode
#include "legacy_runtime_fixture.c"
#undef WaitForSingleObject
#undef Sleep
#undef GetExitCodeProcess
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static HANDLE RetryEntered,RetryGate,ChildRescue;
static volatile LONG TerminalFailures,TerminalBackoffs,TerminalFailureSeen;
static DWORD TerminalResult;
static DWORD WINAPI TerminalWait(HANDLE object,DWORD timeout)
{
    DWORD saved=GetLastError();
    if(timeout==INFINITE&&ReadAcquire(&TerminalFailures)>0&&GetProcessId(object)) {
        InterlockedDecrement(&TerminalFailures);InterlockedIncrement(&TerminalFailureSeen);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);return TerminalResult;
    }
    SetLastError(saved);return WaitForSingleObject(object,timeout);
}
static VOID WINAPI TerminalSleep(DWORD delay)
{
    if(delay==1000&&ReadAcquire(&TerminalFailureSeen)) {
        InterlockedIncrement(&TerminalBackoffs);CHECK(SetEvent(RetryEntered)&&WaitForSingleObject(RetryGate,10000)==WAIT_OBJECT_0);return;
    }
    Sleep(delay);
}
static BOOL WINAPI TerminalExitCode(HANDLE process,DWORD *code)
{
    if(ReadAcquire(&TerminalFailureSeen)&&WaitForSingleObject(process,0)==WAIT_TIMEOUT) {
        /* Negative-control rescue: never leave the test child orphaned. */
        CHECK(SetEvent(ChildRescue)&&WaitForSingleObject(process,5000)==WAIT_OBJECT_0);
        fputs("LIVE_PROCESS_RELEASE_AFTER_FAILED_WAIT\n",stderr);ExitProcess(45);
    }
    return GetExitCodeProcess(process,code);
}
__declspec(dllexport) VOID WINAPI ConfigureTerminal(HANDLE entered,HANDLE retry,HANDLE child,DWORD result)
{RetryEntered=entered;RetryGate=retry;ChildRescue=child;TerminalResult=result;TerminalFailures=2;TerminalBackoffs=TerminalFailureSeen=0;}
__declspec(dllexport) VOID WINAPI TerminalState(DWORD *values,WCHAR *directory)
{
    AcquireSRWLockShared(&HelperLock);HELPER_JOB *job=HelperJobs;
    values[0]=HelperCount;values[1]=job&&job->Process?GetProcessId(job->Process):0;
    if(job)CHECK(!wcscpy_s(directory,MAX_PATH,job->Directory));else directory[0]=0;
    ReleaseSRWLockShared(&HelperLock);
    values[2]=(DWORD)ReadAcquire(&TerminalBackoffs);
    AcquireSRWLockShared(&DependencyLock);values[3]=DependencyUsers;ReleaseSRWLockShared(&DependencyLock);
}
