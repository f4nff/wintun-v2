/* SPDX-License-Identifier: GPL-2.0 */
/* Real non-PB_API_TESTS session DLL. Native I/O capability/error responses are
 * hosted contracts, not execution on Windows 7 or an installed NDIS driver. */
#ifdef PB_API_TESTS
#error Native policy host must not use PB_API_TESTS
#endif
#include "../safety/guard_memory.h"
#include "native_policy_fixture.h"
#include "../../common/platform.h"
#include <string.h>
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct FIXTURE {
    HMODULE Library;PB_INTERFACE_V2 Api;PB_LIFECYCLE_INTERFACE Life;PB_SESSION *Session;PB_USER_QUEUE *Queue;PB_LEASE Loan;
    NP_CONFIG Config;NP_INITIALIZE Initialize;NP_CONFIGURE Configure;NP_OBSERVE Observe;NP_SET_ACTIVE SetActive;
} FIXTURE;
static ULONG Scenarios,JoinedCallers,JoinedCallbacks,Detected;
static VOID Load(FIXTURE *f,LPCWSTR path,DWORD mode,DWORD queues,BOOL nativeFirst)
{
    memset(f,0,sizeof(*f));CHECK(!GetModuleHandleW(path));f->Library=LoadLibraryW(path);CHECK(f->Library);
    GET_API api;GET_LIFE life;
#define GET(field,name) do{*(FARPROC *)&(field)=GetProcAddress(f->Library,name);CHECK(field);}while(0)
    GET(api,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(f->Initialize,"Initialize");GET(f->Configure,"Configure");
    GET(f->Observe,"Observe");GET(f->SetActive,"SetNativeActive");
#undef GET
    CHECK(api(2,sizeof(f->Api),&f->Api)==PB_OK && life(1,sizeof(f->Life),&f->Life)==PB_OK);
    f->Config.Mode=mode;f->Config.NdisVersion=PB_NDIS_VERSION(mode==NP_LOW_CAPABILITY?20:89);
    f->Config.PolicyError=nativeFirst?ERROR_NOT_ENOUGH_MEMORY:ERROR_ACCESS_DENIED;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i) {
        BOOL manual=i!=NP_PARK_ENTERED && i!=NP_PARK_GATE;
        f->Config.Events[i]=CreateEventW(NULL,manual,FALSE,NULL);CHECK(f->Config.Events[i]);
    }
    f->Initialize(&f->Config);
    PB_CONFIG config={sizeof(config),queues,4,4,0,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS};
    CHECK(f->Api.Open(L"hosted-native-policy",&config,&f->Session)==PB_OK);f->Configure(f->Session);
    CHECK(f->Api.OpenQueue(f->Session,queues-1,&f->Queue)==PB_OK);uint32_t n;
    CHECK(f->Api.ReserveBatch(f->Queue,&f->Loan,1,&n)==PB_OK && n==1);f->Loan.Data[0]=0x5a;
    PB_PROBE_STATUS caps;CHECK(f->Api.GetCapabilities(f->Session,&caps)==PB_OK && caps.SessionRegistered==1 && caps.NdisVersion==f->Config.NdisVersion);
    if(mode==NP_LOW_CAPABILITY)CHECK(!(caps.ImplementedMask&(PB_TX_USO4|PB_TX_USO6)) && !caps.AllowedMask);
}
static NP_SNAPSHOT Observe(FIXTURE *f)
{
    NP_SNAPSHOT s;f->Observe(&s);
    CHECK(s.Sessions==1 && s.Bytes && s.Registered && s.Works==1 && s.Pools==1 && s.Peak<=1);
    if(!s.ResourcesClosed)CHECK(s.Allocations>1 && s.Regions==1 && s.Events>=5 && s.Pins==2 && !s.TailTransfers && !s.DeviceCloses);
    return s;
}
static VOID Complete(FIXTURE *f,uint64_t ticket,BOOL forgotten,PB_RESULT expected)
{
    PB_RESULT operation=PB_INVALID;
    if(forgotten) {
        ULONGLONG deadline=GetTickCount64()+5000;PB_CLOSE_PROGRESS p;
        do {CHECK(f->Life.GetCloseProgress(f->Session,&p)==PB_OK && GetTickCount64()<deadline);if(p.PolicyCompleted==ticket)break;SwitchToThread();}while(1);
        CHECK(f->Life.PollPolicy(f->Session,ticket,0,&operation)==PB_STALE && operation==PB_INVALID);
    } else CHECK(f->Life.PollPolicy(f->Session,ticket,5000,&operation)==PB_OK && operation==expected);
    NP_SNAPSHOT s=Observe(f);CHECK(!s.Occupied && s.Completed==ticket && !s.Queued);
}
typedef struct SYNC_CALL {PB_INTERFACE_V2 *Api;PB_SESSION *Session;PB_RESULT Result;} SYNC_CALL;
static DWORD WINAPI SynchronousZero(VOID *opaque)
{SYNC_CALL *call=opaque;call->Result=call->Api->SetPolicy(call->Session,0);return 0;}
static VOID Policy(FIXTURE *f,BOOL forgotten)
{
    DWORD mode=f->Config.Mode;uint32_t mask=mode==NP_LOW_CAPABILITY?PB_TX_USO4:PB_TX_IP4_CHECKSUM;
    uint64_t ticket;CHECK(f->Life.BeginSetPolicy(f->Session,mask,&ticket)==PB_OK && ticket==1);
    DWORD entered=mode==NP_BEFORE_CONTROL?NP_CONTROL_ENTERED:NP_IO_ENTERED;
    DWORD gate=mode==NP_BEFORE_CONTROL?NP_CONTROL_GATE:NP_IO_GATE;
    CHECK(WaitForSingleObject(f->Config.Events[entered],5000)==WAIT_OBJECT_0);
    NP_SNAPSHOT s=Observe(f);CHECK(s.Occupied && !s.Queued && !s.CloseRequested && s.Loans==1 && s.NativeActive==1);
    if(mode==NP_BEFORE_CONTROL)CHECK(!s.PolicyCalls);
    else CHECK(s.PolicyCalls==1 && s.LastPolicyMask==mask);
    if(forgotten)f->Life.ForgetPolicy(f->Session,ticket);
    PB_RESULT output=PB_INVALID;CHECK(f->Life.PollPolicy(f->Session,ticket,0,&output)==PB_EMPTY && output==PB_INVALID);
    uint64_t rejected=UINT64_MAX;CHECK(f->Life.BeginSetPolicy(f->Session,0,&rejected)==PB_BUSY && !rejected);
    if(mode!=NP_LOW_CAPABILITY)CHECK(f->Life.BeginStop(f->Session)==PB_OK);
    CHECK(SetEvent(f->Config.Events[gate]));
    PB_RESULT expected=mode==NP_LOW_CAPABILITY || mode==NP_LATE_FAILURE?PB_UNSUPPORTED:mode==NP_BEFORE_CONTROL?PB_STOPPED:PB_OK;
    if(mode==NP_BEFORE_CONTROL) {
        /* The observer mutation sends a valid extra IOCTL. Record it, allow
         * real DRAIN/ownership cleanup, then report 106 only after unload. */
        ULONGLONG deadline=GetTickCount64()+5000;PB_CLOSE_PROGRESS p;
        do {CHECK(f->Life.GetCloseProgress(f->Session,&p)==PB_OK && GetTickCount64()<deadline);if(p.PolicyCompleted==ticket)break;SwitchToThread();}while(1);
        s=Observe(f);
        if(s.AfterStopCalls){CHECK(s.AfterStopCalls==1 && s.PolicyCalls==1);++Detected;expected=PB_OK;}
        else CHECK(!s.PolicyCalls);
    }
    Complete(f,ticket,forgotten,expected);s=Observe(f);
    if(mode==NP_LOW_CAPABILITY) {
        CHECK(s.LastPolicyError==ERROR_NOT_SUPPORTED && !s.AfterStopCalls && !s.AllowedMask && !s.CloseRequested && s.Phase==PB_LIFE_RUNNING);
        PB_V2_STATUS status;CHECK(f->Api.GetStatus(f->Session,&status)==PB_OK && status.State==PB_SESSION_RUNNING && status.ActiveReferences==1);
        /* Native failure did not stop queue admission or strand its slot. */
        PB_LEASE next;uint32_t n;CHECK(f->Api.ReserveBatch(f->Queue,&next,1,&n)==PB_OK && n==1);
        next.Data[0]=0xa5;CHECK(f->Api.CancelBatch(f->Queue,&next.Descriptor.Token,1)==PB_OK);
        CHECK(f->Life.BeginSetPolicy(f->Session,0,&ticket)==PB_OK && ticket==2);Complete(f,ticket,FALSE,PB_OK);
        SYNC_CALL call={&f->Api,f->Session,PB_INVALID};HANDLE thread=CreateThread(NULL,0,SynchronousZero,&call,0,NULL);
        CHECK(thread && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && call.Result==PB_OK && CloseHandle(thread));++JoinedCallers;
        s=Observe(f);CHECK(s.PolicyCalls==3 && !s.LastPolicyMask && !s.LastPolicyError && !s.Occupied && !s.CloseRequested && s.Phase==PB_LIFE_RUNNING);
        PB_PROBE_STATUS caps;CHECK(f->Api.GetCapabilities(f->Session,&caps)==PB_OK && !caps.AllowedMask && !(caps.ImplementedMask&PB_TX_USO4));
        CHECK(f->Life.BeginStop(f->Session)==PB_OK);
    } else {
        CHECK(s.CloseRequested && s.Phase>=PB_LIFE_STOP_REQUESTED);
        if(mode==NP_LATE_FAILURE)CHECK(s.PolicyCalls==1 && s.LastPolicyError==f->Config.PolicyError && !s.AfterStopCalls);
        if(mode==NP_LATE_SUCCESS)CHECK(s.PolicyCalls==1 && !s.LastPolicyError && !s.AfterStopCalls);
    }
}
static VOID AwaitPark(FIXTURE *f)
{CHECK(WaitForSingleObject(f->Config.Events[NP_PARK_ENTERED],5000)==WAIT_OBJECT_0);}
static VOID ReleasePublic(FIXTURE *f)
{CHECK(f->Session);f->Life.ReleaseSession(f->Session);f->Session=NULL;f->Queue=NULL;}
static VOID CancelLoan(FIXTURE *f)
{CHECK(f->Loan.Data[0]==0x5a);f->Loan.Data[0]=0x6b;CHECK(f->Api.CancelBatch(f->Queue,&f->Loan.Descriptor.Token,1)==PB_OK);}
static VOID Barriers(FIXTURE *f,BOOL nativeFirst,BOOL dropBeforeNative)
{
    AwaitPark(f);NP_SNAPSHOT s=Observe(f);
    CHECK(s.Loans==1 && s.NativeActive==1 && s.Drains==1 && !s.DeviceCloses && s.BlockStage==PB_BLOCK_LEASES);
    CHECK(f->Api.PrepareClose(f->Session,0)==PB_BUSY && f->Api.Wait(f->Queue,0)==PB_STOPPED);
    if(nativeFirst) {
        f->SetActive(0);CHECK(SetEvent(f->Config.Events[NP_PARK_GATE]));AwaitPark(f);s=Observe(f);
        CHECK(s.Loans==1 && !s.NativeActive && !s.DeviceCloses && s.BlockStage==PB_BLOCK_LEASES);
        PB_V2_STATUS status;CHECK(f->Api.GetStatus(f->Session,&status)==PB_OK && status.State==PB_SESSION_STOPPED && !status.ActiveReferences);
        CHECK(f->Api.PrepareClose(f->Session,0)==PB_BUSY);CancelLoan(f);ReleasePublic(f);
    } else {
        CancelLoan(f);CHECK(SetEvent(f->Config.Events[NP_PARK_GATE]));AwaitPark(f);s=Observe(f);
        CHECK(!s.Loans && s.NativeActive==1 && s.BlockStage==PB_BLOCK_KERNEL && s.Queries>0 && !s.DeviceCloses);
        CHECK(f->Api.PrepareClose(f->Session,0)==PB_QUARANTINED);
        if(dropBeforeNative) {
            ReleasePublic(f);s=Observe(f);CHECK(s.PublicReleased && s.References==2 && s.NativeActive==1 && !s.Loans);
            f->SetActive(0);
        } else {f->SetActive(0);ReleasePublic(f);}
    }
    /* No more public session calls: the worker and registry own this core. */
    s=Observe(f);CHECK(s.PublicReleased && s.References==2 && !s.Loans && !s.NativeActive && !s.ResourcesClosed);
    CHECK(SetEvent(f->Config.Events[NP_PARK_GATE]));
}
static VOID Finish(FIXTURE *f,LPCWSTR path)
{
    CHECK(!f->Session && WaitForSingleObject(f->Config.Events[NP_FINAL_ENTERED],5000)==WAIT_OBJECT_0);
    NP_SNAPSHOT s=Observe(f);
    CHECK(s.ResourcesClosed && s.Phase==PB_LIFE_CLOSED && s.References==2 && !s.Regions && s.Allocations==1 && s.Events==2);
    CHECK(s.DeviceCloses==1 && s.Pins==1 && s.TailTransfers==1 && s.ActiveCallbacks==1 && s.Finished==s.Callbacks-1);
    CHECK(FreeLibrary(f->Library) && GetModuleHandleW(path) && WaitForSingleObject(f->Config.Events[NP_DETACHED],0)==WAIT_TIMEOUT);
    CHECK(SetEvent(f->Config.Events[NP_FINAL_GATE]) && WaitForSingleObject(f->Config.Events[NP_DETACHED],5000)==WAIT_OBJECT_0);
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(f->Config.Threads[i]) {
        CHECK(f->Config.ThreadIds[i] && WaitForSingleObject(f->Config.Threads[i],5000)==WAIT_OBJECT_0 && CloseHandle(f->Config.Threads[i]));++JoinedCallbacks;
    }
    CHECK(!GetModuleHandleW(path));for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(f->Config.Events[i]));++Scenarios;
}
static VOID Scenario(LPCWSTR path,DWORD mode,DWORD queues,BOOL forgotten,BOOL nativeFirst,BOOL dropBeforeNative)
{FIXTURE f;Load(&f,path,mode,queues,nativeFirst);Policy(&f,forgotten);Barriers(&f,nativeFirst,dropBeforeNative);Finish(&f,path);}
static VOID Matrix(LPCWSTR path)
{
    for(DWORD queues=1;queues<=2;++queues)for(DWORD nativeFirst=0;nativeFirst<2;++nativeFirst)for(DWORD forgotten=0;forgotten<2;++forgotten)
        for(DWORD drop=0;drop<(nativeFirst?1u:2u);++drop)for(DWORD mode=NP_LOW_CAPABILITY;mode<=NP_LATE_FAILURE;++mode)
            Scenario(path,mode,queues,forgotten!=0,nativeFirst!=0,drop!=0);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"106")));
    if(argc==3)Scenario(argv[1],NP_BEFORE_CONTROL,1,FALSE,FALSE,TRUE);
    else {
        Matrix(argv[1]);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix(argv[1]);
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after<=before);
        printf("PASS: native policy full warmup/measured handles %lu/%lu; every DLL detach checked exact owned resources\n",before,after);
    }
    printf("PASS: %lu non-PB_API_TESTS native policy schedules; %lu joined public callers, %lu joined callback-thread handles; NDIS6.20 USO rejection/retry, Control stop recheck, late IO outcomes, consume/forget, separate loan/native barriers and DLL/quota/pool/handle convergence\n",Scenarios,JoinedCallers,JoinedCallbacks);
    if(Detected){printf("NATIVE_POLICY_IO_AFTER_STOP: %lu extra policy IOCTL observed after Control gate; actual DLL drain/cancel, modeled native-active release, join/unload completed with zero owned residue\n",Detected);return 106;}
    return 0;
}
