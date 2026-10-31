/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
#include "../../api/v2/adapter.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while (0)
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static PB_SESSION *Create(void)
{
    PB_CONFIG config = {sizeof(config),1,4,4,0,0}; PB_SESSION *s;
    void *region; PB_LAYOUT layout; uint64_t epoch;
    CHECK(PbTestOpen(&config,&s,&region,&layout,&epoch)==PB_OK); return s;
}
static void AwaitRuntime(uint32_t count)
{
    ULONGLONG start=GetTickCount64(); PB_RUNTIME_STATUS runtime;
    do {
        CHECK(Life.GetRuntimeStatus(&runtime)==PB_OK);
        if(runtime.Sessions==count) return;
        CHECK(GetTickCount64()-start<5000); SwitchToThread();
    } while(1);
}
static void BlockedClose(uint32_t stage)
{
    PB_SESSION *s=Create(); PB_USER_QUEUE *q; CHECK(Api.OpenQueue(s,0,&q)==PB_OK);
    HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(entered&&gate);
    PbTestBlock(s,stage,entered,gate);
    CHECK(Api.Close(s,0)==PB_QUARANTINED);
    CHECK(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    CHECK(PbTestBoundaryThread(s,stage)!=GetCurrentThreadId());
    /* The following calls must return while the external operation remains held. */
    CHECK(Api.Close(s,0)==PB_QUARANTINED && Api.Wait(q,INFINITE)==PB_STOPPED);
    PB_CLOSE_PROGRESS progress; CHECK(Life.GetCloseProgress(s,&progress)==PB_OK);
    CHECK(progress.Phase>=PB_LIFE_DRAINING && progress.Flags&PB_LIFE_QUARANTINED);
    PB_SESSION *other=Create(); CHECK(Api.Close(other,2000)==PB_OK);
    AwaitRuntime(1);
    Life.ReleaseSession(s); /* Ownership is consumed, do not touch s/q again. */
    PB_CLOSE_PROGRESS retained; CHECK(Life.GetRetained(0,&retained)==PB_OK && retained.SessionId==progress.SessionId);
    CHECK(SetEvent(gate)); AwaitRuntime(0); CHECK(CloseHandle(entered)&&CloseHandle(gate));
}
static void PolicyCancellation(void)
{
    PB_SESSION *s=Create(); HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(entered&&gate);
    PbTestBlock(s,PB_TEST_POLICY,entered,gate);
    uint64_t ticket,other; PB_RESULT applied=PB_INVALID;
    CHECK(Life.BeginSetPolicy(s,0,&ticket)==PB_OK && ticket);
    CHECK(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    CHECK(Life.BeginSetPolicy(s,0,&other)==PB_BUSY && !other);
    CHECK(Life.PollPolicy(s,ticket,0,&applied)==PB_EMPTY);
    CHECK(Life.BeginStop(s)==PB_OK && Api.PrepareClose(s,0)==PB_QUARANTINED);
    CHECK(SetEvent(gate) && Life.PollPolicy(s,ticket,2000,&applied)==PB_OK && applied==PB_OK);
    PB_CLOSE_PROGRESS progress; CHECK(Life.GetCloseProgress(s,&progress)==PB_OK && progress.Phase>=PB_LIFE_STOP_REQUESTED);
    CHECK(Life.BeginSetPolicy(s,0,&other)==PB_STOPPED && Life.PollPolicy(s,ticket,0,&applied)==PB_STALE);
    CHECK(Api.PrepareClose(s,2000)==PB_OK && Api.Close(s,2000)==PB_OK);
    CHECK(CloseHandle(entered)&&CloseHandle(gate)); AwaitRuntime(0);
}
static void AdmissionBudget(void)
{
    PB_RUNTIME_STATUS before; CHECK(Life.GetRuntimeStatus(&before)==PB_OK && !before.Sessions && !before.RetainedBytes);
    CHECK(before.MaxSessions<=64); PB_SESSION *sessions[64];
    for(uint32_t i=0;i<before.MaxSessions;++i) sessions[i]=Create();
    PB_CONFIG config={sizeof(config),1,4,4,0,0}; PB_SESSION *extra=(void *)1; void *region; PB_LAYOUT layout; uint64_t epoch;
    CHECK(PbTestOpen(&config,&extra,&region,&layout,&epoch)==PB_FULL && !extra);
    for(uint32_t i=0;i<before.MaxSessions;++i) Life.ReleaseSession(sessions[i]);
    AwaitRuntime(0);
    PB_SESSION *again=Create(); CHECK(Api.Close(again,2000)==PB_OK);
    AwaitRuntime(0);
    CHECK(Life.GetRuntimeStatus(&before)==PB_OK && !before.Adapters && before.MaxAdapters<=64);
    for(uint32_t i=0;i<before.MaxAdapters;++i) CHECK(PbRuntimeReserveAdapter());
    PB_ADAPTER *adapter=(void *)1;
    /* Admission failure must precede package staging and SwDeviceCreate. No device is installed. */
    CHECK(Api.CreateAdapter(L"budget",L"budget-rejection",&adapter)==PB_FULL && !adapter);
    for(uint32_t i=0;i<before.MaxAdapters;++i) PbRuntimeReleaseAdapter();
    CHECK(Life.GetRuntimeStatus(&before)==PB_OK && !before.Adapters);
}
static void CancelQueuedPolicy(void)
{
    PB_SESSION *s=Create(); HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(entered&&gate);
    PbTestBlock(s,PB_TEST_WORKER_START,entered,gate);
    uint64_t ticket;PB_RESULT applied=PB_INVALID;
    CHECK(Life.BeginSetPolicy(s,0,&ticket)==PB_OK&&WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    CHECK(Life.BeginStop(s)==PB_OK&&SetEvent(gate));
    CHECK(Life.PollPolicy(s,ticket,2000,&applied)==PB_OK&&applied==PB_STOPPED);
    CHECK(!PbTestBoundaryThread(s,PB_TEST_POLICY)&&Api.Close(s,2000)==PB_OK);
    AwaitRuntime(0);CHECK(CloseHandle(entered)&&CloseHandle(gate));
    s=Create();uint64_t next;
    CHECK(Life.BeginSetPolicy(s,0,&ticket)==PB_OK&&Life.PollPolicy(s,ticket,2000,&applied)==PB_OK&&applied==PB_OK);
    CHECK(Life.BeginSetPolicy(s,0,&next)==PB_OK&&next>ticket);
    CHECK(Life.PollPolicy(s,ticket,0,&applied)==PB_STALE);
    CHECK(Life.PollPolicy(s,next,2000,&applied)==PB_OK&&applied==PB_OK&&Api.Close(s,2000)==PB_OK);
    AwaitRuntime(0);
}
static void FailedBarrierRetainsResources(uint32_t stage)
{
    PB_SESSION *s=Create(); PbTestFail(s,stage,TRUE);
    CHECK(Api.Close(s,2000)==PB_CORRUPT);
    /* Close and the next progress read are separate observations. The startup
     * notification can already have begun an automatic retry in between. */
    PB_CLOSE_PROGRESS progress; ULONGLONG snapshotStart=GetTickCount64();
    for(;;) {
        CHECK(Life.GetCloseProgress(s,&progress)==PB_OK && progress.RetainedBytes);
        if(progress.Result==PB_CORRUPT) {
            CHECK(progress.LastError==ERROR_DEVICE_NOT_CONNECTED && progress.Flags&PB_LIFE_QUARANTINED);break;
        }
        CHECK((progress.Result==PB_QUARANTINED || progress.Result==PB_OK) && GetTickCount64()-snapshotStart<2000);
        SwitchToThread();
    }
    PB_RUNTIME_STATUS retained; CHECK(Life.GetRuntimeStatus(&retained)==PB_OK && retained.Sessions && retained.RetainedBytes);
    PbTestFail(s,stage,FALSE);
    ULONGLONG start=GetTickCount64();
    for(;;) {
        PB_RESULT result=Api.Close(s,50);
        if(result==PB_OK) break;
        CHECK((result==PB_CORRUPT||result==PB_QUARANTINED)&&GetTickCount64()-start<5000); SwitchToThread();
    }
    AwaitRuntime(0);
}
static void NativeFaultAbandonsCompletion(void)
{
    PB_SESSION *s=Create(); PB_USER_QUEUE *q; PB_LEASE leases[2],unused; uint32_t n;
    CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,leases,2,&n)==PB_OK && n==2);
    leases[0].Descriptor.Length=20; CHECK(Api.CommitBatch(q,leases,1,&n)==PB_OK && n==1);
    PbTestActive(s,1); PbTestFault(s);
    /* No malformed completion is exposed to DLL and no API Wait turns its
     * private Fatal flag on. The native fault abandons completion delivery. */
    CHECK(Api.PrepareClose(s,0)==PB_BUSY);
    PB_CLOSE_PROGRESS p; CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.Published==1 && p.UserLeases==1 && p.RetainedBytes);
    leases[1].Data[0]=0x5a; CHECK(Api.CancelBatch(q,&leases[1].Descriptor.Token,1)==PB_OK);
    CHECK(Api.PrepareClose(s,20)==PB_QUARANTINED);
    CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.Published==1 && !p.UserLeases && p.RetainedBytes);
    PbTestActive(s,0); CHECK(Api.PrepareClose(s,2000)==PB_OK);
    CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.Phase==PB_LIFE_PREPARED && !p.Published && !p.UserLeases && !p.NativeReferences);
    CHECK(Api.ReserveBatch(q,&unused,1,&n)==PB_STOPPED); /* Still no DLL-local Fatal. */
    CHECK(Api.Close(s,2000)==PB_OK); AwaitRuntime(0);
    puts("PASS: native-only fault with abandoned completion retains user leases and native references; trusted STOPPED reconciles Published even without DLL Fatal.");
}
int main(void)
{
    _Static_assert(sizeof(PB_INTERFACE_V2)==280,"frozen V2 ABI");
    _Static_assert(sizeof(PB_CLOSE_PROGRESS)==96,"progress ABI");
    _Static_assert(sizeof(PB_LIFECYCLE_INTERFACE)==80,"extension ABI");
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    BYTE guarded[24]; memset(guarded,0xa5,sizeof(guarded));
    CHECK(PbGetLifecycleInterface(1,16,(void *)guarded)==PB_OK);
    for(uint32_t i=16;i<sizeof(guarded);++i) CHECK(guarded[i]==0xa5);
    CHECK(PbGetLifecycleInterface(2,sizeof(Life),&Life)==PB_UNSUPPORTED && PbGetLifecycleInterface(1,15,&Life)==PB_UNSUPPORTED);
    BlockedClose(PB_TEST_DRAIN); BlockedClose(PB_TEST_DEVICE_CLOSE); BlockedClose(PB_TEST_ADAPTER_CLOSE); BlockedClose(PB_TEST_QUERY);
    PolicyCancellation(); CancelQueuedPolicy(); AdmissionBudget(); AwaitRuntime(0);
    FailedBarrierRetainsResources(PB_TEST_DRAIN); FailedBarrierRetainsResources(PB_TEST_QUERY); FailedBarrierRetainsResources(PB_TEST_DEVICE_CLOSE);
    NativeFaultAbandonsCompletion();
    puts("PASS: failed drain/query/device-close preserve resources and original errors, then reclaim exactly once after recovery.");
    PB_SESSION *idle=Create(); PB_USER_QUEUE *q; CHECK(Api.OpenQueue(idle,0,&q)==PB_OK);
    uint32_t queries=PbTestQueries(idle);
    for(uint32_t i=0;i<1000;++i) CHECK(Api.Wait(q,0)==PB_EMPTY);
    CHECK(PbTestQueries(idle)==queries && Api.Close(idle,2000)==PB_OK);
    puts("PASS: 1000 empty Wait(0) calls issue zero status queries.");
    puts("PASS: lifecycle extension prefix ABI, four blocked close boundaries, zero-budget return, detached ownership, cross-session progress, late policy acknowledgement and admission budget recovery.");
    return 0;
}
