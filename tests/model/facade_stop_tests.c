/* SPDX-License-Identifier: GPL-2.0 */
#ifdef PB_API_TESTS
#error Facade stop host must not enable API test shortcuts
#endif
#include "../safety/guard_memory.h"
#include "facade_stop_fixture.h"
#define CHECK SAFETY_CHECK
typedef struct JOB {FS_OBJECT Produce;FS_RECEIVE_COPY Receive;FS_RETURN_COPY Return;VOID *Session;HANDLE Entered,Gate;} JOB;
static ULONG Scenarios,JoinedUsers,JoinedCallbacks,RepairedCases,EarlyParks;
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI User(VOID *opaque)
{
    JOB *job=opaque;
    if(job->Produce)job->Produce(job->Session);
    else{BYTE *packet=job->Receive(job->Session);CHECK(SetEvent(job->Entered));Await(job->Gate);job->Return(job->Session,packet);}
    return 0;
}
static VOID JoinUser(HANDLE user){Await(user);CHECK(CloseHandle(user));++JoinedUsers;}
static VOID CheckLegacyGone(const FS_SNAPSHOT *s)
{CHECK(!s->Runtime && !s->Heap && !s->Regions && !s->Handles && !s->Paths && !s->Pins && !s->LegacyBytes);}
static VOID Scenario(LPCWSTR path,DWORD direction,BOOL failure,BOOL control)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);
    NP_INITIALIZE initialize;FS_PREPARE prepare;FS_START start;FS_OBJECT produce,end;FS_RECEIVE_COPY receive;FS_RETURN_COPY release;
    FS_ACTION publish,stop,exit,recover;FS_READ read;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(initialize,"Initialize");GET(prepare,"PrepareFacade");GET(start,"StartFacade");GET(produce,"ProduceFacade");GET(end,"EndFacade");
    GET(publish,"PublishFacadeIncoming");GET(stop,"StopFacadeNative");GET(exit,"WaitFacadeExit");GET(recover,"RecoverFacadeNative");
    GET(receive,"ReceiveFacadeCopy");GET(release,"ReturnFacadeCopy");GET(read,"ReadFacadeStop");
