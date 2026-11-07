/* SPDX-License-Identifier: GPL-2.0 */
/* Actual host.c -> native FormatMessage/WriteFile -> anonymous pipe -> actual
 * HelperConsume -> actual CreateInstanceWin7ViaRundll32. Only ExecuteRunDll32's
 * asynchronous submission is replaced by a synchronous bridge. SetupAPI is
 * hosted: a 199-character ID is a legal API-boundary model, not evidence that
 * Win7's fixed PacketBridgeV2 ROOT generator produces that length. No devices,
 * registry keys, installer processes or DLL worker threads are created. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <delayimp.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <assert.h>
#include "../../api/adapter.h"
#include "../../api/logger.h"
#include "../../api/resource.h"
#include "../../common/system_library.h"
#include "../../include/packetbridge_helper_protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)

static BOOL TestExecuteRunDll32(LPCWSTR,LPCWSTR,LPWSTR,DWORD);
static HDEVINFO WINAPI TestCreate(const GUID *,HWND,PCWSTR,PVOID);
static BOOL WINAPI TestCreateDevice(HDEVINFO,PCWSTR,const GUID *,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestGetParams(HDEVINFO,PSP_DEVINFO_DATA,PSP_DEVINSTALL_PARAMS_W);
static BOOL WINAPI TestSetParams(HDEVINFO,PSP_DEVINFO_DATA,PSP_DEVINSTALL_PARAMS_W);
static BOOL WINAPI TestSelectDevice(HDEVINFO,PSP_DEVINFO_DATA);
static BOOL WINAPI TestSetHardware(HDEVINFO,PSP_DEVINFO_DATA,DWORD,const BYTE *,DWORD);
static BOOL WINAPI TestBuild(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestEnum(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,PSP_DRVINFO_DATA_W);
static BOOL WINAPI TestSelectDriver(HDEVINFO,PSP_DEVINFO_DATA,PSP_DRVINFO_DATA_W);
static BOOL WINAPI TestCall(DI_FUNCTION,HDEVINFO,PSP_DEVINFO_DATA);
static BOOL WINAPI TestGetId(HDEVINFO,PSP_DEVINFO_DATA,PWSTR,DWORD,PDWORD);
static BOOL WINAPI TestSetRemove(HDEVINFO,PSP_DEVINFO_DATA,PSP_CLASSINSTALL_HEADER,DWORD);
static BOOL WINAPI TestDestroyDrivers(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestDestroy(HDEVINFO);
static BOOL WINAPI TestOpen(HDEVINFO,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static LPWSTR *WINAPI TestArgv(LPCWSTR,int *);
static HLOCAL WINAPI TestLocalFree(HLOCAL);
static HANDLE WINAPI TestStdHandle(DWORD);
static DWORD WINAPI TestFormat(DWORD,LPCVOID,DWORD,DWORD,LPWSTR,DWORD,va_list *);
static BOOL WINAPI TestWrite(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
#define SetupDiCreateDeviceInfoListExW TestCreate
#define SetupDiCreateDeviceInfoW TestCreateDevice
#define SetupDiGetDeviceInstallParamsW TestGetParams
#define SetupDiSetDeviceInstallParamsW TestSetParams
#define SetupDiSetSelectedDevice TestSelectDevice
#define SetupDiSetDeviceRegistryPropertyW TestSetHardware
#define SetupDiBuildDriverInfoList TestBuild
#define SetupDiEnumDriverInfoW TestEnum
#define SetupDiSetSelectedDriverW TestSelectDriver
#define SetupDiCallClassInstaller TestCall
#define SetupDiGetDeviceInstanceIdW TestGetId
#define SetupDiSetClassInstallParamsW TestSetRemove
#define SetupDiDestroyDriverInfoList TestDestroyDrivers
#define SetupDiDestroyDeviceInfoList TestDestroy
#define SetupDiOpenDeviceInfoW TestOpen
#define CommandLineToArgvW TestArgv
#define LocalFree TestLocalFree
/* The builder redirects only rundll32.c's process include to a shim: that shim
 * includes the complete actual process header with its Execute function renamed,
 * then maps the caller to TestExecuteRunDll32. All other includes are unchanged. */
