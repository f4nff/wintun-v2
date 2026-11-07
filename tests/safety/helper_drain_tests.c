/* SPDX-License-Identifier: GPL-2.0 */
/* Complete actual runtime/helper, real named pipes and true DLL unload. The
 * retained MAYBE_WOW64 source branch is hosted, not enabled shipping x64 proof.
 * Immediate quota failure follows ReadFile's contract; the pending error cases
 * explicitly model completion status only after witnessing terminal native IO. */
#ifdef PB_API_TESTS
#error Helper drain must use actual runtime/helper code
#endif
#ifndef NDEBUG
#error Compile production with NDEBUG while retaining active test checks
#endif
#include "guard_memory.h"
#include "../../include/packetbridge_version.h"
#include <sddl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HD_CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping fingerprint required");
_Static_assert(PB_VERSION_MAJOR==2&&PB_VERSION_MINOR==0&&PB_VERSION_PATCH==0,"Version remains2.0.0");
enum { HD_PEEK=1,HD_READ,HD_PENDING,HD_NORMAL,HD_EMPTY,HD_INITIAL_PENDING,HD_NATIVE_EOF };
typedef struct HD_CASE {
    DWORD Stage,Stream,Middle,Error,Prior;
    DWORD Result,ResponseBytes,Logs,Faults,PendingWitnesses,NativeMask,CloseWitnesses,ClosedMask;
    DWORD CachedHeaps,CachedLocals,Users,Handles,Allocations,Works;
} HD_CASE;
static const WCHAR StatusPrefix[]=L"0",CreatePrefix[]=L"0 ROOT\\PacketBridgeV2\\head",CreateTail[]=L"tail";
static const WCHAR LogOne[]=L"[+ 123] one\n",LogBoth[]=L"[+ 123] one\n[+ 123] two\n";

