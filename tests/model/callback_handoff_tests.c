/* SPDX-License-Identifier: GPL-2.0 */
/* Final public consumption follows joined callers. Host-only gate observers
 * inspect work-owned core; they never call a consumed session API. */
#include "../safety/guard_memory.h"
#include "callback_handoff_fixture.h"
#include "../../api/v2/test.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef PB_RESULT (*OPEN_TEST)(const PB_CONFIG *,PB_SESSION **,VOID **,PB_LAYOUT *,uint64_t *);
typedef VOID (*BLOCK_TEST)(PB_SESSION *,uint32_t,HANDLE,HANDLE);
typedef VOID (*FAIL_TEST)(PB_SESSION *,uint32_t,BOOL);
typedef struct FIXTURE {
    HMODULE Library;PB_INTERFACE_V2 Api;PB_LIFECYCLE_INTERFACE Life;PB_SESSION *Session;
    CH_CONFIG Config;CH_CONFIGURE Configure;CH_OBSERVE Observe;CH_FAIL_WAIT FailWait;CH_REPAIR Repair;
    BLOCK_TEST Block;FAIL_TEST Fail;uint64_t First,Next;
} FIXTURE;
enum { NEXT_POLICY,LATE_STOP,DROP_PENDING,FAILED_WAIT,DIRECT_DROP,PREPARE_DROP };
static ULONG Scenarios,JoinedCallers,JoinedCallbacks,Detected,Repairs;
static BOOL Control;
static VOID Load(FIXTURE *f,LPCWSTR path,DWORD queues,BOOL failure)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Library=LoadLibraryW(path);CHECK(f->Library);
    GET_API get;GET_LIFE life;OPEN_TEST open;
