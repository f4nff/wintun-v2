/* SPDX-License-Identifier: GPL-2.0 */
/* Complete actual Win7 creation helper, native event/heap/CRT owners, and hosted
 * SetupAPI/helper/wait boundaries. Both NativeMachine paths are exercised in a
 * MAYBE_WOW64 build; the released x64 pipeline does not enable that macro.
 * No real device, registry, installer process or driver is accessed. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <initguid.h>
#include <devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../api/adapter.h"
#include "../../api/logger.h"
#include "../../api/rundll32.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static HDEVINFO WINAPI TestCreate(const GUID *,HWND,PCWSTR,PVOID);
static BOOL WINAPI TestCreateDevice(HDEVINFO,PCWSTR,const GUID *,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestGetParams(HDEVINFO,PSP_DEVINFO_DATA,PSP_DEVINSTALL_PARAMS_W);
static BOOL WINAPI TestSetParams(HDEVINFO,PSP_DEVINFO_DATA,PSP_DEVINSTALL_PARAMS_W);
static BOOL WINAPI TestSelectDevice(HDEVINFO,PSP_DEVINFO_DATA);
static BOOL WINAPI TestSetRegistry(HDEVINFO,PSP_DEVINFO_DATA,DWORD,const BYTE *,DWORD);
static BOOL WINAPI TestBuild(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestEnum(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,PSP_DRVINFO_DATA_W);
static BOOL WINAPI TestSelectDriver(HDEVINFO,PSP_DEVINFO_DATA,PSP_DRVINFO_DATA_W);
static BOOL WINAPI TestCall(DI_FUNCTION,HDEVINFO,PSP_DEVINFO_DATA);
static BOOL WINAPI TestGetId(HDEVINFO,PSP_DEVINFO_DATA,PWSTR,DWORD,PDWORD);
static BOOL WINAPI TestDestroyDrivers(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestDestroy(HDEVINFO);
static BOOL WINAPI TestOpen(HDEVINFO,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestSetProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE,const BYTE *,DWORD,DWORD);
static BOOL WINAPI TestGetProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE *,PBYTE,DWORD,PDWORD,DWORD);
static BOOL WINAPI TestTimes(HANDLE,LPFILETIME,LPFILETIME,LPFILETIME,LPFILETIME);
#define SetupDiCreateDeviceInfoListExW TestCreate
#define SetupDiCreateDeviceInfoW TestCreateDevice
#define SetupDiGetDeviceInstallParamsW TestGetParams
#define SetupDiSetDeviceInstallParamsW TestSetParams
#define SetupDiSetSelectedDevice TestSelectDevice
#define SetupDiSetDeviceRegistryPropertyW TestSetRegistry
#define SetupDiBuildDriverInfoList TestBuild
#define SetupDiEnumDriverInfoW TestEnum
#define SetupDiSetSelectedDriverW TestSelectDriver
#define SetupDiCallClassInstaller TestCall
#define SetupDiGetDeviceInstanceIdW TestGetId
#define SetupDiDestroyDriverInfoList TestDestroyDrivers
#define SetupDiDestroyDeviceInfoList TestDestroy
#define SetupDiOpenDeviceInfoW TestOpen
#define SetupDiSetDevicePropertyW TestSetProperty
#define SetupDiGetDevicePropertyW TestGetProperty
#define GetProcessTimes TestTimes
/* The readiness loop has independent actual-source tests; here its single
 * Boolean boundary is hosted to isolate the installation owner's rollback. */
#include "../../api/adapter_wait_win7.h"
#define WaitForInterfaceWin7 TestWait
static BOOL TestWait(HDEVINFO,PSP_DEVINFO_DATA,LPCWSTR);
#ifndef PB_WIN7_CREATE_SOURCE
#define PB_WIN7_CREATE_SOURCE "../../api/adapter_win7.h"
#endif
#include PB_WIN7_CREATE_SOURCE
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
#undef SetupDiDestroyDriverInfoList
#undef SetupDiDestroyDeviceInfoList
#undef SetupDiOpenDeviceInfoW
#undef SetupDiSetDevicePropertyW
#undef SetupDiGetDevicePropertyW
#undef GetProcessTimes
#undef WaitForInterfaceWin7

