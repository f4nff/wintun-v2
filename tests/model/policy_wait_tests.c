/* SPDX-License-Identifier: GPL-2.0 */
/* Actual policy/lifecycle with real Windows waits, locks and private thread pool.
 * The hosted policy I/O boundary never installs or loads a driver. */
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
#ifndef PB_POLICY_WAIT_SESSION_SOURCE
#define PB_POLICY_WAIT_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_POLICY_WAIT_SESSION_SOURCE
#undef SleepConditionVariableSRW
#ifdef PB_GUARD_TESTS
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#endif
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_SESSION *Current;
static HANDLE FirstEntered,FirstGate,NextEntered,NextGate,WaitEntered,ResumeEntered,ResumeGate,PollDone;
static volatile LONG PollThreadId,Gated,Sleeps;
static BOOLEAN SleepFailure;
static uint64_t OldTicket;
static uint32_t PollTimeout;
static PB_RESULT PollResult,Operation;
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    BOOL watched=(LONG)GetCurrentThreadId()==PollThreadId;
    if(watched) {
        CHECK(Current && cv==&Current->Changed && lock==&Current->State && !flags);
        InterlockedIncrement(&Sleeps);CHECK(SetEvent(WaitEntered));
        if(SleepFailure) {SetLastError(ERROR_OPERATION_ABORTED);return FALSE;}
    }
    BOOL result=SleepConditionVariableSRW(cv,lock,timeout,flags);DWORD error=GetLastError();
    if(watched && !InterlockedExchange(&Gated,1)) {
        /* The caller has observed no state since Sleep. Briefly unlock and
         * delay its return to model another thread winning reacquisition.
         * Restore the exclusive lock before returning; mutate no session state. */
        ReleaseSRWLockExclusive(lock);
        CHECK(SetEvent(ResumeEntered) && WaitForSingleObject(ResumeGate,5000)==WAIT_OBJECT_0);
        AcquireSRWLockExclusive(lock);
    }
    if(!result) SetLastError(error);
    return result;
}
static HANDLE Event(BOOL manual)
{HANDLE event=CreateEventW(NULL,manual,FALSE,NULL);CHECK(event);return event;}
static VOID Create(VOID)
{
    CHECK(!Current && !GuardAllocations);PollThreadId=Gated=Sleeps=0;SleepFailure=FALSE;
    PB_CONFIG config={sizeof(config),1,4,4,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&Current,&region,&layout,&epoch)==PB_OK);Retain(Current);
    FirstEntered=Event(TRUE);FirstGate=Event(TRUE);NextEntered=Event(TRUE);NextGate=Event(TRUE);
    WaitEntered=Event(FALSE);ResumeEntered=Event(TRUE);ResumeGate=Event(TRUE);PollDone=Event(TRUE);
    PbTestBlock(Current,PB_TEST_POLICY,FirstEntered,FirstGate);
    CHECK(Life.BeginSetPolicy(Current,0,&OldTicket)==PB_OK && WaitForSingleObject(FirstEntered,5000)==WAIT_OBJECT_0);
}
static VOID CheckSlot(uint64_t requested,uint64_t completed,BOOL occupied)
{
    AcquireSRWLockExclusive(&Current->State);
    CHECK(Current->Progress.PolicyRequested==requested && Current->Progress.PolicyCompleted==completed && Current->PolicyOccupied==occupied);
    ReleaseSRWLockExclusive(&Current->State);
}
static DWORD WINAPI Poll(VOID *unused)
{
    (void)unused;PollThreadId=(LONG)GetCurrentThreadId();Operation=PB_INVALID;
    PollResult=Life.PollPolicy(Current,OldTicket,PollTimeout,&Operation);CHECK(SetEvent(PollDone));return 0;
}
static HANDLE StartPoll(uint32_t timeout)
{
    PollTimeout=timeout;HANDLE thread=CreateThread(NULL,0,Poll,NULL,0,NULL);
    CHECK(thread && WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0);return thread;
}
static VOID JoinPoll(HANDLE thread)
{CHECK(WaitForSingleObject(PollDone,5000)==WAIT_OBJECT_0 && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));}
static VOID Finish(VOID)
{
    CHECK(Api.Close(Current,2000)==PB_OK);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->ResourcesClosed && !Current->Region);DropReference(Current);Current=NULL;
    PB_RUNTIME_STATUS runtime;CHECK(Life.GetRuntimeStatus(&runtime)==PB_OK && !runtime.Sessions && !runtime.RetainedBytes && !runtime.Adapters && !GuardAllocations);
    HANDLE events[]={FirstEntered,FirstGate,NextEntered,NextGate,WaitEntered,ResumeEntered,ResumeGate,PollDone};
    for(ULONG i=0;i<_countof(events);++i) CHECK(CloseHandle(events[i]));
}
static VOID RetiredWaiter(BOOL forgotten,ULONG replacement)
{
    Create();HANDLE poll=StartPoll(replacement==2?200:INFINITE);
    CHECK(SetEvent(FirstGate) && WaitForSingleObject(ResumeEntered,5000)==WAIT_OBJECT_0);
    WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);CheckSlot(OldTicket,OldTicket,TRUE);
    PB_RESULT applied=PB_INVALID;
    if(forgotten) Life.ForgetPolicy(Current,OldTicket);
    else CHECK(Life.PollPolicy(Current,OldTicket,0,&applied)==PB_OK && applied==PB_OK);
    CheckSlot(OldTicket,OldTicket,FALSE);
    uint64_t next=0,third=0;
    if(replacement) {
        PbTestBlock(Current,PB_TEST_POLICY,NextEntered,NextGate);
        CHECK(Life.BeginSetPolicy(Current,0,&next)==PB_OK && next>OldTicket && WaitForSingleObject(NextEntered,5000)==WAIT_OBJECT_0);
        CheckSlot(next,OldTicket,TRUE);CHECK(Life.BeginSetPolicy(Current,0,&third)==PB_BUSY && !third);
        if(replacement==2) {CHECK(SetEvent(NextGate));WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);CheckSlot(next,next,TRUE);}
    }
    CHECK(SetEvent(ResumeGate));JoinPoll(poll);
