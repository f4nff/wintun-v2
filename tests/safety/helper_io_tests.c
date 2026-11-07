/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <TlHelp32.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());return 1;}}while(0)
static DWORD Threads(VOID)
{
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return MAXDWORD;
    THREADENTRY32 entry={sizeof(entry)};DWORD count=0;
    if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID==GetCurrentProcessId()) {
        HANDLE thread=OpenThread(SYNCHRONIZE|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if(thread) {
            /* A joined thread can remain in the snapshot until kernel reaping.
             * Count live process threads, not already-signalled thread objects. */
            if(GetProcessIdOfThread(thread)==GetCurrentProcessId()&&WaitForSingleObject(thread,0)==WAIT_TIMEOUT)++count;
            CloseHandle(thread);
        } else if(GetLastError()!=ERROR_INVALID_PARAMETER) {CloseHandle(snapshot);return MAXDWORD;}
    }}while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);return count;
}
int wmain(int argc,wchar_t **argv)
{
    CHECK(argc>=2);DWORD total=0,handles=0,minThreads=MAXDWORD,maxThreads=0,joined=0,only=argc>2?wcstoul(argv[2],NULL,10):0;
    for(DWORD cycle=0;cycle<(only?1u:40u);++cycle) {
        HMODULE dll=LoadLibraryW(argv[1]);CHECK(dll);
        DWORD (WINAPI *run)(DWORD)=(DWORD(WINAPI *)(DWORD))GetProcAddress(dll,"RunIoCases");CHECK(run);
        DWORD (WINAPI *threadCount)(VOID)=(DWORD(WINAPI *)(VOID))GetProcAddress(dll,"JoinedIoThreads");CHECK(threadCount);
        DWORD cases=run(only);CHECK(cases);total+=cases;joined+=threadCount();CHECK(FreeLibrary(dll));
        CHECK(!GetModuleHandleW(argv[1]));
        DWORD current;CHECK(GetProcessHandleCount(GetCurrentProcess(),&current));DWORD live=Threads();CHECK(live!=MAXDWORD);
        minThreads=min(minThreads,live);maxThreads=max(maxThreads,live);
        if(!cycle)handles=current;else CHECK(current==handles);
    }
    /* Windows loader/threadpool workers can grow independently of DLL owners.
     * Record the live process range; assert every owned thread really joined. */
    printf("helper I/O: %lu schedules; %lu owned threads joined; allocation/handle residue=0; DLL fully unloaded; diagnostic process threads=%lu..%lu\n",total,joined,minThreads,maxThreads);return 0;
}
