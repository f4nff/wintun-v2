/* SPDX-License-Identifier: GPL-2.0 */
/* Actual adapter metadata population. SetupAPI/registry boundaries are hosted;
 * GUID parsing, heap ownership, event handles and CRT formatting remain native.
 * No adapter, driver, file or real registry value is changed. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../api/adapter.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
#define HeapFree TestHeapFree
#include "../../api/logger.h"
static HKEY WINAPI TestOpen(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,DWORD,REGSAM);
static LSTATUS WINAPI TestRegClose(HKEY);
static HRESULT WINAPI TestParse(LPCOLESTR,LPCLSID);
#define SetupDiOpenDevRegKey TestOpen
#define RegCloseKey TestRegClose
#define CLSIDFromString TestParse
#ifndef PB_ADAPTER_DATA_SOURCE
#define PB_ADAPTER_DATA_SOURCE "../../api/adapter_data.h"
#endif
#include PB_ADAPTER_DATA_SOURCE
#undef HeapFree
#undef SetupDiOpenDevRegKey
#undef RegCloseKey
#undef CLSIDFromString

HANDLE ModuleHeap;
static HANDLE KeyHandle;
static LPWSTR ValuePointer,NamePointer;
static LPCWSTR ValueText;
static DWORD Phase,FailureError,ThreadError,LoggerErrorState,Luid,IfType;
static DWORD Opens,Closes,Strings,Parses,Dwords,Names,Allocations,Frees,Logs,Scenarios;
static BOOL Synthetic;
static HRESULT ParseResult;
static WCHAR LastLog[1024];
static const LPCWSTR ValidGuid=L"{12345678-90ab-cdef-1234-567890abcdef}";
static WINTUN_ADAPTER Adapter;
static VOID Begin(VOID)
{
    CHECK(!KeyHandle&&!ValuePointer&&!NamePointer);
    memset(&Adapter,0,sizeof(Adapter)); Adapter.DevInfo=(HDEVINFO)(ULONG_PTR)0x1111;
    Adapter.DevInfoData.cbSize=sizeof(Adapter.DevInfoData); Adapter.DevInfoData.DevInst=17;
    wcscpy_s(Adapter.DevInstanceID,_countof(Adapter.DevInstanceID),L"hosted-instance");
    Adapter.LuidIndex=0xaaaaaaaa; Adapter.IfType=0xbbbbbbbb; Adapter.IfIndex=0xcccccccc;
    memset(&Adapter.CfgInstanceID,0x55,sizeof(Adapter.CfgInstanceID));
    ValueText=ValidGuid; Phase=0; FailureError=ERROR_ACCESS_DENIED; ThreadError=ERROR_SUCCESS;
    LoggerErrorState=ERROR_INVALID_HANDLE; Luid=0x12345678; IfType=53; Synthetic=FALSE; ParseResult=S_OK;
    Opens=Closes=Strings=Parses=Dwords=Names=Allocations=Frees=Logs=0; LastLog[0]=0;
}
static VOID End(VOID)
{
    CHECK(!KeyHandle&&!ValuePointer&&!NamePointer&&Opens==Closes&&Allocations==Frees);
    CHECK(Adapter.DevInfo==(HDEVINFO)(ULONG_PTR)0x1111&&Adapter.DevInfoData.DevInst==17&&
        !wcscmp(Adapter.DevInstanceID,L"hosted-instance")&&Adapter.IfIndex==0xcccccccc);
    ++Scenarios;
}
static BOOL Fail(DWORD At)
{
    if (Phase!=At) return FALSE; SetLastError(FailureError); return TRUE;
}
static LPWSTR Copy(LPCWSTR Text)
{
    SIZE_T Count=wcslen(Text)+1; LPWSTR Result=HeapAlloc(ModuleHeap,0,Count*sizeof(WCHAR)); CHECK(Result);
    memcpy(Result,Text,Count*sizeof(WCHAR)); ++Allocations; return Result;
}
static HKEY WINAPI TestOpen(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Scope,DWORD Profile,DWORD Type,REGSAM Access)
{
    CHECK(Set==Adapter.DevInfo&&Info==&Adapter.DevInfoData&&Scope==DICS_FLAG_GLOBAL&&!Profile&&Type==DIREG_DRV&&Access==KEY_QUERY_VALUE&&!KeyHandle);
    if (Fail(1)) return INVALID_HANDLE_VALUE;
    KeyHandle=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(KeyHandle); ++Opens; return (HKEY)KeyHandle;
}
static LSTATUS WINAPI TestRegClose(HKEY Key)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle&&CloseHandle(KeyHandle)); KeyHandle=NULL; ++Closes;
    SetLastError(ERROR_INVALID_HANDLE); return ERROR_SUCCESS;
}
LPWSTR RegistryQueryString(HKEY Key,LPCWSTR Name,BOOL Log)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle&&!wcscmp(Name,L"NetCfgInstanceId")&&Log&&!ValuePointer); ++Strings;
    if (Fail(2)) return NULL;
    ValuePointer=Copy(ValueText); SetLastError(ThreadError); return ValuePointer;
}
static HRESULT WINAPI TestParse(LPCOLESTR Text,LPCLSID Guid)
{
    CHECK(Text==ValuePointer&&Guid==&Adapter.CfgInstanceID); ++Parses;
    if (Synthetic) { SetLastError(ThreadError); return ParseResult; }
    return CLSIDFromString(Text,Guid);
}
BOOL RegistryQueryDWORD(HKEY Key,LPCWSTR Name,DWORD *Value,BOOL Log)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle&&Log&&!ValuePointer); ++Dwords;
    CHECK((Dwords==1&&!wcscmp(Name,L"NetLuidIndex")&&Value==&Adapter.LuidIndex)||
        (Dwords==2&&!wcscmp(Name,L"*IfType")&&Value==&Adapter.IfType));
    if (Fail(Dwords==1?3:4)) return FALSE;
    *Value=Dwords==1?Luid:IfType; SetLastError(ThreadError); return TRUE;
}
LPWSTR AdapterGetDeviceObjectFileName(LPCWSTR Instance)
{
    CHECK(Instance==Adapter.DevInstanceID&&KeyHandle&&!ValuePointer&&!NamePointer&&Dwords==2); ++Names;
    if (Fail(5)) return NULL;
    NamePointer=Copy(L"hosted-interface"); SetLastError(ThreadError); return NamePointer;
}
static BOOL WINAPI TestHeapFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    CHECK(Heap==ModuleHeap&&!Flags&&Pointer);
    if (Pointer==ValuePointer) ValuePointer=NULL; else { CHECK(Pointer==NamePointer); NamePointer=NULL; }
    CHECK(HeapFree(Heap,Flags,Pointer)); ++Frees; SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static VOID FormatLog(LPCWSTR Format,va_list Args)
{
    ++Logs; _vsnwprintf_s(LastLog,_countof(LastLog),_TRUNCATE,Format,Args); SetLastError(LoggerErrorState);
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{
    (void)Level; DWORD Error=GetLastError(); FormatLog(Format,Args); SetLastError(Error); return Error;
}
DWORD LoggerErrorV(DWORD Error,LPCWSTR Format,va_list Args)
{ FormatLog(Format,Args); return Error; }
VOID LoggerGetRegistryKeyPath(HKEY Key,LPWSTR Path)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle); DWORD Error=GetLastError();
    wcscpy_s(Path,MAX_REG_PATH,L"<hosted-key>"); SetLastError(Error);
}
static DWORD ParseError(HRESULT Result)
{
    DWORD Error=HRESULT_FACILITY(Result)==FACILITY_WIN32?HRESULT_CODE(Result):ERROR_INVALID_DATA;
    return Error?Error:ERROR_INVALID_DATA;
}
static VOID SuccessfulCase(LPCWSTR Text,DWORD Index,DWORD Kind)
{
    Begin(); ValueText=Text; Luid=Index; IfType=Kind;
    GUID Expected; CHECK(SUCCEEDED(CLSIDFromString(Text,&Expected)));
    CHECK(PopulateAdapterData(&Adapter)); CHECK(!memcmp(&Adapter.CfgInstanceID,&Expected,sizeof(Expected)));
    CHECK(Adapter.LuidIndex==Index&&Adapter.IfType==Kind&&Adapter.InterfaceFilename==NamePointer&&
        !wcscmp(Adapter.InterfaceFilename,L"hosted-interface")&&Opens==1&&Closes==1&&Strings==1&&Parses==1&&Dwords==2&&Names==1&&!Logs);
    Free(Adapter.InterfaceFilename); Adapter.InterfaceFilename=NULL; End();
}
static VOID NativeMatrix(VOID)
{
    const LPCWSTR Good[]={L"{12345678-90ab-cdef-1234-567890abcdef}",L"{12345678-90AB-CDEF-1234-567890ABCDEF}",L"{00000000-0000-0000-0000-000000000000}"};
    const DWORD Values[]={0,1,MAXDWORD};
    for (DWORD text=0;text<_countof(Good);++text) for (DWORD value=0;value<_countof(Values);++value)
        SuccessfulCase(Good[text],Values[value],Values[_countof(Values)-value-1]);
    WCHAR Long[1600]; Long[0]=L'{'; for (DWORD i=1;i<_countof(Long)-1;++i) Long[i]=L'x'; Long[_countof(Long)-1]=0;
    const LPCWSTR Bad[]={L"{not-a-guid}",L"{",L"{0000}",L"{12345678-90ab-cdef-1234-567890abcdeg}",L"{\x4e2d\x6587}",Long};
    const DWORD Stale[]={ERROR_SUCCESS,ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,0xdeadbeef,MAXDWORD};
    for (DWORD text=0;text<_countof(Bad);++text) for (DWORD stale=0;stale<_countof(Stale);++stale)
    {
        Begin(); ValueText=Bad[text]; ThreadError=Stale[stale]; GUID Unused; HRESULT Expected=CLSIDFromString(ValueText,&Unused); CHECK(FAILED(Expected));
        CHECK(!PopulateAdapterData(&Adapter)&&GetLastError()==ParseError(Expected));
        CHECK(!Adapter.InterfaceFilename&&Adapter.LuidIndex==0xaaaaaaaa&&Adapter.IfType==0xbbbbbbbb&&
            Strings==1&&Parses==1&&!Dwords&&!Names&&Logs==1&&Allocations==1&&Frees==1); End();
    }
}
static VOID ContractMatrix(VOID)
{
    const HRESULT Results[]={CO_E_CLASSSTRING,E_INVALIDARG,E_FAIL,E_OUTOFMEMORY,HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
        HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),(HRESULT)0x80070000,(HRESULT)0x90012345};
    const DWORD Stale[]={ERROR_SUCCESS,ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,MAXDWORD};
    const DWORD LoggerErrors[]={ERROR_SUCCESS,ERROR_INVALID_HANDLE,MAXDWORD};
    for (DWORD result=0;result<_countof(Results);++result) for (DWORD stale=0;stale<_countof(Stale);++stale)
        for (DWORD logger=0;logger<_countof(LoggerErrors);++logger)
        {
            Begin(); Synthetic=TRUE; ParseResult=Results[result]; ThreadError=Stale[stale]; LoggerErrorState=LoggerErrors[logger];
            CHECK(!PopulateAdapterData(&Adapter)&&GetLastError()==ParseError(ParseResult));
            CHECK(!Adapter.InterfaceFilename&&!Dwords&&!Names&&Allocations==1&&Frees==1&&Logs==1); End();
        }
}
static VOID FailureMatrix(VOID)
{
    const DWORD Errors[]={ERROR_OUTOFMEMORY,ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_INVALID_DATA};
    for (DWORD phase=1;phase<=5;++phase) for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(); Phase=phase; FailureError=Errors[error];
        CHECK(!PopulateAdapterData(&Adapter)&&GetLastError()==FailureError&&!Adapter.InterfaceFilename&&Logs==1);
        CHECK(Strings==(phase==1?0u:1u)&&Parses==(phase<=2?0u:1u)&&Dwords==(phase<=2?0u:phase==3?1u:2u)&&Names==(phase==5?1u:0u));
        CHECK(Adapter.LuidIndex==(phase<=3?0xaaaaaaaa:Luid)&&Adapter.IfType==(phase<=4?0xbbbbbbbb:IfType)); End();
    }
}
int main(int argc,char **argv)
{
    CHECK(argc<=2); ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if (argc==2)
    {
        CHECK(!strcmp(argv[1],"1")); Begin(); ValueText=L"{not-a-guid}"; ThreadError=ERROR_SUCCESS;
        BOOL Result=PopulateAdapterData(&Adapter); DWORD Error=GetLastError();
        CHECK(!Adapter.InterfaceFilename&&!Dwords&&!Names&&Opens==1&&Closes==1&&Allocations==1&&Frees==1); End();
        if (Result) { CHECK(HeapDestroy(ModuleHeap)); puts("ADAPTER_POPULATED_WITH_INVALID_GUID"); return 89; }
        CHECK(Error==ERROR_INVALID_DATA);
    }
    else
    {
        DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for (DWORD repeat=0;repeat<3;++repeat) { NativeMatrix(); ContractMatrix(); FailureMatrix(); }
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    }
    CHECK(HeapDestroy(ModuleHeap)); printf("PASS: actual adapter metadata HRESULT/ownership/Win7 matrix (%lu scenarios)\n",Scenarios); return 0;
}
