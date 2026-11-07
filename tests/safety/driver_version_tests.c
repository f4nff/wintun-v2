/* SPDX-License-Identifier: GPL-2.0 */
/* Actual query/wait helpers. Native heap, CRT, logger.h and VerQueryValueW;
 * hosted module enumeration/file reads/waits, no actual kernel/device changes. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include "../../api/ntdll.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static LPVOID WINAPI TestHeapAlloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
static NTSTATUS NTAPI TestQuery(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG);
static DWORD WINAPI TestVersionSize(LPCWSTR,LPDWORD);
static BOOL WINAPI TestVersionRead(LPCWSTR,DWORD,DWORD,LPVOID);
static BOOL WINAPI TestVersionRoot(LPCVOID,LPCWSTR,LPVOID,PUINT);
#define HeapAlloc TestHeapAlloc
#define HeapFree TestHeapFree
#include "../../api/logger.h"
#define NtQuerySystemInformation TestQuery
#define GetFileVersionInfoSizeW TestVersionSize
#define GetFileVersionInfoW TestVersionRead
#define VerQueryValueW TestVersionRoot
#ifndef PB_DRIVER_VERSION_SOURCE
#define PB_DRIVER_VERSION_SOURCE "../../api/driver_version.h"
#endif
#include PB_DRIVER_VERSION_SOURCE
#undef HeapAlloc
#undef HeapFree
#undef NtQuerySystemInformation
#undef GetFileVersionInfoSizeW
#undef GetFileVersionInfoW
#undef VerQueryValueW
HANDLE ModuleHeap;
static struct {LPVOID Pointer;SIZE_T Bytes;} Blocks[4];
static DWORD Owned,HeapCalls,FailHeapAt,QueryCalls,Successes,Mismatches,FailQueryAt;
static DWORD ZeroWaits,DelayWaits,FailZeroAt,FailDelayAt,WaitError,LoadedQueries,ModuleCount,MatchIndex;
static DWORD Logs,Scenarios,VersionCalls,VersionFailureStage,VersionError,FileVersion,RuntimeUsers,RuntimeAcquires,RuntimeReleases;
static NTSTATUS QueryFailure;
static BOOL RuntimeFailure;
static LPCSTR ModuleName;
static WCHAR LogLine[1024],VersionPath[1024];
enum {VERSION_BYTES=40+sizeof(VS_FIXEDFILEINFO)};
static LPVOID WINAPI TestHeapAlloc(HANDLE heap,DWORD flags,SIZE_T bytes)
{
    CHECK(heap==ModuleHeap&&!flags);++HeapCalls;
    if(HeapCalls==FailHeapAt){SetLastError(ERROR_INVALID_HANDLE);return NULL;}
    LPVOID pointer=HeapAlloc(heap,flags,bytes);CHECK(pointer);
    DWORD i;for(i=0;i<_countof(Blocks)&&Blocks[i].Pointer;++i){}CHECK(i<_countof(Blocks));
    Blocks[i].Pointer=pointer;Blocks[i].Bytes=bytes;++Owned;return pointer;
}
static BOOL WINAPI TestHeapFree(HANDLE heap,DWORD flags,LPVOID pointer)
{
    CHECK(heap==ModuleHeap&&!flags&&pointer);DWORD i;
    for(i=0;i<_countof(Blocks)&&Blocks[i].Pointer!=pointer;++i){}CHECK(i<_countof(Blocks));
    CHECK(HeapFree(heap,flags,pointer));Blocks[i].Pointer=NULL;Blocks[i].Bytes=0;CHECK(Owned);--Owned;
    SetLastError(ERROR_INVALID_HANDLE);return TRUE;
}
static VOID FormatLog(LPCWSTR format,va_list args)
{++Logs;_vsnwprintf_s(LogLine,_countof(LogLine),_TRUNCATE,format,args);SetLastError(ERROR_INVALID_DATA);}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD error=GetLastError();FormatLog(format,args);SetLastError(error);return error;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{FormatLog(format,args);return error;}
BOOL MainRuntimeAcquire(VOID)
{++RuntimeAcquires;if(RuntimeFailure){SetLastError(ERROR_SHUTDOWN_IN_PROGRESS);return FALSE;}++RuntimeUsers;return TRUE;}
VOID MainRuntimeRelease(VOID)
{CHECK(RuntimeUsers);--RuntimeUsers;++RuntimeReleases;/* Actual release preserves last error. */}
BOOL MainOperationSleep(DWORD delay)
{
    CHECK(!delay||delay==50);DWORD calls=delay?++DelayWaits:++ZeroWaits;
    if(calls==(delay?FailDelayAt:FailZeroAt)){SetLastError(WaitError);return FALSE;}return TRUE;
}
static NTSTATUS NTAPI TestQuery(SYSTEM_INFORMATION_CLASS info,PVOID output,ULONG bytes,PULONG returned)
{
    CHECK((ULONG)info==SystemModuleInformation&&output&&returned);++QueryCalls;
    if(QueryCalls==FailQueryAt)return QueryFailure;
    if(QueryCalls<=Mismatches){*returned=bytes+4096;return STATUS_INFO_LENGTH_MISMATCH;}
    SIZE_T size=FIELD_OFFSET(RTL_PROCESS_MODULES,Modules)+ModuleCount*sizeof(RTL_PROCESS_MODULE_INFORMATION);
    CHECK(bytes>=size);PRTL_PROCESS_MODULES modules=output;memset(output,0,size);
    modules->NumberOfModules=ModuleCount;++Successes;
    for(DWORD i=0;i<ModuleCount;++i){
        LPCSTR name=i==MatchIndex&&Successes<=LoadedQueries?ModuleName:"other.sys";
        PRTL_PROCESS_MODULE_INFORMATION module=modules->Modules+i;
        CHECK(sprintf_s((char *)module->FullPathName,sizeof(module->FullPathName),"\\SystemRoot\\System32\\drivers\\%s",name)>0);
        module->OffsetToFileName=(USHORT)(strrchr((char *)module->FullPathName,'\\')+1-(char *)module->FullPathName);
    }
    *returned=(ULONG)size;SetLastError(ERROR_INVALID_HANDLE);return 0;
}
static BOOL VersionFails(DWORD stage)
{if(VersionFailureStage!=stage)return FALSE;SetLastError(VersionError);return TRUE;}
static DWORD WINAPI TestVersionSize(LPCWSTR path,LPDWORD ignored)
{
    ++VersionCalls;CHECK(path&&ignored&&wcsstr(path,L"\\\\?\\GLOBALROOT\\SystemRoot\\System32\\drivers\\")==path);
    CHECK(wcscpy_s(VersionPath,_countof(VersionPath),path)==0);*ignored=0;return VersionFails(1)?0:VERSION_BYTES;
}
static BOOL WINAPI TestVersionRead(LPCWSTR path,DWORD ignored,DWORD bytes,LPVOID output)
{
    ++VersionCalls;CHECK(!wcscmp(path,VersionPath)&&!ignored&&bytes==VERSION_BYTES&&output);
    if(VersionFails(2))return FALSE;
    memset(output,0,bytes);WORD header[]={VERSION_BYTES,sizeof(VS_FIXEDFILEINFO),0};memcpy(output,header,sizeof(header));
    memcpy((BYTE *)output+6,L"VS_VERSION_INFO",sizeof(L"VS_VERSION_INFO"));
    VS_FIXEDFILEINFO fixed={0};fixed.dwSignature=VS_FFI_SIGNATURE;fixed.dwStrucVersion=0x00010000;fixed.dwFileVersionMS=FileVersion;
    memcpy((BYTE *)output+40,&fixed,sizeof(fixed));return TRUE;
}
static BOOL WINAPI TestVersionRoot(LPCVOID info,LPCWSTR subblock,LPVOID output,PUINT bytes)
{
    ++VersionCalls;CHECK(info&&!wcscmp(subblock,L"\\")&&output&&bytes);
    if(VersionFails(3))return FALSE;
    BOOL found=VerQueryValueW(info,subblock,output,bytes);CHECK(found&&*(VOID **)output&&*bytes==sizeof(VS_FIXEDFILEINFO));return found;
}
static VOID Begin(VOID)
{
    CHECK(!Owned&&!RuntimeUsers);HeapCalls=FailHeapAt=QueryCalls=Successes=Mismatches=FailQueryAt=0;
    ZeroWaits=DelayWaits=FailZeroAt=FailDelayAt=Logs=VersionCalls=VersionFailureStage=RuntimeAcquires=RuntimeReleases=0;
    WaitError=ERROR_CANCELLED;QueryFailure=(NTSTATUS)0xc0000022u;LoadedQueries=0;ModuleCount=2;MatchIndex=1;
    VersionError=ERROR_ACCESS_DENIED;FileVersion=0x00020000;RuntimeFailure=FALSE;ModuleName="packetbridge.sys";
    LogLine[0]=VersionPath[0]=0;SetLastError(ERROR_SUCCESS);
}
static VOID End(VOID)
{CHECK(!Owned&&!RuntimeUsers);for(DWORD i=0;i<_countof(Blocks);++i)CHECK(!Blocks[i].Pointer);++Scenarios;}
static VOID AbsenceAndPresence(VOID)
{
    const LPCSTR names[]={"packetbridge.sys","PACKETBRIDGE.SYS","PacketBridge.Sys"};
    for(DWORD publicCall=0;publicCall<2;++publicCall)for(DWORD modules=0;modules<=3;++modules)for(DWORD match=0;match<3;++match){
        Begin();ModuleCount=modules;MatchIndex=match;
        DWORD result=publicCall?WintunGetRunningDriverVersion():MaybeGetRunningDriverVersion(TRUE);
        CHECK(!result&&GetLastError()==ERROR_FILE_NOT_FOUND&&QueryCalls==1&&ZeroWaits==1&&!DelayWaits&&!VersionCalls);
        CHECK(RuntimeAcquires==publicCall&&RuntimeReleases==publicCall);End();
        if(match>=modules)continue;
        for(DWORD n=0;n<_countof(names);++n){
            Begin();ModuleCount=modules;MatchIndex=match;ModuleName=names[n];LoadedQueries=MAXDWORD;
            result=publicCall?WintunGetRunningDriverVersion():MaybeGetRunningDriverVersion(TRUE);
            CHECK(result==(publicCall?FileVersion:1)&&QueryCalls==1&&ZeroWaits==1&&!DelayWaits&&VersionCalls==(publicCall?3u:0u));
            CHECK(RuntimeAcquires==publicCall&&RuntimeReleases==publicCall);End();
        }
    }
    Begin();RuntimeFailure=TRUE;CHECK(!WintunGetRunningDriverVersion()&&GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS&&!QueryCalls&&!HeapCalls&&RuntimeAcquires==1&&!RuntimeReleases);End();
}
static VOID QueryFailures(VOID)
{
    const NTSTATUS statuses[]={(NTSTATUS)0xc0000022u,(NTSTATUS)0xc0000001u,(NTSTATUS)0xc0000034u,(NTSTATUS)0xc000009au,(NTSTATUS)0xc0000061u};
    for(DWORD mode=0;mode<3;++mode)for(DWORD i=0;i<_countof(statuses);++i)for(DWORD mismatch=0;mismatch<2;++mismatch){
        Begin();Mismatches=mismatch?3:0;FailQueryAt=Mismatches+1;QueryFailure=statuses[i];
        DWORD expected=RtlNtStatusToDosError(QueryFailure);
        DWORD result=mode==0?MaybeGetRunningDriverVersion(TRUE):mode==1?WintunGetRunningDriverVersion():(DWORD)EnsureWintunUnloaded();
        CHECK(!result&&GetLastError()==expected&&QueryCalls==FailQueryAt&&HeapCalls==FailQueryAt&&ZeroWaits==FailQueryAt&&!DelayWaits&&!VersionCalls);End();
    }
    const DWORD mismatches[]={1,3,7};
    for(DWORD mode=0;mode<3;++mode)for(DWORD i=0;i<_countof(mismatches);++i){
        Begin();Mismatches=mismatches[i];
        DWORD result=mode==0?MaybeGetRunningDriverVersion(TRUE):mode==1?WintunGetRunningDriverVersion():(DWORD)EnsureWintunUnloaded();
        CHECK(result==(mode==2?1u:0u)&&GetLastError()==ERROR_FILE_NOT_FOUND&&QueryCalls==Mismatches+1&&ZeroWaits==QueryCalls&&HeapCalls==QueryCalls&&!DelayWaits&&!VersionCalls);End();
    }
    const DWORD attempts[]={1,2,8,1500};
    for(DWORD i=0;i<_countof(statuses);++i)for(DWORD at=0;at<_countof(attempts);++at){
        Begin();LoadedQueries=MAXDWORD;FailQueryAt=attempts[at];QueryFailure=statuses[i];
        DWORD error=RtlNtStatusToDosError(QueryFailure);
        CHECK(!EnsureWintunUnloaded()&&GetLastError()==error&&QueryCalls==FailQueryAt&&ZeroWaits==FailQueryAt&&DelayWaits==FailQueryAt-1);End();
    }
}
static VOID EnsureBudgets(VOID)
{
    const DWORD loaded[]={0,1,2,7,1499,1500,1501};
    for(DWORD i=0;i<_countof(loaded);++i){
        Begin();LoadedQueries=loaded[i];BOOL okay=EnsureWintunUnloaded();DWORD error=GetLastError();
        DWORD queries=loaded[i]<1500?loaded[i]+1:1500;
        CHECK(okay==(loaded[i]<1500)&&QueryCalls==queries&&ZeroWaits==queries&&DelayWaits==queries-1&&!VersionCalls);
        CHECK(error==(DWORD)(okay?ERROR_FILE_NOT_FOUND:ERROR_TIMEOUT));End();
    }
    const DWORD attempts[]={1,2,8,1500},errors[]={ERROR_CANCELLED,ERROR_OPERATION_ABORTED,ERROR_TIMEOUT};
    for(DWORD i=0;i<_countof(attempts);++i)for(DWORD e=0;e<_countof(errors);++e){
        Begin();LoadedQueries=MAXDWORD;FailZeroAt=attempts[i];WaitError=errors[e];
        CHECK(!EnsureWintunUnloaded()&&GetLastError()==WaitError&&ZeroWaits==FailZeroAt&&QueryCalls==FailZeroAt-1&&DelayWaits==FailZeroAt-1);End();
        Begin();LoadedQueries=MAXDWORD;FailDelayAt=attempts[i];WaitError=errors[e];
        CHECK(!EnsureWintunUnloaded()&&GetLastError()==(attempts[i]==1500?ERROR_TIMEOUT:WaitError));
        CHECK(DelayWaits==(attempts[i]==1500?1499:FailDelayAt)&&QueryCalls==(attempts[i]==1500?1500:FailDelayAt));End();
    }
    for(DWORD i=0;i<_countof(attempts);++i){
        Begin();LoadedQueries=MAXDWORD;FailHeapAt=attempts[i];
        CHECK(!EnsureWintunUnloaded()&&GetLastError()==ERROR_OUTOFMEMORY&&HeapCalls==FailHeapAt&&QueryCalls==FailHeapAt-1&&ZeroWaits==FailHeapAt&&DelayWaits==FailHeapAt-1);End();
    }
}
static VOID VersionCases(VOID)
{
    const DWORD versions[]={1,0x00020000,MAXDWORD};
    for(DWORD i=0;i<_countof(versions);++i){
        Begin();LoadedQueries=MAXDWORD;FileVersion=versions[i];CHECK(WintunGetRunningDriverVersion()==FileVersion&&VersionCalls==3&&RuntimeReleases==1);End();
    }
    const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_FILE_NOT_FOUND,ERROR_INVALID_DATA,ERROR_RESOURCE_DATA_NOT_FOUND};
    for(DWORD stage=1;stage<=3;++stage)for(DWORD e=0;e<_countof(errors);++e){
        Begin();LoadedQueries=MAXDWORD;VersionFailureStage=stage;VersionError=errors[e];
        CHECK(!WintunGetRunningDriverVersion()&&GetLastError()==VersionError&&VersionCalls==stage&&RuntimeReleases==1);End();
    }
    Begin();LoadedQueries=MAXDWORD;FileVersion=0;CHECK(!WintunGetRunningDriverVersion()&&GetLastError()==ERROR_VERSION_PARSE_ERROR&&VersionCalls==3&&RuntimeReleases==1);End();
    for(DWORD at=1;at<=2;++at){
        Begin();LoadedQueries=MAXDWORD;FailHeapAt=at;CHECK(!WintunGetRunningDriverVersion()&&GetLastError()==ERROR_OUTOFMEMORY&&HeapCalls==at&&RuntimeReleases==1);End();
    }
}
int main(int argc,char **argv)
{
    ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);
    if(argc==2){
        CHECK(!strcmp(argv[1],"1"));Begin();FailQueryAt=1;BOOL unloaded=EnsureWintunUnloaded();DWORD error=GetLastError();End();
        CHECK(HeapDestroy(ModuleHeap));
        if(unloaded){puts("DRIVER_QUERY_FAILURE_ACCEPTED_AS_UNLOADED");return 90;}
        CHECK(error==ERROR_ACCESS_DENIED);puts("PASS: driver query failure remains a failed unload diagnostic");return 0;
    }
    CHECK(argc==1);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(DWORD repeat=0;repeat<3;++repeat){AbsenceAndPresence();QueryFailures();EnsureBudgets();VersionCases();}
    CHECK(!Owned&&!RuntimeUsers&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    PROCESS_HEAP_ENTRY entry={0};while(HeapWalk(ModuleHeap,&entry))CHECK(!(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY));
    CHECK(HeapDestroy(ModuleHeap));printf("PASS: driver version query/absence/wait budget/ownership matrix (%lu scenarios); native heap/CRT/logger wrappers/VerQueryValueW, no actual kernel enumeration\n",Scenarios);return 0;
}
