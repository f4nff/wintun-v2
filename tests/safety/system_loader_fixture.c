/* SPDX-License-Identifier: GPL-2.0 */
/* Real shipping delay hook and DLL runtime; emulate only missing Win7 flags. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <delayimp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HMODULE WINAPI LegacySearch(LPCSTR,HANDLE,DWORD);
static FARPROC WINAPI LegacyExport(HMODULE,LPCSTR);
__declspec(noreturn) static VOID __cdecl LoadAbort(VOID);
#define LoadLibraryExA LegacySearch
#define GetProcAddress LegacyExport
#define abort LoadAbort
#ifdef PB_TEST_SETUP_HOST
#ifndef PB_SYSTEM_LOADER_HOST_SOURCE
#define PB_SYSTEM_LOADER_HOST_SOURCE "../../setupapihost/host.c"
#endif
#include PB_SYSTEM_LOADER_HOST_SOURCE
#else
#include "legacy_runtime_fixture.c"
#endif
#undef LoadLibraryExA
#undef GetProcAddress
#undef abort
#define LOADER_CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static BOOL Legacy;
static LONG Attempts,Unsupported;
static HMODULE WINAPI LegacySearch(LPCSTR name,HANDLE file,DWORD flags)
{
    InterlockedIncrement(&Attempts);
    if(Legacy && flags==LOAD_LIBRARY_SEARCH_SYSTEM32) {InterlockedIncrement(&Unsupported);SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
    return LoadLibraryExA(name,file,flags);
}
static FARPROC WINAPI LegacyExport(HMODULE module,LPCSTR name)
{
    if(Legacy && !strcmp(name,"AddDllDirectory")) {SetLastError(ERROR_PROC_NOT_FOUND);return NULL;}
    return GetProcAddress(module,name);
}
__declspec(noreturn) static VOID __cdecl LoadAbort(VOID)
{fputs("DOWNLEVEL_DELAYLOAD_ABORTED\n",stderr);ExitProcess(49);}
static DWORD WINAPI LoadWorker(VOID *unused)
{
    (void)unused;
    const CHAR *names[]={"version.dll","setupapi.dll","shlwapi.dll","shell32.dll"};
    WCHAR system[MAX_PATH];UINT length=GetSystemDirectoryW(system,_countof(system));LOADER_CHECK(length && length<_countof(system));
    for(DWORD i=0;i<16;++i)for(DWORD j=0;j<_countof(names);++j) {
        DelayLoadInfo info={0};info.szDll=names[j];
        HMODULE module=(HMODULE)DelayedLoadLibraryHook(dliNotePreLoadLibrary,&info);LOADER_CHECK(module);
        WCHAR path[MAX_PATH];DWORD n=GetModuleFileNameW(module,path,_countof(path));
        LOADER_CHECK(n>length && n<_countof(path) && !_wcsnicmp(path,system,length) && path[length]==L'\\');
        LOADER_CHECK(!GetProcAddress(module,"PoisonedSystemLibrary") && FreeLibrary(module));
    }
    return 0;
}
__declspec(dllexport) DWORD WINAPI ExerciseLoader(BOOL legacy)
{
    Legacy=legacy;Attempts=Unsupported=0;
    DelayLoadInfo info={0};LOADER_CHECK(!DelayedLoadLibraryHook(dliStartProcessing,&info));
#ifndef PB_TEST_SETUP_HOST
    LOADER_CHECK(MainRuntimeAcquire() && Touch());
#endif
    HANDLE workers[4];
    for(DWORD i=0;i<_countof(workers);++i){workers[i]=CreateThread(NULL,0,LoadWorker,NULL,0,NULL);LOADER_CHECK(workers[i]);}
    for(DWORD i=0;i<_countof(workers);++i)LOADER_CHECK(WaitForSingleObject(workers[i],10000)==WAIT_OBJECT_0 && CloseHandle(workers[i]));
#ifndef PB_TEST_SETUP_HOST
    MainRuntimeRelease();LOADER_CHECK(!Loaded() && !OwnedHandles && !OwnedAllocations && !OwnedWorks);
#endif
    LOADER_CHECK(Attempts>=256 && (legacy?Unsupported==Attempts:!Unsupported));return (DWORD)Attempts;
}
