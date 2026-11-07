/* SPDX-License-Identifier: GPL-2.0 */
/* Entire helper host with every SetupAPI device operation replaced. Parsing,
 * formatting, LocalFree and anonymous pipe I/O use actual Windows APIs. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <devguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <limits.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static LPWSTR WINAPI TestCommandLine(VOID);
static LPWSTR *WINAPI TestParse(LPCWSTR,int *);
static HLOCAL WINAPI TestFree(HLOCAL);
static DWORD WINAPI TestFormat(DWORD,LPCVOID,DWORD,DWORD,LPWSTR,DWORD,va_list *);
static HANDLE WINAPI TestStdHandle(DWORD);
static BOOL WINAPI TestWrite(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static HDEVINFO WINAPI TestCreate(const GUID *,HWND,PCWSTR,PVOID);
static BOOL WINAPI TestOpen(HDEVINFO,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestSet(HDEVINFO,PSP_DEVINFO_DATA,PSP_CLASSINSTALL_HEADER,DWORD);
static BOOL WINAPI TestCall(DI_FUNCTION,HDEVINFO,PSP_DEVINFO_DATA);
static BOOL WINAPI TestDestroy(HDEVINFO);
static volatile BOOL AllowUnexpectedDeviceCall;
static BOOL UnexpectedDeviceCall(VOID) {CHECK(AllowUnexpectedDeviceCall);return FALSE;}
#define GetCommandLineW TestCommandLine
#define CommandLineToArgvW TestParse
#define LocalFree TestFree
#define FormatMessageW TestFormat
#define GetStdHandle TestStdHandle
#define WriteFile TestWrite
#define SetupDiCreateDeviceInfoListExW TestCreate
#define SetupDiOpenDeviceInfoW TestOpen
#define SetupDiSetClassInstallParamsW TestSet
#define SetupDiCallClassInstaller TestCall
#define SetupDiDestroyDeviceInfoList TestDestroy
/* Win7-only creation export is included too, but never invoked by these tests.
 * Fail closed if it accidentally reaches any additional device operation. */
