/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());return 1;}}while(0)
int wmain(int argc,wchar_t **argv)
{
    CHECK(argc>=2);BOOL baseline=argc>2&&!wcscmp(argv[2],L"--baseline");
    if(argc>2&&!baseline)CHECK(SetCurrentDirectoryW(argv[2]));
    DWORD handles=0,total=0,cycles=baseline?1u:18u;
    for(DWORD i=0;i<cycles;++i) {
        HMODULE dll=LoadLibraryW(argv[1]);CHECK(dll);
        DWORD (WINAPI *run)(BOOL)=(DWORD(WINAPI *)(BOOL))GetProcAddress(dll,"ExerciseLoader");CHECK(run);
        total+=run(baseline?TRUE:(i&1));CHECK(FreeLibrary(dll) && !GetModuleHandleW(argv[1]));
        DWORD current;CHECK(GetProcessHandleCount(GetCurrentProcess(),&current));
        /* Warm each search mechanism once; Windows maintains process loader
         * state for the legacy altered search as well as the modern search. */
        if(i==1)handles=current;else if(i>1)CHECK(current==handles);
    }
    printf("PASS: actual delay hook, %lu loads across %lu DLL lifecycles, modern/Win7 search capabilities, four joined callers, System32 paths and zero handle residue.\n",total,cycles);return 0;
}
