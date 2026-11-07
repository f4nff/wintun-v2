/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the real runtime/delay-import DLL fixture. Gate its actual last-release
 * unload, inject only CV failure/spurious wakes, and observe real notifications. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <delayimp.h>
#include <stdio.h>
static BOOL WINAPI WaitCv(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
static VOID WINAPI WakeCv(PCONDITION_VARIABLE);
static BOOL WINAPI UnloadDelay(LPCSTR);
#define SleepConditionVariableSRW WaitCv
#define WakeAllConditionVariable WakeCv
#define __FUnloadDelayLoadedDLL2 UnloadDelay
#include "legacy_runtime_fixture.c"
#undef SleepConditionVariableSRW
#undef WakeAllConditionVariable
#undef __FUnloadDelayLoadedDLL2
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static HANDLE UnloadEntered,UnloadGate;
static volatile LONG WaitError,Spurious,WaitCalls,WakeCalls,UnloadCalls;
static DWORD WaitThreads[8],DistinctWaiters; /* Serialized by DependencyLock. */
static BOOL WINAPI WaitCv(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    CHECK(cv==&DependencyChanged&&lock==&DependencyLock&&!flags);
    DWORD thread=GetCurrentThreadId(),index=0;
    while(index<DistinctWaiters&&WaitThreads[index]!=thread)index++;
    if(index==DistinctWaiters){CHECK(DistinctWaiters<8);WaitThreads[DistinctWaiters++]=thread;}
    LONG n=InterlockedIncrement(&WaitCalls),error=ReadAcquire(&WaitError);
    if(error) {
        if(n>1){fputs("FAILED_WAIT_RETRIED\n",stderr);ExitProcess(42);}
        SetLastError((DWORD)error);return FALSE;
    }
    if(timeout==INFINITE&&InterlockedCompareExchange(&Spurious,0,0)>0) {
        InterlockedDecrement(&Spurious);return TRUE;
    }
    return SleepConditionVariableSRW(cv,lock,timeout,flags);
}
static VOID WINAPI WakeCv(PCONDITION_VARIABLE cv)
{
    CHECK(cv==&DependencyChanged);InterlockedIncrement(&WakeCalls);WakeAllConditionVariable(cv);
}
static BOOL WINAPI UnloadDelay(LPCSTR name)
{
    if(InterlockedIncrement(&UnloadCalls)==1&&UnloadGate) {
        CHECK(SetEvent(UnloadEntered)&&WaitForSingleObject(UnloadGate,10000)==WAIT_OBJECT_0);
    }
    return __FUnloadDelayLoadedDLL2(name);
}
__declspec(dllexport) VOID WINAPI ConfigureWait(HANDLE entered,HANDLE gate,DWORD error,DWORD spurious)
{
    CHECK(!DependencyUnloading&&!DependencyUsers);
    UnloadEntered=entered;UnloadGate=gate;WaitError=(LONG)error;Spurious=(LONG)spurious;
    WaitCalls=WakeCalls=UnloadCalls=0;
    DistinctWaiters=0;memset(WaitThreads,0,sizeof(WaitThreads));
}
__declspec(dllexport) VOID WINAPI ClearWaitFailure(VOID) {InterlockedExchange(&WaitError,0);}
__declspec(dllexport) VOID WINAPI RuntimeState(DWORD *values)
{
    AcquireSRWLockShared(&DependencyLock);
    values[0]=DependencyUsers;values[1]=DependencyUnloading;values[2]=RuntimeStopping;
    values[6]=DistinctWaiters;
    ReleaseSRWLockShared(&DependencyLock);
    values[3]=(DWORD)ReadAcquire(&WaitCalls);values[4]=(DWORD)ReadAcquire(&WakeCalls);
    values[5]=0;
    /* Call only with all runtime callers idle when checking the private heap. */
    if(ModuleHeap) {PROCESS_HEAP_ENTRY e={0};while(HeapWalk(ModuleHeap,&e))if(e.wFlags&PROCESS_HEAP_ENTRY_BUSY)values[5]++;}
}
