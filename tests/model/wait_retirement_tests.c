/* SPDX-License-Identifier: GPL-2.0 */
#ifdef PB_API_TESTS
#error Wait retirement host must not enable API test shortcuts
#endif
#include "../safety/guard_memory.h"
#include "wait_retirement_fixture.h"
#include <stdlib.h>
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct JOB {PB_INTERFACE_V2 *Api;PB_USER_QUEUE *Queue;WR_CONFIG *Config;WR_MARK Mark;ULONG Index;} JOB;
static ULONG Scenarios,OwnersJoined,CallbacksJoined,EarlyQueries;
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI Owner(VOID *opaque)
{
    static const ULONG widths[]={1,2,3,7,8,31,63,64};JOB *job=opaque;ULONG width=widths[job->Index];job->Mark(job->Index);
    PB_LEASE tx[64],rx[64];PB_TOKEN txTokens[64],rxTokens[64];uint32_t count;
    CHECK(job->Api->ReceiveBatch(job->Queue,tx,width,&count)==PB_OK && count==width);
    CHECK(job->Api->ReserveBatch(job->Queue,rx,width,&count)==PB_OK && count==width);
    for(ULONG i=0;i<width;++i){txTokens[i]=tx[i].Descriptor.Token;rxTokens[i]=rx[i].Descriptor.Token;CHECK(tx[i].Data[19]==(BYTE)job->Index);}
    PB_RESULT waited=job->Api->Wait(job->Queue,INFINITE);
    if(job->Config->Fault && !job->Index) {
        CHECK(waited==PB_OK);PB_DESCRIPTOR bad;CHECK(job->Api->PollCompletions(job->Queue,&bad,1,&count)==PB_CORRUPT && !count);
    }else CHECK(waited==PB_STOPPED);
    CHECK(SetEvent(job->Config->AfterWait[job->Index]));Await(job->Config->Retire[job->Index]);
    if(job->Config->CancelFirst)CHECK(job->Api->CancelBatch(job->Queue,rxTokens,width)==PB_OK);
    CHECK(job->Api->ReleaseBatch(job->Queue,txTokens,width,&count)==PB_OK && count==width);
    if(!job->Config->CancelFirst)CHECK(job->Api->CancelBatch(job->Queue,rxTokens,width)==PB_OK);
    CHECK(job->Api->Wait(job->Queue,0)==(job->Config->Fault?PB_CORRUPT:PB_STOPPED));return 0;
}
static VOID Scenario(LPCWSTR path,ULONG queues,BOOL cancelFirst,BOOL fault,BOOL control)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);
    GET_API get;GET_LIFE life;WR_PREPARE prepare;NP_CONFIGURE configure;NP_SET_ACTIVE active;
    WR_ACTION publish,bad,arm;WR_MARK mark;WR_INSPECT inspect;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(prepare,"PrepareRetirement");GET(configure,"Configure");GET(active,"SetNativeActive");
    GET(publish,"PublishRetirementIncoming");GET(bad,"PublishRetirementFault");GET(arm,"ArmRetirementTail");GET(mark,"MarkRetirementOwner");GET(inspect,"InspectRetirement");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE runtime;CHECK(get(2,sizeof(api),&api)==PB_OK && life(1,sizeof(runtime),&runtime)==PB_OK);
    WR_CONFIG config={0};config.Queues=queues;config.Last=queues-1;config.CancelFirst=cancelFirst;config.Fault=fault;
    config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x00060014;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}
    HANDLE *gates[]={&config.WaitGate,&config.TailEntered,&config.TailGate,&config.DataAttempt,&config.EarlyQuery};
    for(ULONG i=0;i<_countof(gates);++i){*gates[i]=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(*gates[i]);}
    prepare(&config);PB_CONFIG c={sizeof(c),queues,64,128,0,0};PB_SESSION *session;
    CHECK(api.Open(L"hosted-wait-retirement",&c,&session)==PB_OK);configure(session);publish();JOB jobs[8];HANDLE owners[8];
    for(ULONG i=0;i<queues;++i) {
        config.Entered[i]=CreateEventW(NULL,TRUE,FALSE,NULL);config.AfterWait[i]=CreateEventW(NULL,TRUE,FALSE,NULL);config.Retire[i]=CreateEventW(NULL,TRUE,FALSE,NULL);
        CHECK(config.Entered[i] && config.AfterWait[i] && config.Retire[i]);PB_USER_QUEUE *queue;CHECK(api.OpenQueue(session,i,&queue)==PB_OK);
        jobs[i]=(JOB){&api,queue,&config,mark,i};owners[i]=CreateThread(NULL,0,Owner,jobs+i,0,NULL);CHECK(owners[i]);
    }
    CHECK(WaitForMultipleObjects(queues,config.Entered,TRUE,10000)==WAIT_OBJECT_0 && SetEvent(config.WaitGate));
    if(fault){bad();CHECK(WaitForMultipleObjects(queues,config.AfterWait,TRUE,10000)==WAIT_OBJECT_0);}
    CHECK(runtime.BeginStop(session)==PB_OK && WaitForMultipleObjects(queues,config.AfterWait,TRUE,10000)==WAIT_OBJECT_0);
    Await(config.Native.Events[NP_PARK_ENTERED]);CHECK(api.PrepareClose(session,0)==PB_BUSY);
    for(ULONG i=0;i<queues;++i)if(i!=config.Last) {
        CHECK(SetEvent(config.Retire[i]));Await(owners[i]);CHECK(CloseHandle(owners[i]));owners[i]=NULL;++OwnersJoined;
    }
    arm();CHECK(SetEvent(config.Retire[config.Last]));Await(config.TailEntered);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));
    HANDLE observations[]={config.DataAttempt,config.EarlyQuery};DWORD observed=WaitForMultipleObjects(2,observations,FALSE,10000);
    CHECK(observed==WAIT_OBJECT_0 || observed==WAIT_OBJECT_0+1);BOOL early=observed==WAIT_OBJECT_0+1;
    if(early)CHECK(control && config.TailHeld);
    else CHECK(WaitForSingleObject(config.EarlyQuery,0)==WAIT_TIMEOUT);
    inspect(0);PB_CLOSE_PROGRESS progress;
    CHECK(runtime.GetCloseProgress(session,&progress)==PB_OK && !progress.UserLeases && !progress.Published && progress.Phase<PB_LIFE_PREPARED);
    CHECK(api.PrepareClose(session,0)==PB_QUARANTINED);
    CHECK(SetEvent(config.TailGate));Await(owners[config.Last]);CHECK(CloseHandle(owners[config.Last]));++OwnersJoined;
    Await(config.Native.Events[NP_PARK_ENTERED]);inspect(1);CHECK(api.PrepareClose(session,0)==PB_QUARANTINED);
    /* Every public owner is joined BEFORE final Close consumes the handle. */
    active(0);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));CHECK(api.PrepareClose(session,5000)==PB_OK && api.Close(session,5000)==PB_OK);session=NULL;
    Await(config.Native.Events[NP_FINAL_ENTERED]);CHECK(FreeLibrary(library) && GetModuleHandleW(path));CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));
    Await(config.Native.Events[NP_DETACHED]);
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbacksJoined;}
    CHECK(!GetModuleHandleW(path));
    for(ULONG i=0;i<queues;++i)CHECK(CloseHandle(config.Entered[i]) && CloseHandle(config.AfterWait[i]) && CloseHandle(config.Retire[i]));
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<_countof(gates);++i)CHECK(CloseHandle(*gates[i]));++Scenarios;
    if(early)++EarlyQueries;
}
static VOID Matrix(LPCWSTR path)
{const ULONG queues[]={1,2,8};for(ULONG i=0;i<3;++i)for(ULONG order=0;order<2;++order)for(ULONG fault=0;fault<2;++fault)Scenario(path,queues[i],order!=0,fault!=0,FALSE);}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"121")));
    if(argc==3)Scenario(argv[1],8,FALSE,FALSE,TRUE);
    else{Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: wait retirement full warmup/measured handles %lu/%lu\n",before,after);}
    printf("PASS: %lu full nonPB Wait/stop/retirement schedules; %lu public owner joins, %lu callback OS-thread joins; last zero-loan DataGate/Lifetime tail, independent nativeActive, valid final Close and DLL unload/resource convergence\n",Scenarios,OwnersJoined,CallbacksJoined);
    if(EarlyQueries){CHECK(argc==3 && EarlyQueries==1);puts("RETIREMENT_SNAPSHOT_GATE_MISSING: lifecycle QUERY reached while last owner held shared DataGate after all writes; nativeActive1 prevented preparation, every owner joined before native0 and finalClose, all resources/threads/unload converged");return 121;}
    return 0;
}
