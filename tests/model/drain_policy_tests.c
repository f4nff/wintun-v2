/* SPDX-License-Identifier: GPL-2.0 */
/* Actual unloadable DLL, synchronous Drain and inflight policy. Native I/O is
 * hosted; every public user joins before consuming the session handle. */
#ifdef PB_API_TESTS
#error Drain/policy host requires shipping DLL paths
#endif
#include "../safety/guard_memory.h"
#include "drain_policy_fixture.h"
#include "../../common/platform.h"
#include <string.h>
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct DP_FIXTURE {
    HMODULE Library;PB_INTERFACE_V2 Api;PB_LIFECYCLE_INTERFACE Life;PB_SESSION *Session;
    DP_CONFIG Config;NP_OBSERVE Observe;NP_SET_ACTIVE SetActive;DP_MARK Mark;
    PB_RESULT DrainResult;DWORD DrainError;
} DP_FIXTURE;
typedef struct DP_OWNER {DP_FIXTURE *Fixture;ULONG Queue;} DP_OWNER;
static HANDLE Event(BOOL manual){HANDLE event=CreateEventW(NULL,manual,FALSE,NULL);CHECK(event);return event;}
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static ULONG Scenarios,OwnersJoined,DrainsJoined,CallbacksJoined,FailureCases,RetryCases;
static DWORD WINAPI Owner(VOID *opaque)
{
    DP_OWNER *owner=opaque;DP_FIXTURE *f=owner->Fixture;PB_USER_QUEUE *queue;PB_LEASE rx,tx;uint32_t count;
    f->Mark(owner->Queue);CHECK(f->Api.OpenQueue(f->Session,owner->Queue,&queue)==PB_OK);
    CHECK(f->Api.ReceiveBatch(queue,&tx,1,&count)==PB_OK && count==1);
    CHECK(f->Api.ReserveBatch(queue,&rx,1,&count)==PB_OK && count==1);rx.Data[0]=0x5a;
    CHECK(f->Api.Wait(queue,INFINITE)==PB_STOPPED && SetEvent(f->Config.AfterWait[owner->Queue]));
    Await(f->Config.RetireGate);CHECK(rx.Data[0]==0x5a && tx.Data[0]==(BYTE)owner->Queue);
    CHECK(f->Api.CancelBatch(queue,&rx.Descriptor.Token,1)==PB_OK);
    CHECK(f->Api.ReleaseBatch(queue,&tx.Descriptor.Token,1,&count)==PB_OK && count==1);return 0;
}
static DWORD WINAPI DrainCaller(VOID *opaque)
{
    DP_FIXTURE *f=opaque;f->Config.DrainThread=(LONG)GetCurrentThreadId();
    f->DrainResult=f->Api.Drain(f->Session);f->DrainError=GetLastError();return 0;
}
static VOID Scenario(LPCWSTR path,ULONG queues,BOOL policyFailure,BOOL sleepFailure,BOOL negative)
{
    DP_FIXTURE f={0};CHECK(!GetModuleHandleW(path));f.Library=LoadLibraryW(path);CHECK(f.Library);
    GET_API api;GET_LIFE life;DP_PREPARE prepare;DP_PUBLISH publish;NP_CONFIGURE configure;
#define GET(field,name) do{*(FARPROC *)&(field)=GetProcAddress(f.Library,name);CHECK(field);}while(0)
    GET(api,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(prepare,"PrepareDrainPolicy");GET(publish,"PublishDrainIncoming");
    GET(configure,"Configure");GET(f.Mark,"MarkDrainOwner");GET(f.Observe,"Observe");GET(f.SetActive,"SetNativeActive");
#undef GET
    CHECK(api(2,sizeof(f.Api),&f.Api)==PB_OK && life(1,sizeof(f.Life),&f.Life)==PB_OK);
    f.Config.Queues=queues;f.Config.FailSleep=sleepFailure;f.Config.Native.Mode=policyFailure?NP_LATE_FAILURE:NP_LATE_SUCCESS;
    f.Config.Native.NdisVersion=PB_NDIS_VERSION(89);f.Config.Native.PolicyError=ERROR_ACCESS_DENIED;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)f.Config.Native.Events[i]=Event(i!=NP_PARK_ENTERED && i!=NP_PARK_GATE);
    for(ULONG i=0;i<queues;++i){f.Config.WaitEntered[i]=Event(TRUE);f.Config.AfterWait[i]=Event(TRUE);}
    f.Config.RetireGate=Event(TRUE);f.Config.SleepEntered=Event(TRUE);f.Config.RetryEntered=Event(TRUE);prepare(&f.Config);
    PB_CONFIG config={sizeof(config),queues,4,4,0,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS};
    CHECK(f.Api.Open(L"hosted-drain-policy",&config,&f.Session)==PB_OK);configure(f.Session);publish();
    DP_OWNER owners[8];HANDLE threads[8];
    for(ULONG i=0;i<queues;++i){owners[i]=(DP_OWNER){&f,i};threads[i]=CreateThread(NULL,0,Owner,owners+i,0,NULL);CHECK(threads[i]);Await(f.Config.WaitEntered[i]);}
    uint64_t ticket;CHECK(f.Life.BeginSetPolicy(f.Session,PB_TX_IP4_CHECKSUM,&ticket)==PB_OK && ticket==1);Await(f.Config.Native.Events[NP_IO_ENTERED]);
    HANDLE drain=CreateThread(NULL,0,DrainCaller,&f,0,NULL);CHECK(drain);Await(f.Config.SleepEntered);
    for(ULONG i=0;i<queues;++i)Await(f.Config.AfterWait[i]);
    if(negative){CHECK(sleepFailure);Await(f.Config.RetryEntered);}
    /* Observe takes State. The failed current call returned with it released;
     * real waits/retries must release it in the SDK. No held-State hard gate. */
    NP_SNAPSHOT state;f.Observe(&state);
    CHECK(state.Occupied && !state.Queued && state.Completed==0 && state.Requested==1 && state.PolicyCalls==1 && !state.Drains &&
        state.Loans==(LONG)(2*queues) && state.NativeActive==1 && state.CloseRequested && !state.DeviceCloses);
    PB_RESULT operation=PB_INVALID;CHECK(f.Life.PollPolicy(f.Session,ticket,0,&operation)==PB_EMPTY && operation==PB_INVALID);
    if(sleepFailure) {
        ++FailureCases;CHECK(f.Config.SleepFailures==1);
        if(negative) {
            CHECK(f.Config.SleepCalls>=2 && f.Config.SleepRetries>=1 && WaitForSingleObject(drain,0)==WAIT_TIMEOUT);++RetryCases;
        }else{
            Await(drain);CHECK(f.DrainResult==PB_CORRUPT && f.DrainError==ERROR_OPERATION_ABORTED && f.Config.SleepCalls==1 && !f.Config.SleepRetries);
            CHECK(WaitForSingleObject(f.Config.RetryEntered,0)==WAIT_TIMEOUT);
        }
    }else CHECK(WaitForSingleObject(drain,0)==WAIT_TIMEOUT && !f.Config.SleepFailures && !f.Config.SleepRetries);
    CHECK(SetEvent(f.Config.Native.Events[NP_IO_GATE]));Await(f.Config.Native.Events[NP_PARK_ENTERED]);
    if(!sleepFailure || negative){Await(drain);CHECK(f.DrainResult==(negative?PB_CORRUPT:PB_OK) && f.Config.SleepCalls>=1);}
    CHECK(CloseHandle(drain));++DrainsJoined;
    CHECK(f.Life.PollPolicy(f.Session,ticket,5000,&operation)==PB_OK && operation==(policyFailure?PB_UNSUPPORTED:PB_OK));
    f.Observe(&state);CHECK(!state.Occupied && state.Completed==1 && state.Drains==1 && state.Loans==(LONG)(2*queues) &&
        state.NativeActive==1 && !state.DeviceCloses && state.BlockStage==PB_BLOCK_LEASES);
    CHECK(f.Api.PrepareClose(f.Session,0)==PB_BUSY && SetEvent(f.Config.RetireGate));
    for(ULONG i=0;i<queues;++i){Await(threads[i]);CHECK(CloseHandle(threads[i]));++OwnersJoined;}
    CHECK(SetEvent(f.Config.Native.Events[NP_PARK_GATE]));Await(f.Config.Native.Events[NP_PARK_ENTERED]);
    f.Observe(&state);CHECK(!state.Loans && state.NativeActive==1 && state.Queries && state.BlockStage==PB_BLOCK_KERNEL && !state.DeviceCloses);
    /* All external queue/Drain users joined before a final Close can consume
     * the public reference. The first non-success Close leaves it owned. */
    CHECK(f.Api.Close(f.Session,0)==PB_QUARANTINED);f.SetActive(0);CHECK(SetEvent(f.Config.Native.Events[NP_PARK_GATE]));Await(f.Config.Native.Events[NP_FINAL_ENTERED]);
    CHECK(f.Api.Close(f.Session,0)==PB_OK);f.Session=NULL;f.Observe(&state);
    CHECK(state.ResourcesClosed && state.Phase==PB_LIFE_CLOSED && state.References==2 && !state.Regions && state.Allocations==1 &&
        state.Events==2 && state.Pins==1 && state.TailTransfers==1);
    CHECK(FreeLibrary(f.Library) && GetModuleHandleW(path));CHECK(SetEvent(f.Config.Native.Events[NP_FINAL_GATE]));Await(f.Config.Native.Events[NP_DETACHED]);
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(f.Config.Native.Threads[i]){Await(f.Config.Native.Threads[i]);CHECK(CloseHandle(f.Config.Native.Threads[i]));++CallbacksJoined;}
    CHECK(!GetModuleHandleW(path));for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(f.Config.Native.Events[i]));
    for(ULONG i=0;i<queues;++i)CHECK(CloseHandle(f.Config.WaitEntered[i]) && CloseHandle(f.Config.AfterWait[i]));
    CHECK(CloseHandle(f.Config.RetireGate) && CloseHandle(f.Config.SleepEntered) && CloseHandle(f.Config.RetryEntered));++Scenarios;
}
int wmain(int argc,WCHAR **argv)
{
    BOOL selected=argc==3 && !wcscmp(argv[2],L"123");CHECK(argc==2 || selected);
#ifdef PB_DRAIN_POLICY_WITHOUT_BREAK
    BOOL negative=TRUE;CHECK(selected);
#else
    BOOL negative=FALSE;
#endif
    DWORD before=0,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Scenario(argv[1],8,FALSE,TRUE,negative);
    else {
        const ULONG queues[]={1,2,8};
        for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
            for(ULONG i=0;i<3;++i)for(ULONG policy=0;policy<2;++policy)for(ULONG failure=0;failure<2;++failure)
                Scenario(argv[1],queues[i],policy!=0,failure!=0,FALSE);
        }
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
    /* Windows may retain first-use shared infrastructure in a fresh process.
     * Full runs measure after a complete checked warm matrix. Each selected
     * case still checks every owned object and real detach independently. */
    if(!selected)CHECK(before==after);
    printf("PASS: %lu actual nonPB synchronous Drain/policy schedules, %lu queue owners and %lu Drain callers joined, %lu callback OS handles joined; %lu defensive failures/%lu retry witnesses; actual unload, handles %lu/%lu\n",
        Scenarios,OwnersJoined,DrainsJoined,CallbacksJoined,FailureCases,RetryCases,before,after);
    if(negative){puts("DRAIN_WAIT_FAILURE_RETRIED: single defensive failure retried instead of returning; subsequent real SDK wait released State, late policy and actual Drain completed, every public owner/caller and callback joined before final close/unload, zero owned residue");return 123;}
    return 0;
}
