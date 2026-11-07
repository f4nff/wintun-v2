/* SPDX-License-Identifier: GPL-2.0 */
/* Actual helper pipe ownership with deterministic faults at the OS boundary. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
static BOOL WINAPI IoResult(HANDLE,LPOVERLAPPED,LPDWORD,BOOL);
static BOOL WINAPI IoCancel(HANDLE,LPOVERLAPPED);
static DWORD WINAPI IoWait(HANDLE,DWORD);
static BOOL WINAPI IoReset(HANDLE);
static BOOL WINAPI IoRead(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static VOID WINAPI IoSleep(DWORD);
static VOID IoClosing(HANDLE);
#define GetOverlappedResult IoResult
#define CancelIoEx IoCancel
#define WaitForSingleObject IoWait
#define ResetEvent IoReset
#define ReadFile IoRead
#define Sleep IoSleep
#define PB_FIXTURE_CLOSE_OBSERVER IoClosing
#include "legacy_runtime_fixture.c"
#undef GetOverlappedResult
#undef CancelIoEx
#undef WaitForSingleObject
#undef ResetEvent
#undef ReadFile
#undef Sleep
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static HELPER_STREAM *Observed;
static DWORD Kind,Outcome,CancelResult,FaultStatus,FaultError,Failures,Backoffs,Waits,Queries,Resets;
static DWORD CompleteAt,Spurious,ResetRace,Cases,ResetFailures,ExpectedBackoffs;
static DWORD CreatedThreads,JoinedThreads;
static BOOL Armed,Completed,DrainInjected,Concurrent;
static const BYTE Payload[]={0x37,0,0x38,0};
static VOID Complete(VOID)
{
    if(Completed)return;
    CHECK(Observed&&Observed->Pending);
    if(Outcome==0) CHECK(CancelIoEx(Observed->Read,&Observed->Io));
    else if(Outcome==1) {DWORD bytes;CHECK(WriteFile(Observed->Write,Payload,sizeof(Payload),&bytes,NULL)&&bytes==sizeof(Payload));}
    else {CHECK(TestClose(Observed->Write));Observed->Write=NULL;}
    DWORD bytes;BOOL okay=GetOverlappedResult(Observed->Read,&Observed->Io,&bytes,TRUE);
    CHECK(Outcome==1?okay:(!okay&&(GetLastError()==ERROR_OPERATION_ABORTED||GetLastError()==ERROR_BROKEN_PIPE)));
    Completed=TRUE;
}
static DWORD WINAPI ConcurrentComplete(VOID *unused) {(void)unused;Complete();return 0;}
static BOOL WINAPI IoResult(HANDLE file,LPOVERLAPPED io,LPDWORD bytes,BOOL wait)
{
    if(Armed&&(Kind==1||(Observed&&file==Observed->Read))) {
        if(wait) {SetLastError(ERROR_IO_INCOMPLETE);return FALSE;}
        ++Queries;
        BOOL okay=GetOverlappedResult(file,io,bytes,FALSE);DWORD error=GetLastError();
        if(!okay&&error==ERROR_IO_INCOMPLETE&&Spurious) {--Spurious;CHECK(SetEvent(io->hEvent));}
        SetLastError(error);return okay;
    }
    return GetOverlappedResult(file,io,bytes,wait);
}
static BOOL WINAPI IoCancel(HANDLE file,LPOVERLAPPED io)
{
    if(Armed&&Observed&&file==Observed->Read) {
        /* Accepted cancellation may complete later; NOT_FOUND also is no barrier. */
        SetLastError(CancelResult==1?ERROR_NOT_FOUND:ERROR_ACCESS_DENIED);return CancelResult==0;
    }
    return CancelIoEx(file,io);
}
static DWORD WINAPI IoWait(HANDLE event,DWORD timeout)
{
    if(Armed&&Observed&&event==Observed->Io.hEvent&&timeout==INFINITE) {
        ++Waits;
        if(Failures) {--Failures;SetLastError(FaultError);return FaultStatus;}
        if(!Spurious&&Concurrent) {
            HANDLE worker=CreateThread(NULL,0,ConcurrentComplete,NULL,0,NULL);CHECK(worker);
            ++CreatedThreads;
            DWORD result=WaitForSingleObject(event,10000);
            CHECK(WaitForSingleObject(worker,10000)==WAIT_OBJECT_0&&CloseHandle(worker));++JoinedThreads;return result;
        }
        if(!Spurious)Complete();
    }
    return WaitForSingleObject(event,timeout);
}
static BOOL WINAPI IoReset(HANDLE event)
{
    if(Armed&&Observed&&event==Observed->Io.hEvent) {
        ++Resets;
        if(ResetFailures) {--ResetFailures;if(ResetRace==3)Complete();SetLastError(FaultError);return FALSE;}
        if(ResetRace==1)Complete();
        BOOL okay=ResetEvent(event);
        if(ResetRace==2)Complete();
        return okay;
    }
    return ResetEvent(event);
}
static BOOL WINAPI IoRead(HANDLE file,LPVOID data,DWORD size,LPDWORD bytes,LPOVERLAPPED io)
{
    BOOL okay=ReadFile(file,data,size,bytes,io);
    if(Armed&&Kind==2&&file==Observed->Read&&okay&&!DrainInjected) {
        /* Submission reported pending; completion won the race with its observer. */
        DrainInjected=TRUE;SetLastError(ERROR_IO_PENDING);return FALSE;
    }
    return okay;
}
static VOID WINAPI IoSleep(DWORD delay)
{
    if(Armed&&Observed) {
        CHECK(delay==1000&&Observed->Pending&&OwnedHandles==(Observed->Write?3:2));
        DWORD bytes,flags;BOOL okay=GetOverlappedResult(Observed->Read,&Observed->Io,&bytes,FALSE);DWORD error=okay?0:GetLastError();
        /* An ordinary pump can see reset fail after native completion. Its
         * owner still stays pending until the next terminal query. */
        CHECK(Completed?(okay||error==ERROR_OPERATION_ABORTED||error==ERROR_BROKEN_PIPE):(!okay&&error==ERROR_IO_INCOMPLETE));
        CHECK(GetHandleInformation(Observed->Io.hEvent,&flags));
        ++Backoffs;if(CompleteAt==Backoffs)Complete();return;
    }
    Sleep(delay);
}
static VOID IoClosing(HANDLE file)
{
    if(Armed&&Observed&&(file==Observed->Read||file==Observed->Io.hEvent)) {
        DWORD bytes;BOOL okay=GetOverlappedResult(Observed->Read,&Observed->Io,&bytes,FALSE);
        if(!okay&&GetLastError()==ERROR_IO_INCOMPLETE) {
            CHECK(CancelIoEx(Observed->Read,&Observed->Io));
            GetOverlappedResult(Observed->Read,&Observed->Io,&bytes,TRUE);
            fputs("PENDING_IO_RELEASED\n",stderr);ExitProcess(46);
        }
        /* Only inspect once; the production closer clears Read before Io.hEvent. */
        Observed=NULL;
    }
}
static VOID Begin(HELPER_JOB *job,DWORD kind)
{
    CHECK(!OwnedHandles&&!OwnedAllocations&&!OwnedWorks&&CreatedThreads==JoinedThreads);
    ZeroMemory(job,sizeof(*job));job->ResponseCapacity=_countof(job->Response);
    CHECK(swprintf_s(job->Directory,MAX_PATH,L"C:\\io-%lu-%lu",GetCurrentProcessId(),++Cases)>0);
    Kind=kind;Completed=DrainInjected=FALSE;Observed=NULL;Armed=FALSE;
    Queries=Resets=Waits=Backoffs=Failures=Spurious=ResetRace=CompleteAt=ResetFailures=0;Concurrent=FALSE;
}
static VOID PendingCase(BOOL drain,DWORD outcome,DWORD cancel,DWORD status,DWORD error,DWORD completeAt,DWORD spurious,DWORD race)
{
    HELPER_JOB job;Begin(&job,0);CHECK(HelperPipe(&job,0));Observed=&job.Streams[0];
    HelperPump(&job,Observed);CHECK(Observed->Pending);
    Outcome=outcome;CancelResult=cancel;FaultStatus=status;FaultError=error;
    CompleteAt=completeAt;Spurious=spurious;ResetRace=race;Failures=status?2:0;Armed=TRUE;
    Concurrent=TRUE;
    HelperFinishStream(&job,Observed,drain);Armed=FALSE;
    CHECK(!OwnedHandles&&!job.Streams[0].Pending);
    CHECK(Backoffs==(status?(completeAt?completeAt:2):0));
    CHECK(job.Error==(status?(status==WAIT_FAILED&&error?error:ERROR_GEN_FAILURE):0));
    CHECK(job.ResponseBytes==(drain&&outcome==1?sizeof(Payload):0));
    CHECK(Queries<128&&Waits<64); /* Persistent signalled events must be reset. */
}
static VOID RetryCase(BOOL reset,BOOL completeFirst,DWORD priorError,DWORD error)
{
    HELPER_JOB job;Begin(&job,0);CHECK(HelperPipe(&job,0));Observed=&job.Streams[0];
    HelperPump(&job,Observed);CHECK(Observed->Pending);Outcome=1;CancelResult=1;
    FaultError=error;FaultStatus=WAIT_FAILED;ExpectedBackoffs=64;
    if(reset)ResetFailures=ExpectedBackoffs;else Failures=ExpectedBackoffs;
    CompleteAt=ExpectedBackoffs;job.Error=priorError;Armed=TRUE;
    if(completeFirst) {Complete();Failures=ResetFailures=0;ExpectedBackoffs=0;}
    HelperFinishStream(&job,Observed,TRUE);Armed=FALSE;
    CHECK(!OwnedHandles&&job.ResponseBytes==sizeof(Payload));
    CHECK(Backoffs==ExpectedBackoffs);
    CHECK(job.Error==(priorError?priorError:(completeFirst?0:(FaultError?FaultError:ERROR_GEN_FAILURE))));
    CHECK(Queries<=132&&Waits<=65);
}
static VOID ConnectCase(VOID)
{
    HELPER_JOB job;Begin(&job,1);Armed=TRUE;
    if(!HelperPipe(&job,0)) {Armed=FALSE;HelperFinishStream(&job,&job.Streams[0],FALSE);fputs("CONNECT_COMPLETION_LOST\n",stderr);ExitProcess(47);}
    Armed=FALSE;CHECK(!job.Streams[0].Pending);HelperFinishStream(&job,&job.Streams[0],FALSE);CHECK(!OwnedHandles);
}
static VOID DrainCase(VOID)
{
    HELPER_JOB job;Begin(&job,2);CHECK(HelperPipe(&job,0));Observed=&job.Streams[0];
    DWORD bytes;CHECK(WriteFile(Observed->Write,Payload,sizeof(Payload),&bytes,NULL)&&bytes==sizeof(Payload));
    Armed=TRUE;HelperFinishStream(&job,Observed,TRUE);Armed=FALSE;
    CHECK(!OwnedHandles&&DrainInjected);
    if(job.ResponseBytes!=sizeof(Payload)) {fputs("DRAIN_COMPLETION_LOST\n",stderr);ExitProcess(48);}
    CHECK(!memcmp(job.Response,Payload,sizeof(Payload)));
}
/* Exercise the ordinary, nonblocking pump, rather than only its terminal drain.
 * A signalled manual event is deliberately not evidence of completed I/O. */
