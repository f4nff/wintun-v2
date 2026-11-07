/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Win7 creation export; every SetupAPI operation is hosted. Native heap,
 * events and FormatMessage are used, without enumerating or changing devices.
 * Failed native cleanup retains model ownership until explicit fixture teardown. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include "../../api/adapter.h"
#include "../../api/driver.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static const WCHAR ApiIdentity[]=WINTUN_HWID;
#undef WINTUN_HWID
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
static DWORD WriteFormatted(DWORD,LPCWSTR,...);
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
#ifndef PB_HELPER_IDENTITY_SOURCE
#define PB_HELPER_IDENTITY_SOURCE "../../setupapihost/host_win7.h"
#endif
/* No export is emitted by the hosted executable. */
#define EXPORT warning(suppress: 4068)
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_HELPER_IDENTITY_SOURCE
#pragma warning(pop)
#undef EXPORT

static HANDLE Device,Heap;
static VOID *Drivers;
static DWORD Calls[19],Phase,ErrorCode,CleanupMask,CleanupError,OutputCode,Outputs;
static DWORD Flags,DriverAllocations,DriverFrees,ImplicitFrees,Scenarios,CleanupResidues;
static BOOL StrictMatch,Registered,DriverSelected,RemoveParams;
static WCHAR Name[MAX_DEVICE_ID_LEN],Hardware[MAX_DEVICE_ID_LEN],InfIdentity[64],Response[512];
static DWORD HardwareBytes;
static LPCWSTR ResultId;
static const WCHAR OrdinaryId[]=L"ROOT\\PacketBridgeV2\\0000";
static BOOL Fails(DWORD stage)
{
    ++Calls[stage];
    if(stage==Phase || (CleanupMask&(1u<<stage))){SetLastError(stage==Phase?ErrorCode:CleanupError);return TRUE;}
    SetLastError(ERROR_INVALID_HANDLE);return FALSE;
}
static VOID CheckDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CHECK(Device&&info==(HDEVINFO)Device&&data&&data->cbSize==sizeof(*data)&&data->DevInst==37);}
static HDEVINFO WINAPI TestCreate(const GUID *guid,HWND parent,PCWSTR machine,PVOID reserved)
{
    CHECK(guid==&GUID_DEVCLASS_NET&&!parent&&!machine&&!reserved&&!Device);
    if(Fails(1))return INVALID_HANDLE_VALUE;
    Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);return (HDEVINFO)Device;
}
static BOOL WINAPI TestCreateDevice(HDEVINFO info,PCWSTR name,const GUID *guid,PCWSTR description,HWND parent,DWORD flags,PSP_DEVINFO_DATA data)
{
    CHECK(Device&&info==(HDEVINFO)Device&&name&&guid==&GUID_DEVCLASS_NET&&!description&&!parent&&flags==DICD_GENERATE_ID&&data&&data->cbSize==sizeof(*data));
    CHECK(wcscpy_s(Name,_countof(Name),name)==0);
    if(Fails(2))return FALSE;data->DevInst=37;return TRUE;
}
static BOOL WINAPI TestGetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(params&&params->cbSize==sizeof(*params));if(Fails(3))return FALSE;params->Flags=Flags;return TRUE;}
static BOOL WINAPI TestSetParams(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DEVINSTALL_PARAMS_W params)
{CheckDevice(info,data);CHECK(params&&params->cbSize==sizeof(*params)&&params->Flags==(Flags|DI_QUIETINSTALL));return !Fails(4);}
static BOOL WINAPI TestSelectDevice(HDEVINFO info,PSP_DEVINFO_DATA data)
{CheckDevice(info,data);return !Fails(5);}
static BOOL WINAPI TestSetHardware(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD property,const BYTE *bytes,DWORD count)
{
    CheckDevice(info,data);CHECK(property==SPDRP_HARDWAREID&&bytes&&count<=sizeof(Hardware)&&count%sizeof(WCHAR)==0);
    const WCHAR *text=(const WCHAR *)bytes;SIZE_T length=wcslen(Name);
    CHECK(count==(length+2)*sizeof(WCHAR)&&!memcmp(text,Name,(length+1)*sizeof(WCHAR))&&!text[length+1]);
    memcpy(Hardware,bytes,count);HardwareBytes=count;return !Fails(6);
}
static BOOL WINAPI TestBuild(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{
    CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&HardwareBytes&&!Drivers);
    if(Fails(7))return FALSE;Drivers=HeapAlloc(Heap,0,16);CHECK(Drivers);++DriverAllocations;return TRUE;
}
static BOOL WINAPI TestEnum(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind,DWORD index,PSP_DRVINFO_DATA_W driver)
{
    CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&!index&&Drivers&&driver&&driver->cbSize==sizeof(*driver));
    if(Fails(8))return FALSE;
    if(StrictMatch&&wcscmp(Hardware,InfIdentity)){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}
    driver->Reserved=0x1234;return TRUE;
}
static BOOL WINAPI TestSelectDriver(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_DRVINFO_DATA_W driver)
{
    CheckDevice(info,data);CHECK(Drivers&&driver&&driver->cbSize==sizeof(*driver)&&driver->Reserved==0x1234);
    if(Fails(9))return FALSE;DriverSelected=TRUE;return TRUE;
}
static BOOL WINAPI TestCall(DI_FUNCTION action,HDEVINFO info,PSP_DEVINFO_DATA data)
{
    CheckDevice(info,data);DWORD stage;
    if(action==DIF_REGISTERDEVICE){CHECK(DriverSelected&&!Registered);stage=10;}
    else if(action==DIF_REGISTER_COINSTALLERS){CHECK(Registered);stage=11;}
    else if(action==DIF_INSTALLINTERFACES){CHECK(Registered);stage=12;}
    else if(action==DIF_INSTALLDEVICE){CHECK(Registered);stage=13;}
    else{CHECK(action==DIF_REMOVE&&Registered&&RemoveParams);stage=16;}
    if(Fails(stage))return FALSE;
    if(stage==10)Registered=TRUE;else if(stage==16)Registered=FALSE;return TRUE;
}
static BOOL WINAPI TestGetId(HDEVINFO info,PSP_DEVINFO_DATA data,PWSTR output,DWORD chars,PDWORD required)
{
    CheckDevice(info,data);CHECK(Registered&&output&&chars==MAX_DEVICE_ID_LEN&&required&&*required==chars);
    if(Fails(14)){*required=chars+1;return FALSE;}
    *required=(DWORD)wcslen(ResultId)+1;CHECK(*required<=chars&&wcscpy_s(output,chars,ResultId)==0);return TRUE;
}
static BOOL WINAPI TestSetRemove(HDEVINFO info,PSP_DEVINFO_DATA data,PSP_CLASSINSTALL_HEADER header,DWORD size)
{
    CheckDevice(info,data);SP_REMOVEDEVICE_PARAMS *params=(VOID *)header;
    CHECK(Registered&&params&&size==sizeof(*params)&&header->cbSize==sizeof(*header)&&header->InstallFunction==DIF_REMOVE&&params->Scope==DI_REMOVEDEVICE_GLOBAL&&!params->HwProfile);
    if(Fails(15))return FALSE;RemoveParams=TRUE;return TRUE;
}
static VOID FreeDrivers(BOOL implicit)
{CHECK(Drivers&&HeapFree(Heap,0,Drivers));Drivers=NULL;++DriverFrees;if(implicit)++ImplicitFrees;DriverSelected=FALSE;}
static BOOL WINAPI TestDestroyDrivers(HDEVINFO info,PSP_DEVINFO_DATA data,DWORD kind)
{CheckDevice(info,data);CHECK(kind==SPDIT_COMPATDRIVER&&Drivers);if(Fails(17))return FALSE;FreeDrivers(FALSE);return TRUE;}
static BOOL WINAPI TestDestroy(HDEVINFO info)
{
    CHECK(Device&&info==(HDEVINFO)Device);
    if(Fails(18))return FALSE;
    if(Drivers)FreeDrivers(TRUE);CHECK(CloseHandle(Device));Device=NULL;SetLastError(ERROR_INVALID_HANDLE);return TRUE;
}
static DWORD WriteFormatted(DWORD stream,LPCWSTR format,...)
{
    CHECK(stream==STD_OUTPUT_HANDLE&&!wcscmp(format,L"%1!X! %2!s!"));++Outputs;
    va_list args,copy;va_start(args,format);va_copy(copy,args);
    OutputCode=va_arg(copy,DWORD);LPCWSTR id=va_arg(copy,LPCWSTR);va_end(copy);
    CHECK(!wcscmp(id,OutputCode?L"\"\"":ResultId));
    LPWSTR rendered=NULL;DWORD length=FormatMessageW(FORMAT_MESSAGE_FROM_STRING|FORMAT_MESSAGE_ALLOCATE_BUFFER,format,0,0,(LPWSTR)&rendered,0,&args);
    va_end(args);CHECK(length&&rendered&&wcscpy_s(Response,_countof(Response),rendered)==0&&!LocalFree(rendered));return length;
}
static VOID Begin(VOID)
{
    CHECK(!Device&&!Drivers);memset(Calls,0,sizeof(Calls));memset(Name,0,sizeof(Name));memset(Hardware,0,sizeof(Hardware));
    Phase=CleanupMask=HardwareBytes=Outputs=OutputCode=DriverAllocations=DriverFrees=ImplicitFrees=0;
    ErrorCode=ERROR_ACCESS_DENIED;CleanupError=ERROR_INVALID_HANDLE;Flags=0x12340000;StrictMatch=TRUE;
    Registered=DriverSelected=RemoveParams=FALSE;ResultId=OrdinaryId;Response[0]=0;SetLastError(ERROR_SUCCESS);
}
static DWORD Invoke(VOID)
{CreateInstanceWin7(NULL,NULL,NULL,0);CHECK(Outputs==1);return OutputCode;}
static VOID End(VOID)
{
    if(Device){CHECK(CleanupMask&(1u<<18));++CleanupResidues;if(Drivers)FreeDrivers(TRUE);CHECK(CloseHandle(Device));Device=NULL;}
    CHECK(!Drivers&&DriverAllocations==DriverFrees);++Scenarios;
}
static VOID ReadInf(VOID)
{
    char path[MAX_PATH*4];CHECK(strcpy_s(path,sizeof(path),__FILE__)==0);char *last=strrchr(path,'\\');char *slash=strrchr(path,'/');if(slash&&(!last||slash>last))last=slash;CHECK(last);last[1]=0;
    CHECK(strcat_s(path,sizeof(path),"../../driver/v2/packetbridge.inf")==0);FILE *file=NULL;CHECK(!fopen_s(&file,path,"rt")&&file);
    char line[512];BOOL section=FALSE,found=FALSE;
    while(fgets(line,sizeof(line),file)){
        if(!strncmp(line,"[PacketBridgeV2.NTamd64.6.1]",26)){section=TRUE;continue;}
        if(section&&line[0]=='[')break;
        if(section&&line[0]=='%'&&strchr(line,',')){char *id=strchr(line,',')+1;while(*id==' '||*id=='\t')++id;SIZE_T length=strcspn(id,"\r\n");while(length&&(id[length-1]==' '||id[length-1]=='\t'))--length;CHECK(length&&length<_countof(InfIdentity));for(SIZE_T i=0;i<length;++i)InfIdentity[i]=(WCHAR)(BYTE)id[i];InfIdentity[length]=0;found=TRUE;break;}
    }
    CHECK(!fclose(file)&&found&&!wcscmp(InfIdentity,ApiIdentity));
}
static VOID Matrix(VOID)
{
    const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_NO_MORE_ITEMS,ERROR_INVALID_DATA,0xe0000203u};
    const DWORD flags[]={0,DI_QUIETINSTALL,0x12340000,MAXDWORD};
    for(DWORD f=0;f<_countof(flags);++f){Begin();Flags=flags[f];CHECK(!Invoke()&&!wcscmp(Name,InfIdentity)&&!wcscmp(Hardware,InfIdentity)&&Registered&&Calls[17]==1&&Calls[18]==1&&!ImplicitFrees);End();}
    for(DWORD stage=1;stage<=14;++stage)for(DWORD e=0;e<_countof(errors);++e){
        Begin();Phase=stage;ErrorCode=errors[e];CHECK(Invoke()==(stage==11||stage==12?ERROR_SUCCESS:ErrorCode));
        CHECK(Calls[stage]==1&&Calls[18]==(stage==1?0u:1u));
        CHECK(Calls[17]==(stage>=8&&stage!=10?1u:0u)&&ImplicitFrees==(stage==10?1u:0u));
        CHECK(Calls[15]==(stage>=13?1u:0u)&&Calls[16]==(stage>=13?1u:0u));End();
    }
    const DWORD masks[]={1u<<15,1u<<16,1u<<17,1u<<18,(1u<<15)|(1u<<17),(1u<<16)|(1u<<18),(1u<<17)|(1u<<18)};
    for(DWORD primary=0;primary<3;++primary)for(DWORD mask=0;mask<_countof(masks);++mask)for(DWORD e=0;e<_countof(errors);++e){
        Begin();Phase=primary?primary==1?10:13:0;CleanupMask=masks[mask];CleanupError=errors[e];
        CHECK(Invoke()==(Phase?ErrorCode:ERROR_SUCCESS));
        CHECK(Calls[17]==(Phase==10?0u:1u)&&Calls[18]==1);
        CHECK(Calls[15]==(Phase==13?1u:0u)&&Calls[16]==(Phase==13&&!(CleanupMask&(1u<<15))?1u:0u));
        if(Phase==13)CHECK(Registered==!!(CleanupMask&((1u<<15)|(1u<<16))));End();
    }
    WCHAR longest[MAX_DEVICE_ID_LEN];for(DWORD i=0;i<_countof(longest)-1;++i)longest[i]=(WCHAR)(L'a'+i%26);longest[_countof(longest)-1]=0;
    const LPCWSTR ids[]={OrdinaryId,L"ROOT\\PacketBridgeV2\\\x4e2d\x6587",L"ROOT\\PacketBridgeV2\\id with spaces",longest};
    for(DWORD i=0;i<_countof(ids);++i){Begin();ResultId=ids[i];CHECK(!Invoke());End();}
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ReadInf();Heap=HeapCreate(0,0,0);CHECK(Heap);
    if(argc==2){
        CHECK(!strcmp(argv[1],"1"));Begin();DWORD code=Invoke();BOOL old=!wcscmp(Name,L"Wintun")&&!wcscmp(Hardware,L"Wintun");
        CHECK(Calls[6]==1&&Calls[7]==1&&Calls[8]==1&&Calls[17]==1&&Calls[18]==1);
        if(!old)CHECK(!code&&!wcscmp(Name,InfIdentity)&&!wcscmp(Hardware,InfIdentity)&&Calls[10]==1);
        else CHECK(code==ERROR_NO_MORE_ITEMS&&HardwareBytes==sizeof(L"Wintun")+sizeof(WCHAR)&&!Calls[10]);
        End();CHECK(HeapDestroy(Heap));if(old){puts("HELPER_WIN7_USED_LEGACY_HARDWARE_ID");return 94;}
        puts("PASS: Win7 helper uses actual unified-INF hardware identity");return 0;
    }
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(DWORD repeat=0;repeat<3;++repeat)Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);PROCESS_HEAP_ENTRY entry={0};while(HeapWalk(Heap,&entry))CHECK(!(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY));CHECK(HeapDestroy(Heap));
    printf("PASS: %lu actual Win7 helper identity/SetupAPI schedules; exact double-NUL HWID and INF match, all acquisition/cleanup failures, driver-list parent ownership, original errors and native handle/heap balance; %lu modeled cleanup residues explicitly reclaimed by fixture, no devices accessed\n",Scenarios,CleanupResidues);return 0;
}
