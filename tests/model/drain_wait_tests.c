/* SPDX-License-Identifier: GPL-2.0 */
/* Actual synchronous Drain and lifecycle; hosted I/O/wait-failure boundaries,
 * real Windows locks/events/caller thread/private worker. No native SYS load. */
#define PB_API_TESTS 1
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
static volatile LONG GuardAllocations;
static volatile LONG OwnedHandles;
static HANDLE WINAPI TrackedEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(attributes,manual,initial,name);if(event) InterlockedIncrement(&OwnedHandles);return event;}
static HANDLE WINAPI TrackedThread(LPSECURITY_ATTRIBUTES attributes,SIZE_T stack,LPTHREAD_START_ROUTINE start,LPVOID argument,DWORD flags,LPDWORD id)
{HANDLE thread=CreateThread(attributes,stack,start,argument,flags,id);if(thread) InterlockedIncrement(&OwnedHandles);return thread;}
static BOOL WINAPI TrackedClose(HANDLE handle)
{BOOL closed=CloseHandle(handle);if(closed) CHECK(InterlockedDecrement(&OwnedHandles)>=0);return closed;}
#define CreateEventW TrackedEvent
#define CreateThread TrackedThread
#define CloseHandle TrackedClose
#ifdef PB_GUARD_TESTS
static VOID *GuardCalloc(SIZE_T count,SIZE_T size)
{CHECK(!size || count<=SIZE_MAX/size);VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);InterlockedIncrement(&GuardAllocations);return p;}
static VOID GuardFree(VOID *p) {if(p){PbTestGuardFree(p);CHECK(InterlockedDecrement(&GuardAllocations)>=0);}}
static VOID *WINAPI GuardRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{CHECK(!address && type==(MEM_RESERVE|MEM_COMMIT) && protection==PAGE_READWRITE);return GuardCalloc(1,size);}
static BOOL WINAPI GuardUnmap(VOID *p,SIZE_T size,DWORD type)
{CHECK(!size && type==MEM_RELEASE);GuardFree(p);return TRUE;}
#define calloc GuardCalloc
#define free GuardFree
#define VirtualAlloc GuardRegion
#define VirtualFree GuardUnmap
#endif
#define SleepConditionVariableSRW ObservedSleep
#ifndef PB_DRAIN_WAIT_SESSION_SOURCE
#define PB_DRAIN_WAIT_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_DRAIN_WAIT_SESSION_SOURCE
#undef SleepConditionVariableSRW
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
enum {QUEUED,PENDING,COMPLETED};
enum {FAIL_WAIT,REAL_WAIT,SPURIOUS_WAKE};
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_SESSION *Current;
static PB_USER_QUEUE *Queue;
static PB_TOKEN Loan;
static HANDLE StartEntered,StartGate,DrainEntered,DrainGate,SleepEntered,Done;
static LONG CallerId;
static ULONG Schedule,Mode,FailureBudget,Calls,Failures,Spurious,Recoveries;
static ULONG TotalFailures,TotalFailureCalls,TotalRecoveries;
static BOOL ExpectFixed=TRUE;
static PB_RESULT DrainResult;
static DWORD DrainError;
static HANDLE Event(VOID)
{HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(event);return event;}
static VOID OpenGates(VOID) {CHECK(SetEvent(StartGate) && SetEvent(DrainGate));}
/* Called with State held, and returns with it held, just like sleep. All
 * completion state is written by the actual worker, never by this observer. */
