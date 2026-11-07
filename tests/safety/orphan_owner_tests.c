/* SPDX-License-Identifier: GPL-2.0 */
/* Real child/process handles, production owner/cleanup policy, hosted SetupAPI.
 * Every remove is an observer: no real device, registry or driver is changed. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "guard_memory.h"
#ifndef PB_ORPHAN_ADAPTER_HEADER_SOURCE
#define PB_ORPHAN_ADAPTER_HEADER_SOURCE "../../api/adapter.h"
#endif
#include PB_ORPHAN_ADAPTER_HEADER_SOURCE
#include "../../api/namespace.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static HANDLE WINAPI ProbeOpen(DWORD,BOOL,DWORD);
static BOOL WINAPI ProbeTimes(HANDLE,LPFILETIME,LPFILETIME,LPFILETIME,LPFILETIME);
static DWORD WINAPI ProbeWait(HANDLE,DWORD);
static BOOL WINAPI ProbeClose(HANDLE);
static HDEVINFO WINAPI DeviceSet(const GUID *,PCWSTR,HWND,DWORD,HDEVINFO,PCWSTR,PVOID);
static BOOL WINAPI DeviceEnum(HDEVINFO,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI DeviceProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE *,PBYTE,DWORD,PDWORD,DWORD);
static BOOL WINAPI DeviceRegistry(HDEVINFO,PSP_DEVINFO_DATA,DWORD,PDWORD,PBYTE,DWORD,PDWORD);
static BOOL WINAPI DeviceDestroy(HDEVINFO);
static CONFIGRET WINAPI NodeStatus(PULONG,PULONG,DEVINST,ULONG);
static DWORD ObserveLog(WINTUN_LOGGER_LEVEL,LPCWSTR,...);
static DWORD ObserveError(LPCWSTR,...);
static BOOL IsWindows7;
static HANDLE CleanupStop;
#define OpenProcess ProbeOpen
#define GetProcessTimes ProbeTimes
#define WaitForSingleObject ProbeWait
#define CloseHandle ProbeClose
#define SetupDiGetClassDevsExW DeviceSet
#define SetupDiEnumDeviceInfo DeviceEnum
#define SetupDiGetDevicePropertyW DeviceProperty
#define SetupDiGetDeviceRegistryPropertyW DeviceRegistry
#define SetupDiDestroyDeviceInfoList DeviceDestroy
#define CM_Get_DevNode_Status NodeStatus
#define LOG ObserveLog
#define LOG_LAST_ERROR ObserveError
#ifndef PB_ORPHAN_CLEANUP_SOURCE
#define PB_ORPHAN_CLEANUP_SOURCE "../../api/orphan_cleanup.h"
#endif
#include PB_ORPHAN_CLEANUP_SOURCE
#undef OpenProcess
#undef GetProcessTimes
#undef WaitForSingleObject
#undef CloseHandle
#undef SetupDiGetClassDevsExW
#undef SetupDiEnumDeviceInfo
#undef SetupDiGetDevicePropertyW
#undef SetupDiGetDeviceRegistryPropertyW
#undef SetupDiDestroyDeviceInfoList
#undef CM_Get_DevNode_Status
const DEVPROPKEY DEVPKEY_Wintun_Name={{0x3361c968,0x2f2e,0x4660,{0xb4,0x7e,0x69,0x9c,0xdc,0x4c,0x32,0xb9}},DEVPROPID_FIRST_USABLE+1};
enum { PROP_OK,PROP_FAILURE,PROP_SHORT,PROP_TYPE,PROP_LONG,PROP_MISSING };
static OWNING_PROCESS Owner;
static HANDLE Child,ChildGate,ChildThread,Probe,Info,InstallMutex;
static DWORD OpenError,TimesError,WaitResult,WaitError,PropMode,PropError,PropSize,NameMode,EnumError;
static DWORD EnumCalls,Removes,InfoOwned,ProbeOwned,MutexOwned,Scenarios,OpenCalls,WaitCalls,TimesCalls,CancelAt;
static DWORD RegistryMode,Multi,CurrentIndex,SetError,RemoveError,NameCalls;
static BOOL Cancelled,ProtectedOwner,ModernError,LegacyResult,ForceOpenFailure;
static HANDLE QueryEntered,QueryGate;
static BOOL GateRegistry;
static WCHAR LastName[PB_MAX_ADAPTER_NAME];
static VOID FinishChild(VOID)
{if(Child){CHECK(SetEvent(ChildGate)&&WaitForSingleObject(Child,10000)==WAIT_OBJECT_0);}}
static VOID Control(int code,const char *marker)
{FinishChild();fputs(marker,stderr);fputc('\n',stderr);ExitProcess((UINT)code);}
static VOID PauseQuery(VOID)
{CHECK(SetEvent(QueryEntered)&&WaitForSingleObject(QueryGate,5000)==WAIT_OBJECT_0);}
static HANDLE WINAPI ProbeOpen(DWORD access,BOOL inherit,DWORD pid)
{
    CHECK((access==PROCESS_QUERY_LIMITED_INFORMATION || access==(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE))&&!inherit&&pid==Owner.ProcessId);++OpenCalls;
    if(OpenError||ForceOpenFailure){SetLastError(OpenError);return NULL;}
    Probe=OpenProcess(access,inherit,pid);if(Probe)++ProbeOwned;return Probe;
}
static BOOL WINAPI ProbeTimes(HANDLE process,LPFILETIME creation,LPFILETIME exit,LPFILETIME kernel,LPFILETIME user)
{CHECK(process==Probe&&ProbeOwned==1);++TimesCalls;if(TimesError){SetLastError(TimesError);return FALSE;}return GetProcessTimes(process,creation,exit,kernel,user);}
static DWORD WINAPI ProbeWait(HANDLE process,DWORD timeout)
{CHECK(process==Probe&&ProbeOwned==1&&!timeout);++WaitCalls;if(WaitResult!=MAXDWORD){SetLastError(WaitError);return WaitResult;}return WaitForSingleObject(process,timeout);}
static BOOL WINAPI ProbeClose(HANDLE process)
{CHECK(process==Probe&&ProbeOwned==1);--ProbeOwned;Probe=NULL;return CloseHandle(process);}
static HDEVINFO WINAPI DeviceSet(const GUID *guid,PCWSTR enumerator,HWND parent,DWORD flags,HDEVINFO reuse,PCWSTR machine,PVOID reserved)
{
    CHECK(IsEqualGUID(guid,&GUID_DEVCLASS_NET)&&enumerator&&!parent&&!flags&&!reuse&&!machine&&!reserved&&!InfoOwned);
    if(SetError){SetLastError(SetError);return INVALID_HANDLE_VALUE;}
    Info=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(Info);++InfoOwned;return (HDEVINFO)Info;
}
static BOOL WINAPI DeviceEnum(HDEVINFO set,DWORD index,PSP_DEVINFO_DATA data)
{
    CHECK(set==(HDEVINFO)Info&&InfoOwned&&data->cbSize==sizeof(*data));++EnumCalls;
    if(EnumError){if(EnumCalls>1)Control(56,"ENUM_FAILURE_RETRIED_WITHOUT_PROGRESS");SetLastError(EnumError);return FALSE;}
    if(index>=(Multi?3u:1u)){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}data->DevInst=index+1;CurrentIndex=index+1;return TRUE;
}
static BOOL WINAPI DeviceProperty(HDEVINFO set,PSP_DEVINFO_DATA data,const DEVPROPKEY *key,DEVPROPTYPE *type,PBYTE bytes,DWORD capacity,PDWORD required,DWORD flags)
{
    CHECK(set==(HDEVINFO)Info&&InfoOwned&&data->DevInst==CurrentIndex&&!flags);
    if(IsEqualDevPropKey(*key,DEVPKEY_Wintun_OwningProcess)) {
        CHECK(capacity==sizeof(Owner));if(CancelAt==1)Cancelled=TRUE;
        if(Multi==1 && CurrentIndex==1){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        if(PropMode==PROP_FAILURE||PropMode==PROP_MISSING){SetLastError(PropMode==PROP_MISSING?ERROR_NOT_FOUND:PropError);return FALSE;}
        DWORD size=PropMode==PROP_SHORT?PropSize:PropMode==PROP_LONG?sizeof(Owner)+1:sizeof(Owner);
        *type=PropMode==PROP_TYPE?DEVPROP_TYPE_STRING:DEVPROP_TYPE_BINARY;if(required)*required=size;
        OWNING_PROCESS snapshot=Owner;if(Multi==1 && CurrentIndex==3)++snapshot.CreationTime.dwLowDateTime;
        memcpy(bytes,&snapshot,min(size,capacity));return TRUE;
    }
    CHECK(IsEqualDevPropKey(*key,DEVPKEY_Wintun_Name)&&capacity==PB_MAX_ADAPTER_NAME*sizeof(WCHAR));
    ++NameCalls;
    if(QueryGate&&!GateRegistry)PauseQuery();
    if(CancelAt==2)Cancelled=TRUE;
    if(NameMode) {
        *type=NameMode==1?DEVPROP_TYPE_BINARY:DEVPROP_TYPE_STRING;memset(bytes,0x41,capacity);
        DWORD size=NameMode==3?capacity+1:NameMode==4?1:capacity;if(required)*required=size;
        if(NameMode==6 && required)*required=0;
        if(NameMode==7)((WCHAR *)bytes)[PB_MAX_ADAPTER_NAME-1]=0;
        if(NameMode==8 && required)*required=3;
        if(NameMode==9 && required)*required=MAXDWORD;
        if(NameMode==5){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return TRUE;
    }
    static const WCHAR name[]=L"所有者-安全回收";*type=DEVPROP_TYPE_STRING;if(required)*required=sizeof(name);memcpy(bytes,name,sizeof(name));return TRUE;
}
static BOOL WINAPI DeviceRegistry(HDEVINFO set,PSP_DEVINFO_DATA data,DWORD property,PDWORD type,PBYTE bytes,DWORD capacity,PDWORD required)
{
    CHECK(set==(HDEVINFO)Info&&data->DevInst==CurrentIndex&&property==SPDRP_HARDWAREID);
    static const WCHAR ids[]=WINTUN_HWID L"\0";CHECK(capacity>=sizeof(ids));*type=REG_MULTI_SZ;memcpy(bytes,ids,sizeof(ids));if(required)*required=sizeof(ids);
    if(Multi==3) {
        if(CurrentIndex==1)*type=REG_BINARY;
        if(CurrentIndex==2){static const WCHAR foreign[]=L"Other\0";memcpy(bytes,foreign,sizeof(foreign));if(required)*required=sizeof(foreign);}
    }
    if(RegistryMode==1)*type=REG_BINARY;
    else if(RegistryMode==2){if(required)*required=sizeof(ids)-1;}
    else if(RegistryMode==3){if(required)*required=capacity+1;}
    else if(RegistryMode==4){memset(bytes,0x41,sizeof(ids));}
    else if(RegistryMode==5){memset(bytes,0,sizeof(ids));memcpy(bytes+2,ids,sizeof(ids));if(required)*required=sizeof(ids)+2;}
    else if(RegistryMode==6){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    else if(RegistryMode==7){if(required)*required=MAXDWORD;}
    else if(RegistryMode==8){if(required)*required=2;}
    else if(RegistryMode==9){memcpy(bytes+sizeof(ids),ids,sizeof(ids));if(required)*required=2*sizeof(ids);}
    else if(RegistryMode==10) {
        WCHAR *text=(WCHAR *)bytes;
        for(DWORD i=0;i<_countof(WINTUN_HWID)-1;++i)if(text[i]>=L'A'&&text[i]<=L'Z')text[i]+=L'a'-L'A';
    }
    else if(RegistryMode==11) {
        static const WCHAR foreign[]=WINTUN_HWID L"Suffix\0";
        memcpy(bytes,foreign,sizeof(foreign));if(required)*required=sizeof(foreign);
    }
    else if(RegistryMode>=12&&RegistryMode<=14) {
        WCHAR *text=(WCHAR *)bytes;DWORD count=capacity/sizeof(*text);
        for(DWORD i=0;i<count;++i)text[i]=RegistryMode==14?0:L'Z';
        text[count-2]=text[count-1]=0;
        if(RegistryMode==12) {
            DWORD start=count-_countof(WINTUN_HWID)-1;
            text[start-1]=0;memcpy(text+start,WINTUN_HWID,sizeof(WINTUN_HWID));
        }
        if(required)*required=capacity;
    }
    else if(RegistryMode==15) {
        static const WCHAR duplicate[]=WINTUN_HWID L"\0" WINTUN_HWID L"\0";
        memcpy(bytes,duplicate,sizeof(duplicate));if(required)*required=sizeof(duplicate);
    }
    else if(RegistryMode==16) {
        ((WCHAR *)bytes)[_countof(WINTUN_HWID)-2]=0;if(required)*required=sizeof(ids)-sizeof(WCHAR);
    }
    else if(RegistryMode==17) {
        memset(bytes,0x5a,capacity);((WCHAR *)bytes)[capacity/sizeof(WCHAR)-1]=0;if(required)*required=capacity;
    }
    if(QueryGate&&GateRegistry)PauseQuery();
    if(CancelAt==3)Cancelled=TRUE;return TRUE;
}
static BOOL WINAPI DeviceDestroy(HDEVINFO set)
{CHECK(set==(HDEVINFO)Info&&InfoOwned==1);--InfoOwned;Info=NULL;return CloseHandle((HANDLE)set);}
static CONFIGRET WINAPI NodeStatus(PULONG status,PULONG problem,DEVINST node,ULONG flags)
{CHECK(node==CurrentIndex&&!flags);if(CancelAt==4)Cancelled=TRUE;*status=Multi==2&&CurrentIndex==1?0:DN_HAS_PROBLEM;*problem=1;return ModernError||(Multi==2&&CurrentIndex==2)?CR_ACCESS_DENIED:CR_SUCCESS;}
BOOL AdapterCleanupCancelled(VOID){return Cancelled||(CleanupStop&&WaitForSingleObject(CleanupStop,0)==WAIT_OBJECT_0);}
HANDLE NamespaceTakeDeviceInstallationMutexCancelable(HANDLE stop)
{CHECK(stop==CleanupStop&&!MutexOwned);InstallMutex=CreateMutexW(NULL,FALSE,NULL);CHECK(InstallMutex&&WaitForSingleObject(InstallMutex,1000)==WAIT_OBJECT_0);++MutexOwned;return InstallMutex;}
VOID NamespaceReleaseMutex(HANDLE mutex)
{CHECK(mutex==InstallMutex&&MutexOwned==1&&ReleaseMutex(mutex)&&CloseHandle(mutex));InstallMutex=NULL;--MutexOwned;}
BOOL AdapterRemoveInstance(HDEVINFO set,SP_DEVINFO_DATA *data)
{
    CHECK(set==(HDEVINFO)Info&&InfoOwned&&data->DevInst==CurrentIndex);
    if(Multi&&CurrentIndex<3)Control(53,"UNPROVEN_DEVICE_RECLAIMED");
    if(ProtectedOwner)Control(53,"LIVE_OWNER_RECLAIMED_WITHOUT_PROOF");
    if(AdapterCleanupCancelled())Control(58,"DESTRUCTIVE_CLEANUP_AFTER_CANCELLATION");
    if(ModernError)Control(59,"UNKNOWN_DEVNODE_RECLAIMED");
    if(RemoveError){SetLastError(RemoveError);return FALSE;}++Removes;return TRUE;
}
static DWORD ObserveLog(WINTUN_LOGGER_LEVEL level,LPCWSTR format,...)
{
    (void)level;(void)format;va_list args;va_start(args,format);LPCWSTR name=va_arg(args,LPCWSTR);va_end(args);
    if(wcsnlen_s(name,PB_MAX_ADAPTER_NAME)==PB_MAX_ADAPTER_NAME)Control(57,"UNTERMINATED_DEVICE_NAME_LOGGED");
    CHECK(!wcscpy_s(LastName,ARRAYSIZE(LastName),name));return 0;
}
static DWORD ObserveError(LPCWSTR format,...){(void)format;return GetLastError();}
static VOID StartChild(DWORD code)
{
    WCHAR exe[MAX_PATH],command[MAX_PATH+120];CHECK(GetModuleFileNameW(NULL,exe,MAX_PATH));
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};ChildGate=CreateEventW(&sa,TRUE,FALSE,NULL);CHECK(ChildGate);
    CHECK(swprintf_s(command,ARRAYSIZE(command),L"\"%s\" --owner-child %llx %lu",exe,(unsigned long long)(uintptr_t)ChildGate,code)>0);
    STARTUPINFOW start={.cb=sizeof(start),.dwFlags=STARTF_USESHOWWINDOW,.wShowWindow=SW_HIDE};PROCESS_INFORMATION process={0};
    CHECK(CreateProcessW(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&start,&process));Child=process.hProcess;ChildThread=process.hThread;
    FILETIME unused;Owner.ProcessId=process.dwProcessId;CHECK(GetProcessTimes(Child,&Owner.CreationTime,&unused,&unused,&unused));
}
static VOID EndChild(VOID)
{FinishChild();CHECK(CloseHandle(ChildThread)&&CloseHandle(Child)&&CloseHandle(ChildGate));Child=ChildThread=ChildGate=NULL;}
static VOID Reset(VOID)
{
    CHECK(!InfoOwned&&!ProbeOwned&&!MutexOwned);OpenError=TimesError=WaitError=EnumError=NameMode=0;WaitResult=MAXDWORD;
    PropMode=PROP_OK;PropError=ERROR_ACCESS_DENIED;PropSize=0;OpenCalls=WaitCalls=TimesCalls=EnumCalls=Removes=CancelAt=0;
    RegistryMode=Multi=CurrentIndex=SetError=RemoveError=NameCalls=0;
    Cancelled=ProtectedOwner=ModernError=ForceOpenFailure=FALSE;LegacyResult=FALSE;IsWindows7=TRUE;LastName[0]=0;
    CHECK(!CleanupStop&&!QueryGate&&!QueryEntered);GateRegistry=FALSE;
}
static VOID Scan(BOOL legacy)
{
    if(legacy) {
        HANDLE mutex=NamespaceTakeDeviceInstallationMutexCancelable(CleanupStop);
#ifdef PB_ORPHAN_BASELINE
        AdapterCleanupLegacyDevices();
#else
        LegacyResult=AdapterCleanupLegacyDevices();
#endif
        NamespaceReleaseMutex(mutex);
    }else AdapterCleanupOrphanedDevices();
    CHECK(!InfoOwned&&!ProbeOwned&&!MutexOwned);++Scenarios;
}
static VOID Case(DWORD kind)
{
    Reset();StartChild(kind==2?259:0);
    if(kind==1){OpenError=ERROR_ACCESS_DENIED;ProtectedOwner=TRUE;Scan(FALSE);}
    else if(kind==2){FinishChild();Scan(FALSE);if(!Removes)Control(54,"EXITED_OWNER_RETAINED_WITH_PROCESS_HANDLE");}
    else if(kind==3){PropMode=PROP_FAILURE;ProtectedOwner=TRUE;Scan(FALSE);}
    else if(kind==4){EnumError=ERROR_NOT_ENOUGH_MEMORY;Scan(FALSE);CHECK(EnumCalls==1&&!Removes);}
    else if(kind==5){++Owner.CreationTime.dwLowDateTime;NameMode=1;Scan(FALSE);CHECK(Removes==1);}
    else if(kind==6){++Owner.CreationTime.dwLowDateTime;CancelAt=2;Scan(FALSE);CHECK(!Removes);}
    else if(kind==7){IsWindows7=FALSE;ModernError=TRUE;Scan(FALSE);CHECK(!Removes);}
    EndChild();
}
static VOID AliveMatrix(VOID)
{
    Reset();StartChild(0);OWNING_PROCESS saved=Owner;
    const DWORD openErrors[]={0,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_TOO_MANY_OPEN_FILES,ERROR_INVALID_HANDLE,ERROR_GEN_FAILURE};
    for(DWORD i=0;i<ARRAYSIZE(openErrors);++i) {
        Reset();Owner=saved;OpenError=openErrors[i];ForceOpenFailure=ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&!WaitCalls&&!TimesCalls);
    }
    const DWORD statuses[]={WAIT_FAILED,WAIT_ABANDONED,WAIT_OBJECT_0+1};
    for(DWORD i=0;i<ARRAYSIZE(statuses);++i)for(DWORD zero=0;zero<2;++zero) {
        Reset();Owner=saved;WaitResult=statuses[i];WaitError=zero?0:ERROR_ACCESS_DENIED;ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes);
    }
    for(DWORD i=0;i<ARRAYSIZE(openErrors);++i)if(openErrors[i]) {
        Reset();Owner=saved;TimesError=openErrors[i];ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&TimesCalls==1);
    }
    Reset();Owner=saved;ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&OpenCalls==1&&WaitCalls==1&&TimesCalls==1);
    for(DWORD size=0;size<=sizeof(Owner)+1;++size)if(size!=sizeof(Owner)) {
        Reset();Owner=saved;PropMode=PROP_SHORT;PropSize=size;ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&!OpenCalls&&!NameCalls);
    }
    for(DWORD mode=PROP_FAILURE;mode<=PROP_LONG;++mode) {
        Reset();Owner=saved;PropMode=mode;PropSize=sizeof(Owner)-1;ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&!OpenCalls);
    }
    for(DWORD i=0;i<ARRAYSIZE(openErrors);++i) {
        Reset();Owner=saved;PropMode=PROP_FAILURE;PropError=openErrors[i];ProtectedOwner=TRUE;Scan(FALSE);CHECK(!Removes&&!OpenCalls);
    }
    for(DWORD reuse=0;reuse<3;++reuse) {
        Reset();Owner=saved;
        if(!reuse)++Owner.CreationTime.dwLowDateTime;else if(reuse==1)--Owner.CreationTime.dwLowDateTime;else Owner.CreationTime.dwHighDateTime^=1;
        Scan(FALSE);CHECK(Removes==1&&OpenCalls==1&&ProbeOwned==0);
    }
    Reset();Owner=saved;Owner.ProcessId=0;Scan(FALSE);CHECK(Removes==1&&!ProbeOwned);
    Reset();Owner=saved;PropMode=PROP_MISSING;Scan(FALSE);CHECK(Removes==1&&!OpenCalls);
    for(DWORD modern=0;modern<2;++modern)for(DWORD name=0;name<=9;++name) {
        Reset();Owner=saved;++Owner.CreationTime.dwLowDateTime;IsWindows7=!modern;NameMode=name;Scan(FALSE);CHECK(Removes==1&&NameCalls==1);
        if(name && name!=7)CHECK(!wcscmp(LastName,L"<unknown>"));
        if(name==7)CHECK(wcslen(LastName)==PB_MAX_ADAPTER_NAME-1);
    }
    const DWORD enumErrors[]={ERROR_NO_MORE_ITEMS,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INVALID_HANDLE,ERROR_GEN_FAILURE};
    for(DWORD path=0;path<3;++path)for(DWORD error=0;error<ARRAYSIZE(enumErrors);++error) {
        Reset();Owner=saved;IsWindows7=path!=1;EnumError=enumErrors[error];Scan(path==2);CHECK(EnumCalls==1&&!Removes&&!OpenCalls);
        if(path==2)CHECK(LegacyResult==(error==0));
    }
    for(DWORD path=0;path<3;++path)for(DWORD i=1;i<ARRAYSIZE(openErrors);++i) {
        Reset();Owner=saved;IsWindows7=path!=1;SetError=openErrors[i];Scan(path==2);CHECK(!EnumCalls&&!Removes&&!OpenCalls);if(path==2)CHECK(!LegacyResult);
    }
    for(DWORD stage=1;stage<=4;++stage) {
        Reset();Owner=saved;++Owner.CreationTime.dwLowDateTime;CancelAt=stage;
        if(stage==4)IsWindows7=FALSE;Scan(stage==3);CHECK(Cancelled&&!Removes&&EnumCalls==1);if(stage==3)CHECK(!LegacyResult);
    }
    for(DWORD path=0;path<3;++path) {
        Reset();Owner=saved;IsWindows7=path!=1;Cancelled=TRUE;Scan(path==2);CHECK(!EnumCalls&&!Removes);if(path==2)CHECK(!LegacyResult);
    }
    for(DWORD mode=0;mode<=9;++mode) {
        Reset();Owner=saved;RegistryMode=mode;if(mode)ProtectedOwner=TRUE;Scan(TRUE);
        CHECK(mode?!Removes:Removes==1);if(mode)CHECK(!LegacyResult);
    }
    for(DWORD mode=10;mode<=17;++mode) {
        BOOL match=mode==10||mode==12||mode==15;
        Reset();Owner=saved;RegistryMode=mode;ProtectedOwner=!match;Scan(TRUE);
        CHECK(Removes==(DWORD)match&&LegacyResult==(mode!=17));
    }
    Reset();Owner=saved;RemoveError=ERROR_ACCESS_DENIED;Scan(TRUE);CHECK(!Removes&&!LegacyResult);
    Reset();Owner=saved;Scan(TRUE);CHECK(Removes==1&&LegacyResult); /* A later sweep can recover. */
    for(DWORD multi=1;multi<=3;++multi) {
        Reset();Owner=saved;Multi=multi;IsWindows7=multi!=2;Scan(multi==3);CHECK(Removes==1&&EnumCalls==4);
    }
    EndChild();
}
static DWORD WINAPI ScanWorker(VOID *opaque)
{Scan((BOOL)(uintptr_t)opaque);return 0;}
static VOID ConcurrentCancel(DWORD path,BOOL cancel)
{
    Reset();StartChild(0);++Owner.CreationTime.dwLowDateTime;IsWindows7=path!=1;GateRegistry=path==2;
    CleanupStop=CreateEventW(NULL,TRUE,FALSE,NULL);QueryEntered=CreateEventW(NULL,FALSE,FALSE,NULL);QueryGate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(CleanupStop&&QueryEntered&&QueryGate);
    HANDLE worker=CreateThread(NULL,0,ScanWorker,(VOID *)(uintptr_t)(path==2),0,NULL);CHECK(worker);
    CHECK(WaitForSingleObject(QueryEntered,5000)==WAIT_OBJECT_0&&InfoOwned==1&&MutexOwned==1&&!ProbeOwned&&!Removes);
    if(cancel)CHECK(SetEvent(CleanupStop));
    CHECK(SetEvent(QueryGate)&&WaitForSingleObject(worker,5000)==WAIT_OBJECT_0&&CloseHandle(worker));
    CHECK(Removes==(cancel?0u:1u)&&!InfoOwned&&!ProbeOwned&&!MutexOwned);
#ifndef PB_ORPHAN_BASELINE
    if(path==2)CHECK(LegacyResult==!cancel);
#endif
    CHECK(CloseHandle(CleanupStop)&&CloseHandle(QueryGate)&&CloseHandle(QueryEntered));CleanupStop=QueryEntered=QueryGate=NULL;EndChild();
}
#ifndef PB_ORPHAN_BASELINE
static VOID GuardedReaders(VOID)
{
    Reset();StartChild(0);
    PB_TEST_GUARD record=PbTestGuardCreate(sizeof(Owner));OWNING_PROCESS *snapshot=(OWNING_PROCESS *)PbTestGuardSpan(&record,sizeof(Owner));*snapshot=Owner;
    DWORD old;CHECK(VirtualProtect(snapshot,sizeof(*snapshot),PAGE_READONLY,&old));
    CHECK(!ProcessIsStale(snapshot)&&!ProbeOwned);++Scenarios;
    SP_DEVINFO_DATA data={.cbSize=sizeof(data),.DevInst=1};CurrentIndex=1;
    HDEVINFO set=DeviceSet(&GUID_DEVCLASS_NET,WINTUN_ENUMERATOR,NULL,0,NULL,NULL,NULL);
    PB_TEST_GUARD text=PbTestGuardCreate(PB_MAX_ADAPTER_NAME*sizeof(WCHAR));WCHAR *name=(WCHAR *)PbTestGuardSpan(&text,PB_MAX_ADAPTER_NAME*sizeof(WCHAR));
    for(DWORD mode=0;mode<=9;++mode){NameMode=mode;OrphanDeviceName(set,&data,name);CHECK(wcsnlen_s(name,PB_MAX_ADAPTER_NAME)<PB_MAX_ADAPTER_NAME);++Scenarios;}
    CHECK(DeviceDestroy(set));PbTestGuardDestroy(&text);PbTestGuardDestroy(&record);EndChild();
}
#endif
static VOID ExitedMatrix(VOID)
{
    const DWORD codes[]={0,259};
    for(DWORD i=0;i<ARRAYSIZE(codes);++i) {
        Reset();StartChild(codes[i]);FinishChild();DWORD code;CHECK(GetExitCodeProcess(Child,&code)&&code==codes[i]);
        Scan(FALSE);CHECK(Removes==1&&WaitCalls==1&&!TimesCalls&&!ProbeOwned);
        Reset();Scan(FALSE);CHECK(Removes==1);EndChild();
    }
}
int wmain(int argc,wchar_t **argv)
{
    if(argc==4&&!wcscmp(argv[1],L"--owner-child")) {
        HANDLE gate=(HANDLE)(uintptr_t)_wcstoui64(argv[2],NULL,16);DWORD code=wcstoul(argv[3],NULL,10);
        DWORD waited=WaitForSingleObject(gate,15000);CloseHandle(gate);return waited==WAIT_OBJECT_0?(int)code:60;
    }
    _Static_assert(sizeof(OWNING_PROCESS)==12,"frozen Win7 owner property");
    if(argc>1)Case(wcstoul(argv[1],NULL,10));
    else {
        for(DWORD i=1;i<=7;++i)Case(i);
        DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(DWORD pass=0;pass<3;++pass) {
            AliveMatrix();ExitedMatrix();
            for(DWORD path=0;path<3;++path)for(DWORD cancel=0;cancel<2;++cancel)ConcurrentCancel(path,cancel);
#ifndef PB_ORPHAN_BASELINE
            GuardedReaders();
#endif
            CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after<=before);
        }
    }
    printf("PASS: %lu actual orphan cleanup schedules; live/unknown owners protected, retained exited process reclaimed, enum/cancel/name boundaries and all probe/list/mutex/child handles closed.\n",Scenarios);return 0;
}