#ifndef PB_HELPER_RESPONSE_PARENT_SOURCE
#error Build through Build-HelperResponseTests to preserve the complete sources
#endif
#include PB_HELPER_RESPONSE_PARENT_SOURCE
#undef ExecuteRunDll32
#define GetStdHandle TestStdHandle
#define FormatMessageW TestFormat
#define WriteFile TestWrite
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_HELPER_RESPONSE_HOST_SOURCE
#pragma warning(pop)
#undef SetupDiCreateDeviceInfoListExW
#undef SetupDiCreateDeviceInfoW
#undef SetupDiGetDeviceInstallParamsW
#undef SetupDiSetDeviceInstallParamsW
#undef SetupDiSetSelectedDevice
#undef SetupDiSetDeviceRegistryPropertyW
#undef SetupDiBuildDriverInfoList
#undef SetupDiEnumDriverInfoW
#undef SetupDiSetSelectedDriverW
#undef SetupDiCallClassInstaller
#undef SetupDiGetDeviceInstanceIdW
#undef SetupDiSetClassInstallParamsW
#undef SetupDiDestroyDriverInfoList
#undef SetupDiDestroyDeviceInfoList
#undef SetupDiOpenDeviceInfoW
#undef CommandLineToArgvW
#undef LocalFree
#undef GetStdHandle
#undef FormatMessageW
#undef WriteFile