static VOID PumpCase(DWORD outcome,DWORD spurious,DWORD resetFailures,DWORD race,DWORD priorError,DWORD resetError)
{
    HELPER_JOB job;Begin(&job,3);CHECK(HelperPipe(&job,0));Observed=&job.Streams[0];
    HelperPump(&job,Observed);CHECK(Observed->Pending);
    Outcome=outcome;CancelResult=1;Spurious=spurious;ResetFailures=resetFailures;
    ResetRace=race;FaultError=resetError;CompleteAt=resetFailures;job.Error=priorError;
    Armed=TRUE;CHECK(SetEvent(Observed->Io.hEvent));
    BOOL spin=FALSE,lost=FALSE;
    for(DWORD turn=0;turn<128;++turn) {
        DWORD queries=Queries,resets=Resets;
        HelperPump(&job,Observed);
        CHECK(Queries-queries<=2&&Resets-resets<=1&&!Waits);
        if(!Observed->Pending)break;
        DWORD state=WaitForSingleObject(Observed->Io.hEvent,0);
        if(Completed&&state==WAIT_TIMEOUT){lost=TRUE;break;}
        if(state==WAIT_TIMEOUT) {
            CHECK(!Spurious&&!ResetFailures);
            HANDLE worker=CreateThread(NULL,0,ConcurrentComplete,NULL,0,NULL);CHECK(worker);++CreatedThreads;
            CHECK(WaitForSingleObject(Observed->Io.hEvent,10000)==WAIT_OBJECT_0&&WaitForSingleObject(worker,10000)==WAIT_OBJECT_0&&CloseHandle(worker));++JoinedThreads;
        }else CHECK(state==WAIT_OBJECT_0);
        if(turn==127)spin=TRUE;
    }
    if(spin||lost) {
        /* Rescue real pending I/O before reporting a negative control. */
        Complete();Armed=FALSE;HelperFinishStream(&job,Observed,FALSE);CHECK(!OwnedHandles);
        fputs(spin?"PENDING_HELPER_EVENT_LEFT_SIGNALED\n":"PUMP_LOST_COMPLETION_AFTER_RESET\n",stderr);ExitProcess(spin?77:78);
    }
    CHECK(!Observed->Pending&&Completed&&Backoffs==resetFailures);
    CHECK(job.Error==(priorError?priorError:resetFailures?(resetError?resetError:ERROR_GEN_FAILURE):0));
    CHECK(job.ResponseBytes==(outcome==1?sizeof(Payload):0));
    if(outcome==1)CHECK(!memcmp(job.Response,Payload,sizeof(Payload)));
    HelperFinishStream(&job,Observed,FALSE);Armed=FALSE;CHECK(!OwnedHandles);
}
static VOID PumpFairnessCase(VOID)
{
    HELPER_JOB job;Begin(&job,3);CHECK(HelperPipe(&job,0)&&HelperPipe(&job,1));
    HelperPump(&job,&job.Streams[0]);HelperPump(&job,&job.Streams[1]);
    CHECK(job.Streams[0].Pending&&job.Streams[1].Pending);
    Observed=&job.Streams[0];Outcome=0;Concurrent=TRUE;Armed=TRUE;
    static const WCHAR line[]=L"[+ 123] ready\n";DWORD bytes;
    CHECK(SetEvent(Observed->Io.hEvent)&&WriteFile(job.Streams[1].Write,line,sizeof(line)-sizeof(WCHAR),&bytes,NULL)&&bytes==sizeof(line)-sizeof(WCHAR));
    CHECK(WaitForSingleObject(job.Streams[1].Io.hEvent,10000)==WAIT_OBJECT_0);
    HelperPump(&job,Observed);
    CHECK(Observed->Pending&&!Waits&&!Backoffs&&Queries==2&&Resets==1&&WaitForSingleObject(Observed->Io.hEvent,0)==WAIT_TIMEOUT);
    HelperPump(&job,&job.Streams[1]);
    CHECK(!job.Streams[1].Pending&&job.Streams[1].Total==bytes&&!job.Streams[1].LineLength&&!job.Error);
    HelperFinishStream(&job,Observed,FALSE);HelperFinishStream(&job,&job.Streams[1],FALSE);Armed=FALSE;
    CHECK(!OwnedHandles);
}
__declspec(dllexport) DWORD WINAPI RunIoCases(DWORD only)
{
    CHECK(MainRuntimeAcquire());
    if(only==1)PendingCase(TRUE,1,0,WAIT_FAILED,ERROR_NOT_ENOUGH_MEMORY,2,0,0);
    else if(only==2)ConnectCase();
    else if(only==3)DrainCase();
    else if(only==4)PumpCase(1,0,0,0,0,0);
    else if(only==5)PumpCase(1,0,0,1,0,0);
    else {
        for(DWORD drain=0;drain<2;++drain)for(DWORD outcome=0;outcome<3;++outcome)for(DWORD cancel=0;cancel<3;++cancel) {
            PendingCase(drain,outcome,cancel,0,0,0,0,0);
            const DWORD statuses[]={WAIT_FAILED,WAIT_TIMEOUT,WAIT_ABANDONED};
            for(DWORD s=0;s<_countof(statuses);++s)for(DWORD at=0;at<3;++at)
                PendingCase(drain,outcome,cancel,statuses[s],at?ERROR_NOT_ENOUGH_MEMORY:0,at,0,0);
        }
        for(DWORD outcome=0;outcome<3;++outcome)for(DWORD race=1;race<=2;++race)PendingCase(TRUE,outcome,1,0,0,0,0,race);
        PendingCase(TRUE,1,1,0,0,0,16,0);
        for(DWORD reset=0;reset<2;++reset)for(DWORD first=0;first<2;++first)for(DWORD prior=0;prior<2;++prior)
            RetryCase(reset,first,prior?ERROR_INVALID_DATA:0,ERROR_NOT_ENOUGH_MEMORY);
        for(DWORD prior=0;prior<2;++prior)RetryCase(TRUE,FALSE,prior?ERROR_INVALID_DATA:0,0);
        ConnectCase();DrainCase();
        for(DWORD outcome=0;outcome<3;++outcome) {
            PumpCase(outcome,0,0,0,0,0);PumpCase(outcome,16,0,0,0,0);
            for(DWORD race=1;race<=2;++race)PumpCase(outcome,0,0,race,0,0);
            for(DWORD prior=0;prior<2;++prior)for(DWORD zero=0;zero<2;++zero)
                PumpCase(outcome,0,64,0,prior?ERROR_INVALID_DATA:0,zero?0:ERROR_ACCESS_DENIED);
            PumpCase(outcome,0,1,3,0,0);
        }
        PumpFairnessCase();
    }
    MainRuntimeRelease();CHECK(!OwnedHandles&&!OwnedAllocations&&!OwnedWorks&&CreatedThreads==JoinedThreads);return Cases;
}
__declspec(dllexport) DWORD WINAPI JoinedIoThreads(VOID) {CHECK(CreatedThreads==JoinedThreads);return JoinedThreads;}
