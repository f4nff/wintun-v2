/* SPDX-License-Identifier: GPL-2.0 */
/* Actual helper setup; stop at its first path-combine boundary. No package,
 * files, pipes, process or device is created. Oversize API results deliberately
 * leave the output untouched, as an insufficient-buffer result may do. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shlwapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../../api/logger.h"
#include "../../api/resource.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static UINT WINAPI TestWindowsDirectory(LPWSTR,UINT);
static LPWSTR WINAPI TestCombine(LPWSTR,LPCWSTR,LPCWSTR);
#define GetWindowsDirectoryW TestWindowsDirectory
#define PathCombineW TestCombine
#ifndef PB_HELPER_SOURCE
#define PB_HELPER_SOURCE "../../api/rundll32_process.h"
#endif
#include PB_HELPER_SOURCE
#undef GetWindowsDirectoryW
#undef PathCombineW

HINSTANCE ResourceModule;
HANDLE ModuleHeap;
SECURITY_ATTRIBUTES SecurityAttributes={sizeof(SecurityAttributes),NULL,FALSE};
SECURITY_ATTRIBUTES PipeSecurityAttributes={sizeof(PipeSecurityAttributes),NULL,FALSE};
USHORT NativeMachine=IMAGE_FILE_MACHINE_AMD64;
static UINT ReturnedLength;
static DWORD NativeError,WindowsCalls,CombineCalls,Scenarios;
static BOOL BaselineControl;
static volatile LONG UnexpectedCalls,ContinueAfterPath;
VOID MainRuntimeRetain(VOID) { InterlockedIncrement(&UnexpectedCalls); }
VOID MainRuntimeRelease(VOID) { InterlockedIncrement(&UnexpectedCalls); }
VOID MainRuntimeBeginShutdown(VOID) { InterlockedIncrement(&UnexpectedCalls); }
BOOL ResourceCreateTemporaryDirectory(LPWSTR directory) { (void)directory; InterlockedIncrement(&UnexpectedCalls); return UnexpectedCalls<0; }
BOOL ResourceCopyToFile(LPCWSTR destination,LPCWSTR resource) { (void)destination; (void)resource; InterlockedIncrement(&UnexpectedCalls); return UnexpectedCalls<0; }
static BOOL CALLBACK NoLog(WINTUN_LOGGER_LEVEL level,DWORD64 timestamp,LPCWSTR line)
{ (void)level; (void)timestamp; (void)line; InterlockedIncrement(&UnexpectedCalls); return TRUE; }
WINTUN_LOGGER_CALLBACK Logger=NoLog;
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{ (void)level; (void)format; (void)args; InterlockedIncrement(&UnexpectedCalls); return GetLastError(); }
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{ (void)format; (void)args; InterlockedIncrement(&UnexpectedCalls); SetLastError(error); return error; }

static UINT WINAPI TestWindowsDirectory(LPWSTR Output,UINT Capacity)
{
    CHECK(Capacity==MAX_PATH); ++WindowsCalls;
    if (ReturnedLength&&ReturnedLength<Capacity) {
        CHECK(ReturnedLength>=3);
        Output[0]=L'C'; Output[1]=L':'; Output[2]=L'\\';
        for (UINT i=3;i<ReturnedLength;++i) Output[i]=L'w';
        Output[ReturnedLength]=0;
    }
    SetLastError(NativeError); return ReturnedLength;
}
static LPWSTR WINAPI TestCombine(LPWSTR Output,LPCWSTR Root,LPCWSTR Tail)
{
    ++CombineCalls;
    if (ReturnedLength>=MAX_PATH) {
        if (BaselineControl) { fputs("HELPER_COMBINED_TRUNCATED_WINDOWS_DIRECTORY\n",stderr); ExitProcess(88); }
        CHECK(FALSE);
    }
    CHECK(ReturnedLength&&wcsnlen(Root,MAX_PATH)==ReturnedLength);
    CHECK(!wcscmp(Tail,L"Sysnative\\rundll32.exe"));
    /* A controlled failure here isolates directory validation from extraction
     * and process submission. No invalid root is dereferenced in the control. */
    SetLastError(ERROR_BUFFER_OVERFLOW); return ContinueAfterPath?Output:NULL;
}
static VOID Run(UINT Length,DWORD Error)
{
    HELPER_JOB job={0}; ReturnedLength=Length; NativeError=Error;
    WindowsCalls=CombineCalls=0; SetLastError(ERROR_INVALID_DATA);
    CHECK(!HelperStart(&job)); DWORD Actual=GetLastError();
    CHECK(WindowsCalls==1&&CombineCalls==(Length&&Length<MAX_PATH?1u:0u));
    CHECK(Actual==(!Length?Error:Length>=MAX_PATH?ERROR_INSUFFICIENT_BUFFER:ERROR_BUFFER_OVERFLOW));
    CHECK(!job.Directory[0]&&!job.DllPath[0]&&!job.Process&&!job.CommandLine);
    CHECK(!UnexpectedCalls);
    for (DWORD i=0;i<2;++i) CHECK(!job.Streams[i].Read&&!job.Streams[i].Write&&!job.Streams[i].Io.hEvent);
    ++Scenarios;
}
int main(int argc,char **argv)
{
    CHECK(argc<=2); ModuleHeap=GetProcessHeap();
    if (argc==2) { CHECK(!strcmp(argv[1],"1")); BaselineControl=TRUE; Run(MAX_PATH+1,ERROR_SUCCESS); }
    else {
        DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        const UINT Lengths[]={0,3,12,MAX_PATH-1,MAX_PATH,MAX_PATH+1,MAXUINT};
        const DWORD Errors[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY};
        for (DWORD repeat=0;repeat<3;++repeat) for (DWORD n=0;n<_countof(Lengths);++n)
            for (DWORD e=0;e<_countof(Errors);++e) Run(Lengths[n],Errors[e]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    }
    printf("PASS: %lu actual helper-start directory-boundary scenarios; no extraction, I/O, process or device submission.\n",Scenarios); return 0;
}
