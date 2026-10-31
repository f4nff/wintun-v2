/* SPDX-License-Identifier: GPL-2.0 */
/* Production name/bound-INF recovery; every device mutation is mocked. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <newdev.h>
extern const GUID GUID_DEVCLASS_NET;
extern const DEVPROPKEY DEVPKEY_Device_DriverInfPath;
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../api/v2/adapter.h"
#include "../../api/v2/driver_package.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
static const WCHAR *Names[]={L"VPN-Link",L"vpn-link",L"VPN-Link-old",L"WLAN",L"Other VPN",NULL,L"Other friendly name"};
static ULONG Quiet[7], DevicesRemoved, PackagesRemoved, DeviceAttempts, PackageAttempts, InfReads;
static ULONG Creates, Repairs, Mode, CallbackLive, EmptyDevices, FailDevice, FailPackage, MissingStore, MissingAll, MissingInf, MissingMapping;
static WCHAR Directory[MAX_PATH], Published[2][MAX_PATH], Store[2][MAX_PATH];
static HDEVINFO DeviceSet=(HDEVINFO)(ULONG_PTR)1;
static BOOL Match(ULONG i) {return i==0 || i==1 || i==5;}
static HDEVINFO WINAPI Devices(const GUID *guid,PCWSTR enumerator,HWND parent,DWORD flags)
{CHECK(IsEqualGUID(guid,&GUID_DEVCLASS_NET) && !enumerator && !parent && !flags);return DeviceSet;}
static BOOL WINAPI EnumDevice(HDEVINFO set,DWORD index,SP_DEVINFO_DATA *out)
{
    CHECK(set==DeviceSet);if(EmptyDevices || index==_countof(Names)) {SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}
    CHECK(index<_countof(Names) && out->cbSize==sizeof(*out));out->ClassGuid=GUID_DEVCLASS_NET;out->Reserved=index;return TRUE;
}
static BOOL WINAPI Property(HDEVINFO set,SP_DEVINFO_DATA *device,DWORD property,PDWORD type,PBYTE out,DWORD capacity,PDWORD returned)
{
    CHECK(set==DeviceSet && device->Reserved<_countof(Names));ULONG i=(ULONG)device->Reserved;
    CHECK(property==SPDRP_FRIENDLYNAME || property==SPDRP_DEVICEDESC);
    const WCHAR *value=property==SPDRP_FRIENDLYNAME?Names[i]:L"VPN-Link";
    if(!value) {SetLastError(ERROR_INVALID_DATA);return FALSE;}
    DWORD bytes=(DWORD)((wcslen(value)+1)*sizeof(WCHAR));CHECK(capacity>=bytes);
    memcpy(out,value,bytes);*returned=bytes;*type=REG_SZ;return TRUE;
}
static BOOL WINAPI InfProperty(HDEVINFO set,SP_DEVINFO_DATA *device,const DEVPROPKEY *key,DEVPROPTYPE *type,
    PBYTE out,DWORD capacity,PDWORD returned,DWORD flags)
{
    CHECK(set==DeviceSet && Match((ULONG)device->Reserved) && IsEqualGUID(&key->fmtid,&DEVPKEY_Device_DriverInfPath.fmtid) &&
        key->pid==DEVPKEY_Device_DriverInfPath.pid && !flags && !DeviceAttempts);
    InfReads++;
    if(MissingInf && device->Reserved==5) {SetLastError(ERROR_NOT_FOUND);return FALSE;}
    const WCHAR *value=device->Reserved==5?L"oem43.inf":L"OEM42.INF";
    DWORD bytes=(DWORD)((wcslen(value)+1)*sizeof(WCHAR));CHECK(capacity>=bytes);
    memcpy(out,value,bytes);*returned=bytes;*type=DEVPROP_TYPE_STRING;return TRUE;
}
static UINT WINAPI WindowsDirectory(PWSTR out,UINT capacity)
{CHECK(!wcscpy_s(out,capacity,Directory));return (UINT)wcslen(Directory);}
static BOOL WINAPI StoreLocation(PCWSTR path,PVOID reserved1,PCWSTR reserved2,PWSTR out,DWORD capacity,PDWORD required)
{
    CHECK(!reserved1 && !reserved2 && !required && !DeviceAttempts);
    if(MissingMapping) {SetLastError(ERROR_FILE_NOT_FOUND);return FALSE;}
    ULONG i=!_wcsicmp(path,Published[0])?0:1;CHECK(!_wcsicmp(path,Published[i]));
    CHECK(!wcscpy_s(out,capacity,Store[i]));return TRUE;
}
static BOOL WINAPI GetParameters(HDEVINFO set,SP_DEVINFO_DATA *device,SP_DEVINSTALL_PARAMS_W *parameters)
{CHECK(set==DeviceSet && Match((ULONG)device->Reserved) && parameters->cbSize==sizeof(*parameters));parameters->Flags=0;return TRUE;}
static BOOL WINAPI SetParameters(HDEVINFO set,SP_DEVINFO_DATA *device,SP_DEVINSTALL_PARAMS_W *parameters)
{CHECK(set==DeviceSet && Match((ULONG)device->Reserved) && (parameters->Flags&DI_QUIETINSTALL));Quiet[device->Reserved]++;return TRUE;}
static BOOL WINAPI RemoveDevice(HWND parent,HDEVINFO set,SP_DEVINFO_DATA *device,DWORD flags,BOOL *deferred)
{
    CHECK(!parent && set==DeviceSet && Match((ULONG)device->Reserved) && !flags && deferred && Quiet[device->Reserved]);
    CHECK(!CallbackLive && InfReads==3);DeviceAttempts++;*deferred=TRUE;
    if(FailDevice) {SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    DevicesRemoved++;return TRUE;
}
static BOOL WINAPI DestroySet(HDEVINFO set)
{CHECK(set==DeviceSet);SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static BOOL WINAPI RemovePackage(HWND parent,PCWSTR path,DWORD flags,BOOL *deferred)
{
    CHECK(!parent && !flags && deferred && !CallbackLive);PackageAttempts++;*deferred=TRUE;
    BOOL published=!_wcsicmp(path,Published[0]) || !_wcsicmp(path,Published[1]);
    CHECK(published || !wcscmp(path,Store[0]) || !wcscmp(path,Store[1]));
    if(FailPackage) {SetLastError(ERROR_SHARING_VIOLATION);return FALSE;}
    if(MissingAll || (MissingStore && !published)) {SetLastError(ERROR_FILE_NOT_FOUND);return FALSE;}
    PackagesRemoved++;return TRUE;
}
static DWORD WINAPI Attributes(PCWSTR path)
{CHECK(!_wcsicmp(path,Published[0]) || !_wcsicmp(path,Published[1]) || !wcscmp(path,Store[0]) || !wcscmp(path,Store[1]));if(MissingAll){SetLastError(ERROR_FILE_NOT_FOUND);return INVALID_FILE_ATTRIBUTES;}return FILE_ATTRIBUTE_NORMAL;}
#define SetupDiGetClassDevsW Devices
#define SetupDiEnumDeviceInfo EnumDevice
#define SetupDiGetDeviceRegistryPropertyW Property
#define SetupDiGetDevicePropertyW InfProperty
#define SetupDiGetDeviceInstallParamsW GetParameters
#define SetupDiSetDeviceInstallParamsW SetParameters
#define DiUninstallDevice RemoveDevice
#define SetupDiDestroyDeviceInfoList DestroySet
#define GetSystemWindowsDirectoryW WindowsDirectory
#define SetupGetInfDriverStoreLocationW StoreLocation
#define GetFileAttributesW Attributes
#define DiUninstallDriverW RemovePackage
#include "../../api/v2/driver_recovery.h"
#include "../../api/v2/adapter_install.h"
static void Reset(void)
{
    DevicesRemoved=PackagesRemoved=DeviceAttempts=PackageAttempts=InfReads=Creates=Repairs=CallbackLive=EmptyDevices=0;
    FailDevice=FailPackage=MissingStore=MissingAll=MissingInf=MissingMapping=0;memset(Quiet,0,sizeof(Quiet));
}
static PB_ADAPTER_DIAGNOSTICS_EX Report(void)
{PB_ADAPTER_DIAGNOSTICS_EX d={.Base={.Size=sizeof(d),.Schema=PB_ADAPTER_DIAG_SCHEMA_EX}};return d;}
static PB_RESULT Construct(const WCHAR *name,const WCHAR *instance,PB_ADAPTER **out,PB_ADAPTER_DIAGNOSTICS *d,PB_DRIVER_BINDING *binding)
{
    CHECK(!wcscmp(name,L"VPN-Link") && instance && !CallbackLive);Creates++;*out=NULL;CHECK(d->Attempts==Creates);
    if((Mode!=1 && Mode!=4 && Mode!=5 && Creates==2) || Mode==2) {*out=(PB_ADAPTER *)(ULONG_PTR)1;return PB_OK;}
    CallbackLive=1;d->Stage=Mode==5?PB_ADAPTER_PREPARE_PACKAGE:PB_ADAPTER_FIND_INTERFACE;d->Win32Error=ERROR_REVISION_MISMATCH;
    d->HResult=0x8007001f;d->ProblemCode=52;d->ProblemStatus=0xc0000428;d->ProblemFieldsKnown=3;
    if(Mode==6) {
        /* The failed name-validated devnode disappears at SwDeviceClose. */
        wcscpy_s(binding->Published,_countof(binding->Published),Published[0]);
        wcscpy_s(binding->Store,_countof(binding->Store),Store[0]);EmptyDevices=1;
    }
    CallbackLive=0;return Mode==4?PB_FULL:PB_UNSUPPORTED;
}
static BOOL Repair(const WCHAR *name,const PB_DRIVER_BINDING *binding,PB_ADAPTER_DIAGNOSTICS *d)
{
    CHECK(!CallbackLive && Creates==1 && !Repairs && d->FirstWin32Error==ERROR_REVISION_MISMATCH &&
        d->FirstHResult==0x8007001f && d->FirstProblemStatus==0xc0000428);Repairs++;
    if(Mode==3) FailDevice=1;
    return PbRemoveNamedAdapter(name,binding,d);
}
int main(void)
{
    CHECK(!wcscpy_s(Directory,_countof(Directory),L"C:\\PB-private-model"));
    for(ULONG i=0;i<2;++i) {
        CHECK(swprintf_s(Published[i],MAX_PATH,L"%s\\INF\\oem%lu.inf",Directory,42+i)>0);
        CHECK(swprintf_s(Store[i],MAX_PATH,L"%s\\Store\\package%lu\\packetbridge.inf",Directory,i)>0);
    }
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    _Static_assert(sizeof(PB_ADAPTER_DIAGNOSTICS)==80,"schema1");_Static_assert(sizeof(PB_ADAPTER_DIAGNOSTICS_EX)==180,"schema2");
    for(Mode=0;Mode<=6;++Mode) {
        Reset();PB_ADAPTER_DIAGNOSTICS_EX d=Report();PB_ADAPTER *out=NULL;PB_DRIVER_BINDING binding={0};
        PB_RESULT result=PbCreateWithRecovery(L"VPN-Link",L"test",&out,&d.Base,Construct,Repair,&binding);
        CHECK(Creates==(Mode==2 || Mode==4 || Mode==5?1u:2u) && Repairs==(Mode==2 || Mode==4 || Mode==5?0u:1u));
        if(Mode==1 || Mode==5) CHECK(result==PB_UNSUPPORTED && !out && GetLastError()==ERROR_REVISION_MISMATCH);
        else if(Mode==4) CHECK(result==PB_FULL && !out);else CHECK(result==PB_OK && out && d.Base.Stage==PB_ADAPTER_COMPLETE);
        if(Mode<=1) CHECK(DevicesRemoved==3 && PackagesRemoved==2 && d.MatchedDevices==3 && d.FoundPackages==2);
        if(Mode==3) CHECK(d.Base.RecoveryError==ERROR_ACCESS_DENIED && d.RecoveryStep==PB_RECOVERY_REMOVE_DEVICE && PackagesRemoved==2);
        if(Mode==6) CHECK(!DevicesRemoved && PackagesRemoved==1 && d.FoundPackages==1 && !d.MatchedDevices);
    }
    for(ULONG fault=0;fault<5;++fault) {
        Reset();PB_ADAPTER_DIAGNOSTICS_EX d=Report();
        if(fault==0) MissingMapping=1;if(fault==1) MissingStore=1;if(fault==2) MissingAll=1;if(fault==3) FailPackage=1;if(fault==4) MissingInf=1;
        BOOL okay=PbRemoveNamedAdapter(L"VPN-Link",NULL,&d.Base);
        CHECK(DevicesRemoved==3 && InfReads==3 && d.MatchedDevices==3);
        if(fault==3) CHECK(!okay && GetLastError()==ERROR_SHARING_VIOLATION && d.RecoveryStep==PB_RECOVERY_REMOVE_DRIVER && PackageAttempts==2);
        else CHECK(okay && !d.Base.RecoveryError);
        if(fault==0) CHECK(PackageAttempts==2 && PackagesRemoved==2);
        if(fault==1) CHECK(PackageAttempts==4 && PackagesRemoved==2);
        if(fault==2) CHECK(!PackagesRemoved && d.AlreadyAbsentPackages==2);
        if(fault==4) CHECK(PackagesRemoved==1);
    }
    Reset();PB_ADAPTER_DIAGNOSTICS_EX d=Report();CHECK(PbRemoveNamedAdapter(L"No such adapter",NULL,&d.Base) && !DeviceAttempts && !PackageAttempts);
    CHECK(!PbRecoveryOemName(L"wintun.inf") && !PbRecoveryOemName(L"oem.inf") && !PbRecoveryOemName(L"oem4.inf.bad"));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("PASS: production configured-name recovery: active+absent VPN-Link devices, exact case-insensitive names and description fallback, unrelated names excluded, bound INF snapshot before removal, shared OEM dedup, vanished failed devnode saved binding, store/OEM fallback, already-absent package, original raw errors and API step, partial cleanup retry, quiet nonnull outputs, one cleanup+retry, callback barrier and exact handle balance; no installed driver changed.");return 0;
}
