/* SPDX-License-Identifier: GPL-2.0 */
/* Exact shared loader policy; guarded input and bounded Unicode path tests. */
#include "../safety/guard_memory.h"
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
static HMODULE WINAPI SearchA(LPCSTR,HANDLE,DWORD);
static HMODULE WINAPI SearchW(LPCWSTR,HANDLE,DWORD);
static HMODULE WINAPI Kernel(LPCWSTR);
static FARPROC WINAPI Export(HMODULE,LPCSTR);
static UINT WINAPI Directory(LPWSTR,UINT);
#define LoadLibraryExA SearchA
#define LoadLibraryExW SearchW
#define GetModuleHandleW Kernel
#define GetProcAddress Export
#define GetSystemDirectoryW Directory
#include "../../common/system_library.h"
#undef LoadLibraryExA
#undef LoadLibraryExW
#undef GetModuleHandleW
#undef GetProcAddress
#undef GetSystemDirectoryW
#define CHECK SAFETY_CHECK
static BOOL ModernOkay,Patched,KernelOkay,LegacyOkay;
static DWORD ModernError,LegacyError,DirectoryError;
static UINT ReportedDirectory,CallsA,CallsW,CallsKernel,CallsExport,CallsDirectory,Cases;
static WCHAR SystemPath[MAX_PATH],Expected[MAX_PATH];
static HMODULE WINAPI SearchA(LPCSTR name,HANDLE file,DWORD flags)
{CHECK(name && !file && flags==LOAD_LIBRARY_SEARCH_SYSTEM32);++CallsA;SetLastError(ModernError);return ModernOkay?(HMODULE)(ULONG_PTR)1:NULL;}
static HMODULE WINAPI SearchW(LPCWSTR name,HANDLE file,DWORD flags)
{CHECK(!file && flags==LOAD_WITH_ALTERED_SEARCH_PATH && !wcscmp(name,Expected));++CallsW;SetLastError(LegacyError);return LegacyOkay?(HMODULE)(ULONG_PTR)2:NULL;}
static HMODULE WINAPI Kernel(LPCWSTR name)
{CHECK(!wcscmp(name,L"kernel32.dll"));++CallsKernel;SetLastError(ERROR_BAD_ENVIRONMENT);return KernelOkay?(HMODULE)(ULONG_PTR)3:NULL;}
static FARPROC WINAPI Export(HMODULE kernel,LPCSTR name)
{CHECK(kernel==(HMODULE)(ULONG_PTR)3 && !strcmp(name,"AddDllDirectory"));++CallsExport;SetLastError(ERROR_PROC_NOT_FOUND);return Patched?(FARPROC)(ULONG_PTR)4:NULL;}
static UINT WINAPI Directory(LPWSTR path,UINT capacity)
{CHECK(capacity==MAX_PATH);++CallsDirectory;if(ReportedDirectory && ReportedDirectory<capacity)memcpy(path,SystemPath,(ReportedDirectory+1)*sizeof(*path));SetLastError(DirectoryError);return ReportedDirectory;}
static VOID Reset(VOID)
{
    ModernOkay=Patched=FALSE;KernelOkay=LegacyOkay=TRUE;
    ModernError=ERROR_INVALID_PARAMETER;LegacyError=ERROR_BAD_EXE_FORMAT;DirectoryError=ERROR_ACCESS_DENIED;
    CHECK(!wcscpy_s(SystemPath,MAX_PATH,L"C:\\Windows\\系統\\System32"));ReportedDirectory=(UINT)wcslen(SystemPath);
    CallsA=CallsW=CallsKernel=CallsExport=CallsDirectory=0;Expected[0]=0;
}
static HMODULE Run(LPCSTR name) {++Cases;return LoadSystemLibrary(name);}
static VOID ExpectPath(LPCSTR name)
{
    UINT d=ReportedDirectory;CHECK(d && d<MAX_PATH);memcpy(Expected,SystemPath,d*sizeof(WCHAR));
    if(Expected[d-1]!=L'\\')Expected[d++]=L'\\';
    size_t n=strlen(name);CHECK(d+n<MAX_PATH);for(size_t i=0;i<=n;++i)Expected[d+i]=(BYTE)name[i];
}
static VOID Capabilities(VOID)
{
    Reset();ModernOkay=TRUE;CHECK(Run("shell32.dll")== (HMODULE)(ULONG_PTR)1 && CallsA==1 && !CallsKernel && !CallsW && !CallsDirectory);
    Reset();ExpectPath("shell32.dll");CHECK(Run("shell32.dll")== (HMODULE)(ULONG_PTR)2 && CallsA==1 && CallsKernel==1 && CallsExport==1 && CallsDirectory==1 && CallsW==1);
    Reset();Patched=TRUE;CHECK(!Run("shell32.dll") && GetLastError()==ERROR_INVALID_PARAMETER && CallsA==1 && !CallsW && !CallsDirectory);
    Reset();KernelOkay=FALSE;CHECK(!Run("shell32.dll") && GetLastError()==ERROR_INVALID_PARAMETER && !CallsExport && !CallsDirectory && !CallsW);
    const DWORD errors[]={ERROR_MOD_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_BAD_EXE_FORMAT,ERROR_NOT_ENOUGH_MEMORY,ERROR_DLL_INIT_FAILED};
    for(UINT i=0;i<_countof(errors);++i){Reset();ModernError=errors[i];CHECK(!Run("shell32.dll") && GetLastError()==errors[i] && CallsA==1 && !CallsKernel && !CallsW);}
    for(UINT zero=0;zero<2;++zero){Reset();ReportedDirectory=0;DirectoryError=zero?0:ERROR_ACCESS_DENIED;CHECK(!Run("shell32.dll") && GetLastError()==(DWORD)(zero?ERROR_GEN_FAILURE:ERROR_ACCESS_DENIED) && !CallsW);}
    Reset();LegacyOkay=FALSE;ExpectPath("shell32.dll");CHECK(!Run("shell32.dll") && GetLastError()==LegacyError && CallsW==1);
}
static VOID Names(VOID)
{
    const CHAR *invalid[]={NULL,"",".","..",".dll","../a.dll","a/b.dll","a\\b.dll","C:a.dll","a.dll "," a.dll","a.dl","a.dlLx","a","a?.dll","a*.dll"};
    for(UINT i=0;i<_countof(invalid);++i){Reset();CHECK(!Run(invalid[i]) && GetLastError()==ERROR_INVALID_NAME && !CallsA && !CallsW);}
    for(UINT c=0;c<256;++c) {
        BOOL allowed=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
        Reset();ModernOkay=TRUE;CHAR name[]="abcd.dll";name[2]=(CHAR)c;
        if(allowed)CHECK(Run(name)==(HMODULE)(ULONG_PTR)1 && CallsA==1);
        else CHECK(!Run(name) && GetLastError()==ERROR_INVALID_NAME && !CallsA);
    }
    PB_TEST_GUARD guard=PbTestGuardCreate(MAX_PATH+1);
    for(UINT length=0;length<=MAX_PATH;++length) {
        CHAR *name=(CHAR *)PbTestGuardSpan(&guard,length+1);memset(name,'a',length);name[length]=0;
        if(length>=5)memcpy(name+length-4,".DLL",4);
        Reset();ModernOkay=TRUE;
        if(length>=5 && length<MAX_PATH)CHECK(Run(name)==(HMODULE)(ULONG_PTR)1 && CallsA==1);
        else CHECK(!Run(name) && GetLastError()==ERROR_INVALID_NAME && !CallsA);
        if(length>=5 && length<MAX_PATH) {
            Reset();BOOL fits=(SIZE_T)ReportedDirectory+1+length+1<=MAX_PATH;
            if(fits)ExpectPath(name);
            CHECK(fits?Run(name)==(HMODULE)(ULONG_PTR)2:(!Run(name)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER));CHECK(CallsW==(UINT)fits);
        }
    }
    CHAR *unterminated=(CHAR *)PbTestGuardSpan(&guard,MAX_PATH);memset(unterminated,'a',MAX_PATH);
    Reset();CHECK(!Run(unterminated) && GetLastError()==ERROR_INVALID_NAME && !CallsA);PbTestGuardDestroy(&guard);
    Reset();ModernOkay=TRUE;CHECK(Run("api-ms-win-test_v1.DlL")== (HMODULE)(ULONG_PTR)1);
    for(UINT bits=0;bits<8;++bits) {
        CHAR name[]="version.dll";for(UINT i=0;i<3;++i)if(bits&(1u<<i))name[8+i]-='a'-'A';
        Reset();ModernOkay=TRUE;CHECK(Run(name)==(HMODULE)(ULONG_PTR)1);
    }
}
static VOID PathBounds(VOID)
{
    for(UINT n=3;n<=MAX_PATH+2;++n)for(UINT slash=0;slash<2;++slash) {
        Reset();ReportedDirectory=n;
        if(n<MAX_PATH){for(UINT i=0;i<n;++i)SystemPath[i]=L'系';SystemPath[0]=L'C';SystemPath[1]=L':';SystemPath[2]=L'\\';if(slash)SystemPath[n-1]=L'\\';SystemPath[n]=0;}
        BOOL fits=n<MAX_PATH && (SIZE_T)n+(SystemPath[n-1]!=L'\\')+5+1<=MAX_PATH;
        if(fits)ExpectPath("a.dll");
        CHECK(fits?Run("a.dll")== (HMODULE)(ULONG_PTR)2:(!Run("a.dll") && GetLastError()==ERROR_INSUFFICIENT_BUFFER));CHECK(CallsW==(UINT)fits);
    }
}
int main(VOID)
{
    Capabilities();Names();PathBounds();
    printf("PASS: %u shared loader cases, guarded/nonterminated names, every byte/name/path boundary, Unicode system paths, exact legacy flag, capability-only fallback and original failures.\n",Cases);return 0;
}