#define SetupDiCreateDeviceInfoW(...) UnexpectedDeviceCall()
#define SetupDiGetDeviceInstallParamsW(...) UnexpectedDeviceCall()
#define SetupDiSetDeviceInstallParamsW(...) UnexpectedDeviceCall()
#define SetupDiSetSelectedDevice(...) UnexpectedDeviceCall()
#define SetupDiSetDeviceRegistryPropertyW(...) UnexpectedDeviceCall()
#define SetupDiBuildDriverInfoList(...) UnexpectedDeviceCall()
#define SetupDiEnumDriverInfoW(a,b,c,d,e) ((VOID)(a),(VOID)(b),(VOID)(c),(VOID)(d),(VOID)(e),UnexpectedDeviceCall())
#define SetupDiSetSelectedDriverW(...) UnexpectedDeviceCall()
#define SetupDiGetDeviceInstanceIdW(a,b,c,d,e) ((VOID)(a),(VOID)(b),(VOID)(c),(VOID)(d),(VOID)(e),UnexpectedDeviceCall())
#define SetupDiDestroyDriverInfoList(...) ((VOID)UnexpectedDeviceCall())
#ifndef PB_HELPER_HOST_SOURCE
#define PB_HELPER_HOST_SOURCE "../../setupapihost/host.c"
#endif
#pragma warning(push)
#pragma warning(disable:4100) /* Required rundll32 ABI parameters are unused. */
#include PB_HELPER_HOST_SOURCE
#pragma warning(pop)
#undef GetCommandLineW
#undef CommandLineToArgvW
#undef LocalFree
#undef FormatMessageW
#undef GetStdHandle
#undef WriteFile
#undef SetupDiCreateDeviceInfoListExW
#undef SetupDiOpenDeviceInfoW
#undef SetupDiSetClassInstallParamsW
#undef SetupDiCallClassInstaller
#undef SetupDiDestroyDeviceInfoList
#undef SetupDiCreateDeviceInfoW
#undef SetupDiGetDeviceInstallParamsW
#undef SetupDiSetDeviceInstallParamsW
#undef SetupDiSetSelectedDevice
#undef SetupDiSetDeviceRegistryPropertyW
#undef SetupDiBuildDriverInfoList
#undef SetupDiEnumDriverInfoW
#undef SetupDiSetSelectedDriverW
#undef SetupDiGetDeviceInstanceIdW
#undef SetupDiDestroyDriverInfoList
static LPCWSTR CommandLine,Instance;
static LPWSTR *ArgsOwned;
static LPWSTR FormatOwned;
static HANDLE PipeRead,PipeWrite,Device;
static DWORD Which,Phase,ApiError,ParseError,ParseCalls,FormatCalls,Writes,Creates,Opens,Sets,Calls,Destroys,ArgFrees,FormatFrees,Scenarios;
static BOOL ParseFailure,WriteCount;
static int FailureCount;
static LPWSTR WINAPI TestCommandLine(VOID) {return (LPWSTR)CommandLine;}
static LPWSTR *WINAPI TestParse(LPCWSTR line,int *count)
{
    CHECK(line==CommandLine&&count&&!ArgsOwned);++ParseCalls;
    if(ParseFailure){if(WriteCount)*count=FailureCount;SetLastError(ParseError);return NULL;}
    ArgsOwned=CommandLineToArgvW(line,count);CHECK(ArgsOwned);return ArgsOwned;
}
static HLOCAL WINAPI TestFree(HLOCAL memory)
{
    if(memory){
        if(memory==(HLOCAL)ArgsOwned){ArgsOwned=NULL;++ArgFrees;}
        else{CHECK(memory==(HLOCAL)FormatOwned);FormatOwned=NULL;++FormatFrees;}
    }
    CHECK(!LocalFree(memory));SetLastError(ERROR_INVALID_HANDLE);return NULL;
}
static DWORD WINAPI TestFormat(DWORD flags,LPCVOID source,DWORD id,DWORD lang,LPWSTR output,DWORD size,va_list *args)
{
    CHECK(flags==(FORMAT_MESSAGE_FROM_STRING|FORMAT_MESSAGE_ALLOCATE_BUFFER)&&!id&&!lang&&output&&!size&&args&&!FormatOwned);
    CHECK(!wcscmp(source,L"%1!X!"));++FormatCalls;
    DWORD count=FormatMessageW(flags,source,id,lang,output,size,args);CHECK(count);FormatOwned=*(LPWSTR *)output;CHECK(FormatOwned);return count;
}
static HANDLE WINAPI TestStdHandle(DWORD which) {CHECK(which==STD_OUTPUT_HANDLE&&PipeWrite);return PipeWrite;}
static BOOL WINAPI TestWrite(HANDLE file,LPCVOID bytes,DWORD size,LPDWORD written,LPOVERLAPPED overlapped)
{
    CHECK(file==PipeWrite&&bytes==FormatOwned&&size&&size%sizeof(WCHAR)==0&&written&&!overlapped);++Writes;
    return WriteFile(file,bytes,size,written,overlapped);
}
static BOOL Fails(DWORD phase) {if(Phase!=phase)return FALSE;SetLastError(ApiError);return TRUE;}
static HDEVINFO WINAPI TestCreate(const GUID *guid,HWND parent,PCWSTR machine,PVOID reserved)
{
    CHECK(guid==&GUID_DEVCLASS_NET&&!parent&&!machine&&!reserved&&!Device);++Creates;
    if(Fails(1))return INVALID_HANDLE_VALUE;
    Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);return (HDEVINFO)Device;
}
static BOOL WINAPI TestOpen(HDEVINFO info,PCWSTR instance,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{
    CHECK(info==(HDEVINFO)Device&&Device&&!wcscmp(instance,Instance)&&!parent&&flags==DIOD_INHERIT_CLASSDRVS&&data->cbSize==sizeof(*data));++Opens;
    return !Fails(2);
}
static BOOL WINAPI TestSet(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_CLASSINSTALL_HEADER params,DWORD size)
{
    CHECK(info==(HDEVINFO)Device&&Device&&data->cbSize==sizeof(*data)&&params->cbSize==sizeof(*params));++Sets;
    if(!Which){SP_REMOVEDEVICE_PARAMS *p=(VOID *)params;CHECK(size==sizeof(*p)&&p->ClassInstallHeader.InstallFunction==DIF_REMOVE&&p->Scope==DI_REMOVEDEVICE_GLOBAL);}
    else{SP_PROPCHANGE_PARAMS *p=(VOID *)params;CHECK(size==sizeof(*p)&&p->ClassInstallHeader.InstallFunction==DIF_PROPERTYCHANGE&&p->Scope==DICS_FLAG_GLOBAL&&p->StateChange==(DWORD)(Which==1?DICS_ENABLE:DICS_DISABLE));}
    return !Fails(3);
}
static BOOL WINAPI TestCall(DI_FUNCTION action,HDEVINFO info,PSP_DEVINFO_DATA data)
{CHECK(info==(HDEVINFO)Device&&Device&&data->cbSize==sizeof(*data)&&action==(DI_FUNCTION)(Which?DIF_PROPERTYCHANGE:DIF_REMOVE));++Calls;return !Fails(4);}
static BOOL WINAPI TestDestroy(HDEVINFO info)
{CHECK(info==(HDEVINFO)Device&&Device&&CloseHandle(Device));Device=NULL;++Destroys;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static VOID Begin(DWORD which)
{
    CHECK(!ArgsOwned&&!FormatOwned&&!Device&&!PipeRead&&!PipeWrite);Which=which;
    CommandLine=L"rundll32.exe helper.dll,Entry ROOT\\PacketBridgeV2\\0000";Instance=L"ROOT\\PacketBridgeV2\\0000";
    Phase=0;ApiError=ERROR_ACCESS_DENIED;ParseError=ERROR_NOT_ENOUGH_MEMORY;ParseFailure=FALSE;WriteCount=TRUE;FailureCount=0;
    ParseCalls=FormatCalls=Writes=Creates=Opens=Sets=Calls=Destroys=ArgFrees=FormatFrees=0;
    CHECK(CreatePipe(&PipeRead,&PipeWrite,NULL,0));SetLastError(ERROR_SUCCESS);
}
static DWORD Invoke(VOID)
{
    if(Which==0)RemoveInstance(NULL,NULL,NULL,0);else if(Which==1)EnableInstance(NULL,NULL,NULL,0);else DisableInstance(NULL,NULL,NULL,0);
    CHECK(ParseCalls==1&&FormatCalls==1&&Writes==1&&!ArgsOwned&&!FormatOwned&&!Device&&FormatFrees==1);
    DWORD available,count;CHECK(PeekNamedPipe(PipeRead,NULL,0,NULL,&available,NULL)&&available&&available<32&&available%sizeof(WCHAR)==0);
    WCHAR text[16]={0};CHECK(ReadFile(PipeRead,text,available,&count,NULL)&&count==available);WCHAR *end;DWORD code=wcstoul(text,&end,16);CHECK(!*end);
    WCHAR expected[16];CHECK(swprintf_s(expected,_countof(expected),L"%lX",code)>0&&!wcscmp(text,expected));
    CHECK(PeekNamedPipe(PipeRead,NULL,0,NULL,&available,NULL)&&!available);return code;
}
static VOID End(VOID)
{
    CHECK(!ArgsOwned&&!FormatOwned&&!Device&&Destroys==(Creates&&!Phase?1u:Creates&&Phase!=1?1u:0u));
    CHECK(CloseHandle(PipeRead)&&CloseHandle(PipeWrite));PipeRead=PipeWrite=NULL;++Scenarios;
}
static VOID ArgumentFailures(VOID)
{
    const int counts[]={INT_MIN,-1,0,1,2,3,INT_MAX};
    const DWORD errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_OUTOFMEMORY,ERROR_ACCESS_DENIED,ERROR_SUCCESS,0xdeadbeef};
    for(DWORD which=0;which<3;++which)for(DWORD e=0;e<_countof(errors);++e){
        for(DWORD i=0;i<_countof(counts);++i){
            Begin(which);ParseFailure=TRUE;ParseError=errors[e];FailureCount=counts[i];
            CHECK(Invoke()==(ParseError?ParseError:ERROR_GEN_FAILURE)&&!Creates&&!Opens&&!Sets&&!Calls&&!ArgFrees);End();
        }
        Begin(which);ParseFailure=TRUE;ParseError=errors[e];WriteCount=FALSE;
        CHECK(Invoke()==(ParseError?ParseError:ERROR_GEN_FAILURE)&&!Creates&&!ArgFrees);End();
    }
    const LPCWSTR missing[]={L"",L"rundll32.exe",L"rundll32.exe helper.dll,Entry",L"\"path with spaces\" \"entry with spaces\"",L"rundll32.exe\t helper.dll,Entry"};
    for(DWORD which=0;which<3;++which)for(DWORD i=0;i<_countof(missing);++i){
        Begin(which);CommandLine=missing[i];CHECK(Invoke()==ERROR_INVALID_PARAMETER&&!Creates&&!Opens&&!Sets&&!Calls&&ArgFrees==1);End();
    }
}
static VOID SuccessfulArguments(VOID)
{
    const struct {LPCWSTR Line,Id;} cases[]={
        {L"rundll32.exe helper.dll,Entry ROOT\\PacketBridgeV2\\0000",L"ROOT\\PacketBridgeV2\\0000"},
        {L"\"C:\\Windows\\rundll32.exe\" \"C:\\helper path\\helper.dll,Entry\" \"ROOT\\PacketBridgeV2\\0000\"",L"ROOT\\PacketBridgeV2\\0000"},
        {L"rundll32.exe helper.dll,Entry \"id with spaces\"",L"id with spaces"},
        {L"rundll32.exe helper.dll,Entry \"ROOT\\\x4e2d\x6587\\\x03b1\"",L"ROOT\\\x4e2d\x6587\\\x03b1"},
        {L"rundll32.exe helper.dll,Entry \"\"",L""},
        {L"rundll32.exe helper.dll,Entry id extra \"ignored data\"",L"id"}
    };
    for(DWORD which=0;which<3;++which)for(DWORD i=0;i<_countof(cases);++i){
        Begin(which);CommandLine=cases[i].Line;Instance=cases[i].Id;
        CHECK(!Invoke()&&Creates==1&&Opens==1&&Sets==1&&Calls==1&&Destroys==1&&ArgFrees==1);End();
    }
    WCHAR line[5000],id[4096];for(DWORD i=0;i<_countof(id)-1;++i)id[i]=(WCHAR)(L'a'+i%26);id[_countof(id)-1]=0;
    CHECK(swprintf_s(line,_countof(line),L"rundll32.exe helper.dll,Entry \"%s\"",id)>0);
    for(DWORD which=0;which<3;++which){
        Begin(which);CommandLine=line;Instance=id;CHECK(!Invoke()&&Creates==1&&Opens==1&&Sets==1&&Calls==1&&Destroys==1&&ArgFrees==1);End();
    }
}
static VOID DeviceFailures(VOID)
{
    const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_PATH_NOT_FOUND,ERROR_INVALID_PARAMETER,0xe000020bu};
    for(DWORD which=0;which<3;++which)for(DWORD phase=1;phase<=4;++phase)for(DWORD e=0;e<_countof(errors);++e){
        Begin(which);Phase=phase;ApiError=errors[e];DWORD expected=!which&&phase==2&&ApiError==ERROR_PATH_NOT_FOUND?ERROR_SUCCESS:ApiError;
        CHECK(Invoke()==expected&&Creates==1&&Opens==(phase>=2?1u:0u)&&Sets==(phase>=3?1u:0u)&&Calls==(phase==4?1u:0u)&&ArgFrees==1);End();
    }
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);
    if(argc==2){
        DWORD kind=strtoul(argv[1],NULL,10);CHECK(kind==1||kind==2);BOOL failed=FALSE;
        for(DWORD which=0;which<3;++which){
            Begin(which);if(kind==1)ParseFailure=TRUE;else CommandLine=L"rundll32.exe helper.dll,Entry";
            DWORD response=Invoke();CHECK(!Creates&&!Opens&&!Sets&&!Calls&&ArgFrees==(kind==1?0u:1u));
            printf("TRACE: helper control=%lu handler=%lu response=%lX argument_frees=%lu format_frees=%lu\n",kind,which,response,ArgFrees,FormatFrees);
            if(response)CHECK(response==(DWORD)(kind==1?ERROR_NOT_ENOUGH_MEMORY:ERROR_INVALID_PARAMETER));End();if(!response)failed=TRUE;
        }
        if(failed){puts(kind==1?"HELPER_PARSE_FAILURE_REPORTED_SUCCESS":"HELPER_MISSING_ARGUMENT_REPORTED_SUCCESS");return 91+(int)kind;}
        puts("PASS: helper argument failure produces the expected nonzero HEX response");return 0;
    }
    DWORD initial,before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial));
    /* Warm the real Shell32/parser/formatter paths before measuring stable OS
     * handles. Every warm-up case still checks its exact resource ownership. */
    ArgumentFailures();SuccessfulArguments();DeviceFailures();CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    printf("TRACE: helper handles before_warm=%lu after_warm=%lu\n",initial,before);
    for(DWORD repeat=0;repeat<2;++repeat){
        ArgumentFailures();SuccessfulArguments();DeviceFailures();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
        printf("TRACE: helper handles measured_round=%lu baseline=%lu final=%lu\n",repeat,before,after);CHECK(before==after);
    }
    printf("PASS: %lu actual helper-host schedules; parser failure/untouched counts, missing and quoted/Unicode/long arguments, SDK failures, exact HEX responses, LocalFree ownership and zero handle delta\n",Scenarios);return 0;
}
