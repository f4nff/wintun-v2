/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the complete native-policy driver boundary and ownership ledger. The
 * generated session entry redirects only three test accounting functions and
 * shared-lock entry; it does not change production code in the current build. */
#ifdef PB_API_TESTS
#error Retained owner regression requires the shipping session implementation
#endif
#include "guard_memory.h"
#include "../model/retained_owner_fixture.h"
static HANDLE DiagnosticEntered,DiagnosticGate;
static volatile LONG DiagnosticThread,DiagnosticArmed,DiagnosticHeld;
static volatile LONG DestroyThread,PoolCloseThread,CoreUnpinThread;
static VOID RetainedFree(VOID *);
static VOID WINAPI RetainedPoolClose(PTP_POOL);
static BOOL WINAPI RetainedUnpin(HMODULE);
static VOID WINAPI RetainedShared(PSRWLOCK);
/* Definitions before the generated session entry retain their base names.
 * At session entry the aliases switch to the small observer wrappers below.
 * This keeps one owner implementation instead of copying its resource model. */
#define TrackedFree RetainedBaseFree
#define TrackedPoolClose RetainedBasePoolClose
#define TrackedUnpin RetainedBaseUnpin
#ifndef PB_RETAINED_SESSION_SOURCE
#error Build through Build-RetainedOwnerTests for explicit session source binding
#endif
#include "native_policy_fixture.c"
#undef TrackedFree
#undef TrackedPoolClose
#undef TrackedUnpin
static VOID RetainedFree(VOID *p)
{
    if(p && p==Current)InterlockedExchange(&DestroyThread,(LONG)GetCurrentThreadId());
    RetainedBaseFree(p);
}
static VOID WINAPI RetainedPoolClose(PTP_POOL pool)
{
    InterlockedExchange(&PoolCloseThread,(LONG)GetCurrentThreadId());RetainedBasePoolClose(pool);
}
static BOOL WINAPI RetainedUnpin(HMODULE module)
{
    InterlockedExchange(&CoreUnpinThread,(LONG)GetCurrentThreadId());return RetainedBaseUnpin(module);
}
static VOID WINAPI RetainedShared(PSRWLOCK lock)
{
    /* GetRetained has already looked up and (normally) pinned this core, then
     * released RuntimeLock. GetCloseProgress has not acquired State or read
     * any shell fields yet. No observer Retain masks a missing diagnostic pin. */
    if(Current && GetCurrentThreadId()==(DWORD)ReadAcquire(&DiagnosticThread) && lock==&Current->State &&
        InterlockedCompareExchange(&DiagnosticArmed,0,1)==1) {
        InterlockedExchange(&DiagnosticHeld,1);CHECK(SetEvent(DiagnosticEntered));
        CHECK(WaitForSingleObject(DiagnosticGate,10000)==WAIT_OBJECT_0);InterlockedExchange(&DiagnosticHeld,0);
    }
    AcquireSRWLockShared(lock);
}
__declspec(dllexport) VOID WINAPI RetainedConfigure(HANDLE entered,HANDLE gate)
{CHECK(Current && entered && gate);DiagnosticEntered=entered;DiagnosticGate=gate;}
__declspec(dllexport) VOID WINAPI RetainedMark(VOID)
{
    CHECK(Current && !DiagnosticArmed && !DiagnosticThread);
    InterlockedExchange(&DiagnosticThread,(LONG)GetCurrentThreadId());InterlockedExchange(&DiagnosticArmed,1);
}
__declspec(dllexport) VOID WINAPI RetainedRead(RO_SNAPSHOT *out)
{
    CHECK(out);memset(out,0,sizeof(*out));PB_SESSION *s=Current;
    if(s) {
        /* Either the externally owned public handle, or a hard callback/diag
         * gate, keeps this shell alive. Read without adding a hidden owner. */
        CHECK(DiagnosticHeld || ParkHeld || FinalHeld || !s->PublicReleased);
        AcquireSRWLockShared(&s->State);
        out->Core=1;out->References=ReadAcquire(&s->References);out->Phase=(LONG)s->Progress.Phase;
        out->Closed=s->ResourcesClosed;out->Registered=s->Registered;out->SessionId=s->Epoch;
        ReleaseSRWLockShared(&s->State);
    }
    out->DiagnosticHeld=DiagnosticHeld;out->Allocations=Allocations;out->Regions=Regions;out->Events=Events;out->Works=Works;out->Pools=Pools;out->Pins=Pins;
    out->Callbacks=CallbackCount;out->Finished=Finished;out->Active=ActiveCallbacks;out->DestroyThread=DestroyThread;
    out->PoolCloseThread=PoolCloseThread;out->CoreUnpinThread=CoreUnpinThread;out->CallbackThread=CallbackThread;
    AcquireSRWLockShared(&RuntimeLock);out->Sessions=RuntimeCount;out->Bytes=RuntimeBytes;ReleaseSRWLockShared(&RuntimeLock);
}
