/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the native-policy fixture's complete driver boundary and resource
 * ledger. The generated session entry redirects only I/O and lock entry. */
#ifdef PB_API_TESTS
#error Status lifetime fixture must execute the shipping session implementation
#endif
#include "guard_memory.h"
#include "../model/status_lifetime_fixture.h"
static SL_CONFIG *ReaderConfig;
static volatile LONG ReaderThread,ReaderHeld,ExclusiveImmediate,ExclusiveHeld,IoCalls;
static BOOL WINAPI ReaderIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static VOID WINAPI ReaderAcquire(PSRWLOCK);
#ifndef PB_STATUS_LIFETIME_SESSION_SOURCE
#error Build through Build-StatusLifetimeTests for explicit source binding
#endif
#include "native_policy_fixture.c"
static BOOL WINAPI ReaderIo(HANDLE h,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlap)
{
    InterlockedIncrement(&IoCalls);
    if(GetCurrentThreadId()==(DWORD)ReadAcquire(&ReaderThread)) {
        CHECK(ReaderConfig && Current && (code==PB_V2_IOCTL_QUERY || code==PB_PROBE_IOCTL_QUERY));
        InterlockedExchange(&ReaderHeld,1);CHECK(SetEvent(ReaderConfig->Events[SL_READ_ENTERED]));
        CHECK(WaitForSingleObject(ReaderConfig->Events[SL_READ_GATE],10000)==WAIT_OBJECT_0);
        /* Mutation cleanup keeps the actual exclusive worker parked before
         * ReleaseResources until this unpinned call has returned and joined. */
        if(ReaderConfig->Fails) {InterlockedExchange(&ReaderHeld,0);SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        BOOL result=HostedIo(h,code,input,inputSize,output,outputSize,returned,overlap);
        InterlockedExchange(&ReaderHeld,0);return result;
    }
    return HostedIo(h,code,input,inputSize,output,outputSize,returned,overlap);
}
static VOID WINAPI ReaderAcquire(PSRWLOCK lock)
{
    if(Current && lock==&Current->Lifetime) {
        CHECK(ReaderConfig && ReaderHeld && !ExclusiveHeld);
        /* A real immediate exclusive acquisition makes the missing-reader-pin
         * witness deterministic. The normal path falls through to the same
         * blocking SRW exclusive acquisition as production. */
        BOOL immediate=TryAcquireSRWLockExclusive(lock);InterlockedExchange(&ExclusiveImmediate,immediate);
        CHECK(SetEvent(ReaderConfig->Events[SL_EXCLUSIVE_ATTEMPTED]));
        if(!immediate)AcquireSRWLockExclusive(lock);
        InterlockedExchange(&ExclusiveHeld,1);CHECK(SetEvent(ReaderConfig->Events[SL_EXCLUSIVE_HELD]));
        CHECK(WaitForSingleObject(ReaderConfig->Events[SL_EXCLUSIVE_GATE],10000)==WAIT_OBJECT_0);
        InterlockedExchange(&ExclusiveHeld,0);return;
    }
    ObservedAcquire(lock);
}
__declspec(dllexport) VOID WINAPI ConfigureStatusReader(SL_CONFIG *config)
{CHECK(Current && config && !ReaderConfig);ReaderConfig=config;}
__declspec(dllexport) VOID WINAPI MarkStatusReader(VOID)
{CHECK(ReaderConfig && !ReaderThread);InterlockedExchange(&ReaderThread,(LONG)GetCurrentThreadId());}
__declspec(dllexport) VOID WINAPI ReadStatusLifetime(SL_SNAPSHOT *out)
{
    CHECK(out);*out=(SL_SNAPSHOT){.Core=Current!=NULL,.ReaderHeld=ReaderHeld,.ExclusiveImmediate=ExclusiveImmediate,.ExclusiveHeld=ExclusiveHeld,.IoCalls=IoCalls,
        .Allocations=Allocations,.Regions=Regions,.Events=Events,.Works=Works,.Pools=Pools,.Pins=Pins,.Callbacks=CallbackCount,.Finished=Finished,.Active=ActiveCallbacks};
    AcquireSRWLockShared(&RuntimeLock);out->Sessions=RuntimeCount;out->Bytes=RuntimeBytes;ReleaseSRWLockShared(&RuntimeLock);
}
