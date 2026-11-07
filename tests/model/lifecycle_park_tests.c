/* SPDX-License-Identifier: GPL-2.0 */
/* Real lifecycle worker and protected device handle. No installed SYS required. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
static VOID ParkEnter(VOID);
static DWORD ParkResult(HANDLE,DWORD);
static VOID WINAPI ParkSleep(DWORD);
#define PB_CLOSE_RETRY_PARK_ENTER ParkEnter
#define PB_CLOSE_RETRY_PARK_RESULT ParkResult
#define Sleep ParkSleep
#define main ExistingCloseRetryMain
#include "close_retry_tests.c"
#undef main
#undef Sleep
static HANDLE BackoffEntered,BackoffGate;
static volatile LONG WaitFailures,Backoffs,Unbacked,Timeouts;
static DWORD Status,Scenarios;
static VOID ParkEnter(VOID)
{
    if(ReadAcquire(&Unbacked)) {fputs("FAILED_PARK_RETRIED_WITHOUT_BACKOFF\n",stderr);ExitProcess(50);}
}
static DWORD ParkResult(HANDLE event,DWORD timeout)
{
    if(ReadAcquire(&Timeouts)>0) {InterlockedDecrement(&Timeouts);SetLastError(ERROR_ACCESS_DENIED);return WAIT_TIMEOUT;}
    if(ReadAcquire(&WaitFailures)>0) {
        InterlockedDecrement(&WaitFailures);InterlockedExchange(&Unbacked,1);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);return Status;
    }
    return WaitForSingleObject(event,timeout);
}
static VOID WINAPI ParkSleep(DWORD delay)
{
    CHECK(delay==1000 && InterlockedExchange(&Unbacked,0)==1);
    CHECK(Observed && !Observed->ResourcesClosed && Observed->Region && DeviceOwned && Works==1 && WorkPools==1 && RuntimeCount==1);
    InterlockedIncrement(&Backoffs);
    CHECK(SetEvent(BackoffEntered) && WaitForSingleObject(BackoffGate,5000)==WAIT_OBJECT_0);
}
static VOID AssertRetained(PB_SESSION *s,BOOL loan)
{
    PB_CLOSE_PROGRESS p;CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.RetainedBytes && p.Phase<PB_LIFE_CLOSED);
    CHECK(s==Observed && s->Region && !s->ResourcesClosed && DeviceOwned && !SwCloses && RuntimeCount==1 && RuntimeAdapterCount==1 && RuntimeBytes);
    CHECK(p.UserLeases==(loan?1u:0u));
    if(!loan)CHECK(p.Phase==PB_LIFE_RELEASING && p.LastError==FirstCloseError && CloseFailures);
}
static VOID ReleaseBlock(PB_USER_QUEUE *q,PB_LEASE *lease,BOOL loan)
{
    if(loan) {lease->Data[0]=0x5a;CHECK(Api.CancelBatch(q,&lease->Descriptor.Token,1)==PB_OK);}
    CHECK(SetHandleInformation(Device,HANDLE_FLAG_PROTECT_FROM_CLOSE,0) && SetEvent(Progress));
}
static VOID Scenario(DWORD failures,DWORD status,BOOL loan,BOOL dropPublic,BOOL recoverLate)
{
    Begin(FAIL_NONE,TRUE);PB_ADAPTER *a=Adapter();PB_SESSION *s;CHECK(CallOpen(a,&s)==PB_OK);Retain(s);
    PB_USER_QUEUE *q=NULL;PB_LEASE lease={0};uint32_t n;
    if(loan)CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,&lease,1,&n)==PB_OK && n==1);
    Status=status;WaitFailures=(LONG)failures;Backoffs=Unbacked=Timeouts=0;
    BackoffEntered=CreateEventW(NULL,FALSE,FALSE,NULL);BackoffGate=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(BackoffEntered && BackoffGate);
    if(dropPublic)Life.ReleaseSession(s);
    else {PB_RESULT r=Api.Close(s,0);CHECK(r==PB_BUSY || r==PB_QUARANTINED || r==PB_CORRUPT);}
    Api.CloseAdapter(a);AwaitPark();AssertRetained(s,loan);
    if(!failures) {ReleaseBlock(q,&lease,loan);CHECK(SetEvent(ParkGate));}
    else for(DWORD i=0;i<failures;++i) {
        if(i)AwaitPark();
        CHECK(SetEvent(ParkGate) && WaitForSingleObject(BackoffEntered,5000)==WAIT_OBJECT_0);
        CHECK(Backoffs==(LONG)i+1 && WaitFailures==(LONG)(failures-i-1));AssertRetained(s,loan);
        if(i+1==failures && !recoverLate)ReleaseBlock(q,&lease,loan);
        CHECK(SetEvent(BackoffGate));
    }
    if(failures && recoverLate) {AwaitPark();AssertRetained(s,loan);ReleaseBlock(q,&lease,loan);CHECK(SetEvent(ParkGate));}
    WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    CHECK(!Unbacked && Backoffs==(LONG)failures && s->ResourcesClosed && !s->Region && CloseSuccesses==1 && SwCloses==1 && SwCloseOrder>DeviceCloseOrder);
    if(!dropPublic)CHECK(Api.Close(s,2000)==PB_OK);
    DropReference(s);End();CHECK(CloseHandle(BackoffEntered) && CloseHandle(BackoffGate));++Scenarios;
}
static VOID TimeoutScenario(BOOL loan,BOOL dropPublic)
{
    Begin(FAIL_NONE,TRUE);PB_ADAPTER *a=Adapter();PB_SESSION *s;CHECK(CallOpen(a,&s)==PB_OK);Retain(s);
    PB_USER_QUEUE *q=NULL;PB_LEASE lease={0};uint32_t n;
    if(loan)CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,&lease,1,&n)==PB_OK && n==1);
    Backoffs=WaitFailures=Unbacked=0;Timeouts=1;
    if(dropPublic)Life.ReleaseSession(s);else {PB_RESULT r=Api.Close(s,0);CHECK(r==PB_BUSY || r==PB_QUARANTINED || r==PB_CORRUPT);}
    Api.CloseAdapter(a);AwaitPark();AssertRetained(s,loan);CHECK(SetEvent(ParkGate));
    AwaitPark();CHECK(!Timeouts && !Backoffs && !Unbacked);AssertRetained(s,loan);
    ReleaseBlock(q,&lease,loan);CHECK(SetEvent(ParkGate));WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    CHECK(s->ResourcesClosed && CloseSuccesses==1 && SwCloses==1 && !Backoffs);
    if(!dropPublic)CHECK(Api.Close(s,2000)==PB_OK);DropReference(s);End();++Scenarios;
}
int main(int argc,char **argv)
{
    (void)argv;
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    if(argc>1)Scenario(1,WAIT_FAILED,FALSE,FALSE,FALSE);
    else {
        for(DWORD loan=0;loan<2;++loan)for(DWORD drop=0;drop<2;++drop) {
            Scenario(0,WAIT_OBJECT_0,loan,drop,FALSE);TimeoutScenario(loan,drop);
        }
        const DWORD counts[]={1,2,64},statuses[]={WAIT_FAILED,WAIT_ABANDONED,WAIT_OBJECT_0+1};
        for(DWORD c=0;c<_countof(counts);++c)for(DWORD r=0;r<_countof(statuses);++r)
            for(DWORD loan=0;loan<2;++loan)for(DWORD drop=0;drop<2;++drop)for(DWORD late=0;late<2;++late)
                Scenario(counts[c],statuses[r],loan,drop,late);
    }
    printf("PASS: %lu real lifecycle park schedules, each failed wait backs off; concurrent final loan return, protected native close, public release, adapter/module ownership and zero resource residue.\n",Scenarios);return 0;
}
