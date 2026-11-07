/* SPDX-License-Identifier: GPL-2.0 */
/* Actual resource implementation, real CRT formatting and live OS handles.
 * Resource/file/directory APIs are hosted: no real file, Temp directory,
 * package, driver or device is created, deleted or installed. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shlwapi.h>
#include <NTSecAPI.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../../api/logger.h"
#include "../../api/resource.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static HRSRC WINAPI TestFind(HMODULE,LPCWSTR,LPCWSTR);
static DWORD WINAPI TestSize(HMODULE,HRSRC);
static HGLOBAL WINAPI TestLoad(HMODULE,HRSRC);
static LPVOID WINAPI TestLock(HGLOBAL);
static HANDLE WINAPI TestFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL WINAPI TestWrite(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static BOOL WINAPI TestClose(HANDLE);
static UINT WINAPI TestWindowsDirectory(LPWSTR,UINT);
static LPWSTR WINAPI TestCombine(LPWSTR,LPCWSTR,LPCWSTR);
static BOOLEAN WINAPI TestRandom(PVOID,ULONG);
static BOOL WINAPI TestDirectory(LPCWSTR,LPSECURITY_ATTRIBUTES);
#define FindResourceW TestFind
#define SizeofResource TestSize
#define LoadResource TestLoad
#define LockResource TestLock
#define CreateFileW TestFile
#define WriteFile TestWrite
#define CloseHandle TestClose
#define GetWindowsDirectoryW TestWindowsDirectory
#define PathCombineW TestCombine
#undef RtlGenRandom
#define RtlGenRandom TestRandom
#define CreateDirectoryW TestDirectory
#ifndef PB_RESOURCE_SOURCE
#define PB_RESOURCE_SOURCE "../../api/resource.c"
#endif
#include PB_RESOURCE_SOURCE
#undef FindResourceW
#undef SizeofResource
#undef LoadResource
#undef LockResource
#undef CreateFileW
#undef WriteFile
#undef CloseHandle
#undef GetWindowsDirectoryW
#undef PathCombineW
#undef RtlGenRandom
#undef CreateDirectoryW
HINSTANCE ResourceModule;
SECURITY_ATTRIBUTES SecurityAttributes={sizeof(SecurityAttributes),NULL,FALSE};
static const BYTE Payload[]={0x50,0x42,0x76,0x32,0,1,2,3,4,5,6,7,8,9,10,11};
static DWORD Stage,Error,ResourceId,LookupCalls,SizeCalls,LoadCalls,LockCalls,Logs,Scenarios;
static DWORD Opens,Closes,Writes,Written,CombineCalls,CombineFail,WindowsLength,DirCalls,DeleteCalls,ForeignDeletes;
static BOOL Numeric,SkipErrorFormatting,OwnedDirectory,ForeignDirectory;
static LPCWSTR ExpectedName;
static HANDLE File;
static WCHAR LastLog[1024],OwnedPath[MAX_PATH],CandidatePath[MAX_PATH];
static BOOL Fail(DWORD expected)
{if(Stage!=expected)return FALSE;SetLastError(Error);return TRUE;}
static VOID FormatLog(LPCWSTR format,va_list args)
{
    ++Logs;_vsnwprintf_s(LastLog,_countof(LastLog),_TRUNCATE,format,args);
    /* Represents a user logger that invokes another Win32 API. */
    SetLastError(ERROR_INVALID_DATA);
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD saved=GetLastError();FormatLog(format,args);SetLastError(saved);return saved;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{if(!SkipErrorFormatting)FormatLog(format,args);else {SkipErrorFormatting=FALSE;SetLastError(ERROR_INVALID_DATA);}return error;}
static HRSRC WINAPI TestFind(HMODULE module,LPCWSTR name,LPCWSTR type)
{
    CHECK(module==ResourceModule&&IS_INTRESOURCE(type)&&(ULONG_PTR)type==10);++LookupCalls;
    CHECK(Numeric?(IS_INTRESOURCE(name)&&(ULONG_PTR)name==ResourceId):(!IS_INTRESOURCE(name)&&!wcscmp(name,ExpectedName)));
    return Fail(1)?NULL:(HRSRC)(ULONG_PTR)0x11110000;
}
static DWORD WINAPI TestSize(HMODULE module,HRSRC resource)
{CHECK(module==ResourceModule&&resource==(HRSRC)(ULONG_PTR)0x11110000);++SizeCalls;return Fail(2)?0:sizeof(Payload);}
static HGLOBAL WINAPI TestLoad(HMODULE module,HRSRC resource)
{CHECK(module==ResourceModule&&resource==(HRSRC)(ULONG_PTR)0x11110000);++LoadCalls;return Fail(3)?NULL:(HGLOBAL)(ULONG_PTR)0x22220000;}
static LPVOID WINAPI TestLock(HGLOBAL resource)
{CHECK(resource==(HGLOBAL)(ULONG_PTR)0x22220000);++LockCalls;return Fail(4)?NULL:(LPVOID)Payload;}
static HANDLE WINAPI TestFile(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD attributes,HANDLE existing)
{
    CHECK(name&&!wcscmp(name,L"hosted-output.sys")&&access==GENERIC_WRITE&&!share&&security==&SecurityAttributes&&disposition==CREATE_NEW&&
        attributes==(FILE_ATTRIBUTE_NORMAL|FILE_ATTRIBUTE_TEMPORARY)&&!existing&&!File);
    if(Fail(5))return INVALID_HANDLE_VALUE;
    File=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(File);++Opens;return File;
}
static BOOL WINAPI TestWrite(HANDLE file,LPCVOID bytes,DWORD count,LPDWORD written,LPOVERLAPPED io)
{CHECK(file==File&&File&&bytes==Payload&&count==sizeof(Payload)&&!io);++Writes;if(Fail(6))return FALSE;*written=Written;return TRUE;}
static BOOL WINAPI TestClose(HANDLE file)
{CHECK(file==File&&File&&CloseHandle(file));File=NULL;++Closes;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static UINT WINAPI TestWindowsDirectory(LPWSTR directory,UINT count)
{
    CHECK(count==MAX_PATH);if(Fail(7))return 0;
    if(WindowsLength>=count)return WindowsLength;
    CHECK(WindowsLength>=3);directory[0]=L'C';directory[1]=L':';directory[2]=L'\\';
    for(UINT i=3;i<WindowsLength;++i)directory[i]=L'w';directory[WindowsLength]=0;return WindowsLength;
}
static LPWSTR WINAPI TestCombine(LPWSTR out,LPCWSTR root,LPCWSTR tail)
{++CombineCalls;if(CombineCalls==CombineFail){SetLastError(ERROR_INVALID_DATA);return NULL;}return PathCombineW(out,root,tail);}
static BOOLEAN WINAPI TestRandom(PVOID bytes,ULONG count)
{CHECK(count==32);if(Fail(8))return FALSE;memset(bytes,0xab,count);return TRUE;}
static BOOL WINAPI TestDirectory(LPCWSTR path,LPSECURITY_ATTRIBUTES security)
{
    CHECK(path&&security==&SecurityAttributes&&!OwnedDirectory);++DirCalls;wcscpy_s(CandidatePath,MAX_PATH,path);
    if(Fail(9)){ForeignDirectory=Error==ERROR_ALREADY_EXISTS;return FALSE;}
    OwnedDirectory=TRUE;wcscpy_s(OwnedPath,MAX_PATH,path);return TRUE;
}
/* Mirrors the helper's Directory[0] cleanup condition, with a hosted ownership
 * table. A candidate collision is foreign; never call real RemoveDirectoryW. */
static VOID CleanupPublishedDirectory(LPCWSTR path)
{
    if(!path[0])return;++DeleteCalls;
    if(!OwnedDirectory||wcscmp(path,OwnedPath)){++ForeignDeletes;return;}
    OwnedDirectory=FALSE;
}
static VOID Begin(VOID)
{
    CHECK(!File&&!OwnedDirectory&&Opens==Closes);
    ResourceModule=(HMODULE)(ULONG_PTR)0x33330000;Stage=0;Error=ERROR_NOT_ENOUGH_MEMORY;ResourceId=101;Numeric=TRUE;ExpectedName=NULL;
    LookupCalls=SizeCalls=LoadCalls=LockCalls=Logs=Opens=Closes=Writes=CombineCalls=CombineFail=DirCalls=DeleteCalls=ForeignDeletes=0;
    Written=sizeof(Payload);WindowsLength=12;SkipErrorFormatting=ForeignDirectory=FALSE;LastLog[0]=OwnedPath[0]=CandidatePath[0]=0;
}
static VOID End(VOID){CHECK(!File&&!OwnedDirectory&&Opens==Closes&&!ForeignDeletes);++Scenarios;}
static VOID Name(LPCWSTR input)
{
    if(IS_INTRESOURCE(input)){Numeric=TRUE;ResourceId=(DWORD)(ULONG_PTR)input;}
    else if(!wcscmp(input,L"packetbridge.sys")){Numeric=TRUE;ResourceId=101;}
    else if(!wcscmp(input,L"packetbridge.inf")){Numeric=TRUE;ResourceId=102;}
    else if(!wcscmp(input,L"packetbridge.cat")){Numeric=TRUE;ResourceId=103;}
    else {Numeric=FALSE;ExpectedName=input;}
}
static VOID LookupMatrix(VOID)
{
    const LPCWSTR names[]={MAKEINTRESOURCEW(0),MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(102),MAKEINTRESOURCEW(103),MAKEINTRESOURCEW(65535),
        L"packetbridge.sys",L"packetbridge.inf",L"packetbridge.cat",L"missing",L"PACKETBRIDGE.SYS",L"资源-测试"};
    const DWORD errors[]={0,ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_RESOURCE_NAME_NOT_FOUND};
    for(DWORD n=0;n<ARRAYSIZE(names);++n) {
        Begin();Name(names[n]);DWORD size=99;const VOID *bytes=ResourceGetAddress(names[n],&size);
        CHECK(bytes==Payload&&size==sizeof(Payload)&&!Logs&&LookupCalls==1&&SizeCalls==1&&LoadCalls==1&&LockCalls==1);End();
        for(DWORD stage=1;stage<=4;++stage)for(DWORD e=0;e<ARRAYSIZE(errors);++e) {
            Begin();Name(names[n]);Stage=stage;Error=errors[e];size=99;
            CHECK(!ResourceGetAddress(names[n],&size)&&!size&&GetLastError()==(stage==4?ERROR_LOCK_FAILED:Error?Error:ERROR_GEN_FAILURE)&&Logs==1);
            CHECK(LookupCalls==1&&SizeCalls==(stage>=2?1u:0u)&&LoadCalls==(stage>=3?1u:0u)&&LockCalls==(stage>=4?1u:0u));
            if(IS_INTRESOURCE(names[n]))CHECK(wcsstr(LastLog,L"#"));else CHECK(wcsstr(LastLog,names[n]));End();
        }
    }
}
static VOID CopyMatrix(VOID)
{
    const LPCWSTR names[]={MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(65535),L"packetbridge.sys",L"named-resource"};
    const DWORD errors[]={0,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_ALREADY_EXISTS};
    for(DWORD n=0;n<ARRAYSIZE(names);++n)for(DWORD stage=1;stage<=6;++stage)for(DWORD e=0;e<ARRAYSIZE(errors);++e) {
        Begin();Name(names[n]);Stage=stage;Error=errors[e];
        CHECK(!ResourceCopyToFile(L"hosted-output.sys",names[n])&&GetLastError()==(stage==4?ERROR_LOCK_FAILED:Error?Error:ERROR_GEN_FAILURE));
        CHECK(Opens==(stage==6?1u:0u)&&Closes==Opens&&Writes==(stage==6?1u:0u));End();
    }
    for(DWORD bytes=0;bytes<sizeof(Payload);++bytes) {
        Begin();Written=bytes;CHECK(!ResourceCopyToFile(L"hosted-output.sys",MAKEINTRESOURCEW(101))&&GetLastError()==ERROR_WRITE_FAULT&&Opens==1&&Closes==1);End();
    }
    Begin();CHECK(ResourceCopyToFile(L"hosted-output.sys",MAKEINTRESOURCEW(101))&&Opens==1&&Closes==1&&Writes==1);End();
}
static VOID DirectoryMatrix(VOID)
{
    const DWORD stages[]={7,8,9};const DWORD errors[]={0,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_ALREADY_EXISTS};
    WCHAR output[MAX_PATH];
    for(DWORD s=0;s<ARRAYSIZE(stages);++s)for(DWORD e=0;e<ARRAYSIZE(errors);++e) {
        Begin();Stage=stages[s];Error=errors[e];wcscpy_s(output,MAX_PATH,L"stale-caller-data");
        CHECK(!ResourceCreateTemporaryDirectory(output)&&!output[0]&&GetLastError()==(Stage==8?ERROR_GEN_FAILURE:Error?Error:ERROR_GEN_FAILURE));
        CleanupPublishedDirectory(output);CHECK(!DeleteCalls&&!OwnedDirectory);End();
    }
    for(DWORD combine=1;combine<=2;++combine) {
        Begin();CombineFail=combine;output[0]=L'x';CHECK(!ResourceCreateTemporaryDirectory(output)&&!output[0]&&GetLastError()==ERROR_BUFFER_OVERFLOW&&!DirCalls);End();
    }
    const DWORD lengths[]={MAX_PATH,MAX_PATH+1,MAXDWORD};
    for(DWORD i=0;i<ARRAYSIZE(lengths);++i) {
        Begin();WindowsLength=lengths[i];output[0]=L'x';CHECK(!ResourceCreateTemporaryDirectory(output)&&!output[0]&&GetLastError()==ERROR_INSUFFICIENT_BUFFER&&!CombineCalls&&!DirCalls);End();
    }
    for(DWORD length=189;length<=190;++length) {
        Begin();WindowsLength=length;output[0]=L'x';BOOL okay=ResourceCreateTemporaryDirectory(output);
        if(length==189){CHECK(okay&&wcslen(output)==259&&OwnedDirectory&&DirCalls==1);CleanupPublishedDirectory(output);}
        else CHECK(!okay&&!output[0]&&GetLastError()==ERROR_BUFFER_OVERFLOW&&!DirCalls);
        End();
    }
    Begin();CHECK(ResourceCreateTemporaryDirectory(output)&&OwnedDirectory&&DirCalls==1&&!wcscmp(output,OwnedPath)&&wcsstr(output,L"abababab"));
    CleanupPublishedDirectory(output);CHECK(DeleteCalls==1);End();
}
static VOID Control(DWORD mode)
{
    Begin();Stage=1;LPCWSTR name=mode==2?L"packetbridge.sys":MAKEINTRESOURCEW(101);DWORD size=99;
    if(mode==4) {
        WCHAR directory[MAX_PATH]={0};Stage=9;Error=ERROR_ALREADY_EXISTS;
        CHECK(!ResourceCreateTemporaryDirectory(directory));
        BOOL published=directory[0]!=0;CleanupPublishedDirectory(directory);
        if(published){CHECK(ForeignDirectory&&ForeignDeletes==1);fputs("UNOWNED_TEMP_CANDIDATE_PUBLISHED\n",stderr);ExitProcess(83);}
        End();return;
    }
    SkipErrorFormatting=mode==3;
    __try {if(mode==3)CHECK(!ResourceCopyToFile(L"hosted-output.sys",name));else CHECK(!ResourceGetAddress(name,&size));}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        CHECK(!File&&!Opens&&!OwnedDirectory);
        fputs(mode==3?"COPY_NUMERIC_RESOURCE_ID_FORMATTED_AS_POINTER\n":"NUMERIC_RESOURCE_ID_FORMATTED_AS_POINTER\n",stderr);ExitProcess(mode==3?82:81);
    }
    End();
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc<=2);
    if(argc==2)Control(wcstoul(argv[1],NULL,10));
    else {
        DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(DWORD pass=0;pass<3;++pass){LookupMatrix();CopyMatrix();DirectoryMatrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);}
    }
    printf("PASS: %lu actual resource failure/ownership scenarios; real CRT formats IDs and aliases, original errors preserved, live file handles balanced, failed directories never published or removed, no real filesystem/device changes.\n",Scenarios);return 0;
}
