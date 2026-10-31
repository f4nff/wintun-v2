/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
typedef struct { CRITICAL_SECTION Lock; LONG References; BOOLEAN Closing; HANDLE Idle; } EX_RUNDOWN_REF;
typedef struct { VOID *Reserved; } DEVICE_OBJECT;
typedef struct { EX_RUNDOWN_REF ControlRundown; volatile LONG Halting; DEVICE_OBJECT *FunctionalDeviceObject; } TUN_CTX;
static SRWLOCK TunDispatchCtxGuard = SRWLOCK_INIT;
static HANDLE RevokeWaiting, RevokeDone;
static VOID KeEnterCriticalRegion(VOID) {}
static VOID KeLeaveCriticalRegion(VOID) {}
static VOID ExAcquireResourceSharedLite(SRWLOCK *lock,BOOLEAN wait) { (void)wait; AcquireSRWLockShared(lock); }
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait) { (void)wait; AcquireSRWLockExclusive(lock); }
static __declspec(thread) BOOLEAN Exclusive;
/* The helpers acquire either kind once per critical section. */
static VOID ReleaseResource(SRWLOCK *lock) { if (Exclusive) { Exclusive=FALSE; ReleaseSRWLockExclusive(lock); } else ReleaseSRWLockShared(lock); }
static VOID AcquireExclusive(SRWLOCK *lock,BOOLEAN wait) { ExAcquireResourceExclusiveLite(lock,wait); Exclusive=TRUE; }
#define ExAcquireResourceExclusiveLite AcquireExclusive
#define ExReleaseResourceLite ReleaseResource
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *refs)
{
    EnterCriticalSection(&refs->Lock); BOOLEAN allowed=!refs->Closing;
    if (allowed) { refs->References++; ResetEvent(refs->Idle); }
    LeaveCriticalSection(&refs->Lock); return allowed;
}
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *refs)
{
    EnterCriticalSection(&refs->Lock); CHECK(refs->References>0);
    if (!--refs->References) SetEvent(refs->Idle);
    LeaveCriticalSection(&refs->Lock);
}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{
    EnterCriticalSection(&refs->Lock); refs->Closing=TRUE; LeaveCriticalSection(&refs->Lock);
    SetEvent(RevokeWaiting); CHECK(WaitForSingleObject(refs->Idle,5000)==WAIT_OBJECT_0);
}
#include "../../driver/context_gate.h"
static DWORD WINAPI Revoke(VOID *arg) { TunRevokeContext(arg); SetEvent(RevokeDone); return 0; }
int main(VOID)
{
    TUN_CTX ctx={0}; DEVICE_OBJECT device={0}; ctx.FunctionalDeviceObject=&device;
    InitializeCriticalSection(&ctx.ControlRundown.Lock); ctx.ControlRundown.Idle=CreateEventW(NULL,TRUE,TRUE,NULL);
    RevokeWaiting=CreateEventW(NULL,TRUE,FALSE,NULL); RevokeDone=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(ctx.ControlRundown.Idle && RevokeWaiting && RevokeDone);
    CHECK(!TunReferenceDeviceCtx(&device)); /* Not yet published. */
    device.Reserved=&ctx;
    for (ULONG i=0;i<100000;++i) { TUN_CTX *ref=TunReferenceDeviceCtx(&device); CHECK(ref==&ctx); TunDereferenceCtx(ref); }
    TUN_CTX *held=TunReferenceDeviceCtx(&device); CHECK(held==&ctx);
    CHECK(TryAcquireSRWLockExclusive(&TunDispatchCtxGuard)); ReleaseSRWLockExclusive(&TunDispatchCtxGuard);
    HANDLE thread=CreateThread(NULL,0,Revoke,&ctx,0,NULL); CHECK(thread);
    CHECK(WaitForSingleObject(RevokeWaiting,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(RevokeDone,0)==WAIT_TIMEOUT);
    /* This is the callback cycle that used to wait behind Halt's global writer. */
    CHECK(!TunReferenceDeviceCtx(&device));
    CHECK(TryAcquireSRWLockExclusive(&TunDispatchCtxGuard)); ReleaseSRWLockExclusive(&TunDispatchCtxGuard);
    CHECK(!TunReferenceCtxLocked(&ctx));
    TunDereferenceCtx(held); CHECK(WaitForSingleObject(RevokeDone,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && !ctx.ControlRundown.References);
    CloseHandle(thread); CloseHandle(RevokeWaiting); CloseHandle(RevokeDone); CloseHandle(ctx.ControlRundown.Idle);
    DeleteCriticalSection(&ctx.ControlRundown.Lock);
    puts("PASS: actual context gate, 100000 lookups, revoke waits for existing refs without holding global lookup lock; nested lookup rejects immediately.");
    return 0;
}