HINSTANCE ResourceModule;
HANDLE ModuleHeap;
SECURITY_ATTRIBUTES SecurityAttributes={sizeof(SecurityAttributes),NULL,FALSE};
SECURITY_ATTRIBUTES PipeSecurityAttributes={sizeof(PipeSecurityAttributes),NULL,FALSE};
USHORT NativeMachine=IMAGE_FILE_MACHINE_AMD64;
static HANDLE Device,OutputPipe;
static VOID *Drivers,*Locals[8];
static DWORD LocalCount,LocalAllocations,LocalFrees,DeviceCreates,DeviceCloses,DriverCreates,DriverFrees;
static DWORD Stage,InjectedError,Fragment,BridgeError,FrameChars,ReadBytes,Scenarios,Calls[19];
static DWORD WriteMode,CancelAt,Fragments,ParserCalls;
static DWORD ExpectedHandles;
static BOOL Registered,RemoveSet,ParserFailure,FormatFailure,BaselineControl;
static LPCWSTR ResultId,RawResponse;
static WCHAR Generated[MAX_DEVICE_ID_LEN];
static const WCHAR OrdinaryId[]=L"ROOT\\PacketBridgeV2\\0000";
static volatile LONG UnexpectedCalls;
static VOID Unexpected(VOID){InterlockedIncrement(&UnexpectedCalls);CHECK(!UnexpectedCalls);}
VOID MainRuntimeRetain(VOID){Unexpected();}
VOID MainRuntimeRelease(VOID){Unexpected();}
VOID MainRuntimeBeginShutdown(VOID){Unexpected();}
BOOL ResourceCreateTemporaryDirectory(LPWSTR output){(void)output;Unexpected();return FALSE;}
BOOL ResourceCopyToFile(LPCWSTR path,LPCWSTR resource){(void)path;(void)resource;Unexpected();return FALSE;}
static BOOL CALLBACK NoLog(WINTUN_LOGGER_LEVEL level,DWORD64 time,LPCWSTR text)
{(void)level;(void)time;CHECK(text);return TRUE;}
WINTUN_LOGGER_CALLBACK Logger=NoLog;
static DWORD RenderLog(LPCWSTR format,va_list args)
{WCHAR text[1024];CHECK(_vsnwprintf_s(text,_countof(text),_TRUNCATE,format,args)>=0);SetLastError(ERROR_BAD_ENVIRONMENT);return ERROR_BAD_ENVIRONMENT;}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD error=GetLastError();RenderLog(format,args);SetLastError(error);return error;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{RenderLog(format,args);return error;}
static BOOL Fails(DWORD stage)
{++Calls[stage];if(stage==Stage){SetLastError(InjectedError);return TRUE;}SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
static VOID CheckDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CHECK(Device&&info==(HDEVINFO)Device&&data&&data->cbSize==sizeof(*data)&&data->DevInst==37);}
static HDEVINFO WINAPI TestCreate(const GUID *guid,HWND parent,PCWSTR machine,PVOID reserved)
{CHECK(guid==&GUID_DEVCLASS_NET&&!parent&&!machine&&!reserved&&!Device);if(Fails(1))return INVALID_HANDLE_VALUE;Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);++DeviceCreates;return(HDEVINFO)Device;}
static BOOL WINAPI TestCreateDevice(HDEVINFO info,PCWSTR name,const GUID *guid,PCWSTR description,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{CHECK(info==(HDEVINFO)Device&&!wcscmp(name,PB_HARDWARE_ID)&&guid==&GUID_DEVCLASS_NET&&!description&&!parent&&flags==DICD_GENERATE_ID&&data&&data->cbSize==sizeof(*data));if(Fails(2))return FALSE;data->DevInst=37;return TRUE;}
static BOOL WINAPI TestGetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(params&&params->cbSize==sizeof(*params));if(Fails(3))return FALSE;params->Flags=0;return TRUE;}
static BOOL WINAPI TestSetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(params&&params->Flags==DI_QUIETINSTALL);return!Fails(4);}
static BOOL WINAPI TestSelectDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CheckDevice(info,data);return!Fails(5);}
static BOOL WINAPI TestSetHardware(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD property,const BYTE *bytes,DWORD count)
{CheckDevice(info,data);CHECK(property==SPDRP_HARDWAREID&&count==sizeof(PB_HARDWARE_ID)+sizeof(WCHAR)&&!wcscmp((LPCWSTR)bytes,PB_HARDWARE_ID)&&!((LPCWSTR)bytes)[_countof(PB_HARDWARE_ID)]);return!Fails(6);}
static BOOL WINAPI TestBuild(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&!Drivers);if(Fails(7))return FALSE;Drivers=HeapAlloc(ModuleHeap,0,16);CHECK(Drivers);++DriverCreates;return TRUE;}
static BOOL WINAPI TestEnum(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind,DWORD index,PSP_DRVINFO_DATA_W driver)
{CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&!index&&Drivers&&driver&&driver->cbSize==sizeof(*driver));if(Fails(8))return FALSE;driver->Reserved=0x1234;return TRUE;}
static BOOL WINAPI TestSelectDriver(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DRVINFO_DATA_W driver)
{CheckDevice(info,data);CHECK(Drivers&&driver&&driver->Reserved==0x1234);return!Fails(9);}
static BOOL WINAPI TestCall(DI_FUNCTION action,HDEVINFO info,PSP_DEVINFO_DATA data)
{CheckDevice(info,data);DWORD stage=action==DIF_REGISTERDEVICE?10:action==DIF_REGISTER_COINSTALLERS?11:action==DIF_INSTALLINTERFACES?12:action==DIF_INSTALLDEVICE?13:16;CHECK(stage!=16||(action==DIF_REMOVE&&Registered&&RemoveSet));if(Fails(stage))return FALSE;if(stage==10)Registered=TRUE;if(stage==16)Registered=FALSE;return TRUE;}
static BOOL WINAPI TestGetId(HDEVINFO info,PSP_DEVINFO_DATA data,PWSTR output,DWORD chars,PDWORD required)
{CheckDevice(info,data);CHECK(Registered&&output&&chars==MAX_DEVICE_ID_LEN&&required);if(Fails(14)){*required=MAX_DEVICE_ID_LEN+1;return FALSE;}*required=(DWORD)wcslen(ResultId)+1;CHECK(*required<=chars&&!wcscpy_s(output,chars,ResultId));return TRUE;}
static BOOL WINAPI TestSetRemove(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_CLASSINSTALL_HEADER header,DWORD size)
{CheckDevice(info,data);CHECK(Registered&&header&&size==sizeof(SP_REMOVEDEVICE_PARAMS)&&header->InstallFunction==DIF_REMOVE);if(Fails(15))return FALSE;RemoveSet=TRUE;return TRUE;}
static VOID FreeDrivers(VOID){CHECK(Drivers&&HeapFree(ModuleHeap,0,Drivers));Drivers=NULL;++DriverFrees;}
static BOOL WINAPI TestDestroyDrivers(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&Drivers);++Calls[17];FreeDrivers();SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static BOOL WINAPI TestDestroy(HDEVINFO info)
{CHECK(info==(HDEVINFO)Device&&Device);++Calls[18];if(Drivers)FreeDrivers();CHECK(CloseHandle(Device));Device=NULL;++DeviceCloses;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static BOOL WINAPI TestOpen(HDEVINFO info,PCWSTR id,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{(void)info;(void)id;(void)parent;(void)flags;(void)data;Unexpected();return FALSE;}
static VOID TrackLocal(VOID *value)
{if(value){CHECK(LocalCount<_countof(Locals));Locals[LocalCount++]=value;++LocalAllocations;}}
static LPWSTR *WINAPI TestArgv(LPCWSTR text,int *count)
{++ParserCalls;if(ParserFailure){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}LPWSTR *result=CommandLineToArgvW(text,count);TrackLocal(result);return result;}
static HLOCAL WINAPI TestLocalFree(HLOCAL value)
{if(value){DWORD i;for(i=0;i<LocalCount&&Locals[i]!=value;++i){}CHECK(i<LocalCount&&!LocalFree(value));Locals[i]=Locals[--LocalCount];++LocalFrees;}SetLastError(ERROR_INVALID_HANDLE);return NULL;}
static HANDLE WINAPI TestStdHandle(DWORD stream){CHECK(stream==STD_OUTPUT_HANDLE&&OutputPipe);return OutputPipe;}
static DWORD WINAPI TestFormat(DWORD flags,LPCVOID source,DWORD id,DWORD language,LPWSTR output,DWORD size,va_list *args)
{CHECK(flags==(FORMAT_MESSAGE_FROM_STRING|FORMAT_MESSAGE_ALLOCATE_BUFFER)&&!wcscmp(source,PB_HELPER_CREATE_RESPONSE_FORMAT));if(FormatFailure){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}DWORD result=FormatMessageW(flags,source,id,language,output,size,args);CHECK(result);TrackLocal(*(LPWSTR *)output);FrameChars=result;return result;}
static BOOL WINAPI TestWrite(HANDLE file,LPCVOID bytes,DWORD count,LPDWORD written,LPOVERLAPPED io)
{CHECK(file==OutputPipe&&!io);if(WriteMode==1){*written=0;SetLastError(ERROR_WRITE_FAULT);return FALSE;}if(WriteMode==2&&count)count=1;return WriteFile(file,bytes,count,written,io);}

static BOOL TestExecuteRunDll32(LPCWSTR function,LPCWSTR arguments,LPWSTR response,DWORD capacity)
{
    CHECK(!wcscmp(function,L"CreateInstanceWin7")&&!arguments[0]&&response&&capacity&&capacity<=_countof(((HELPER_JOB *)0)->Response));
    response[0]=0;HELPER_JOB job={0};job.ResponseCapacity=capacity;
    job.Cancel=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(job.Cancel);
    HANDLE input;CHECK(CreatePipe(&input,&OutputPipe,NULL,4096));
    if(RawResponse){DWORD bytes=(DWORD)wcslen(RawResponse)*sizeof(WCHAR),written;CHECK(WriteFile(OutputPipe,RawResponse,bytes,&written,NULL)&&written==bytes);FrameChars=bytes/sizeof(WCHAR);}
    else CreateInstanceWin7(NULL,NULL,NULL,0);
    CHECK(CloseHandle(OutputPipe));OutputPipe=NULL;
    if(CancelAt==1)CHECK(SetEvent(job.Cancel));
    for(;;){DWORD count=0;BOOL okay=ReadFile(input,job.Streams[0].Bytes,Fragment,&count,NULL);if(!okay){CHECK(GetLastError()==ERROR_BROKEN_PIPE);break;}CHECK(count);++Fragments;ReadBytes+=count;
        if(CancelAt==2)CHECK(SetEvent(job.Cancel));
        if(HelperCancelled(&job))HelperError(&job,ERROR_CANCELLED);else HelperConsume(&job,&job.Streams[0],count);
    }
    CHECK(CloseHandle(input));
    /* Same terminal byte/NUL boundary as HelperWorker; this synchronous bridge
     * does not model its scheduling, process wait, module pin or shutdown. */
    if(!job.ResponseBytes||job.ResponseBytes%sizeof(WCHAR))HelperError(&job,ERROR_INVALID_DATA);
    job.Response[job.ResponseBytes/sizeof(WCHAR)]=0;
    BridgeError=job.Error;if(!job.Error)memcpy(response,job.Response,(job.ResponseBytes/sizeof(WCHAR)+1)*sizeof(WCHAR));
    CHECK(CloseHandle(job.Cancel));SetLastError(job.Error);return!job.Error;
}
static VOID Begin(VOID)
{
    CHECK(!Device&&!Drivers&&!OutputPipe&&!LocalCount);Stage=WriteMode=CancelAt=FrameChars=ReadBytes=BridgeError=Fragments=ParserCalls=0;
    DeviceCreates=DeviceCloses=DriverCreates=DriverFrees=LocalAllocations=LocalFrees=0;
    memset(Calls,0,sizeof(Calls));InjectedError=ERROR_ACCESS_DENIED;Fragment=4096;Registered=RemoveSet=ParserFailure=FormatFailure=FALSE;ResultId=OrdinaryId;RawResponse=NULL;
}
static VOID End(VOID)
{CHECK(!Device&&!Drivers&&!OutputPipe&&!LocalCount&&DeviceCreates==DeviceCloses&&DriverCreates==DriverFrees&&LocalAllocations==LocalFrees);CHECK(!HelperCount&&!HelperJobs&&!HelperIdle&&!HelperStop);if(ExpectedHandles){DWORD handles;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==ExpectedHandles);}++Scenarios;}
static VOID MakeId(DWORD length)
{
    static const WCHAR prefix[]=L"ROOT\\PacketBridgeV2\\";
    CHECK(length<MAX_DEVICE_ID_LEN);for(DWORD i=0;i<length;++i)Generated[i]=L'A';
    /* Maximal-ID controls preserve the actual host's enumerator/hardware form.
     * Shorter-than-prefix strings isolate the decoder's length contract only. */
    if(length>=_countof(prefix))memcpy(Generated,prefix,sizeof(prefix)-sizeof(WCHAR));
    Generated[length]=0;ResultId=Generated;
}
static VOID Run(BOOL success,DWORD error,LPCWSTR expected)
{
    WCHAR regular[MAX_DEVICE_ID_LEN+2];WCHAR *output=regular+1;VOID *pages=NULL;SYSTEM_INFO info;GetSystemInfo(&info);
#ifdef PB_GUARD_TESTS
    pages=VirtualAlloc(NULL,(SIZE_T)info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(pages);DWORD old;CHECK(VirtualProtect((BYTE *)pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old));output=(WCHAR *)((BYTE *)pages+info.dwPageSize)-MAX_DEVICE_ID_LEN;
#endif
    for(DWORD i=0;i<MAX_DEVICE_ID_LEN;++i)output[i]=0x5151;output[-1]=0x4545;regular[MAX_DEVICE_ID_LEN+1]=0x4545;
    SetLastError(ERROR_BAD_ENVIRONMENT);BOOL result=CreateInstanceWin7ViaRundll32(output);DWORD actual=GetLastError();
    CHECK(output[-1]==0x4545);if(!pages)CHECK(regular[MAX_DEVICE_ID_LEN+1]==0x4545);
    BOOL baseline=BaselineControl&&!result&&BridgeError==ERROR_BUFFER_OVERFLOW&&wcslen(ResultId)==MAX_DEVICE_ID_LEN-1;
    if(!baseline){CHECK(result==success);if(result)CHECK(expected&&!wcscmp(output,expected));else{CHECK(actual==error);for(DWORD i=0;i<MAX_DEVICE_ID_LEN;++i)CHECK(output[i]==0x5151);}}
    if(pages)CHECK(VirtualFree(pages,0,MEM_RELEASE));End();
    if(baseline){CHECK(Scenarios==1);CHECK(HeapDestroy(ModuleHeap));ModuleHeap=NULL;fputs("HELPER_MAX_ID_SUCCESS_FRAME_EXCEEDS_TRANSPORT_CAPACITY\n",stderr);ExitProcess(107);}
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);DWORD before,after;
    /* Initialize native formatter/parser system caches before the process-wide
     * handle comparison; the warm-up still checks every fixture-owned resource. */
    Begin();Run(TRUE,0,ResultId);Scenarios=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));ExpectedHandles=before;
    C_ASSERT(PB_HELPER_STATUS_RESPONSE_CAPACITY==9&&PB_HELPER_CREATE_RESPONSE_CAPACITY>=MAX_DEVICE_ID_LEN+2);
    if(argc==2){CHECK(!strcmp(argv[1],"1"));BaselineControl=TRUE;Begin();MakeId(MAX_DEVICE_ID_LEN-1);Fragment=1;Run(TRUE,0,ResultId);CHECK(FrameChars==201&&ReadBytes==402);}
    else {
        const DWORD chunks[]={1,2,3,7,397,4096};
        for(DWORD length=1;length<MAX_DEVICE_ID_LEN;++length)for(DWORD c=0;c<_countof(chunks);++c){Begin();MakeId(length);Fragment=chunks[c];Run(TRUE,0,ResultId);CHECK(FrameChars==length+2&&ReadBytes==FrameChars*sizeof(WCHAR)&&Fragments);}
        for(DWORD c=0;c<_countof(chunks);++c){Begin();ResultId=L"ROOT\\PacketBridgeV2\\\x6d4b\x8bd5_\xd83d\xde80";Fragment=chunks[c];Run(TRUE,0,ResultId);}
        const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INSUFFICIENT_BUFFER,0x80000000u,MAXDWORD};
        for(DWORD stage=1;stage<=14;++stage)for(DWORD e=0;e<_countof(errors);++e)for(DWORD c=0;c<3;++c){Begin();Stage=stage;InjectedError=errors[e];Fragment=chunks[c];BOOL bestEffort=stage==11||stage==12;Run(bestEffort,InjectedError,bestEffort?ResultId:NULL);CHECK(ParserCalls==1);if(!bestEffort&&stage>=13)CHECK(Calls[15]&&Calls[16]);}
        for(DWORD c=0;c<_countof(chunks);++c){Begin();Fragment=chunks[c];ParserFailure=TRUE;Run(FALSE,ERROR_INVALID_PARAMETER,NULL);}
        for(DWORD c=0;c<_countof(chunks);++c)for(DWORD cancel=1;cancel<=2;++cancel){Begin();Fragment=chunks[c];CancelAt=cancel;Run(FALSE,ERROR_CANCELLED,NULL);CHECK(!ParserCalls);}
        for(DWORD mode=1;mode<=2;++mode){Begin();WriteMode=mode;Run(FALSE,ERROR_INVALID_DATA,NULL);CHECK(!ParserCalls);}
        Begin();FormatFailure=TRUE;Run(FALSE,ERROR_INVALID_DATA,NULL);CHECK(!ParserCalls);
        /* Malformed worker frames, not successful SetupAPI outputs. Capacity
         * growth must never turn a 200+ ID into a silently truncated success. */
        WCHAR frame[PB_HELPER_CREATE_RESPONSE_CAPACITY+8];
        for(DWORD length=MAX_DEVICE_ID_LEN;length<=PB_HELPER_CREATE_RESPONSE_CAPACITY+3;++length){Begin();frame[0]=L'0';frame[1]=L' ';for(DWORD i=0;i<length;++i)frame[i+2]=L'B';frame[length+2]=0;RawResponse=frame;Run(FALSE,length+2>=PB_HELPER_CREATE_RESPONSE_CAPACITY?ERROR_BUFFER_OVERFLOW:ERROR_INVALID_DATA,NULL);}
        const WCHAR *invalid[]={L"0 \"\"",L"0",L""};
        for(DWORD i=0;i<_countof(invalid);++i){Begin();RawResponse=invalid[i];Run(FALSE,i==0?ERROR_INVALID_DATA:i==1?ERROR_INVALID_PARAMETER:ERROR_INVALID_DATA,NULL);}
        /* The actual Execute admission rejects invalid capacities before any
         * event, heap, work, module or runtime ownership can be acquired. */
        WCHAR admissionBuffer[2];const DWORD invalidCaps[]={0,PB_HELPER_CREATE_RESPONSE_CAPACITY+1,MAXDWORD};
        for(DWORD i=0;i<_countof(invalidCaps);++i){Begin();CHECK(!ActualExecuteRunDll32(L"CreateInstanceWin7",L"",admissionBuffer,invalidCaps[i])&&GetLastError()==ERROR_INVALID_PARAMETER);End();}
        Begin();CHECK(!ActualExecuteRunDll32(L"CreateInstanceWin7",L"",NULL,1)&&GetLastError()==ERROR_INVALID_PARAMETER);End();
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);CHECK(!LocalCount&&HeapDestroy(ModuleHeap));
    printf("PASS: %lu actual host/transport/parent response scenarios; native formatter, parser, pipes and heap; no devices or installer process.\n",Scenarios);return 0;
}
