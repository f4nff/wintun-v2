/* SPDX-License-Identifier: GPL-2.0 */
/* Exact EnvInit body; version/capability results are SDK-boundary models.
 * The borrowed kernel32 handle and current-process pseudo-handle are native. */
#ifdef PB_API_TESTS
#error Environment tests require shipping paths
#endif
#ifndef NDEBUG
#error Environment tests require the shipping NDEBUG branch
#endif
#ifndef PB_ENV_INIT_SOURCE
#error Build through Build-EnvInitTests for an exact production fragment
#endif
#include "../safety/guard_memory.h"
#include "../../include/packetbridge_version.h"
#include "../../api/main.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"environment tests require shipping fingerprint layout");

/* The compiler/SDK calls remain native x64. Only the retained legacy source's
 * process-machine constant models an x86 caller, so native-I386 and WOW-AMD64
 * fallback outcomes are distinguishable. This is not an x86 build/ABI test. */
#ifdef MAYBE_WOW64
#undef IMAGE_FILE_PROCESS
#define IMAGE_FILE_PROCESS IMAGE_FILE_MACHINE_I386
#endif
BOOL IsWindows7,IsWindows10;
USHORT NativeMachine=IMAGE_FILE_PROCESS;
enum {MissingKernel,MissingSymbol,FailedModern,ModernSuccess};
typedef struct ENV_CASE {
    ULONG Stage;BOOL LegacyOkay,LegacyWow;
    USHORT Process,Native,Expected;
} ENV_CASE;
static HMODULE BorrowedKernel;
static DWORD Major,Minor,VersionCalls,KernelCalls,ExportCalls,ModernCalls,LegacyCalls,Cases;
static ENV_CASE Selected;
static VOID EnvVersion(DWORD *major,DWORD *minor,DWORD *build)
{CHECK(major&&minor&&!build);++VersionCalls;*major=Major;*minor=Minor;SetLastError(ERROR_INVALID_DATA);}
static HMODULE WINAPI EnvKernel(LPCWSTR name)
{CHECK(!wcscmp(name,L"kernel32.dll"));++KernelCalls;SetLastError(ERROR_MOD_NOT_FOUND);return Selected.Stage==MissingKernel?NULL:BorrowedKernel;}
static BOOL WINAPI EnvModern(HANDLE process,USHORT *machine,USHORT *native)
{
    CHECK(process==GetCurrentProcess()&&machine&&native);++ModernCalls;
    if(Selected.Stage==FailedModern){*machine=*native=IMAGE_FILE_MACHINE_UNKNOWN;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    CHECK(Selected.Stage==ModernSuccess);*machine=Selected.Process;*native=Selected.Native;return TRUE;
}
static FARPROC WINAPI EnvExport(HMODULE module,LPCSTR name)
{
    CHECK(module==BorrowedKernel&&!strcmp(name,"IsWow64Process2"));++ExportCalls;
    if(Selected.Stage==MissingSymbol){SetLastError(ERROR_PROC_NOT_FOUND);return NULL;}
    BOOL(WINAPI *function)(HANDLE,USHORT *,USHORT *)=EnvModern;FARPROC result;
    _Static_assert(sizeof(result)==sizeof(function),"function pointer storage");memcpy(&result,&function,sizeof(result));return result;
}
static BOOL WINAPI EnvLegacy(HANDLE process,PBOOL wow)
{
    CHECK(process==GetCurrentProcess()&&wow);++LegacyCalls;
    if(!Selected.LegacyOkay){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    *wow=Selected.LegacyWow;return TRUE;
}
#define RtlGetNtVersionNumbers EnvVersion
#define GetModuleHandleW EnvKernel
#define GetProcAddress EnvExport
#define IsWow64Process EnvLegacy
#include PB_ENV_INIT_SOURCE
#undef RtlGetNtVersionNumbers
#undef GetModuleHandleW
#undef GetProcAddress
#undef IsWow64Process

static VOID Run(DWORD major,DWORD minor,BOOL seven,BOOL ten,const ENV_CASE *scenario)
{
    Major=major;Minor=minor;Selected=*scenario;VersionCalls=KernelCalls=ExportCalls=ModernCalls=LegacyCalls=0;
    IsWindows7=!seven;IsWindows10=!ten;NativeMachine=IMAGE_FILE_PROCESS;
    EnvInit();CHECK(VersionCalls==1&&IsWindows7==seven&&IsWindows10==ten);
#ifdef MAYBE_WOW64
    CHECK(NativeMachine==scenario->Expected&&KernelCalls==1&&ExportCalls==(DWORD)(scenario->Stage!=MissingKernel));
    CHECK(ModernCalls==(DWORD)(scenario->Stage==FailedModern||scenario->Stage==ModernSuccess));
    CHECK(LegacyCalls==(DWORD)(scenario->Stage!=ModernSuccess));
#else
    CHECK(NativeMachine==IMAGE_FILE_PROCESS&&!KernelCalls&&!ExportCalls&&!ModernCalls&&!LegacyCalls);
#endif
    CHECK(GetModuleHandleW(L"kernel32.dll")==BorrowedKernel);++Cases;
}
int main(VOID)
{
    BorrowedKernel=GetModuleHandleW(L"kernel32.dll");CHECK(BorrowedKernel);
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    /* SP1 is the compile/runtime support floor, not an extra value returned by
     * RtlGetNtVersionNumbers. The actual branch only inspects major/minor. */
    const struct {DWORD Major,Minor;BOOL Seven,Ten;} versions[]={
        {6,1,TRUE,FALSE},{6,2,FALSE,FALSE},{6,3,FALSE,FALSE},
        {10,0,FALSE,TRUE},{10,1,FALSE,TRUE},{11,0,FALSE,TRUE},{MAXDWORD,MAXDWORD,FALSE,TRUE}
    };
    ENV_CASE legacy={MissingSymbol,TRUE,FALSE,0,0,IMAGE_FILE_PROCESS};
    for(ULONG i=0;i<_countof(versions);++i)Run(versions[i].Major,versions[i].Minor,versions[i].Seven,versions[i].Ten,&legacy);
#ifdef MAYBE_WOW64
    /* Capability faults are tested on a modern-version model separately from
     * version classification. Missing kernel is defensive, not a claim that
     * a live Windows process can unload its imported kernel32. */
    const ENV_CASE capabilities[]={
        {MissingKernel,TRUE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {MissingKernel,TRUE,TRUE,0,0,IMAGE_FILE_MACHINE_AMD64},
        {MissingKernel,FALSE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {MissingSymbol,TRUE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {MissingSymbol,TRUE,TRUE,0,0,IMAGE_FILE_MACHINE_AMD64},
        {MissingSymbol,FALSE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {FailedModern,TRUE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {FailedModern,TRUE,TRUE,0,0,IMAGE_FILE_MACHINE_AMD64},
        {FailedModern,FALSE,FALSE,0,0,IMAGE_FILE_PROCESS},
        {ModernSuccess,FALSE,FALSE,IMAGE_FILE_MACHINE_UNKNOWN,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_I386},
        {ModernSuccess,FALSE,FALSE,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_ARM64,IMAGE_FILE_MACHINE_ARM64},
        {ModernSuccess,FALSE,FALSE,IMAGE_FILE_MACHINE_I386,IMAGE_FILE_MACHINE_AMD64,IMAGE_FILE_MACHINE_AMD64}
    };
    for(ULONG i=0;i<_countof(capabilities);++i)Run(10,0,FALSE,TRUE,capabilities+i);
#else
    /* Keep the declared boundary adapters referenced under /W4 without
     * executing a branch excluded by the actual production preprocessor. */
    (void)&EnvKernel;(void)&EnvExport;(void)&EnvLegacy;
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
#ifdef MAYBE_WOW64
    CHECK(Cases==19);const char *mode="retained MAYBE_WOW64 source contract";
#else
    CHECK(Cases==7);const char *mode="shipping x64 no-MAYBE_WOW64 branch";
#endif
    printf("PASS: %lu actual EnvInit cases, %s; native borrowed kernel32 unchanged, explicit version/capability/failure expectations, no owned resources; handles %lu/%lu\n",Cases,mode,before,after);
    return 0;
}
