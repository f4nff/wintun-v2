/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Win7 readiness loop with hosted SetupAPI/registry/CM boundaries.
 * Key and device stand-ins own real event handles; the filename owns real heap
 * memory. No real registry, adapter, device, driver or elapsed sleep is used. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <devguid.h>
#include <devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../api/adapter.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
#define HeapFree TestHeapFree
#include "../../api/logger.h"
static HKEY WINAPI TestOpen(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,DWORD,REGSAM);
static LSTATUS WINAPI TestQuery(HKEY,LPCWSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
static CONFIGRET WINAPI TestStatus(PULONG,PULONG,DEVINST,ULONG);
static HANDLE WINAPI TestCreate(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static LSTATUS WINAPI TestRegClose(HKEY);
static BOOL WINAPI TestClose(HANDLE);
#define SetupDiOpenDevRegKey TestOpen
#define RegQueryValueExW TestQuery
#define CM_Get_DevNode_Status TestStatus
#define CreateFileW TestCreate
#define RegCloseKey TestRegClose
#define CloseHandle TestClose
/* A whole frozen adapter_win7.h can be used for the historical control. Its
 * unused installation helpers are discarded; they never submit SDK calls. */
#pragma warning(disable:4505)
#ifndef PB_WIN7_WAIT_SOURCE
#define PB_WIN7_WAIT_SOURCE "../../api/adapter_wait_win7.h"
#endif
#include PB_WIN7_WAIT_SOURCE
#undef HeapFree
#undef SetupDiOpenDevRegKey
#undef RegQueryValueExW
#undef CM_Get_DevNode_Status
#undef CreateFileW
#undef RegCloseKey
#undef CloseHandle

HANDLE ModuleHeap;
static HANDLE KeyHandle,DeviceHandle;
static LPWSTR NamePointer;
static DWORD Step,Sleeps,CancelAt,KeyAt,NameAt,FileAt,RegistryAt,CmAt,StatusAt,NumberAt;
static DWORD RegistryError,StatusBits,KeyCalls,NameCalls,FileCalls,QueryCalls,CmCalls;
static DWORD KeysOpened,KeysClosed,FilesOpened,FilesClosed,NamesAllocated,NamesFreed,Scenarios;
static BOOL NullKey;
static const HDEVINFO DeviceSet=(HDEVINFO)(ULONG_PTR)0x1234;
static SP_DEVINFO_DATA Device={.cbSize=sizeof(Device),.DevInst=42};
static VOID Begin(VOID)
{
    CHECK(!KeyHandle&&!DeviceHandle&&!NamePointer);
    Step=Sleeps=KeyAt=NameAt=FileAt=RegistryAt=CmAt=StatusAt=NumberAt=0;
    CancelAt=MAXDWORD; RegistryError=ERROR_FILE_NOT_FOUND; StatusBits=0; NullKey=FALSE;
    KeyCalls=NameCalls=FileCalls=QueryCalls=CmCalls=0;
    KeysOpened=KeysClosed=FilesOpened=FilesClosed=NamesAllocated=NamesFreed=0;
}
static VOID End(VOID)
{
    CHECK(!KeyHandle&&!DeviceHandle&&!NamePointer&&KeysOpened==KeysClosed&&FilesOpened==FilesClosed&&NamesAllocated==NamesFreed);
    ++Scenarios;
}
BOOL MainOperationSleep(DWORD Milliseconds)
{
    CHECK(Milliseconds==10); ++Sleeps;
    if (Sleeps>=CancelAt) { SetLastError(ERROR_CANCELLED); return FALSE; }
    ++Step; return TRUE;
}
static HKEY WINAPI TestOpen(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Scope,DWORD Profile,DWORD Type,REGSAM Access)
{
    CHECK(Set==DeviceSet&&Info==&Device&&Scope==DICS_FLAG_GLOBAL&&!Profile&&Type==DIREG_DRV&&Access==KEY_QUERY_VALUE&&!KeyHandle);
    ++KeyCalls;
    if (NullKey) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    if (Step<KeyAt) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    KeyHandle=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(KeyHandle); ++KeysOpened; return (HKEY)KeyHandle;
}
LPWSTR AdapterGetDeviceObjectFileName(LPCWSTR Instance)
{
    CHECK(!wcscmp(Instance,L"hosted-instance")&&!NamePointer); ++NameCalls;
    if (Step<NameAt) { SetLastError(ERROR_DEVICE_NOT_AVAILABLE); return NULL; }
    NamePointer=HeapAlloc(ModuleHeap,0,32*sizeof(WCHAR)); CHECK(NamePointer);
    wcscpy_s(NamePointer,32,L"hosted-device"); ++NamesAllocated; return NamePointer;
}
static HANDLE WINAPI TestCreate(LPCWSTR Name,DWORD Access,DWORD Share,LPSECURITY_ATTRIBUTES Security,DWORD Disposition,DWORD Attributes,HANDLE Template)
{
    CHECK(Name==NamePointer&&!wcscmp(Name,L"hosted-device")&&Access==(GENERIC_READ|GENERIC_WRITE)&&
        Share==(FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE)&&!Security&&Disposition==OPEN_EXISTING&&!Attributes&&!Template&&!DeviceHandle);
    ++FileCalls;
    if (Step<FileAt) { SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
    DeviceHandle=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(DeviceHandle); ++FilesOpened; return DeviceHandle;
}
static LSTATUS WINAPI TestQuery(HKEY Key,LPCWSTR Name,LPDWORD Reserved,LPDWORD Type,LPBYTE Data,LPDWORD Bytes)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle&&DeviceHandle&&NamePointer&&!wcscmp(Name,L"NetCfgInstanceId")&&!Reserved&&Type&&!Data&&Bytes&&!*Bytes);
    ++QueryCalls;
    if (Step<RegistryAt) { SetLastError(ERROR_INVALID_HANDLE); return (LSTATUS)RegistryError; }
    *Type=REG_SZ; *Bytes=78; return ERROR_SUCCESS;
}
static CONFIGRET WINAPI TestStatus(PULONG Status,PULONG Number,DEVINST Instance,ULONG Flags)
{
    CHECK(Status&&Number&&Instance==Device.DevInst&&!Flags&&KeyHandle&&DeviceHandle&&NamePointer); ++CmCalls;
    if (Step<CmAt) return CR_NO_SUCH_DEVNODE;
    *Status=StatusBits|(Step<StatusAt?DN_HAS_PROBLEM:0); *Number=Step<NumberAt?1:0; return CR_SUCCESS;
}
static LSTATUS WINAPI TestRegClose(HKEY Key)
{
    CHECK(Key==(HKEY)KeyHandle&&KeyHandle&&CloseHandle(KeyHandle)); KeyHandle=NULL; ++KeysClosed;
    SetLastError(ERROR_INVALID_HANDLE); return ERROR_SUCCESS;
}
static BOOL WINAPI TestClose(HANDLE Handle)
{
    CHECK(Handle==DeviceHandle&&DeviceHandle&&CloseHandle(Handle)); DeviceHandle=NULL; ++FilesClosed;
    SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static BOOL WINAPI TestHeapFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    CHECK(Heap==ModuleHeap&&!Flags&&Pointer==NamePointer&&NamePointer&&HeapFree(Heap,Flags,Pointer));
    NamePointer=NULL; ++NamesFreed; SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{
    (void)Level;(void)Format;(void)Args; CHECK(FALSE); return GetLastError();
}
static BOOL Run(VOID) { return WaitForInterfaceWin7(DeviceSet,&Device,L"hosted-instance"); }
static VOID SuccessMatrix(VOID)
{
    const DWORD Delays[]={0,1,7,1499};
    for (DWORD boundary=0;boundary<8;++boundary) for (DWORD delay=0;delay<_countof(Delays);++delay)
    {
        Begin(); DWORD At=Delays[delay];
        switch(boundary)
        {
        case 0: KeyAt=At; break; case 1: NameAt=At; break; case 2: FileAt=At; break; case 3: RegistryAt=At; break;
        case 4: CmAt=At; break; case 5: StatusAt=At; break; case 6: NumberAt=At; break;
        default: KeyAt=NameAt=FileAt=RegistryAt=CmAt=StatusAt=NumberAt=At; break;
        }
        CHECK(Run()&&Sleeps==At&&Step==At&&KeysOpened==1&&FilesOpened==1&&NamesAllocated==1);
        CHECK(QueryCalls&&CmCalls); End();
    }
    const DWORD Errors[]={ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_INVALID_HANDLE,ERROR_MORE_DATA,ERROR_GEN_FAILURE,MAXDWORD};
    for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(); RegistryAt=3; RegistryError=Errors[error];
        CHECK(Run()&&Sleeps==3&&QueryCalls==4&&CmCalls==1); End();
    }
    Begin(); StatusBits=DN_STARTED|DN_DISABLEABLE; CHECK(Run()&&QueryCalls==1&&CmCalls==1); End();
}
static VOID ExhaustionMatrix(VOID)
{
    for (DWORD boundary=0;boundary<8;++boundary)
    {
        Begin();
        switch(boundary)
        {
        case 0: KeyAt=MAXDWORD; break; case 1: NameAt=MAXDWORD; break; case 2: FileAt=MAXDWORD; break;
        case 3: RegistryAt=MAXDWORD; break; case 4: CmAt=MAXDWORD; break; case 5: StatusAt=MAXDWORD; break;
        case 6: NumberAt=MAXDWORD; break; default: NullKey=TRUE; break;
        }
        CHECK(!Run()&&Sleeps==1499&&Step==1499);
        if (boundary<=2||boundary==7) CHECK(!QueryCalls&&!CmCalls);
        if (boundary==3) CHECK(QueryCalls==1500&&!CmCalls);
        if (boundary>=4&&boundary<=6) CHECK(QueryCalls==1500&&CmCalls==1500);
        if (boundary==7) CHECK(KeyCalls==1&&!KeysOpened); End();
    }
    const DWORD Errors[]={ERROR_FILE_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_INVALID_HANDLE,ERROR_MORE_DATA};
    for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(); RegistryAt=MAXDWORD; RegistryError=Errors[error];
        CHECK(!Run()&&QueryCalls==1500&&!CmCalls&&Sleeps==1499); End();
    }
}
static VOID CancellationMatrix(VOID)
{
    const DWORD Stops[]={1,2,1499};
    for (DWORD boundary=0;boundary<8;++boundary) for (DWORD stop=0;stop<_countof(Stops);++stop)
    {
        Begin(); CancelAt=Stops[stop];
        switch(boundary)
        {
        case 0: KeyAt=MAXDWORD; break; case 1: NameAt=MAXDWORD; break; case 2: FileAt=MAXDWORD; break;
        case 3: RegistryAt=MAXDWORD; break; case 4: CmAt=MAXDWORD; break; case 5: StatusAt=MAXDWORD; break;
        case 6: NumberAt=MAXDWORD; break; default: KeyAt=NameAt=FileAt=MAXDWORD; break;
        }
        CHECK(!Run()&&Sleeps==CancelAt&&Step==CancelAt-1); End();
    }
}
int main(int argc,char **argv)
{
    CHECK(argc<=2); ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if (argc==2)
    {
        CHECK(!strcmp(argv[1],"1")); Begin(); RegistryAt=1;
        BOOL Ready=Run(); BOOL Early=Ready&&QueryCalls==1;
        if (Early)
        {
            End(); CHECK(HeapDestroy(ModuleHeap)); puts("WIN7_INTERFACE_READY_BEFORE_REGISTRY_VALUE"); return 87;
        }
        CHECK(Ready&&QueryCalls==2&&CmCalls==1&&Sleeps==1); End();
    }
    else
    {
        DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for (DWORD repeat=0;repeat<3;++repeat) { SuccessMatrix(); ExhaustionMatrix(); CancellationMatrix(); }
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    }
    CHECK(HeapDestroy(ModuleHeap)); printf("PASS: actual Win7 interface readiness/ownership/cancellation (%lu scenarios)\n",Scenarios); return 0;
}
