/* SPDX-License-Identifier: GPL-2.0 */
/* Actual C synchronous policy/lifecycle. Native I/O and a non-timeout sleep
 * failure are hosted boundaries; real Windows locks/events/private pool. */
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
#ifndef PB_SYNC_POLICY_SESSION_SOURCE
#define PB_SYNC_POLICY_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_SYNC_POLICY_SESSION_SOURCE
#undef SleepConditionVariableSRW
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
enum {QUEUED,PENDING,COMPLETED,REPLACED_PENDING,REPLACED_COMPLETED};
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_SESSION *Current;
static HANDLE FirstEntered,FirstGate,NextEntered,NextGate,StartEntered,StartGate,Done;
static volatile LONG SyncThreadId,FaultArmed,Faults;
static ULONG Schedule;
static uint64_t Replacement;
static PB_RESULT SyncResult;
static DWORD SyncError;
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    if((LONG)GetCurrentThreadId()==SyncThreadId && InterlockedExchange(&FaultArmed,0)) {
        CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);
        InterlockedIncrement(&Faults);
        if(Schedule>=COMPLETED) {
            /* Arrange completion/peer ownership while sleep has released State.
             * Restore its lock before returning the injected failure. */
            ReleaseSRWLockExclusive(lock);
            CHECK(WaitForSingleObject(FirstEntered,5000)==WAIT_OBJECT_0 && SetEvent(FirstGate));
            WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
            if(Schedule>=REPLACED_PENDING) {
                Life.ForgetPolicy(Current,1);
                PbTestFail(Current,PB_TEST_POLICY,FALSE);PbTestBlock(Current,PB_TEST_POLICY,NextEntered,NextGate);
                CHECK(Life.BeginSetPolicy(Current,0,&Replacement)==PB_OK && Replacement==2 && WaitForSingleObject(NextEntered,5000)==WAIT_OBJECT_0);
                if(Schedule==REPLACED_COMPLETED) {CHECK(SetEvent(NextGate));WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);}
            }
            AcquireSRWLockExclusive(lock);
        }
        SetLastError(ERROR_OPERATION_ABORTED);return FALSE;
    }
    return SleepConditionVariableSRW(cv,lock,timeout,flags);
}
static HANDLE Event(VOID)
{HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(event);return event;}
static VOID Create(ULONG schedule,BOOL failure,BOOL inject)
{
    CHECK(!Current && !GuardAllocations);Schedule=schedule;Replacement=0;SyncThreadId=Faults=0;FaultArmed=inject;
    PB_CONFIG config={sizeof(config),1,4,4,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&Current,&region,&layout,&epoch)==PB_OK);Retain(Current);
    FirstEntered=Event();FirstGate=Event();NextEntered=Event();NextGate=Event();StartEntered=Event();StartGate=Event();Done=Event();
    PbTestBlock(Current,PB_TEST_POLICY,FirstEntered,FirstGate);PbTestFail(Current,PB_TEST_POLICY,failure);
    if(schedule==QUEUED) PbTestBlock(Current,PB_TEST_WORKER_START,StartEntered,StartGate);
}
static DWORD WINAPI Sync(VOID *unused)
{(void)unused;SyncThreadId=(LONG)GetCurrentThreadId();SyncResult=Api.SetPolicy(Current,0);SyncError=GetLastError();CHECK(SetEvent(Done));return 0;}
static HANDLE Start(VOID)
{HANDLE thread=CreateThread(NULL,0,Sync,NULL,0,NULL);CHECK(thread);return thread;}
static VOID Join(HANDLE thread)
{CHECK(WaitForSingleObject(Done,5000)==WAIT_OBJECT_0 && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));}
static VOID Slot(uint64_t requested,uint64_t completed,BOOL occupied,BOOL abandoned)
{
    AcquireSRWLockExclusive(&Current->State);
    CHECK(Current->Progress.PolicyRequested==requested && Current->Progress.PolicyCompleted==completed && !!Current->PolicyOccupied==!!occupied && !!Current->PolicyAbandoned==!!abandoned);
    ReleaseSRWLockExclusive(&Current->State);
}
static VOID Busy(VOID)
{uint64_t ticket=UINT64_MAX;CHECK(Life.BeginSetPolicy(Current,0,&ticket)==PB_BUSY && !ticket);}
static VOID Finish(VOID)
{
    CHECK(Api.Close(Current,2000)==PB_OK);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);DropReference(Current);Current=NULL;
    PB_RUNTIME_STATUS status;CHECK(Life.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !status.RetainedBytes && !status.Adapters && !GuardAllocations);
    CHECK(CloseHandle(FirstEntered) && CloseHandle(FirstGate) && CloseHandle(NextEntered) && CloseHandle(NextGate) && CloseHandle(StartEntered) && CloseHandle(StartGate) && CloseHandle(Done));
}
static VOID NextRequest(VOID)
{
    PbTestFail(Current,PB_TEST_POLICY,FALSE);PbTestBlock(Current,PB_TEST_POLICY,NextEntered,NextGate);
    uint64_t ticket;CHECK(Life.BeginSetPolicy(Current,0,&ticket)==PB_OK && ticket==2 && WaitForSingleObject(NextEntered,5000)==WAIT_OBJECT_0);
    Busy();PB_RESULT operation=PB_INVALID;CHECK(Life.PollPolicy(Current,ticket,0,&operation)==PB_EMPTY && operation==PB_INVALID);
    CHECK(SetEvent(NextGate) && Life.PollPolicy(Current,ticket,2000,&operation)==PB_OK && operation==PB_OK);
}
static VOID FailedWait(ULONG schedule,BOOL failure)
{
    Create(schedule,failure,TRUE);HANDLE thread=Start();Join(thread);CHECK(SyncResult==PB_CORRUPT && SyncError==ERROR_OPERATION_ABORTED && Faults==1 && !Current->Fatal);
    if(schedule<=PENDING) {
        CHECK(WaitForSingleObject(schedule==QUEUED?StartEntered:FirstEntered,5000)==WAIT_OBJECT_0);
#ifdef PB_SYNC_POLICY_BASELINE
        Slot(1,0,TRUE,FALSE);
#else
        Slot(1,0,TRUE,TRUE);
#endif
        Busy();CHECK(Current->Region && !Current->ResourcesClosed);
        CHECK(SetEvent(FirstGate) && SetEvent(StartGate));WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    }
    if(schedule<REPLACED_PENDING) {
#ifdef PB_SYNC_POLICY_BASELINE
        Slot(1,1,TRUE,FALSE);Busy();
        /* The synchronous caller never received ticket 1. Only the frozen
         * fixture recovers its orphan explicitly so teardown stays bounded. */
        Life.ForgetPolicy(Current,1);
#else
        Slot(1,1,FALSE,TRUE);
#endif
        NextRequest();
    } else {
        Slot(2,schedule==REPLACED_PENDING?1:2,TRUE,FALSE);Busy();
        PB_RESULT operation=PB_INVALID;
        if(schedule==REPLACED_PENDING) {CHECK(Life.PollPolicy(Current,Replacement,0,&operation)==PB_EMPTY && operation==PB_INVALID);CHECK(SetEvent(NextGate));}
        CHECK(Life.PollPolicy(Current,Replacement,2000,&operation)==PB_OK && operation==PB_OK);
    }
    Finish();
}
static VOID Ordinary(BOOL failure)
{
    Create(PENDING,failure,FALSE);HANDLE thread=Start();CHECK(WaitForSingleObject(FirstEntered,5000)==WAIT_OBJECT_0 && SetEvent(FirstGate));Join(thread);
    CHECK(!Faults && SyncResult==(failure?PB_UNSUPPORTED:PB_OK));WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);Slot(1,1,FALSE,FALSE);NextRequest();Finish();
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    for(ULONG schedule=QUEUED;schedule<=REPLACED_COMPLETED;++schedule) for(ULONG failure=0;failure<2;++failure) FailedWait(schedule,failure!=0);
    Ordinary(FALSE);Ordinary(TRUE);
    Create(PENDING,FALSE,TRUE);CHECK(Api.SetPolicy(Current,0x80000000u)==PB_INVALID && !Faults);Slot(0,0,FALSE,FALSE);Finish();
    puts("PASS: 13 actual synchronous policy scenarios: injected wait failure with queued/pending/completed I/O and pending/completed replacement, native success/failure, ordinary acknowledgement and invalid admission; frozen hidden-ticket orphan reproduced, current abandonment preserves real I/O/replacement ownership and runtime budgets.");return 0;
}