#define GET(field,name) do{*(FARPROC *)&(field)=GetProcAddress(f->Library,name);CHECK(field);}while(0)
    GET(get,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(open,"PbTestOpen");
    GET(f->Configure,"Configure");GET(f->Observe,"Observe");GET(f->FailWait,"InjectWaitFailure");GET(f->Repair,"RepairMissingWorkPin");
    GET(f->Block,"PbTestBlock");GET(f->Fail,"PbTestFail");
#undef GET
    CHECK(get(2,sizeof(f->Api),&f->Api)==PB_OK && life(1,sizeof(f->Life),&f->Life)==PB_OK);
    for(DWORD i=0;i<CH_EVENT_COUNT;++i){f->Config.Events[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f->Config.Events[i]);}
    PB_CONFIG config={sizeof(config),queues,4,4,0,0};VOID *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(open(&config,&f->Session,&region,&layout,&epoch)==PB_OK);
    f->Configure(f->Session,&f->Config);f->Fail(f->Session,PB_TEST_POLICY,failure);
    CHECK(f->Life.BeginSetPolicy(f->Session,0,&f->First)==PB_OK && f->First==1);
    CHECK(WaitForSingleObject(f->Config.Events[CH_IDLE_ENTERED],5000)==WAIT_OBJECT_0);
}
static CH_SNAPSHOT Observe(FIXTURE *f,LONG references,BOOL idle)
{
    CH_SNAPSHOT s;f->Observe(&s);
    if(s.References!=references) {
        CHECK(Control && !Detected && references==3 && s.References==2 && idle && s.Started==1 && !s.Finished);
        ++Detected;f->Repair();++Repairs;f->Observe(&s);
    }
    CHECK(s.References==references);
    CHECK(s.Active==1 && s.Peak==1 && s.Works==1 && s.Pools==1 && s.Sessions==1 && s.Bytes && s.Registered);
    CHECK(s.IdleHeld==idle);
    if(!s.Closed)CHECK(s.Regions==1 && s.Allocations>1 && s.Events>2 && s.ModulePresent && s.TailPresent && s.Pins==2 && !s.TailTransfers);
    return s;
}
static VOID RetireFirst(FIXTURE *f,BOOL forgotten,BOOL failure)
{
    CH_SNAPSHOT s=Observe(f,3,TRUE);
    CHECK(s.Started==1 && !s.Finished && !s.Scheduled && s.Phase==PB_LIFE_RUNNING && s.Requested==1 && s.Completed==1 && s.Occupied && !s.Queued);
    if(forgotten)f->Life.ForgetPolicy(f->Session,f->First);
    else {PB_RESULT operation=PB_INVALID;CHECK(f->Life.PollPolicy(f->Session,f->First,0,&operation)==PB_OK && operation==(failure?PB_UNSUPPORTED:PB_OK));}
    s=Observe(f,3,TRUE);CHECK(!s.Occupied);
}
typedef struct SYNC_CALL {PB_INTERFACE_V2 *Api;PB_SESSION *Session;PB_RESULT Result;DWORD Error;} SYNC_CALL;
static DWORD WINAPI SetPolicyCaller(VOID *opaque)
{SYNC_CALL *call=opaque;call->Result=call->Api->SetPolicy(call->Session,0);call->Error=GetLastError();return 0;}
static VOID QueueNext(FIXTURE *f,BOOL failure,BOOL abandon,BOOL failedWait)
{
    f->Fail(f->Session,PB_TEST_POLICY,failure);
    f->Block(f->Session,PB_TEST_POLICY,f->Config.Events[CH_POLICY_ENTERED],f->Config.Events[CH_POLICY_GATE]);
    if(failedWait) {
        f->FailWait();SYNC_CALL call={&f->Api,f->Session,PB_INVALID,0};
        HANDLE thread=CreateThread(NULL,0,SetPolicyCaller,&call,0,NULL);CHECK(thread);
        CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && call.Result==PB_CORRUPT && call.Error==ERROR_OPERATION_ABORTED && CloseHandle(thread));
        ++JoinedCallers;f->Next=2;
    } else {
        CHECK(f->Life.BeginSetPolicy(f->Session,0,&f->Next)==PB_OK && f->Next==2);
        if(abandon)f->Life.ForgetPolicy(f->Session,f->Next);
    }
    CH_SNAPSHOT s=Observe(f,4,TRUE);
    CHECK(s.Started==1 && !s.Finished && s.Scheduled && s.Queued && s.Occupied && s.Requested==2 && s.Completed==1);
    CHECK(s.Abandoned==(abandon || failedWait));
    /* Keep the old callback fixed after its real State unlock. This bounded
     * observation and Dispatch's lifetime-wide active counter check serial
     * execution; no SubmitThreadpoolWork failure is assumed. */
    CHECK(WaitForSingleObject(f->Config.Events[CH_POLICY_ENTERED],10)==WAIT_TIMEOUT);
    CHECK(SetEvent(f->Config.Events[CH_IDLE_GATE]));
    CHECK(WaitForSingleObject(f->Config.Events[CH_POLICY_ENTERED],5000)==WAIT_OBJECT_0);
    s=Observe(f,3,FALSE);CHECK(s.Started==2 && s.Finished==1 && !s.Queued && s.Occupied);
}
static VOID ReleasePublic(FIXTURE *f)
{
    CHECK(f->Session);f->Life.ReleaseSession(f->Session);f->Session=NULL;
    /* Further observations use the hard-gated callback's actual owner pins,
     * without extending lifetime through an extra test-only Retain. */
}
static VOID FinalTail(FIXTURE *f,LPCWSTR path)
{
    CHECK(!f->Session && WaitForSingleObject(f->Config.Events[CH_FINAL_ENTERED],5000)==WAIT_OBJECT_0);
    CH_SNAPSHOT s=Observe(f,2,FALSE);
    /* Closing/progress events belong to core and survive physical resources
     * until its registry/work references are released after this gate. */
    CHECK(s.Closed && s.Phase==PB_LIFE_CLOSED && !s.Scheduled && !s.Regions && s.Allocations==1 && s.Events==2);
    CHECK(s.ModulePresent && !s.TailPresent && s.Pins==1 && s.TailTransfers==1 && s.FinalHeld && s.Started>=2 && s.Finished==s.Started-1);
    CHECK(FreeLibrary(f->Library) && GetModuleHandleW(path) && WaitForSingleObject(f->Config.Events[CH_DETACHED],0)==WAIT_TIMEOUT);
    /* Only native host events/waits follow. The callback may free its core and
     * DLL before this thread is next scheduled. */
    CHECK(SetEvent(f->Config.Events[CH_FINAL_GATE]) && WaitForSingleObject(f->Config.Events[CH_DETACHED],5000)==WAIT_OBJECT_0);
    for(DWORD i=0;i<CH_MAX_CALLBACKS;++i)if(f->Config.Threads[i]) {
        CHECK(f->Config.ThreadIds[i] && WaitForSingleObject(f->Config.Threads[i],5000)==WAIT_OBJECT_0 && CloseHandle(f->Config.Threads[i]));
        ++JoinedCallbacks;
    }
    CHECK(!GetModuleHandleW(path));
    for(DWORD i=0;i<CH_EVENT_COUNT;++i)CHECK(CloseHandle(f->Config.Events[i]));
    ++Scenarios;
}
static VOID Scenario(LPCWSTR path,DWORD kind,DWORD queues,BOOL firstFail,BOOL forgetFirst,BOOL nextFail,BOOL abandon)
{
    FIXTURE f;Load(&f,path,queues,firstFail);RetireFirst(&f,forgetFirst,firstFail);
    if(kind>=DIRECT_DROP) {
        CHECK(f.Life.BeginStop(f.Session)==PB_OK);
        if(kind==PREPARE_DROP)CHECK(f.Api.PrepareClose(f.Session,0)==PB_QUARANTINED);
        CH_SNAPSHOT s=Observe(&f,4,TRUE);
        CHECK(s.Started==1 && s.Scheduled && s.Phase==PB_LIFE_STOP_REQUESTED);
        ReleasePublic(&f);s=Observe(&f,3,TRUE);CHECK(s.Scheduled);
        CHECK(SetEvent(f.Config.Events[CH_IDLE_GATE]));
    } else {
        QueueNext(&f,nextFail,abandon,kind==FAILED_WAIT);
        if(kind==LATE_STOP || kind==FAILED_WAIT)CHECK(f.Life.BeginStop(f.Session)==PB_OK);
        if(kind==DROP_PENDING || kind==FAILED_WAIT) {
            ReleasePublic(&f);CH_SNAPSHOT s=Observe(&f,2,FALSE);CHECK(s.Scheduled && s.Abandoned);
            CHECK(SetEvent(f.Config.Events[CH_POLICY_GATE]));
        } else {
            CHECK(SetEvent(f.Config.Events[CH_POLICY_GATE]));
            if(abandon) {
                /* Forgotten pending work keeps its slot until real I/O ends.
                 * Await completion while the public handle remains owned. */
                ULONGLONG deadline=GetTickCount64()+5000;PB_CLOSE_PROGRESS p;
                do {CHECK(f.Life.GetCloseProgress(f.Session,&p)==PB_OK && GetTickCount64()<deadline);if(p.PolicyCompleted==2)break;SwitchToThread();}while(1);
                PB_RESULT operation=PB_INVALID;CHECK(f.Life.PollPolicy(f.Session,f.Next,0,&operation)==PB_STALE && operation==PB_INVALID);
            } else {
                PB_RESULT operation=PB_INVALID;
                CHECK(f.Life.PollPolicy(f.Session,f.Next,5000,&operation)==PB_OK && operation==(nextFail?PB_UNSUPPORTED:PB_OK));
            }
            if(kind==LATE_STOP) {PB_CLOSE_PROGRESS p;CHECK(f.Life.GetCloseProgress(f.Session,&p)==PB_OK && p.Phase>=PB_LIFE_STOP_REQUESTED);}
            ReleasePublic(&f);
        }
    }
    FinalTail(&f,path);
}
static VOID Matrix(LPCWSTR path)
{
    for(DWORD queues=1;queues<=2;++queues)for(DWORD first=0;first<2;++first)for(DWORD forgotten=0;forgotten<2;++forgotten) {
        for(DWORD kind=NEXT_POLICY;kind<=FAILED_WAIT;++kind)for(DWORD next=0;next<2;++next)
            for(DWORD abandon=(kind>=DROP_PENDING?1u:0u);abandon<2;++abandon)
                Scenario(path,kind,queues,first!=0,forgotten!=0,next!=0,abandon!=0);
        Scenario(path,DIRECT_DROP,queues,first!=0,forgotten!=0,FALSE,FALSE);
        Scenario(path,PREPARE_DROP,queues,first!=0,forgotten!=0,FALSE,FALSE);
    }
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"101")));Control=argc==3;
    if(Control)Scenario(argv[1],NEXT_POLICY,1,FALSE,FALSE,FALSE,FALSE);
    else {
        Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
        CHECK(after<=before);printf("PASS: complete warmup/measured handle convergence %lu/%lu; owned objects checked at every DLL detach\n",before,after);
    }
    printf("PASS: %lu actual lifecycle callback handoff schedules, %lu joined synchronous callers and %lu joined callback-thread handles; real State unlock tail, work/registry/module pins, max-one callback, late success/failure/abandon/stop, wait failure and full DLL/quota/resource convergence\n",Scenarios,JoinedCallers,JoinedCallbacks);
    if(Detected){CHECK(Repairs==1);puts("CALLBACK_WORK_PIN_MISSING: observer confirmed missing work reference; negative-only repair=1, shutdown/join/unload completed with zero owned residue");return 101;}
    CHECK(!Repairs);return 0;
}
