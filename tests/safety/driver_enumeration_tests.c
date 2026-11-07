/* SPDX-License-Identifier: GPL-2.0 */
/* Actual driver scan/install/delete/rollback implementation. All SetupAPI,
 * resource/file boundaries are hosted; no device, registry or file is changed.
 * Set/mutex stand-ins own native handles, list nodes own native heap memory,
 * PathCombine/PathFindFileName and CRT formatting remain native. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <SetupAPI.h>
#include <Shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include <devguid.h>
#include "../../api/driver.h"
#include "../../api/adapter.h"
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
#define HeapFree TestHeapFree
#include "../../api/logger.h"
#include "../../api/resource.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
struct _SP_DEVINFO_DATA_LIST { SP_DEVINFO_DATA Data; struct _SP_DEVINFO_DATA_LIST *Next; };
static HDEVINFO WINAPI TestCreateSet(const GUID *,HWND,PCWSTR,PVOID);
static BOOL WINAPI TestCreateDevice(HDEVINFO,PCWSTR,const GUID *,PCWSTR,HWND,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestHardware(HDEVINFO,PSP_DEVINFO_DATA,DWORD,const BYTE *,DWORD);
static BOOL WINAPI TestBuild(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestEnum(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,PSP_DRVINFO_DATA_W);
static HDEVINFO WINAPI TestExisting(const GUID *,PCWSTR,HWND,DWORD,HDEVINFO,PCWSTR,PVOID);
static BOOL WINAPI TestDetail(HDEVINFO,PSP_DEVINFO_DATA,PSP_DRVINFO_DATA_W,PSP_DRVINFO_DETAIL_DATA_W,DWORD,PDWORD);
static BOOL WINAPI TestDestroyDrivers(HDEVINFO,PSP_DEVINFO_DATA,DWORD);
static BOOL WINAPI TestDestroySet(HDEVINFO);
static BOOL WINAPI TestUninstall(PCWSTR,DWORD,PVOID);
static BOOL WINAPI TestStage(PCWSTR,PCWSTR,DWORD,DWORD,PWSTR,DWORD,PDWORD,PWSTR *);
static LPWSTR WINAPI TestCombine(LPWSTR,LPCWSTR,LPCWSTR);
static BOOL WINAPI TestDelete(LPCWSTR);
static BOOL WINAPI TestRemove(LPCWSTR);
static BOOL DisableAllOurAdapters(HDEVINFO,SP_DEVINFO_DATA_LIST **);
static BOOL EnableAllOurAdapters(HDEVINFO,SP_DEVINFO_DATA_LIST *);
static BOOL EnsureWintunUnloaded(VOID);
HANDLE NamespaceTakeDriverInstallationMutex(VOID);
VOID NamespaceReleaseMutex(HANDLE);
#define SetupDiCreateDeviceInfoListExW TestCreateSet
#define SetupDiCreateDeviceInfoW TestCreateDevice
#define SetupDiSetDeviceRegistryPropertyW TestHardware
#define SetupDiBuildDriverInfoList TestBuild
#define SetupDiEnumDriverInfoW TestEnum
#define SetupDiGetClassDevsExW TestExisting
#define SetupDiGetDriverInfoDetailW TestDetail
#define SetupDiDestroyDriverInfoList TestDestroyDrivers
#define SetupDiDestroyDeviceInfoList TestDestroySet
#define SetupUninstallOEMInfW TestUninstall
#define SetupCopyOEMInfW TestStage
#define PathCombineW TestCombine
#define DeleteFileW TestDelete
#define RemoveDirectoryW TestRemove
#define WINTUN_INF_FILETIME { 1, 2 }
#define WINTUN_INF_VERSION ((DWORDLONG)2 << 48)
#ifndef PB_DRIVER_INSTALL_SOURCE
#define PB_DRIVER_INSTALL_SOURCE "../../api/driver_install.h"
#endif
#include PB_DRIVER_INSTALL_SOURCE
#undef SetupDiCreateDeviceInfoListExW
#undef SetupDiCreateDeviceInfoW
#undef SetupDiSetDeviceRegistryPropertyW
#undef SetupDiBuildDriverInfoList
#undef SetupDiEnumDriverInfoW
#undef SetupDiGetClassDevsExW
#undef SetupDiGetDriverInfoDetailW
#undef SetupDiDestroyDriverInfoList
#undef SetupDiDestroyDeviceInfoList
#undef SetupUninstallOEMInfW
#undef SetupCopyOEMInfW
#undef PathCombineW
#undef DeleteFileW
#undef RemoveDirectoryW
#undef HeapFree

HANDLE ModuleHeap;
USHORT NativeMachine=IMAGE_FILE_PROCESS;
BOOL IsWindows7=TRUE;
static HANDLE Mutex,Primary,Existing;
static BOOL DriverList,Directory,Files[3],PartialCopy;
static DWORD Phase,Error,EnumError,EnumFailAt,Records,OldMask,DetailFailMask,OversizeMask,UninstallFailMask;
static DWORD DetailError,UninstallError,FailCombineAt,FailCopyAt,CancelAt;
static DWORD EnumCalls,DetailCalls,Uninstalls,Stages,Temps,Combines,Copies,Deletes,Removes,Sleeps;
static DWORD Runtime,RuntimeAcquires,RuntimeReleases,SetsCreated,SetsDestroyed,ListsBuilt,ListsDestroyed,MutexesOpened,MutexesClosed;
static DWORD NodesAllocated,NodesFreed,Enables,Disables,Logs,Scenarios;
static BOOL Deleting;
static SP_DEVINFO_DATA_LIST *Nodes[4];
static WCHAR LastLog[1024];
static const WCHAR Root[]=L"C:\\hosted-owner";
static VOID Begin(BOOL Delete)
{
    CHECK(!Mutex&&!Primary&&!Existing&&!DriverList&&!Directory&&!Runtime);
    for (DWORD i=0;i<3;++i) CHECK(!Files[i]);
    for (DWORD i=0;i<_countof(Nodes);++i) CHECK(!Nodes[i]);
    Deleting=Delete; Phase=0; Error=ERROR_ACCESS_DENIED; EnumError=ERROR_NOT_ENOUGH_MEMORY; EnumFailAt=MAXDWORD;
    Records=OldMask=DetailFailMask=OversizeMask=UninstallFailMask=FailCombineAt=FailCopyAt=CancelAt=0;
    DetailError=ERROR_ACCESS_DENIED; UninstallError=ERROR_SHARING_VIOLATION; PartialCopy=FALSE;
    EnumCalls=DetailCalls=Uninstalls=Stages=Temps=Combines=Copies=Deletes=Removes=Sleeps=0;
    RuntimeAcquires=RuntimeReleases=SetsCreated=SetsDestroyed=ListsBuilt=ListsDestroyed=MutexesOpened=MutexesClosed=0;
    NodesAllocated=NodesFreed=Enables=Disables=Logs=0; LastLog[0]=0;
}
static VOID End(VOID)
{
    CHECK(!Mutex&&!Primary&&!Existing&&!DriverList&&!Directory&&!Runtime&&RuntimeAcquires==RuntimeReleases&&
        SetsCreated==SetsDestroyed&&ListsBuilt==ListsDestroyed&&MutexesOpened==MutexesClosed&&NodesAllocated==NodesFreed);
    for (DWORD i=0;i<3;++i) CHECK(!Files[i]);
    for (DWORD i=0;i<_countof(Nodes);++i) CHECK(!Nodes[i]);
    ++Scenarios;
}
static BOOL Fail(DWORD At)
{ if (Phase!=At) return FALSE; SetLastError(Error); return TRUE; }
static VOID FormatLog(LPCWSTR Format,va_list Args)
{ ++Logs; _vsnwprintf_s(LastLog,_countof(LastLog),_TRUNCATE,Format,Args); SetLastError(ERROR_INVALID_DATA); }
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{ (void)Level; DWORD Saved=GetLastError(); FormatLog(Format,Args); SetLastError(Saved); return Saved; }
DWORD LoggerErrorV(DWORD Code,LPCWSTR Format,va_list Args)
{ FormatLog(Format,Args); return Code; }
BOOL MainRuntimeAcquire(VOID)
{ CHECK(Deleting&&!Runtime); if(Fail(1))return FALSE; ++Runtime; ++RuntimeAcquires; return TRUE; }
VOID MainRuntimeRelease(VOID)
{ CHECK(Deleting&&Runtime==1); --Runtime; ++RuntimeReleases; SetLastError(ERROR_INVALID_HANDLE); }
VOID AdapterCleanupOrphanedDevices(VOID) { CHECK(Deleting&&Runtime==1); }
BOOL MainOperationSleep(DWORD Delay)
{ CHECK(!Delay); ++Sleeps; if(Sleeps!=CancelAt)return TRUE; SetLastError(ERROR_CANCELLED); return FALSE; }
HANDLE NamespaceTakeDriverInstallationMutex(VOID)
{
    CHECK(!Mutex); if(Fail(2))return NULL;
    Mutex=CreateMutexW(NULL,TRUE,NULL); CHECK(Mutex); ++MutexesOpened; return Mutex;
}
VOID NamespaceReleaseMutex(HANDLE Handle)
{ CHECK(Handle==Mutex&&Mutex&&ReleaseMutex(Handle)&&CloseHandle(Handle)); Mutex=NULL; ++MutexesClosed; SetLastError(ERROR_INVALID_HANDLE); }
static HDEVINFO WINAPI TestCreateSet(const GUID *Class,HWND Window,PCWSTR Machine,PVOID Reserved)
{
    CHECK(IsEqualGUID(Class,&GUID_DEVCLASS_NET)&&!Window&&!Machine&&!Reserved&&!Primary);
    if(Fail(3))return INVALID_HANDLE_VALUE;
    Primary=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(Primary); ++SetsCreated; return (HDEVINFO)Primary;
}
static BOOL WINAPI TestCreateDevice(HDEVINFO Set,PCWSTR Id,const GUID *Class,PCWSTR Description,HWND Window,DWORD Flags,PSP_DEVINFO_DATA Info)
{
    CHECK(Set==(HDEVINFO)Primary&&!wcscmp(Id,WINTUN_HWID)&&IsEqualGUID(Class,&GUID_DEVCLASS_NET)&&!Description&&!Window&&
        Flags==DICD_GENERATE_ID&&Info->cbSize==sizeof(*Info)); if(Fail(4))return FALSE;
    Info->DevInst=1; Info->ClassGuid=*Class; return TRUE;
}
static BOOL WINAPI TestHardware(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Property,const BYTE *Data,DWORD Bytes)
{
    CHECK(Set==(HDEVINFO)Primary&&Info->DevInst==1&&Property==SPDRP_HARDWAREID&&Bytes==(sizeof(WINTUN_HWID)+sizeof(WCHAR))&&
        !wcscmp((LPCWSTR)Data,WINTUN_HWID)&&!((LPCWSTR)Data)[Bytes/sizeof(WCHAR)-1]); return !Fail(5);
}
static BOOL WINAPI TestBuild(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Type)
{
    CHECK(Set==(HDEVINFO)Primary&&Info->DevInst==1&&Type==SPDIT_COMPATDRIVER&&!DriverList);
    if(Fail(6))return FALSE; DriverList=TRUE; ++ListsBuilt; return TRUE;
}
static BOOL WINAPI TestEnum(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Type,DWORD Index,PSP_DRVINFO_DATA_W Driver)
{
    CHECK(Set==(HDEVINFO)Primary&&Info->DevInst==1&&Type==SPDIT_COMPATDRIVER&&DriverList&&Driver->cbSize==sizeof(*Driver)); ++EnumCalls;
    if(Index==EnumFailAt){SetLastError(EnumError);return FALSE;}
    if(Index>=Records){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}
    Driver->DriverDate=(FILETIME)WINTUN_INF_FILETIME; Driver->DriverVersion=WINTUN_INF_VERSION+Index+1;
    if(OldMask&(1u<<Index))Driver->DriverVersion=WINTUN_INF_VERSION-1;
    Driver->Reserved=Index; return TRUE;
}
static HDEVINFO WINAPI TestExisting(const GUID *Class,PCWSTR Enumerator,HWND Window,DWORD Flags,HDEVINFO Set,PCWSTR Machine,PVOID Reserved)
{
    CHECK(IsEqualGUID(Class,&GUID_DEVCLASS_NET)&&!wcscmp(Enumerator,WINTUN_ENUMERATOR)&&!Window&&Flags==DIGCF_PRESENT&&!Set&&!Machine&&!Reserved&&!Existing);
    if(Fail(7))return INVALID_HANDLE_VALUE;
    Existing=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(Existing); ++SetsCreated; return (HDEVINFO)Existing;
}
static BOOL DisableAllOurAdapters(HDEVINFO Set,SP_DEVINFO_DATA_LIST **Head)
{
    CHECK(Set==(HDEVINFO)Existing&&Existing&&!*Head&&!Disables); ++Disables;
    for (DWORD i=0;i<2;++i)
    {
        Nodes[i]=Zalloc(sizeof(*Nodes[i])); CHECK(Nodes[i]); ++NodesAllocated;
        Nodes[i]->Data.cbSize=sizeof(Nodes[i]->Data); Nodes[i]->Data.DevInst=10+i; Nodes[i]->Next=*Head; *Head=Nodes[i];
    }
    return TRUE;
}
static BOOL EnableAllOurAdapters(HDEVINFO Set,SP_DEVINFO_DATA_LIST *Head)
{
    CHECK(Set==(HDEVINFO)Existing&&Existing&&Head==Nodes[1]&&Head->Next==Nodes[0]&&!Head->Next->Next); ++Enables;
    return TRUE;
}
static BOOL EnsureWintunUnloaded(VOID) { CHECK(Existing&&Disables==1); return TRUE; }
static BOOL WINAPI TestDetail(HDEVINFO Set,PSP_DEVINFO_DATA Info,PSP_DRVINFO_DATA_W Driver,PSP_DRVINFO_DETAIL_DATA_W Detail,DWORD Capacity,PDWORD Required)
{
    CHECK(Set==(HDEVINFO)Primary&&Info->DevInst==1&&DriverList&&Capacity==0x2000&&Detail->cbSize==sizeof(*Detail)&&Required); ++DetailCalls;
    DWORD Index=(DWORD)Driver->Reserved; CHECK(Index<4);
    if(DetailFailMask&(1u<<Index)){SetLastError(DetailError);return FALSE;}
    /* This includes every static field guaranteed on insufficient tail space;
     * only the variable hardware/compatible-ID tail exceeds Capacity. */
    ZeroMemory(Detail,offsetof(SP_DRVINFO_DETAIL_DATA_W,HardwareID)); Detail->cbSize=sizeof(*Detail);
    Detail->InfDate=(FILETIME)WINTUN_INF_FILETIME;
    swprintf_s(Detail->InfFileName,_countof(Detail->InfFileName),L"C:\\Windows\\Inf\\oem%lu.inf",Index);
    if(OversizeMask&(1u<<Index)){*Required=Capacity+2048;SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    *Required=sizeof(*Detail); return TRUE;
}
static BOOL WINAPI TestUninstall(PCWSTR Path,DWORD Flags,PVOID Reserved)
{
    CHECK(!Reserved&&Flags==(Deleting?0u:SUOI_FORCEDELETE)); ++Uninstalls; DWORD Index=MAXDWORD;
    CHECK(swscanf_s(Path,L"oem%lu.inf",&Index)==1&&Index<Records);
    if(UninstallFailMask&(1u<<Index)){SetLastError(UninstallError);return FALSE;} return TRUE;
}
static BOOL WINAPI TestDestroyDrivers(HDEVINFO Set,PSP_DEVINFO_DATA Info,DWORD Type)
{
    CHECK(Set==(HDEVINFO)Primary&&Info->DevInst==1&&Type==SPDIT_COMPATDRIVER&&DriverList); DriverList=FALSE; ++ListsDestroyed;
    SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static BOOL WINAPI TestDestroySet(HDEVINFO Set)
{
    HANDLE *Handle=Set==(HDEVINFO)Primary?&Primary:&Existing; CHECK(Set==(HDEVINFO)*Handle&&*Handle);
    if(Handle==&Primary)CHECK(!DriverList);
    CHECK(CloseHandle(*Handle)); *Handle=NULL; ++SetsDestroyed; SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
BOOL ResourceCreateTemporaryDirectory(LPWSTR Output)
{
    ++Temps; Output[0]=0; CHECK(!Directory); if(Fail(8))return FALSE;
    wcscpy_s(Output,MAX_PATH,Root); Directory=TRUE; return TRUE;
}
static LPWSTR WINAPI TestCombine(LPWSTR Output,LPCWSTR DirectoryPath,LPCWSTR Leaf)
{
    CHECK(Directory&&!wcscmp(DirectoryPath,Root)); ++Combines;
    if(Combines==FailCombineAt){SetLastError(ERROR_INVALID_DATA);return NULL;}
    return PathCombineW(Output,DirectoryPath,Leaf);
}
BOOL ResourceCopyToFile(LPCWSTR Destination,LPCWSTR Source)
{
    CHECK(Directory&&!wcscmp(PathFindFileNameW(Destination),Source)&&Copies<3); DWORD Index=Copies++;
    if(Copies==FailCopyAt)
    { if(PartialCopy)Files[Index]=TRUE; SetLastError(Error); return FALSE; }
    Files[Index]=TRUE; return TRUE;
}
static BOOL WINAPI TestStage(PCWSTR Source,PCWSTR Media,DWORD MediaType,DWORD Style,PWSTR Destination,DWORD Capacity,PDWORD Required,PWSTR *Component)
{
    CHECK(Directory&&Files[0]&&Files[1]&&Files[2]&&!wcscmp(PathFindFileNameW(Source),L"packetbridge.inf")&&!Media&&MediaType==SPOST_NONE&&
        !Style&&!Destination&&!Capacity&&!Required&&!Component); ++Stages; return !Fail(9);
}
static BOOL WINAPI TestDelete(LPCWSTR Path)
{
    CHECK(Directory); ++Deletes; const WCHAR *Leaf=PathFindFileNameW(Path); DWORD Index;
    if(!wcscmp(Leaf,L"packetbridge.cat"))Index=0;else if(!wcscmp(Leaf,L"packetbridge.sys"))Index=1;else{CHECK(!wcscmp(Leaf,L"packetbridge.inf"));Index=2;}
    BOOL Owned=Files[Index]; Files[Index]=FALSE; SetLastError(Owned?ERROR_INVALID_HANDLE:ERROR_FILE_NOT_FOUND); return Owned;
}
static BOOL WINAPI TestRemove(LPCWSTR Path)
{ CHECK(Directory&&!wcscmp(Path,Root)&&!Files[0]&&!Files[1]&&!Files[2]); Directory=FALSE; ++Removes; SetLastError(ERROR_INVALID_HANDLE); return TRUE; }
/* The real Free wrapper preserves last error around this native primitive. */
static BOOL WINAPI TestHeapFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    for (DWORD i=0;i<_countof(Nodes);++i)
        if(Pointer==Nodes[i])
        {
            CHECK(Pointer&&Heap==ModuleHeap&&!Flags&&HeapFree(Heap,Flags,Pointer));
            Nodes[i]=NULL; ++NodesFreed; SetLastError(ERROR_INVALID_HANDLE); return TRUE;
        }
    CHECK(FALSE); return FALSE;
}
static BOOL RunInstall(VOID)
{
    HDEVINFO Set=(HDEVINFO)(ULONG_PTR)0x2222; SP_DEVINFO_DATA_LIST *Head=(SP_DEVINFO_DATA_LIST *)(ULONG_PTR)0x3333;
    BOOL Result=DriverInstall(&Set,&Head); DWORD Saved=GetLastError();
    if(Result)DriverInstallDeferredCleanup(Set,Head);
    else CHECK(Set==(HDEVINFO)(ULONG_PTR)0x2222&&Head==(SP_DEVINFO_DATA_LIST *)(ULONG_PTR)0x3333);
    SetLastError(Saved); return Result;
}
static int Control(DWORD Which)
{
    Begin(Which!=1); Records=1;
    if(Which<=2){EnumFailAt=0;EnumError=ERROR_NOT_ENOUGH_MEMORY;}
    if(Which==1){Phase=8;Error=ERROR_WRITE_PROTECT;}
    if(Which==3){DetailFailMask=1;DetailError=ERROR_ACCESS_DENIED;}
    if(Which==4)OversizeMask=1;
    BOOL Result=Deleting?DeleteDriverInternal():RunInstall(); DWORD Saved=GetLastError();
    if((Which==1&&Temps)||(Which>=2&&Which<=3&&Result)||(Which==4&&!Uninstalls))
    {
        End(); CHECK(HeapDestroy(ModuleHeap));
        puts(Which==1?"DRIVER_INSTALL_AFTER_INCOMPLETE_ENUMERATION":Which==4?"DRIVER_STATIC_INF_SKIPPED_ON_LONG_IDS":"DRIVER_DELETE_SCAN_FAILURE_ACCEPTED_AS_SUCCESS");
        return Which==1?96:Which==4?98:97;
    }
    if(Which==4)CHECK(Result&&Uninstalls==1);
    else CHECK(!Result&&Saved==(Which==3?DetailError:EnumError)&&!Temps&&!Stages&&!Uninstalls);
    End(); return 0;
}
static VOID SuccessMatrix(VOID)
{
    for (DWORD deleting=0;deleting<2;++deleting) for (DWORD records=0;records<=3;++records)
    {
        Begin(deleting!=0); Records=records;
        CHECK(Deleting?DeleteDriverInternal():RunInstall());
        CHECK(EnumCalls==records+1&&DetailCalls==(Deleting?records:0)&&Uninstalls==(Deleting?records:0));
        CHECK(Temps==(!Deleting&&!records?1u:0u)&&Stages==Temps); End();
    }
    for (DWORD records=1;records<=3;++records) for (DWORD oversized=0;oversized<2;++oversized)
    {
        Begin(FALSE); Records=records; OldMask=(1u<<records)-1; OversizeMask=oversized?OldMask:0;
        CHECK(RunInstall()&&Disables==1&&Enables==1&&NodesAllocated==2&&NodesFreed==2&&Uninstalls==records&&Stages==1); End();
        Begin(TRUE); Records=records; OversizeMask=oversized?((1u<<records)-1):0;
        CHECK(DeleteDriverInternal()&&Uninstalls==records&&DetailCalls==records&&!Stages); End();
    }
    Begin(FALSE); Records=3; OldMask=5; OversizeMask=1;
    CHECK(RunInstall()&&Uninstalls==2&&Enables==1&&!Temps); End();
    /* Installer removal is historically best effort. Ordinary detail/remove
     * failures remain warnings here; only deletion must report incomplete work. */
    Begin(FALSE); Records=1; OldMask=1; DetailFailMask=1;
    CHECK(RunInstall()&&!Uninstalls&&Stages==1&&Enables==1); End();
    Begin(FALSE); Records=1; OldMask=1; UninstallFailMask=1;
    CHECK(RunInstall()&&Uninstalls==1&&Stages==1&&Enables==1); End();
}
static VOID ScanFailureMatrix(VOID)
{
    const DWORD Errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_INVALID_DATA,ERROR_FILE_NOT_FOUND};
    for (DWORD deleting=0;deleting<2;++deleting) for (DWORD at=0;at<=3;++at) for (DWORD error=0;error<_countof(Errors);++error)
        for (DWORD previousOld=0;previousOld<2;++previousOld)
        {
            Begin(deleting!=0); Records=3; EnumFailAt=at; EnumError=Errors[error]; OldMask=previousOld?7:0;
            BOOL Result=Deleting?DeleteDriverInternal():RunInstall();
            CHECK(!Result&&GetLastError()==EnumError&&EnumCalls==at+1&&!Temps&&!Stages);
            CHECK(Uninstalls==(Deleting||previousOld?at:0));
            if(!Deleting&&previousOld&&at)CHECK(Disables==1&&Enables==1&&NodesAllocated==2&&NodesFreed==2); End();
        }
    /* Only detail queries have the documented usable-static-fields exception.
     * The same error on enumeration still means the scan is incomplete. */
    for (DWORD deleting=0;deleting<2;++deleting) for (DWORD at=0;at<=3;++at) for (DWORD previousOld=0;previousOld<2;++previousOld)
    {
        Begin(deleting!=0); Records=3; EnumFailAt=at; EnumError=ERROR_INSUFFICIENT_BUFFER; OldMask=previousOld?7:0;
        CHECK(!(Deleting?DeleteDriverInternal():RunInstall())&&GetLastError()==EnumError&&EnumCalls==at+1&&!Temps&&!Stages);
        CHECK(Uninstalls==(Deleting||previousOld?at:0)); End();
    }
    /* Both a retained version candidate and a disabled adapter prefix may
     * exist when the scan fails. Neither permits publishing a successful scan. */
    for (DWORD old=0;old<2;++old)
    {
        Begin(FALSE); Records=3; OldMask=1u<<old; EnumFailAt=2;
        CHECK(!RunInstall()&&GetLastError()==EnumError&&EnumCalls==3&&!Temps&&!Stages&&Uninstalls==1);
        CHECK(Disables==1&&Enables==1&&NodesAllocated==2&&NodesFreed==2); End();
    }
    for (DWORD at=0;at<3;++at) for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(TRUE); Records=3; DetailFailMask=1u<<at; DetailError=Errors[error];
        CHECK(!DeleteDriverInternal()&&GetLastError()==DetailError&&EnumCalls==4&&DetailCalls==3&&Uninstalls==2); End();
        Begin(TRUE); Records=3; UninstallFailMask=1u<<at; UninstallError=Errors[error];
        CHECK(!DeleteDriverInternal()&&GetLastError()==UninstallError&&Uninstalls==3); End();
    }
    Begin(TRUE); Records=3; DetailFailMask=1; DetailError=ERROR_ACCESS_DENIED; EnumFailAt=2; EnumError=ERROR_NOT_ENOUGH_MEMORY;
    CHECK(!DeleteDriverInternal()&&GetLastError()==DetailError&&DetailCalls==2&&Uninstalls==1); End();
    Begin(TRUE); Records=3; UninstallFailMask=1; UninstallError=ERROR_SHARING_VIOLATION; DetailFailMask=2; DetailError=ERROR_ACCESS_DENIED;
    CHECK(!DeleteDriverInternal()&&GetLastError()==UninstallError&&Uninstalls==2); End();
    Begin(TRUE); Records=3; DetailFailMask=1; DetailError=ERROR_ACCESS_DENIED; UninstallFailMask=2; UninstallError=ERROR_SHARING_VIOLATION;
    CHECK(!DeleteDriverInternal()&&GetLastError()==DetailError&&Uninstalls==2); End();
    Begin(TRUE); Records=3; DetailFailMask=1; DetailError=ERROR_ACCESS_DENIED; OversizeMask=2;
    CHECK(!DeleteDriverInternal()&&GetLastError()==DetailError&&Uninstalls==2); End();
}
static VOID RollbackMatrix(VOID)
{
    const DWORD Errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_INVALID_DATA};
    for (DWORD deleting=0;deleting<2;++deleting) for (DWORD phase=deleting?1u:2u;phase<=(deleting?6u:9u);++phase)
        for (DWORD error=0;error<_countof(Errors);++error)
        {
            Begin(deleting!=0); Phase=phase; Error=Errors[error];
            if(phase==7){Records=1;OldMask=1;}
            CHECK(!(Deleting?DeleteDriverInternal():RunInstall())&&GetLastError()==Error); End();
        }
    for (DWORD previousOld=0;previousOld<2;++previousOld) for (DWORD at=1;at<=3;++at)
    {
        Begin(FALSE); if(previousOld){Records=1;OldMask=1;} FailCombineAt=at;
        CHECK(!RunInstall()&&GetLastError()==ERROR_BUFFER_OVERFLOW&&Combines==at&&!Copies&&!Deletes&&Removes==1);
        if(previousOld)CHECK(Enables==1&&NodesFreed==2); End();
    }
    for (DWORD phase=8;phase<=9;++phase) for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(FALSE); Records=1; OldMask=1; Phase=phase; Error=Errors[error];
        CHECK(!RunInstall()&&GetLastError()==Error&&Disables==1&&Enables==1&&NodesAllocated==2&&NodesFreed==2); End();
    }
    for (DWORD previousOld=0;previousOld<2;++previousOld) for (DWORD at=1;at<=3;++at) for (DWORD partial=0;partial<2;++partial)
        for (DWORD error=0;error<_countof(Errors);++error)
        {
            Begin(FALSE); if(previousOld){Records=1;OldMask=1;} FailCopyAt=at; PartialCopy=partial!=0; Error=Errors[error];
            CHECK(!RunInstall()&&GetLastError()==Error&&Copies==at&&Deletes==3&&Removes==1&&!Stages);
            if(previousOld)CHECK(Enables==1&&NodesFreed==2); End();
        }
    for (DWORD deleting=0;deleting<2;++deleting) for (DWORD at=1;at<=4;++at)
    {
        Begin(deleting!=0); Records=3; CancelAt=at;
        CHECK(!(Deleting?DeleteDriverInternal():RunInstall())&&GetLastError()==ERROR_CANCELLED&&EnumCalls==at-1&&!Temps&&!Stages); End();
    }
    Begin(FALSE); Records=1; OldMask=1; CancelAt=2;
    CHECK(!RunInstall()&&GetLastError()==ERROR_CANCELLED&&Disables==1&&Enables==1&&NodesFreed==2&&!Uninstalls&&!Temps); End();
    for (DWORD detail=0;detail<2;++detail)
    {
        Begin(TRUE); Records=3; CancelAt=2; if(detail)DetailFailMask=1;else UninstallFailMask=1;
        CHECK(!DeleteDriverInternal()&&GetLastError()==ERROR_CANCELLED&&EnumCalls==1); End();
    }
}
int main(int argc,char **argv)
{
    CHECK(argc<=2); ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if(argc==2)
    {
        DWORD Which=(DWORD)strtoul(argv[1],NULL,10); CHECK(Which>=1&&Which<=4);
        int Result=Control(Which); if(Result)return Result;
    }
    else
    {
        DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for (DWORD repeat=0;repeat<3;++repeat){SuccessMatrix();ScanFailureMatrix();RollbackMatrix();}
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    }
    CHECK(HeapDestroy(ModuleHeap)); printf("PASS: actual driver enumeration/static-INF/rollback ownership (%lu scenarios)\n",Scenarios); return 0;
}