static VOID CompleteWhileUnlocked(PSRWLOCK lock)
{
    ReleaseSRWLockExclusive(lock);OpenGates();AcquireSRWLockExclusive(lock);
    ULONGLONG deadline=GetTickCount64()+5000;
    while(!Current->Draining && Current->Progress.Result!=PB_CORRUPT) {
        DWORD remaining=Remaining(deadline,5000);CHECK(remaining);
        CHECK(SleepConditionVariableSRW(&Current->Changed,lock,remaining,0) || GetLastError()==ERROR_TIMEOUT);
    }
}
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    if((LONG)GetCurrentThreadId()!=CallerId) return SleepConditionVariableSRW(cv,lock,timeout,flags);
    CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);
    ++Calls;
    if(Mode==FAIL_WAIT) {
        if(Calls==1) {
            /* Establish queued/pending I/O before reporting failure. No lock
             * is held by this fixture while waiting for the worker boundary. */
            ReleaseSRWLockExclusive(lock);
            CHECK(WaitForSingleObject(Schedule==QUEUED?StartEntered:DrainEntered,5000)==WAIT_OBJECT_0);
            AcquireSRWLockExclusive(lock);
            if(Schedule==COMPLETED) CompleteWhileUnlocked(lock);
        }
        if(Calls<=FailureBudget) {++Failures;SetLastError(ERROR_OPERATION_ABORTED);return FALSE;}
        /* Frozen-only bounded recovery after repeated immediate failures.
         * Production must return on the first failure and never reach here. */
        CHECK(Calls==FailureBudget+1 && !Recoveries++);
        CompleteWhileUnlocked(lock);return TRUE;
    }
    if(Mode==SPURIOUS_WAKE && Calls==1) {++Spurious;return TRUE;}
    CHECK(SetEvent(SleepEntered));return SleepConditionVariableSRW(cv,lock,timeout,flags);
}
static VOID Create(ULONG schedule,ULONG mode,ULONG budget,BOOL nativeFailure)
{
    CHECK(!Current && !GuardAllocations && !OwnedHandles);Schedule=schedule;Mode=mode;FailureBudget=budget;
    Calls=Failures=Spurious=Recoveries=0;CallerId=0;DrainResult=PB_INVALID;DrainError=0;
    PB_CONFIG config={sizeof(config),1,4,4,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&Current,&region,&layout,&epoch)==PB_OK);Retain(Current);
    StartEntered=Event();StartGate=Event();DrainEntered=Event();DrainGate=Event();SleepEntered=Event();Done=Event();
    PbTestBlock(Current,PB_TEST_WORKER_START,StartEntered,StartGate);PbTestBlock(Current,PB_TEST_DRAIN,DrainEntered,DrainGate);
    if(schedule!=QUEUED) CHECK(SetEvent(StartGate));
    PbTestFail(Current,PB_TEST_DRAIN,nativeFailure);PbTestActive(Current,1);
    CHECK(Api.OpenQueue(Current,0,&Queue)==PB_OK);PB_LEASE lease;uint32_t count;
    CHECK(Api.ReserveBatch(Queue,&lease,1,&count)==PB_OK && count==1);Loan=lease.Descriptor.Token;
}
static DWORD WINAPI Caller(VOID *unused)
{(void)unused;CallerId=(LONG)GetCurrentThreadId();DrainResult=Api.Drain(Current);DrainError=GetLastError();CHECK(SetEvent(Done));return 0;}
static VOID Join(HANDLE thread)
{CHECK(WaitForSingleObject(Done,5000)==WAIT_OBJECT_0 && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));}
static VOID Finish(VOID)
{
    CHECK(Current->CloseRequested && !Current->Fatal && Current->Region && !Current->ResourcesClosed && Queue->Loans==1);
    CHECK(WaitForSingleObject(Current->ClosingEvent,0)==WAIT_OBJECT_0 && Api.Wait(Queue,INFINITE)==PB_STOPPED);
    PB_LEASE lease;uint32_t count;CHECK(Api.ReserveBatch(Queue,&lease,1,&count)==PB_STOPPED && !count && Queue->Loans==1);
    CHECK(Api.PrepareClose(Current,0)==PB_BUSY && !Current->ResourcesClosed);
    CHECK(Api.CancelBatch(Queue,&Loan,1)==PB_OK && !Queue->Loans);
    PB_RESULT prepared=Api.PrepareClose(Current,0);CHECK((prepared==PB_QUARANTINED || prepared==PB_CORRUPT) && !Current->ResourcesClosed);
    PB_RUNTIME_STATUS status;CHECK(Life.GetRuntimeStatus(&status)==PB_OK && status.Sessions==1 && status.RetainedBytes==Current->Layout.RegionSize);
    /* The error return does not acknowledge native drain or clear its active
     * references. Repair only the hosted I/O fault, then release the real gates. */
    PbTestFail(Current,PB_TEST_DRAIN,FALSE);OpenGates();
    AcquireSRWLockExclusive(&Current->State);ULONGLONG deadline=GetTickCount64()+5000;
    while(!Current->Draining) {
        DWORD remaining=Remaining(deadline,5000);CHECK(remaining);
        CHECK(SleepConditionVariableSRW(&Current->Changed,&Current->State,remaining,0) || GetLastError()==ERROR_TIMEOUT);
    }
    ReleaseSRWLockExclusive(&Current->State);
    CHECK(Api.PrepareClose(Current,0)==PB_QUARANTINED && !Current->ResourcesClosed && Current->Region);
    PbTestActive(Current,0);CHECK(Api.Close(Current,2000)==PB_OK);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);DropReference(Current);Current=NULL;Queue=NULL;
    CHECK(Life.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !status.RetainedBytes && !status.Adapters && !GuardAllocations);
    CHECK(CloseHandle(StartEntered) && CloseHandle(StartGate) && CloseHandle(DrainEntered) && CloseHandle(DrainGate) && CloseHandle(SleepEntered) && CloseHandle(Done));
    CHECK(!OwnedHandles);
}
static VOID FailedWait(ULONG schedule,ULONG budget,BOOL nativeFailure)
{
    Create(schedule,FAIL_WAIT,budget,nativeFailure);HANDLE thread=CreateThread(NULL,0,Caller,NULL,0,NULL);CHECK(thread);Join(thread);
    if(ExpectFixed) {
        CHECK(DrainResult==PB_CORRUPT && DrainError==ERROR_OPERATION_ABORTED && Failures==1 && Calls==1 && !Recoveries);
        if(schedule!=COMPLETED) CHECK(!Current->Draining); /* Actual DRAIN remains blocked. */
    } else CHECK(DrainResult==(nativeFailure?PB_CORRUPT:PB_OK) && Failures==budget && Calls==budget+(schedule==COMPLETED?0u:1u) && Recoveries==(ULONG)(schedule!=COMPLETED));
    TotalFailures+=Failures;TotalFailureCalls+=Calls;TotalRecoveries+=Recoveries;
    Finish();
}
static VOID Ordinary(ULONG schedule,BOOL spurious,BOOL nativeFailure)
{
    Create(schedule,spurious?SPURIOUS_WAKE:REAL_WAIT,0,nativeFailure);
    HANDLE thread=CreateThread(NULL,0,Caller,NULL,0,NULL);CHECK(thread && WaitForSingleObject(SleepEntered,5000)==WAIT_OBJECT_0);
    CHECK(!Current->Draining && WaitForSingleObject(Done,0)==WAIT_TIMEOUT);
    OpenGates();Join(thread);CHECK(DrainResult==(nativeFailure?PB_CORRUPT:PB_OK) && !Failures && !Recoveries && Spurious==(ULONG)spurious && Calls>=1+(ULONG)spurious);
    Finish();
}
int main(int argc,char **argv)
{
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--require-error-exit")));
#ifdef PB_DRAIN_WAIT_BASELINE
    ExpectFixed=argc==2;
#endif
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK && Api.Drain(NULL)==PB_INVALID);
    for(ULONG schedule=QUEUED;schedule<=PENDING;++schedule) for(ULONG failure=0;failure<2;++failure) {
        FailedWait(schedule,1,failure!=0);FailedWait(schedule,8,failure!=0);
        Ordinary(schedule,FALSE,failure!=0);Ordinary(schedule,TRUE,failure!=0);
    }
    FailedWait(COMPLETED,1,FALSE);FailedWait(COMPLETED,1,TRUE);
    CHECK(TotalFailures==(ExpectFixed?10u:38u) && TotalFailureCalls==(ExpectFixed?10u:46u) && TotalRecoveries==(ExpectFixed?0u:8u));
    printf("PASS: wait-failure counts injected=%lu caller_wait_calls=%lu bounded_recoveries=%lu fixed=%u.\n",TotalFailures,TotalFailureCalls,TotalRecoveries,ExpectFixed);
    puts("PASS: 19 actual Drain wait scenarios: queued/pending/completed native I/O, one/eight immediate wait failures, native success/failure, real wait/spurious wake and null admission; frozen retries require bounded explicit recovery, current returns original wait error once; user loan/native reference barriers and runtime/guarded/handle balance preserved.");return 0;
}