#ifdef PB_POLICY_WAIT_BASELINE
    if(replacement==2) CHECK(PollResult==PB_EMPTY && Operation==PB_INVALID);
    else CHECK(PollResult==PB_OK && Operation==PB_OK); /* False second acknowledgement. */
    CheckSlot(replacement?next:OldTicket,replacement==2?next:OldTicket,replacement==2);
#else
    CHECK(PollResult==PB_STALE && Operation==PB_INVALID && Sleeps==1);
    CheckSlot(replacement?next:OldTicket,replacement==2?next:OldTicket,replacement!=0);
#endif
    if(replacement) {
        PB_RESULT begun=Life.BeginSetPolicy(Current,0,&third);
#ifdef PB_POLICY_WAIT_BASELINE
        if(replacement==1) {
            CHECK(begun==PB_OK && third>next);
            applied=PB_INVALID;CHECK(Life.PollPolicy(Current,next,0,&applied)==PB_STALE && applied==PB_INVALID);
        } else CHECK(begun==PB_BUSY && !third);
#else
        CHECK(begun==PB_BUSY && !third);
#endif
        CHECK(SetEvent(NextGate));WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
        CHECK(Life.PollPolicy(Current,third?third:next,0,&applied)==PB_OK && applied==PB_OK);
    }
    Finish();
}
static VOID UnreadyWake(BOOL stopping)
{
    Create();PB_USER_QUEUE *q;PB_LEASE loan;uint32_t count;
    CHECK(Api.OpenQueue(Current,0,&q)==PB_OK && Api.ReserveBatch(q,&loan,1,&count)==PB_OK && count==1);
    PbTestActive(Current,1);HANDLE poll=StartPoll(INFINITE);
    if(stopping) CHECK(Life.BeginStop(Current)==PB_OK);
    else {AcquireSRWLockExclusive(&Current->State);WakeAllConditionVariable(&Current->Changed);ReleaseSRWLockExclusive(&Current->State);}
    CHECK(WaitForSingleObject(ResumeEntered,5000)==WAIT_OBJECT_0);CheckSlot(OldTicket,0,TRUE);
    CHECK(SetEvent(ResumeGate) && WaitForSingleObject(WaitEntered,5000)==WAIT_OBJECT_0 && Sleeps>=2 && WaitForSingleObject(PollDone,0)==WAIT_TIMEOUT);
    CHECK(SetEvent(FirstGate));JoinPoll(poll);CHECK(PollResult==PB_OK && Operation==PB_OK && Sleeps>=2);
    PB_CLOSE_PROGRESS progress;CHECK(Life.GetCloseProgress(Current,&progress)==PB_OK && progress.UserLeases==1 && progress.RetainedBytes && !Current->ResourcesClosed && Current->TestActive==1);
    loan.Data[0]=0x5a;CHECK(Api.CancelBatch(q,&loan.Descriptor.Token,1)==PB_OK);PbTestActive(Current,0);Finish();
}
static VOID TimeoutAndFailure(BOOL failure)
{
    Create();PB_RESULT applied=PB_INVALID;uint64_t next=0;
    if(failure) {SleepFailure=TRUE;HANDLE poll=StartPoll(INFINITE);JoinPoll(poll);CHECK(PollResult==PB_CORRUPT && Operation==PB_INVALID && Sleeps==1);SleepFailure=FALSE;}
    else CHECK(Life.PollPolicy(Current,OldTicket,5,&applied)==PB_EMPTY && applied==PB_INVALID);
    CheckSlot(OldTicket,0,TRUE);CHECK(Life.BeginSetPolicy(Current,0,&next)==PB_BUSY && !next);
    CHECK(SetEvent(FirstGate) && Life.PollPolicy(Current,OldTicket,2000,&applied)==PB_OK && applied==PB_OK);Finish();
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    for(ULONG forgotten=0;forgotten<2;++forgotten) for(ULONG replacement=0;replacement<3;++replacement) RetiredWaiter((BOOL)forgotten,replacement);
    UnreadyWake(FALSE);UnreadyWake(TRUE);TimeoutAndFailure(FALSE);TimeoutAndFailure(TRUE);
#ifdef PB_POLICY_WAIT_BASELINE
    puts("PASS: ten actual policy wait scenarios: old retired waiters double-acknowledge/clear pending replacement slots, admit a third request and invalidate the second ticket; completed replacements make finite old waiters time out on an unreachable completion. Unready/stop wakes, timeout and injected sleep failure retain pending ownership; balanced teardown.");
#else
    puts("PASS: ten actual policy wait scenarios: retired waiters return STALE without output mutation or a second sleep, pending/completed replacements retain their slot, third requests stay BUSY. Unready/stop wakes rewait then acknowledge once; user loan/native barrier, timeout and injected sleep failure preserve ownership; balanced teardown.");
#endif
    return 0;
}