#undef GET
    NP_CONFIG host={0};host.Mode=NP_LATE_SUCCESS;host.NdisVersion=direction==FS_SEND?0x00060014:0x00060059;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i){host.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Events[i]);}
    FS_CONFIG config={.Direction=direction,.QueryFailure=failure};
    for(DWORD i=0;i<FS_EVENT_COUNT;++i){config.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(config.Events[i]);}
    HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(entered && gate);
    initialize(&host);prepare(&config);VOID *session=start();Await(config.Events[FS_WAIT_ENTERED]);
    JOB job={.Session=session,.Entered=entered,.Gate=gate};HANDLE user;
    if(direction==FS_SEND) {
        job.Produce=produce;user=CreateThread(NULL,0,User,&job,0,NULL);CHECK(user);JoinUser(user);user=NULL;
        CHECK(SetEvent(config.Events[FS_WAIT_GATE]));Await(config.Events[FS_PUMP_ENTERED]);
    } else {
        publish();CHECK(SetEvent(config.Events[FS_WAIT_GATE]));Await(config.Events[FS_PUMP_ENTERED]);
        job.Receive=receive;job.Return=release;user=CreateThread(NULL,0,User,&job,0,NULL);CHECK(user);Await(entered);
    }
    stop();CHECK(SetEvent(config.Events[FS_PUMP_GATE]));FS_SNAPSHOT s;
    if(direction==FS_RECEIVE) {
        Await(config.Events[FS_STOP_ENTERED]);Await(host.Events[NP_PARK_ENTERED]);read(&s);
        CHECK(s.Core && s.Loans==63 && !s.CloseCalls && !s.DeviceCloses && s.NativeRegions==1 && s.NativeActive==1);++EarlyParks;
        /* First park only witnesses callback admission, not pending return.
         * Let that owner reach Close with zero loans, then release this early
         * callback park so actual QUERY can supply the later close result. */
        CHECK(SetEvent(config.Events[FS_STOP_GATE]));Await(config.Events[FS_CLOSE_ENTERED]);
        read(&s);CHECK(!s.Loans && s.CloseCalls==1);CHECK(SetEvent(host.Events[NP_PARK_GATE]));
    } else Await(config.Events[FS_CLOSE_ENTERED]);
    Await(host.Events[NP_PARK_ENTERED]);read(&s);
    CHECK(s.Core && !s.Loans && !s.Published && s.NativeActive==1 && s.NativeRegions==1 && !s.DeviceCloses && s.Sessions==1 && s.Bytes);
    if(direction==FS_SEND)CHECK(s.CommitStopped==1 && s.Cancelled==8 && !s.RepairLoans);
    if(s.RepairLoans){CHECK(control && direction==FS_RECEIVE && s.RepairLoans==63);++RepairedCases;}
    if(failure) {
        exit();
        if(user){CHECK(SetEvent(gate));JoinUser(user);user=NULL;}
        end(session);session=NULL;read(&s);CheckLegacyGone(&s);
        CHECK(s.Core && s.PublicReleased && s.Registered && s.NativeActive==1 && s.NativeRegions==1 && !s.DeviceCloses && s.CloseCalls==1 && s.CloseResult==PB_CORRUPT);
        recover();CHECK(SetEvent(host.Events[NP_PARK_GATE]));Await(host.Events[NP_FINAL_ENTERED]);
    } else {
        CHECK(direction==FS_SEND && !user);recover();CHECK(SetEvent(host.Events[NP_PARK_GATE]));Await(host.Events[NP_FINAL_ENTERED]);end(session);session=NULL;
    }
    read(&s);CheckLegacyGone(&s);CHECK(s.NativeClosed && !s.NativeRegions && s.DeviceCloses==1 && !s.Loans && !s.Published && s.CloseCalls==1);
    CHECK(s.CloseResult==(failure?PB_CORRUPT:PB_OK) && !user && !session);
    CHECK(SetEvent(host.Events[NP_FINAL_GATE]));
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(host.Threads[i]){Await(host.Threads[i]);CHECK(CloseHandle(host.Threads[i]));++JoinedCallbacks;}
    read(&s);CheckLegacyGone(&s);CHECK(!s.Core && !s.NativeAllocations && !s.NativeRegions && !s.NativeEvents && !s.NativeWorks && !s.NativePools && !s.NativePins && !s.Sessions && !s.Bytes && !s.Active && s.Callbacks==s.Finished);
    CHECK(FreeLibrary(library));Await(host.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Events[i]));
    for(DWORD i=0;i<FS_EVENT_COUNT;++i)CHECK(CloseHandle(config.Events[i]));CHECK(CloseHandle(entered) && CloseHandle(gate));++Scenarios;
}
static VOID Matrix(LPCWSTR path)
{
    for(DWORD i=0;i<8;++i){Scenario(path,FS_SEND,FALSE,FALSE);Scenario(path,FS_SEND,TRUE,FALSE);}
    for(DWORD i=0;i<16;++i)Scenario(path,FS_RECEIVE,TRUE,FALSE);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"116")));
    if(argc==3)Scenario(argv[1],FS_RECEIVE,TRUE,TRUE);
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: facade stop full warmup/measured handles %lu/%lu\n",before,after);}
    printf("PASS: %lu actual facade/nonPB session schedules; %lu joined public users, %lu callback-thread handle joins, %lu forced early BeginStop parks before Close; Wake before wait, Reserve8/CommitStopped/Cancel8, pending63 stopped return, private-copy release after worker exit before End, actual Close5000 and retained handoff, resource convergence and DLL unload\n",Scenarios,JoinedUsers,JoinedCallbacks,EarlyParks);
    if(RepairedCases){CHECK(argc==3 && RepairedCases==1);puts("FACADE_PENDING_RETURN_MISSING: worker Close boundary observed63 loans; same queue owner explicitly retired known63 tokens for negative cleanup before actual Close/Release, all users/workers joined and resources/unload converged");return 116;}
    return 0;
}
