/* SPDX-License-Identifier: GPL-2.0 */
/* Real executor/runtime and OS thread pool; blocking native boundaries are gates. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <assert.h>
#include <string.h>
#include "../../api/logger.h"
static LONG FailAt, Steps, Handles, Allocations, Works, Pins, Results, Disposals, Pools;
static HANDLE Detached;
static BOOL Fail(VOID) { if (InterlockedIncrement(&Steps) != FailAt) return FALSE; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return TRUE; }
static HANDLE WINAPI Event(LPSECURITY_ATTRIBUTES s, BOOL m, BOOL i, LPCWSTR n)
{ if (Fail()) return NULL; HANDLE h=CreateEventW(s,m,i,n); if(h) InterlockedIncrement(&Handles); return h; }
static BOOL WINAPI Close(HANDLE h) { assert(Handles>0); InterlockedDecrement(&Handles); return CloseHandle(h); }
static BOOL WINAPI Token(HANDLE process,DWORD access,PHANDLE token)
{ BOOL okay=OpenProcessToken(process,access,token); if(okay) InterlockedIncrement(&Handles); return okay; }
static LPVOID WINAPI Allocate(HANDLE h,DWORD f,SIZE_T n)
{ if(Fail()) return NULL; LPVOID p=HeapAlloc(h,f,n); if(p) InterlockedIncrement(&Allocations); return p; }
static BOOL WINAPI Deallocate(HANDLE h,DWORD f,LPVOID p)
{ assert(Allocations>0); InterlockedDecrement(&Allocations); return HeapFree(h,f,p); }
static BOOL WINAPI Module(DWORD flags,LPCWSTR address,HMODULE *module)
{ if(Fail()) return FALSE; BOOL okay=GetModuleHandleExW(flags,address,module); if(okay) InterlockedIncrement(&Pins); return okay; }
static BOOL WINAPI Library(HMODULE module) { assert(Pins>0); InterlockedDecrement(&Pins); return FreeLibrary(module); }
static VOID WINAPI Tail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{ assert(Pins>0); InterlockedDecrement(&Pins); FreeLibraryWhenCallbackReturns(instance,module); }
static PTP_WORK WINAPI Work(PTP_WORK_CALLBACK callback,PVOID context,PTP_CALLBACK_ENVIRON environment)
{ if(Fail()) return NULL; PTP_WORK w=CreateThreadpoolWork(callback,context,environment); if(w) InterlockedIncrement(&Works); return w; }
static VOID WINAPI CloseWork(PTP_WORK w) { assert(Works>0); InterlockedDecrement(&Works); CloseThreadpoolWork(w); }
static PTP_POOL WINAPI Pool(PVOID reserved) { if(Fail()) return NULL; PTP_POOL p=CreateThreadpool(reserved); if(p) InterlockedIncrement(&Pools); return p; }
static BOOL WINAPI Minimum(PTP_POOL pool,DWORD count) { return Fail()?FALSE:SetThreadpoolThreadMinimum(pool,count); }
static VOID WINAPI ClosePool(PTP_POOL pool) { assert(Pools>0); InterlockedDecrement(&Pools); CloseThreadpool(pool); }
#define CreateEventW Event
#define CloseHandle Close
#define OpenProcessToken Token
#define HeapAlloc Allocate
#define HeapFree Deallocate
#define GetModuleHandleExW Module
#define FreeLibrary Library
#define FreeLibraryWhenCallbackReturns Tail
#define CreateThreadpoolWork Work
#define CloseThreadpoolWork CloseWork
#define CreateThreadpool Pool
#define SetThreadpoolThreadMinimum Minimum
#define CloseThreadpool ClosePool
#define MAIN_OPERATION_TIMEOUT 250u
#define DllMain ProductionDllMain
#include "../../api/main.c"
#undef DllMain
#undef CreateEventW
#undef CloseHandle
#undef OpenProcessToken
#undef HeapAlloc
#undef HeapFree
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef CreateThreadpool
#undef SetThreadpoolThreadMinimum
#undef CloseThreadpool
VOID AdapterCleanupDetach(VOID) {}
VOID NamespaceInit(VOID) {}
VOID NamespaceDone(VOID) {}
VOID Rundll32Detach(VOID) {}
VOID Rundll32RequestShutdown(VOID) {}
typedef struct OP_TEST_INPUT { HANDLE Entered,Gate,DisposeEntered,DisposeGate; DWORD Mode; WCHAR Name[16]; } OP_TEST_INPUT;
static ULONG_PTR Run(const VOID *opaque)
{
    const OP_TEST_INPUT *input=opaque;
    if(input->Entered) SetEvent(input->Entered);
    if(input->Mode==1 && !MainWaitForObject(input->Gate,NULL,INFINITE)) return 0;
    if(input->Mode==2 && WaitForSingleObject(input->Gate,10000)!=WAIT_OBJECT_0) ExitProcess(71);
    if(input->Mode==3) {
        if(!MainOperationSleep(150) || MainWaitForObject(input->Gate,NULL,200)) return 0;
        return 0;
    }
    if(input->Mode==4) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if(input->Mode==5 && (MainRunOperation(Run,NULL,input,sizeof(*input)) || GetLastError()!=ERROR_BUSY)) ExitProcess(72);
    if(input->Mode==6 && (!MainWaitForObject(input->Gate,NULL,INFINITE) || !ReleaseMutex(input->Gate))) ExitProcess(76);
    if(wcscmp(input->Name,L"owned")) ExitProcess(73);
    OP_TEST_INPUT *result=HeapAlloc(GetProcessHeap(),0,sizeof(*result)); if(!result) return 0;
    *result=*input; InterlockedIncrement(&Results); return (ULONG_PTR)result;
}
static VOID Dispose(ULONG_PTR value)
{
    OP_TEST_INPUT *result=(OP_TEST_INPUT *)value;
    if(result->DisposeEntered) SetEvent(result->DisposeEntered);
    if(result->DisposeGate && WaitForSingleObject(result->DisposeGate,10000)!=WAIT_OBJECT_0) ExitProcess(74);
    InterlockedIncrement(&Disposals); InterlockedDecrement(&Results); HeapFree(GetProcessHeap(),0,result);
}
__declspec(dllexport) VOID WINAPI Configure(HANDLE detached) { Detached=detached; }
__declspec(dllexport) ULONG_PTR WINAPI Execute(const OP_TEST_INPUT *input) { return MainRunOperation(Run,Dispose,input,sizeof(*input)); }
__declspec(dllexport) VOID WINAPI ReleaseResult(ULONG_PTR value) { Dispose(value); }
__declspec(dllexport) VOID WINAPI SetFailure(LONG failure) { InterlockedExchange(&Steps,0); InterlockedExchange(&FailAt,failure); }
__declspec(dllexport) DWORD WINAPI Acquisitions(VOID) { return (DWORD)Steps; }
__declspec(dllexport) DWORD WINAPI Owned(VOID) { return (DWORD)(Handles-(OperationIdle?2:0)+Pools-(OperationPool?1:0)+Allocations+Works+Pins+Results); }
__declspec(dllexport) DWORD WINAPI Disposed(VOID) { return (DWORD)Disposals; }
__declspec(dllexport) DWORD WINAPI Jobs(VOID) { AcquireSRWLockShared(&OperationLock); DWORD count=OperationCount; ReleaseSRWLockShared(&OperationLock); return count; }
__declspec(dllexport) BOOL WINAPI Shutdown(DWORD timeout)
{
    ULONGLONG deadline=GetTickCount64()+timeout; MainRuntimeBeginShutdown();
    return MainOperationShutdown(MainRemaining(deadline,timeout)) && MainRuntimeShutdown(MainRemaining(deadline,timeout));
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,PVOID reserved)
{
    if(reason==DLL_PROCESS_ATTACH) { _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT); }
    BOOL okay=ProductionDllMain(module,reason,reserved);
    if(reason==DLL_PROCESS_DETACH && !reserved) {
        if(Handles || Allocations || Works || Pins || Results || Pools || OperationCount) ExitProcess(75);
        if(Detached) SetEvent(Detached);
    }
    return okay;
}