#ifdef PB_HELPER_DRAIN_FIXTURE
#ifndef PB_HELPER_DRAIN_SOURCE
#error Bind the complete fixed legacy fixture through the builder
#endif
static BOOL WINAPI DrainRead(HANDLE,VOID *,DWORD,DWORD *,OVERLAPPED *);
static BOOL WINAPI DrainPeek(HANDLE,VOID *,DWORD,DWORD *,DWORD *,DWORD *);
static BOOL WINAPI DrainResult(HANDLE,OVERLAPPED *,DWORD *,BOOL);
static VOID DrainClosing(HANDLE);
static VOID DrainFinalCheck(VOID);
static HANDLE WINAPI DrainHeapCreate(DWORD,SIZE_T,SIZE_T);
static BOOL WINAPI DrainHeapDestroy(HANDLE);
static BOOL WINAPI DrainSddl(LPCWSTR,DWORD,PSECURITY_DESCRIPTOR *,ULONG *);
static BOOL WINAPI DrainSid(PSID,LPWSTR *);
static HLOCAL WINAPI DrainLocalFree(HLOCAL);
#define ReadFile DrainRead
#define PeekNamedPipe DrainPeek
#define GetOverlappedResult DrainResult
#define PB_FIXTURE_CLOSE_OBSERVER DrainClosing
#define HeapCreate DrainHeapCreate
#define HeapDestroy DrainHeapDestroy
#define ConvertStringSecurityDescriptorToSecurityDescriptorW DrainSddl
#define ConvertSidToStringSidW DrainSid
#define LocalFree DrainLocalFree
#include PB_HELPER_DRAIN_SOURCE
#undef ReadFile
#undef PeekNamedPipe
#undef GetOverlappedResult
#undef HeapCreate
#undef HeapDestroy
#undef ConvertStringSecurityDescriptorToSecurityDescriptorW
#undef ConvertSidToStringSidW
#undef LocalFree
static HD_CASE *Active;
static HELPER_JOB *ActiveJob;
static BOOL Armed,PriorPump,PendingInjection;
static DWORD ReadTurns,PeekTurns,LogCalls,Faults,PendingWitnesses,NativeMask,CloseWitnesses,ClosedMask,HeapOwners,LocalOwners;
static HANDLE RuntimeHeap;
static HLOCAL Locals[3];
static HANDLE WINAPI DrainHeapCreate(DWORD flags,SIZE_T initial,SIZE_T maximum)
{HANDLE heap=HeapCreate(flags,initial,maximum);HD_CHECK(heap&&!RuntimeHeap&&!HeapOwners);RuntimeHeap=heap;++HeapOwners;return heap;}
static BOOL WINAPI DrainHeapDestroy(HANDLE heap)
{HD_CHECK(heap==RuntimeHeap&&HeapOwners==1);BOOL okay=HeapDestroy(heap);HD_CHECK(okay);RuntimeHeap=NULL;--HeapOwners;return okay;}
static VOID OwnLocal(HLOCAL memory)
{HD_CHECK(memory);for(DWORD i=0;i<3;++i)if(!Locals[i]){Locals[i]=memory;++LocalOwners;return;}HD_CHECK(FALSE);}
static BOOL WINAPI DrainSddl(LPCWSTR text,DWORD revision,PSECURITY_DESCRIPTOR *descriptor,ULONG *size)
{BOOL okay=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,revision,descriptor,size);HD_CHECK(okay);OwnLocal(*descriptor);return okay;}
static BOOL WINAPI DrainSid(PSID sid,LPWSTR *text)
{BOOL okay=ConvertSidToStringSidW(sid,text);HD_CHECK(okay);OwnLocal(*text);return okay;}
static HLOCAL WINAPI DrainLocalFree(HLOCAL memory)
{
    if(!memory)return LocalFree(memory);
    for(DWORD i=0;i<3;++i)if(Locals[i]==memory){HLOCAL result=LocalFree(memory);HD_CHECK(!result&&LocalOwners);Locals[i]=NULL;--LocalOwners;return result;}
    HD_CHECK(FALSE);return memory;
}
static DWORD StreamIndex(HANDLE file)
{HD_CHECK(ActiveJob);for(DWORD i=0;i<2;++i)if(file==ActiveJob->Streams[i].Read)return i;HD_CHECK(FALSE);return 0;}
static BOOL WINAPI DrainRead(HANDLE file,VOID *data,DWORD length,DWORD *count,OVERLAPPED *io)
{
    HD_CHECK(count&&io&&io->hEvent&&data&&length);DWORD index=StreamIndex(file);
    BOOL target=Armed&&index==Active->Stream;
    if(target){++ReadTurns;if(PriorPump||(Active->Stage==HD_READ&&ReadTurns==Active->Middle+1)){
        ++Faults;*count=0;SetLastError(PriorPump?Active->Prior:Active->Error);return FALSE;
    }}
    DWORD requested=length;
    if(target&&Active->Middle&&!PriorPump&&ReadTurns==1)requested=min(length,index?(DWORD)(sizeof(LogOne)-sizeof(WCHAR)):(DWORD)sizeof(WCHAR));
    BOOL okay=ReadFile(file,data,requested,count,io);DWORD error=okay?0:GetLastError();
    if(okay||error==ERROR_IO_PENDING)NativeMask|=1u<<index;
    if(target&&Active->Stage==HD_PENDING&&ReadTurns==Active->Middle+1){
        /* Available data completes the real read. Report the other permitted
         * submission race (pending, completion already terminal), then inject
         * only its terminal status at the completion-query SDK boundary. */
        DWORD nativeBytes;HD_CHECK(GetOverlappedResult(file,io,&nativeBytes,TRUE)&&nativeBytes==requested);
        PendingInjection=TRUE;*count=0;SetLastError(ERROR_IO_PENDING);return FALSE;
    }
    SetLastError(error);return okay;
}
static BOOL WINAPI DrainPeek(HANDLE file,VOID *buffer,DWORD size,DWORD *read,DWORD *available,DWORD *left)
{
    if(Armed&&!PriorPump&&StreamIndex(file)==Active->Stream){++PeekTurns;if(Active->Stage==HD_PEEK&&PeekTurns==Active->Middle+1){HD_CHECK(available);++Faults;SetLastError(Active->Error);return FALSE;}}
    return PeekNamedPipe(file,buffer,size,read,available,left);
}
static BOOL WINAPI DrainResult(HANDLE file,OVERLAPPED *io,DWORD *count,BOOL wait)
{
    if(PendingInjection&&file==ActiveJob->Streams[Active->Stream].Read){
        DWORD nativeBytes;HD_CHECK(count&&!wait&&GetOverlappedResult(file,io,&nativeBytes,FALSE));
        ++PendingWitnesses;++Faults;PendingInjection=FALSE;*count=0;SetLastError(Active->Error);return FALSE;
    }
    return GetOverlappedResult(file,io,count,wait);
}
static VOID DrainClosing(HANDLE file)
{
    if(!ActiveJob)return;
    for(DWORD i=0;i<2;++i)if(file==ActiveJob->Streams[i].Read){
        HELPER_STREAM *stream=&ActiveJob->Streams[i];HD_CHECK(!stream->Pending&&!PendingInjection);
        if(NativeMask&(1u<<i)){DWORD bytes;BOOL okay=GetOverlappedResult(file,&stream->Io,&bytes,FALSE);DWORD error=okay?0:GetLastError();HD_CHECK(okay||error!=ERROR_IO_INCOMPLETE);++CloseWitnesses;}
        ClosedMask|=1u<<i;
    }
}
static VOID CALLBACK DrainLogger(WINTUN_LOGGER_LEVEL level,DWORD64 timestamp,LPCWSTR message)
{HD_CHECK(level==WINTUN_LOG_INFO&&timestamp==123&&(!wcscmp(message,L"one")||!wcscmp(message,L"two")));++LogCalls;SetLastError(ERROR_ACCESS_DENIED);}
static VOID WriteAll(HANDLE file,const WCHAR *text,DWORD bytes)
{DWORD count;HD_CHECK(WriteFile(file,text,bytes,&count,NULL)&&count==bytes);}
__declspec(dllexport) VOID WINAPI RunDrainCase(HD_CASE *test)
{
    HD_CHECK(test&&!Active&&!ActiveJob&&!OwnedHandles&&!OwnedAllocations&&!OwnedWorks);
    HD_CHECK(MainRuntimeAcquire()&&DependencyUsers==1&&RuntimeReady&&HeapOwners==1&&LocalOwners==2);
    HELPER_JOB job={0};job.ResponseCapacity=_countof(job.Response);Active=test;ActiveJob=&job;
    Armed=PriorPump=PendingInjection=FALSE;ReadTurns=PeekTurns=LogCalls=Faults=PendingWitnesses=NativeMask=CloseWitnesses=ClosedMask=0;
    HD_CHECK(swprintf_s(job.Directory,_countof(job.Directory),L"C:\\drain-test-%lu",GetCurrentProcessId())>0&&HelperPipe(&job,0)&&HelperPipe(&job,1));
    WintunSetLogger(DrainLogger);
    const WCHAR *prefix=test->Stream?StatusPrefix:CreatePrefix;
    WriteAll(job.Streams[0].Write,prefix,(DWORD)(wcslen(prefix)*sizeof(WCHAR)));HelperPump(&job,&job.Streams[0]);
    if(job.Streams[0].Pending){HD_CHECK(WaitForSingleObject(job.Streams[0].Io.hEvent,5000)==WAIT_OBJECT_0);HelperPump(&job,&job.Streams[0]);}
    HD_CHECK(!job.Streams[0].Pending&&!job.Error&&job.ResponseBytes==wcslen(prefix)*sizeof(WCHAR));
    if(test->Prior){
        DWORD stream=test->Stream;test->Stream=1-stream;Armed=PriorPump=TRUE;HelperPump(&job,&job.Streams[test->Stream]);
        Armed=PriorPump=FALSE;test->Stream=stream;HD_CHECK(job.Error==test->Prior);ReadTurns=PeekTurns=Faults=0;
    }
    HELPER_STREAM *target=&job.Streams[test->Stream];
    if(test->Stage<=HD_NORMAL)WriteAll(target->Write,test->Stream?LogBoth:CreateTail,test->Stream?(DWORD)(sizeof(LogBoth)-sizeof(WCHAR)):(DWORD)(sizeof(CreateTail)-sizeof(WCHAR)));
    if(test->Stage==HD_INITIAL_PENDING){HelperPump(&job,target);HD_CHECK(target->Pending);}
    if(test->Stage==HD_NATIVE_EOF){HD_CHECK(TestClose(target->Write));target->Write=NULL;}
    Armed=TRUE;HelperFinishStream(&job,&job.Streams[0],TRUE);HelperFinishStream(&job,&job.Streams[1],TRUE);Armed=FALSE;
    HD_CHECK(!PendingInjection&&!job.Streams[0].Pending&&!job.Streams[1].Pending&&ClosedMask==3&&!OwnedHandles&&!OwnedAllocations&&!OwnedWorks);
    test->Result=job.Error;test->ResponseBytes=job.ResponseBytes;test->Logs=LogCalls;test->Faults=Faults;test->PendingWitnesses=PendingWitnesses;
    test->NativeMask=NativeMask;test->CloseWitnesses=CloseWitnesses;test->ClosedMask=ClosedMask;
    ActiveJob=NULL;Active=NULL;WintunSetLogger(NULL);MainRuntimeRelease();
    test->CachedHeaps=HeapOwners;test->CachedLocals=LocalOwners;test->Users=DependencyUsers;
    test->Handles=(DWORD)OwnedHandles;test->Allocations=(DWORD)OwnedAllocations;test->Works=(DWORD)OwnedWorks;
    HD_CHECK(!DependencyUsers&&!DependencyUnloading&&HeapOwners==1&&LocalOwners==2);
}
static VOID DrainFinalCheck(VOID)
{HD_CHECK(!Active&&!ActiveJob&&!PendingInjection&&!OwnedHandles&&!OwnedAllocations&&!OwnedWorks&&!HelperCount&&!HelperJobs&&!DependencyUsers&&!DependencyUnloading&&!HeapOwners&&!LocalOwners&&!RuntimeHeap);for(DWORD i=0;i<3;++i)HD_CHECK(!Locals[i]);}
#else
typedef VOID (WINAPI *HD_RUN)(HD_CASE *);
static DWORD Cases,Lost,HostBlocks;
static VOID Scenario(LPCWSTR path,DWORD stage,DWORD stream,DWORD middle,DWORD error,DWORD prior,const char *phase)
{
    HD_CHECK(!HostBlocks&&!GetModuleHandleW(path));HMODULE module=LoadLibraryW(path);HD_CHECK(module);
    HD_RUN run;*(FARPROC *)&run=GetProcAddress(module,"RunDrainCase");HD_CHECK(run);
#ifdef PB_GUARD_TESTS
    HD_CASE *test=PbTestGuardAlloc(sizeof(*test));
#else
    HD_CASE *test=malloc(sizeof(*test));HD_CHECK(test);
#endif
    ++HostBlocks;memset(test,0,sizeof(*test));test->Stage=stage;test->Stream=stream;test->Middle=middle;test->Error=error;test->Prior=prior;
    run(test);
    DWORD expected=prior?prior:stage<=HD_PENDING?(error==ERROR_BROKEN_PIPE||error==ERROR_OPERATION_ABORTED?0:error?error:ERROR_GEN_FAILURE):0;
    HD_CHECK(test->ClosedMask==3&&!test->Users&&!test->Handles&&!test->Allocations&&!test->Works&&test->CachedHeaps==1&&test->CachedLocals==2);
    HD_CHECK((test->NativeMask&1)&&test->NativeMask<=3&&test->CloseWitnesses==((test->NativeMask&1)?1u:0u)+((test->NativeMask&2)?1u:0u));
    HD_CHECK(test->Faults==(stage<=HD_PENDING?1u:0u)&&test->PendingWitnesses==(stage==HD_PENDING?1u:0u));
    if(stage==HD_NORMAL)HD_CHECK(test->ResponseBytes==(stream?sizeof(StatusPrefix)-sizeof(WCHAR):sizeof(CreatePrefix)+sizeof(CreateTail)-2*sizeof(WCHAR))&&test->Logs==(stream?2u:0u));
#ifdef PB_HELPER_DRAIN_BEFORE
    HD_CHECK(stage==HD_READ&&stream==1&&!middle&&error==ERROR_NOT_ENOUGH_QUOTA&&!prior&&!test->Result&&expected==ERROR_NOT_ENOUGH_QUOTA);++Lost;
#else
    HD_CHECK(test->Result==expected);
#endif
    DWORD observed=test->Result;HD_CHECK(FreeLibrary(module)&&!GetModuleHandleW(path));
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(test);
#else
    free(test);
#endif
    --HostBlocks;++Cases;
    printf("CASE phase=%s stage=%lu stream=%lu middle=%lu error=%lu prior=%lu observed=%lu expected=%lu owners=0 dll=unloaded\n",phase,stage,stream,middle,error,prior,observed,expected);
}
static VOID Matrix(LPCWSTR path,const char *phase)
{
    for(DWORD stage=HD_PEEK;stage<=HD_PENDING;++stage)for(DWORD stream=0;stream<2;++stream)for(DWORD middle=0;middle<2;++middle)for(DWORD prior=0;prior<2;++prior)for(DWORD error=0;error<2;++error)
        Scenario(path,stage,stream,middle,error?(stage==HD_PEEK?ERROR_BROKEN_PIPE:ERROR_OPERATION_ABORTED):ERROR_NOT_ENOUGH_QUOTA,prior?ERROR_NOT_ENOUGH_MEMORY:0,phase);
    for(DWORD stage=HD_PEEK;stage<=HD_PENDING;++stage)for(DWORD stream=0;stream<2;++stream)Scenario(path,stage,stream,0,0,0,phase);
    for(DWORD stage=HD_NORMAL;stage<=HD_NATIVE_EOF;++stage)for(DWORD stream=0;stream<2;++stream)Scenario(path,stage,stream,0,0,0,phase);
}
int wmain(int argc,WCHAR **argv)
{
    HD_CHECK(argc==2||(argc==3&&!wcscmp(argv[2],L"137")));BOOL control=argc==3;
#ifdef PB_HELPER_DRAIN_BEFORE
    HD_CHECK(control);
#endif
    DWORD warm=control?2:62,measured=control?1:62;
    if(control){for(DWORD i=0;i<warm;++i)Scenario(argv[1],HD_READ,1,0,ERROR_NOT_ENOUGH_QUOTA,0,"warm");}else Matrix(argv[1],"warm");
    DWORD before,after;HD_CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(control)Scenario(argv[1],HD_READ,1,0,ERROR_NOT_ENOUGH_QUOTA,0,"measure");else Matrix(argv[1],"measure");
    HD_CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&Cases==warm+measured&&!HostBlocks);
    printf("PASS: %lu helper drain cases warm%lu measure%lu; canonical error/EOF and terminal native I/O before close, cached heap/SDDL freed at actual DLL detach; lost%lu handles%lu/%lu\n",Cases,warm,measured,Lost,before,after);
#ifdef PB_HELPER_DRAIN_BEFORE
    HD_CHECK(Lost==Cases);puts("HELPER_POST_EXIT_DRAIN_ERROR_NOT_REPORTED: complete valid stdout, quota1816 lost from second stream; real I/O/handles/runtime cleanup and true DLL unload precede137");return 137;
#else
    HD_CHECK(!Lost);return 0;
#endif
}
#endif
