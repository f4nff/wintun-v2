/* SPDX-License-Identifier: GPL-2.0 */
/* Compile the actual registry implementation with native heap operations and
 * CRT formatting. Registry reads are hosted: no real keys or devices change.
 * Heap wrappers poison uninitialized bytes, inject failures and track ownership. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static LPVOID WINAPI TestHeapAlloc(HANDLE,DWORD,SIZE_T);
static LPVOID WINAPI TestHeapReAlloc(HANDLE,DWORD,LPVOID,SIZE_T);
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
static LSTATUS WINAPI TestQuery(HKEY,LPCWSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
static DWORD WINAPI TestExpand(LPCWSTR,LPWSTR,DWORD);
#define HeapAlloc TestHeapAlloc
#define HeapReAlloc TestHeapReAlloc
#define HeapFree TestHeapFree
#include "../../api/logger.h"
#define RegQueryValueExW TestQuery
#define ExpandEnvironmentStringsW TestExpand
#ifndef PB_REGISTRY_SOURCE
#define PB_REGISTRY_SOURCE "../../api/registry.c"
#endif
#include PB_REGISTRY_SOURCE
#undef HeapAlloc
#undef HeapReAlloc
#undef HeapFree
#undef RegQueryValueExW
#undef ExpandEnvironmentStringsW

HANDLE ModuleHeap;
static struct { LPVOID Pointer; SIZE_T Bytes; } Blocks[16];
static DWORD Live,HeapCalls,FailHeapAt,Queries,QueryFailures,QueryError,ValueType,ValueBytes;
static DWORD Expands,FailExpandAt,Logs,Scenarios,Control;
static BYTE ValueData[2048];
static SIZE_T LastReallocSize;
static DWORD LastReallocFlags;
static WCHAR LastLog[1024];

static DWORD FindBlock(LPCVOID Pointer)
{
    for (DWORD i=0;i<_countof(Blocks);++i) if (Blocks[i].Pointer==Pointer) return i;
    CHECK(FALSE); return 0;
}
static BOOL HeapFailure(VOID)
{
    ++HeapCalls;
    if (HeapCalls!=FailHeapAt) return FALSE;
    SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static LPVOID WINAPI TestHeapAlloc(HANDLE Heap,DWORD Flags,SIZE_T Size)
{
    CHECK(Heap==ModuleHeap);
    if (HeapFailure()) return NULL;
    LPVOID Pointer=HeapAlloc(Heap,Flags,Size); CHECK(Pointer);
    if (!(Flags&HEAP_ZERO_MEMORY)) memset(Pointer,0x41,Size);
    DWORD i; for (i=0;i<_countof(Blocks)&&Blocks[i].Pointer;++i) {}
    CHECK(i<_countof(Blocks)); Blocks[i].Pointer=Pointer; Blocks[i].Bytes=Size; ++Live;
    return Pointer;
}
static LPVOID WINAPI TestHeapReAlloc(HANDLE Heap,DWORD Flags,LPVOID Pointer,SIZE_T Size)
{
    CHECK(Heap==ModuleHeap&&Pointer); DWORD i=FindBlock(Pointer);
    LastReallocFlags=Flags; LastReallocSize=Size;
    if (HeapFailure()) return NULL;
    LPVOID Result=HeapReAlloc(Heap,Flags,Pointer,Size); CHECK(Result);
    Blocks[i].Pointer=Result; Blocks[i].Bytes=Size; return Result;
}
static BOOL WINAPI TestHeapFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    CHECK(Heap==ModuleHeap&&Pointer); DWORD i=FindBlock(Pointer);
    CHECK(HeapFree(Heap,Flags,Pointer)); Blocks[i].Pointer=NULL; Blocks[i].Bytes=0; CHECK(Live); --Live;
    SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static VOID FormatLog(LPCWSTR Format,va_list Args)
{
    ++Logs; _vsnwprintf_s(LastLog,_countof(LastLog),_TRUNCATE,Format,Args);
    SetLastError(ERROR_INVALID_DATA);
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{
    (void)Level; DWORD Error=GetLastError(); FormatLog(Format,Args); SetLastError(Error); return Error;
}
DWORD LoggerErrorV(DWORD Error,LPCWSTR Format,va_list Args)
{
    FormatLog(Format,Args); return Error;
}
VOID LoggerGetRegistryKeyPath(HKEY Key,LPWSTR Path)
{
    (void)Key; DWORD Error=GetLastError(); wcscpy_s(Path,MAX_REG_PATH,L"<hosted-key>"); SetLastError(Error);
}
static LSTATUS WINAPI TestQuery(HKEY Key,LPCWSTR Name,LPDWORD Reserved,LPDWORD Type,LPBYTE Data,LPDWORD Bytes)
{
    CHECK(Key==(HKEY)(ULONG_PTR)0x1111&&!Reserved&&Type&&Data&&Bytes);
    CHECK(!Name||!wcscmp(Name,L"value")); ++Queries;
    if (QueryError&&Queries>QueryFailures) return (LSTATUS)QueryError;
    if (Queries<=QueryFailures||*Bytes<ValueBytes)
    {
        CHECK(ValueBytes>*Bytes); *Bytes=ValueBytes; return ERROR_MORE_DATA;
    }
    *Type=ValueType; memcpy(Data,ValueData,ValueBytes); *Bytes=ValueBytes;
    SetLastError(ERROR_INVALID_HANDLE); return ERROR_SUCCESS;
}
static DWORD WINAPI TestExpand(LPCWSTR Source,LPWSTR Destination,DWORD Length)
{
    DWORD i=FindBlock(Source); SIZE_T Capacity=Blocks[i].Bytes/sizeof(WCHAR);
    if (wcsnlen(Source,Capacity)>=Capacity)
    {
        if (Control==2)
        {
            for (DWORD block=0;block<_countof(Blocks);++block)
                if (Blocks[block].Pointer) Free(Blocks[block].Pointer);
            CHECK(!Live&&HeapDestroy(ModuleHeap));
            puts("REGISTRY_EXPAND_INPUT_NOT_TERMINATED"); fflush(stdout); ExitProcess(85);
        }
        CHECK(FALSE);
    }
    CHECK(Destination&&Blocks[FindBlock(Destination)].Bytes==(SIZE_T)Length*sizeof(WCHAR));
    ++Expands;
    if (Expands==FailExpandAt) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    return ExpandEnvironmentStringsW(Source,Destination,Length);
}
static VOID Begin(VOID)
{
    CHECK(!Live); HeapCalls=FailHeapAt=Queries=QueryFailures=QueryError=Expands=FailExpandAt=Logs=0;
    ValueType=REG_SZ; ValueBytes=0; LastReallocSize=0; LastReallocFlags=0; LastLog[0]=0;
    memset(ValueData,0x41,sizeof(ValueData)); SetLastError(ERROR_SUCCESS);
}
static VOID End(VOID) { CHECK(!Live); ++Scenarios; }
static LPWSTR Input(SIZE_T Capacity,DWORD Length,BOOL Terminated)
{
    CHECK(Capacity>=Length);
    LPWSTR Pointer=Alloc(Capacity*sizeof(WCHAR)); CHECK(Pointer);
    for (DWORD i=0;i<Length;++i) Pointer[i]=(WCHAR)(L'a'+i%26);
    if (Terminated) { CHECK(Length); Pointer[Length-1]=0; }
    return Pointer;
}
static VOID DirectCase(DWORD Length,SIZE_T Capacity,DWORD Type,BOOL Terminated)
{
    Begin(); LPWSTR Pointer=Input(Capacity,Length,Terminated); LPWSTR Before=Pointer;
    WCHAR Expected[1100]; CHECK(Length<_countof(Expected));
    for (DWORD i=0;i<Length;++i) Expected[i]=(WCHAR)(L'a'+i%26);
    Expected[Terminated?Length-1:Length]=0;
    CHECK(RegistryGetString(&Pointer,Length,Type)); CHECK(!wcscmp(Pointer,Expected));
    if (!Terminated)
        CHECK(LastReallocSize==((SIZE_T)Length+1)*sizeof(WCHAR)&&LastReallocFlags==HEAP_ZERO_MEMORY);
    else if (Type!=REG_EXPAND_SZ) CHECK(Pointer==Before&&!LastReallocSize);
    CHECK(Live==1); Free(Pointer); End();
}
static VOID DirectMatrix(VOID)
{
    const DWORD Lengths[]={0,1,2,3,255,256,257,1023};
    const DWORD Types[]={REG_SZ,REG_EXPAND_SZ,REG_MULTI_SZ};
    for (DWORD t=0;t<_countof(Types);++t) for (DWORD n=0;n<_countof(Lengths);++n)
    {
        DWORD Length=Lengths[n]; SIZE_T Capacities[]={Length,(SIZE_T)Length+1,(SIZE_T)Length+17,Length>256?Length:256};
        for (DWORD c=0;c<_countof(Capacities);++c)
        {
            DirectCase(Length,Capacities[c],Types[t],FALSE);
            if (Length) DirectCase(Length,Capacities[c],Types[t],TRUE);
        }
    }
    /* REG_MULTI_SZ deliberately retains only its first string. */
    Begin(); LPWSTR Multi=Input(32,7,FALSE); memcpy(Multi,L"ab\0cd\0\0",14);
    LPWSTR Before=Multi; CHECK(RegistryGetString(&Multi,7,REG_MULTI_SZ)&&Multi==Before&&!wcscmp(Multi,L"ab"));
    Free(Multi); End();
}
static VOID QueryCase(DWORD Bytes,DWORD Type,BOOL Terminated,BOOL NullName)
{
    Begin(); ValueBytes=Bytes; ValueType=Type; DWORD Length=Bytes/sizeof(WCHAR);
    for (DWORD i=0;i<Length;++i) ((WCHAR *)ValueData)[i]=(WCHAR)(L'a'+i%26);
    if (Terminated) { CHECK(Length); ((WCHAR *)ValueData)[Length-1]=0; }
    WCHAR Expected[1100]; CHECK(Length<_countof(Expected));
    for (DWORD i=0;i<Length;++i) Expected[i]=(WCHAR)(L'a'+i%26);
    Expected[Terminated?Length-1:Length]=0;
    LPWSTR Pointer=RegistryQueryString((HKEY)(ULONG_PTR)0x1111,NullName?NULL:L"value",TRUE);
    CHECK(Pointer&&!wcscmp(Pointer,Expected)&&Live==1&&Queries==(Bytes>512?2u:1u));
    Free(Pointer); End();
}
static VOID QueryMatrix(VOID)
{
    /* Odd byte sizes preserve the existing complete-WCHAR truncation policy. */
    const DWORD Sizes[]={0,1,2,3,5,6,7,510,511,512,513,514,701,2048};
    const DWORD Types[]={REG_SZ,REG_EXPAND_SZ,REG_MULTI_SZ};
    for (DWORD t=0;t<_countof(Types);++t) for (DWORD n=0;n<_countof(Sizes);++n)
        for (DWORD empty=0;empty<2;++empty)
        {
            QueryCase(Sizes[n],Types[t],FALSE,empty!=0);
            if (Sizes[n]>=2) QueryCase(Sizes[n],Types[t],TRUE,empty!=0);
        }
}
static VOID FailureMatrix(VOID)
{
    const DWORD Types[]={REG_SZ,REG_EXPAND_SZ,REG_MULTI_SZ};
    for (DWORD t=0;t<_countof(Types);++t) for (DWORD length=0;length<=3;++length)
    {
        Begin(); LPWSTR Pointer=Input(256,length,FALSE); LPWSTR Before=Pointer; FailHeapAt=2;
        CHECK(!RegistryGetString(&Pointer,length,Types[t])&&GetLastError()==ERROR_OUTOFMEMORY);
        CHECK(Pointer==Before&&Live==1&&Blocks[FindBlock(Pointer)].Bytes==512);
        if (length) CHECK(Pointer[0]==L'a'); Free(Pointer); End();
    }
    /* Query owns its input; direct sanitization retains the caller's pointer on
     * all failure paths. Allocation failure after repair must retain its new pointer. */
    for (DWORD direct=0;direct<2;++direct) for (DWORD fail=1;fail<=4;++fail)
    {
        if (direct&&fail==1) continue;
        Begin(); ValueBytes=6; ValueType=REG_EXPAND_SZ; memcpy(ValueData,L"abc",6);
        LPWSTR Pointer=NULL;
        if (direct) Pointer=Input(256,3,FALSE);
        FailHeapAt=fail;
        if (direct)
        {
            CHECK(!RegistryGetString(&Pointer,3,REG_EXPAND_SZ)&&GetLastError()==ERROR_OUTOFMEMORY);
            CHECK(Pointer&&Live==1&&Pointer[0]==L'a'); Free(Pointer);
        }
        else CHECK(!RegistryQueryString((HKEY)(ULONG_PTR)0x1111,L"value",TRUE)&&GetLastError()==ERROR_OUTOFMEMORY);
        End();
    }
    for (DWORD direct=0;direct<2;++direct) for (DWORD fail=1;fail<=2;++fail)
    {
        Begin(); ValueBytes=6; ValueType=REG_EXPAND_SZ; memcpy(ValueData,L"abc",6); FailExpandAt=fail;
        LPWSTR Pointer=NULL;
        if (direct)
        {
            Pointer=Input(256,3,FALSE);
            CHECK(!RegistryGetString(&Pointer,3,REG_EXPAND_SZ)&&GetLastError()==ERROR_ENVVAR_NOT_FOUND);
            CHECK(Pointer&&Live==1&&!wcscmp(Pointer,L"abc")); Free(Pointer);
        }
        else CHECK(!RegistryQueryString((HKEY)(ULONG_PTR)0x1111,L"value",TRUE)&&GetLastError()==ERROR_ENVVAR_NOT_FOUND);
        CHECK(Logs==1&&Expands==fail); End();
    }
    const DWORD Errors[]={ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_INVALID_HANDLE};
    for (DWORD e=0;e<_countof(Errors);++e) for (DWORD log=0;log<2;++log) for (DWORD retry=0;retry<2;++retry)
    {
        Begin(); QueryError=Errors[e]; QueryFailures=retry; ValueBytes=retry?701:6;
        CHECK(!RegistryQueryString((HKEY)(ULONG_PTR)0x1111,L"value",log)&&GetLastError()==Errors[e]);
        CHECK(Logs==log&&Queries==retry+1); End();
    }
    const DWORD InvalidTypes[]={REG_NONE,REG_BINARY,REG_DWORD,REG_QWORD,MAXDWORD};
    for (DWORD t=0;t<_countof(InvalidTypes);++t)
    {
        Begin(); ValueType=InvalidTypes[t]; ValueBytes=6;
        CHECK(!RegistryQueryString((HKEY)(ULONG_PTR)0x1111,L"value",TRUE)&&GetLastError()==ERROR_INVALID_DATATYPE&&Logs==1);
        End();
    }
}
static VOID ExpansionCases(VOID)
{
    const LPCWSTR Strings[]={L"",L"%SystemRoot%",L"%PATH%",L"%PB_REGISTRY_UNDEFINED_5DD90794%",L"%%",L"before%SystemRoot%after",L"\x4e2d\x6587"};
    for (DWORD i=0;i<_countof(Strings);++i) for (DWORD terminated=0;terminated<2;++terminated)
    {
        WCHAR Expected[32768]; DWORD Required=ExpandEnvironmentStringsW(Strings[i],Expected,_countof(Expected));
        CHECK(Required&&Required<=_countof(Expected));
        Begin(); DWORD Length=(DWORD)wcslen(Strings[i])+terminated;
        LPWSTR Pointer=Input(Length+256,Length,FALSE); memcpy(Pointer,Strings[i],Length*sizeof(WCHAR));
        CHECK(RegistryGetString(&Pointer,Length,REG_EXPAND_SZ)&&!wcscmp(Pointer,Expected)&&Live==1);
        Free(Pointer); End();
    }
}
static int NegativeControl(DWORD Which)
{
    Begin(); Control=Which; ValueType=Which==2?REG_EXPAND_SZ:REG_SZ; ValueBytes=6; memcpy(ValueData,L"abc",6);
    LPWSTR Pointer=RegistryQueryString((HKEY)(ULONG_PTR)0x1111,L"value",FALSE); CHECK(Pointer);
    BOOL Terminated=Pointer[3]==0;
    if (!Terminated&&Which==1)
    {
        printf("actual_bytes=6 repaired_bytes=%zu value[3]=0x%04x\n",HeapSize(ModuleHeap,0,Pointer),(unsigned int)Pointer[3]);
        Free(Pointer); End(); puts("REGISTRY_SHRINK_DID_NOT_TERMINATE_STRING"); return 84;
    }
    CHECK(!wcscmp(Pointer,L"abc")); Free(Pointer); End(); puts("PASS: registry string negative control is repaired"); return 0;
}
int main(int argc,char **argv)
{
    ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if (argc>1)
    {
        DWORD Which=(DWORD)strtoul(argv[1],NULL,10); CHECK(Which==1||Which==2);
        int Result=NegativeControl(Which); CHECK(HeapDestroy(ModuleHeap)); return Result;
    }
    DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
    for (DWORD repeat=0;repeat<3;++repeat) { DirectMatrix(); QueryMatrix(); FailureMatrix(); ExpansionCases(); }
    CHECK(!Live&&GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    CHECK(HeapDestroy(ModuleHeap)); printf("PASS: registry string repair/ownership/CRT/Win7 matrix (%lu scenarios)\n",Scenarios); return 0;
}