HANDLE ModuleHeap;
USHORT NativeMachine=IMAGE_FILE_PROCESS;
const DEVPROPKEY DEVPKEY_Wintun_Name={{0x3361c968,0x2f2e,0x4660,{0xb4,0x7e,0x69,0x9c,0xdc,0x4c,0x32,0xb9}},DEVPROPID_FIRST_USABLE+1};
static HANDLE Device;
static VOID *Drivers;
static DWORD Stage,ErrorCode,Calls[26],CleanupMask,CleanupError,Scenarios,ExpectedHandles;
static DWORD DeviceCreates,DeviceCloses,DriverCreates,DriverFrees,ExplicitFrees,Rescues;
static DWORD LogCount,WaitCalls,LastProperty,NameBytes,TunnelBytes;
static BOOL Registered,Selected,HelperMode,BaselineControl,WaitSuccess;
static LPCWSTR Name,Tunnel,ResultId;
static WCHAR Id[MAX_DEVICE_ID_LEN];
static const FILETIME CreationTime={0x12345678,0x87654321};
static const WCHAR OrdinaryId[]=L"ROOT\\PacketBridgeV2\\0000";
static BOOL Fails(DWORD stage)
{++Calls[stage];if(Stage==stage||(CleanupMask&(1u<<stage))){SetLastError(Stage==stage?ErrorCode:CleanupError);return TRUE;}SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
static VOID CheckDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CHECK(Device&&info==(HDEVINFO)Device&&data&&data->cbSize==sizeof(*data)&&data->DevInst==37);}
static HDEVINFO WINAPI TestCreate(const GUID *guid,HWND parent,PCWSTR machine,PVOID reserved)
{CHECK(!Device&&guid==&GUID_DEVCLASS_NET&&!parent&&!machine&&!reserved);if(Fails(1))return INVALID_HANDLE_VALUE;Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);++DeviceCreates;return(HDEVINFO)Device;}
static BOOL WINAPI TestCreateDevice(HDEVINFO info,PCWSTR name,const GUID *guid,PCWSTR description,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{CHECK(!HelperMode&&info==(HDEVINFO)Device&&!wcscmp(name,WINTUN_HWID)&&guid==&GUID_DEVCLASS_NET&&description==Tunnel&&!parent&&flags==DICD_GENERATE_ID&&data&&data->cbSize==sizeof(*data));if(Fails(2))return FALSE;data->DevInst=37;return TRUE;}
static BOOL WINAPI TestGetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(!HelperMode&&params&&params->cbSize==sizeof(*params));if(Fails(3))return FALSE;params->Flags=0x12340000;return TRUE;}
static BOOL WINAPI TestSetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(!HelperMode&&params&&params->Flags==(0x12340000|DI_QUIETINSTALL));return!Fails(4);}
static BOOL WINAPI TestSelectDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CheckDevice(info,data);return!Fails(5);}
static BOOL WINAPI TestSetRegistry(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD property,const BYTE *bytes,DWORD count)
{
    CheckDevice(info,data);CHECK(bytes);DWORD stage;
    if(property==SPDRP_HARDWAREID){CHECK(!HelperMode&&count==sizeof(WINTUN_HWID)+sizeof(WCHAR)&&!wcscmp((LPCWSTR)bytes,WINTUN_HWID)&&!((LPCWSTR)bytes)[_countof(WINTUN_HWID)]);stage=6;}
    else{CHECK(Registered&&(property==SPDRP_FRIENDLYNAME||property==SPDRP_DEVICEDESC)&&bytes==(const BYTE *)Tunnel&&count==TunnelBytes);stage=property==SPDRP_FRIENDLYNAME?13:14;}
    LastProperty=property;return!Fails(stage);
}
static BOOL WINAPI TestBuild(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{CheckDevice(info,data);CHECK(!HelperMode&&kind==SPDIT_COMPATDRIVER&&!Drivers);if(Fails(7))return FALSE;Drivers=HeapAlloc(ModuleHeap,0,16);CHECK(Drivers);++DriverCreates;return TRUE;}
static BOOL WINAPI TestEnum(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind,DWORD index,PSP_DRVINFO_DATA_W driver)
{CheckDevice(info,data);CHECK(!HelperMode&&Drivers&&kind==SPDIT_COMPATDRIVER&&!index&&driver&&driver->cbSize==sizeof(*driver));if(Fails(8))return FALSE;driver->Reserved=0x1234;return TRUE;}
static BOOL WINAPI TestSelectDriver(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DRVINFO_DATA_W driver)
{CheckDevice(info,data);CHECK(!HelperMode&&Drivers&&driver&&driver->Reserved==0x1234);if(Fails(9))return FALSE;Selected=TRUE;return TRUE;}
static BOOL WINAPI TestCall(DI_FUNCTION action,HDEVINFO info,PSP_DEVINFO_DATA data)
{CheckDevice(info,data);CHECK(!HelperMode);DWORD stage=action==DIF_REGISTERDEVICE?10:action==DIF_REGISTER_COINSTALLERS?11:action==DIF_INSTALLINTERFACES?12:action==DIF_INSTALLDEVICE?17:0;CHECK(stage&&Selected);if(Fails(stage))return FALSE;if(stage==10)Registered=TRUE;return TRUE;}
static BOOL WINAPI TestGetId(HDEVINFO info,PSP_DEVINFO_DATA data,PWSTR output,DWORD chars,PDWORD required)
{CheckDevice(info,data);CHECK(Registered&&output&&chars==MAX_DEVICE_ID_LEN&&required&&*required==chars);if(Fails(18))return FALSE;*required=(DWORD)wcslen(ResultId)+1;CHECK(*required<=chars&&!wcscpy_s(output,chars,ResultId));return TRUE;}
static VOID FreeDrivers(BOOL explicit)
{CHECK(Drivers&&HeapFree(ModuleHeap,0,Drivers));Drivers=NULL;++DriverFrees;if(explicit)++ExplicitFrees;}
static BOOL WINAPI TestDestroyDrivers(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER);if(Fails(23))return FALSE;if(!Drivers){CHECK(HelperMode);SetLastError(ERROR_NO_DRIVER_SELECTED);return FALSE;}FreeDrivers(TRUE);return TRUE;}
static BOOL WINAPI TestDestroy(HDEVINFO info)
{CHECK(Device&&info==(HDEVINFO)Device);if(Fails(24))return FALSE;if(Drivers)FreeDrivers(FALSE);CHECK(CloseHandle(Device));Device=NULL;++DeviceCloses;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static BOOL WINAPI TestOpen(HDEVINFO info,PCWSTR id,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{CHECK(HelperMode&&Registered&&info==(HDEVINFO)Device&&!wcscmp(id,ResultId)&&!parent&&flags==DIOD_INHERIT_CLASSDRVS&&data&&data->cbSize==sizeof(*data));if(Fails(21))return FALSE;data->DevInst=37;return TRUE;}
static BOOL WINAPI TestSetProperty(HDEVINFO info,PSP_DEVINFO_DATA data,const DEVPROPKEY *key,DEVPROPTYPE type,const BYTE *bytes,DWORD count,DWORD flags)
{
    CheckDevice(info,data);CHECK(Registered&&key&&bytes&&!flags);DWORD stage;
    if(key==&DEVPKEY_Wintun_Name){CHECK(type==DEVPROP_TYPE_STRING&&bytes==(const BYTE *)Name&&count==NameBytes);stage=15;}
    else{CHECK(key==&DEVPKEY_Wintun_OwningProcess&&type==DEVPROP_TYPE_BINARY&&count==sizeof(OWNING_PROCESS));const OWNING_PROCESS *owner=(const OWNING_PROCESS *)bytes;CHECK(owner->ProcessId==GetCurrentProcessId()&&!memcmp(&owner->CreationTime,&CreationTime,sizeof(CreationTime)));stage=16;}
    return!Fails(stage);
}
static BOOL WINAPI TestGetProperty(HDEVINFO info,PSP_DEVINFO_DATA data,const DEVPROPKEY *key,DEVPROPTYPE *type,PBYTE bytes,DWORD count,PDWORD required,DWORD flags)
{CheckDevice(info,data);CHECK(key==&DEVPKEY_Device_ProblemCode&&type&&bytes&&count==sizeof(INT32)&&!required&&!flags);if(Fails(20))return FALSE;*type=DEVPROP_TYPE_UINT32;*(INT32 *)bytes=28;return TRUE;}
static BOOL WINAPI TestTimes(HANDLE process,LPFILETIME creation,LPFILETIME exit,LPFILETIME kernel,LPFILETIME user)
{CHECK(process==GetCurrentProcess()&&creation&&exit&&kernel&&user);if(Fails(19))return FALSE;*creation=CreationTime;return TRUE;}
BOOL CreateInstanceWin7ViaRundll32(LPWSTR output)
{CHECK(HelperMode&&Device&&!Registered&&output);if(Fails(22))return FALSE;CHECK(!wcscpy_s(output,MAX_DEVICE_ID_LEN,ResultId));Registered=TRUE;return TRUE;}
BOOL AdapterRemoveInstance(HDEVINFO info,SP_DEVINFO_DATA *data)
{CheckDevice(info,data);CHECK(Registered);if(Fails(25))return FALSE;Registered=FALSE;return TRUE;}
static BOOL TestWait(HDEVINFO info,PSP_DEVINFO_DATA data,LPCWSTR id)
{CheckDevice(info,data);CHECK(Registered&&!wcscmp(id,ResultId));++WaitCalls;SetLastError(ErrorCode);return WaitSuccess;}
LPWSTR AdapterGetDeviceObjectFileName(LPCWSTR instance)
{(void)instance;CHECK(FALSE);return NULL;}
BOOL MainOperationSleep(DWORD delay)
{(void)delay;CHECK(FALSE);return FALSE;}
static DWORD FormatLog(LPCWSTR format,va_list args)
{WCHAR text[1024];CHECK(_vsnwprintf_s(text,_countof(text),_TRUNCATE,format,args)>=0);++LogCount;SetLastError(ERROR_BAD_ENVIRONMENT);return ERROR_BAD_ENVIRONMENT;}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD error=GetLastError();FormatLog(format,args);SetLastError(error);return error;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{FormatLog(format,args);return error;}
static VOID Begin(BOOL helper)
{
    CHECK(!Device&&!Drivers);Stage=CleanupMask=0;ErrorCode=ERROR_ACCESS_DENIED;CleanupError=ERROR_INVALID_HANDLE;
    DeviceCreates=DeviceCloses=DriverCreates=DriverFrees=ExplicitFrees=Rescues=LogCount=WaitCalls=LastProperty=0;
    memset(Calls,0,sizeof(Calls));Registered=Selected=FALSE;WaitSuccess=TRUE;HelperMode=helper;NativeMachine=helper?IMAGE_FILE_MACHINE_ARM64:IMAGE_FILE_PROCESS;
    Name=L"probe";Tunnel=L"probe Tunnel";ResultId=OrdinaryId;
}
static VOID Run(BOOL success,DWORD error)
{
    NameBytes=(DWORD)(wcslen(Name)+1)*sizeof(WCHAR);TunnelBytes=(DWORD)(wcslen(Tunnel)+1)*sizeof(WCHAR);
    WINTUN_ADAPTER regular={0};WINTUN_ADAPTER *adapter=&regular;VOID *pages=NULL;
#ifdef PB_GUARD_TESTS
    SYSTEM_INFO info;GetSystemInfo(&info);pages=VirtualAlloc(NULL,(SIZE_T)info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(pages);DWORD old;CHECK(VirtualProtect((BYTE *)pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old));adapter=(WINTUN_ADAPTER *)((BYTE *)pages+info.dwPageSize-sizeof(*adapter));
#endif
    SetLastError(ERROR_SUCCESS);BOOL actual=CreateAdapterWin7(adapter,Name,Tunnel);DWORD resultError=GetLastError();
    BOOL baseline=BaselineControl&&Device&&HelperMode&&Stage==22&&!actual;
    CHECK(Calls[1]==1&&DeviceCreates==(Stage==1?0u:1u));
    if(baseline)CHECK(Calls[22]==1&&!Calls[24]&&DeviceCreates==1&&!DeviceCloses);
    CHECK(actual==success);if(!actual)CHECK(resultError==error);else CHECK(!wcscmp(adapter->DevInstanceID,ResultId));
    CHECK(!adapter->SwDevice&&!adapter->DevInfo&&!adapter->InterfaceFilename&&!adapter->RuntimeOwned);
    if(Device){CHECK(baseline||(CleanupMask&(1u<<24)));if(Drivers)FreeDrivers(FALSE);CHECK(CloseHandle(Device));Device=NULL;++DeviceCloses;++Rescues;}
    CHECK(!Drivers&&DeviceCreates==DeviceCloses&&DriverCreates==DriverFrees);if(pages)CHECK(VirtualFree(pages,0,MEM_RELEASE));
    DWORD handles;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==ExpectedHandles);++Scenarios;
    if(baseline){CHECK(Rescues==1&&HeapDestroy(ModuleHeap));fputs("WIN7_HELPER_FAILURE_SKIPS_DEVINFO_RELEASE\n",stderr);ExitProcess(108);}
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);CHECK(GetProcessHandleCount(GetCurrentProcess(),&ExpectedHandles));
    if(argc==2){CHECK(!strcmp(argv[1],"1"));BaselineControl=TRUE;Begin(TRUE);Stage=22;Run(FALSE,ErrorCode);CHECK(!Rescues);}
    else {
        const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_NOT_SUPPORTED,ERROR_CANCELLED,ERROR_TIMEOUT,ERROR_SHUTDOWN_IN_PROGRESS,MAXDWORD};
        for(DWORD helper=0;helper<=1;++helper)for(DWORD stage=0;stage<=25;++stage)for(DWORD e=0;e<_countof(errors);++e){
            Begin(helper);Stage=stage;ErrorCode=errors[e];
            if(stage>=23)CleanupMask=1u<<stage,Stage=0;
            BOOL reached=!stage||stage==1||(helper?(stage>=13&&stage<=16)||stage==18||stage==19||stage==21||stage==22:stage<=19&&stage!=20);
            BOOL failure=reached&&stage&&stage!=11&&stage!=12&&stage<23;
            DWORD expected=(stage==8||stage==9)?ERROR_DRIVER_INSTALL_BLOCKED:ErrorCode;
            Run(!failure,expected);
            CHECK(!Rescues||stage==24);if(helper)CHECK(!Calls[2]&&!Calls[7]&&!Calls[10]);else CHECK(!Calls[21]&&!Calls[22]);
        }
        for(DWORD helper=0;helper<=1;++helper)for(DWORD e=0;e<_countof(errors);++e)for(DWORD cleanup=0;cleanup<8;++cleanup){
            Begin(helper);WaitSuccess=FALSE;ErrorCode=errors[e];CleanupMask=((cleanup&1)?1u<<23:0)|((cleanup&2)?1u<<24:0)|((cleanup&4)?1u<<25:0);
            Run(FALSE,ERROR_DEVICE_REINITIALIZATION_NEEDED);CHECK(WaitCalls==1&&Calls[20]==1&&Calls[25]==1);
        }
        for(DWORD helper=0;helper<=1;++helper)for(DWORD e=0;e<_countof(errors);++e){Begin(helper);Stage=20;WaitSuccess=FALSE;ErrorCode=errors[e];Run(FALSE,ERROR_DEVICE_REINITIALIZATION_NEEDED);CHECK(Calls[20]==1);}
        for(DWORD helper=0;helper<=1;++helper)for(DWORD stage=13;stage<=19;++stage)for(DWORD cleanup=0;cleanup<8;++cleanup){
            Begin(helper);Stage=stage;CleanupMask=((cleanup&1)?1u<<23:0)|((cleanup&2)?1u<<24:0)|((cleanup&4)?1u<<25:0);
            Run(helper&&stage==17,ERROR_ACCESS_DENIED);
        }
        /* Legal maximum output capacity, plus Unicode/name-boundary inputs. */
        WCHAR name[PB_MAX_ADAPTER_NAME],tunnel[PB_MAX_ADAPTER_NAME+8];
        for(DWORD i=0;i<_countof(name)-1;++i)name[i]=L'\x6d4b';name[_countof(name)-1]=0;
        for(DWORD i=0;i<_countof(tunnel)-1;++i)tunnel[i]=L'\x8bd5';tunnel[_countof(tunnel)-1]=0;
        static const WCHAR prefix[]=L"ROOT\\" WINTUN_HWID L"\\";
        /* Short strings are a capacity/decoder model; only the long cases carry
         * the ROOT/hardware structure. No native generator-length claim. */
        for(DWORD helper=0;helper<=1;++helper)for(DWORD length=1;length<MAX_DEVICE_ID_LEN;++length){Begin(helper);for(DWORD i=0;i<length;++i)Id[i]=L'A';Id[length]=0;if(length>=_countof(prefix))memcpy(Id,prefix,sizeof(prefix)-sizeof(WCHAR));ResultId=Id;Name=name;Tunnel=tunnel;Run(TRUE,0);}
    }
    CHECK(!Device&&!Drivers&&HeapDestroy(ModuleHeap));printf("PASS: %lu actual Win7 creation/rollback owner scenarios; native heap/event/CRT, hosted SDK/helper/wait, no devices.\n",Scenarios);return 0;
}
