/* SPDX-License-Identifier: GPL-2.0 */
/* Actual GetRetained is the only diagnostic core owner. Current and negative
 * calls both keep an ordinary DLL reference until every public caller joins. */
#ifdef PB_API_TESTS
#error Retained owner regression must not use the hosted session shortcut
#endif
#include "../safety/guard_memory.h"
#include "retained_owner_fixture.h"
#include "../../include/packetbridge_version.h"
#include <string.h>
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"retained owner host requires the shipping fingerprint layout");
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct DIAGNOSTIC {
    RO_MARK Mark;PB_LIFECYCLE_INTERFACE *Life;PB_CLOSE_PROGRESS Progress;PB_RESULT Result;DWORD ThreadId;
} DIAGNOSTIC;
static ULONG Scenarios,JoinedDiagnostics,JoinedCallbacks,MissingPins;
static DWORD WINAPI Diagnostic(VOID *opaque)
{
    DIAGNOSTIC *call=opaque;call->ThreadId=GetCurrentThreadId();call->Mark();
    call->Result=call->Life->GetRetained(0,&call->Progress);return 0;
}
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static VOID JoinDiagnostic(HANDLE thread,DIAGNOSTIC *call,uint64_t sessionId,BOOL missing)
{
    Await(thread);CHECK(call->Result==PB_OK && call->Progress.SessionId==sessionId);
    if(missing)CHECK(call->Progress.Phase==PB_LIFE_RUNNING && call->Progress.RetainedBytes && !call->Progress.Started);
    else CHECK(call->Progress.Phase==PB_LIFE_CLOSED && !call->Progress.RetainedBytes && !call->Progress.UserLeases && !call->Progress.Published);
    CHECK(CloseHandle(thread));++JoinedDiagnostics;
}
static VOID Scenario(LPCWSTR path,BOOL control)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);
    GET_API get;GET_LIFE life;NP_INITIALIZE initialize;NP_CONFIGURE configure;NP_SET_ACTIVE setActive;
    RO_CONFIGURE configureDiagnostic;RO_MARK mark;RO_READ read;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(life,"PbGetLifecycleInterface");GET(initialize,"Initialize");GET(configure,"Configure");GET(setActive,"SetNativeActive");
    GET(configureDiagnostic,"RetainedConfigure");GET(mark,"RetainedMark");GET(read,"RetainedRead");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE runtime;CHECK(get(2,sizeof(api),&api)==PB_OK && life(1,sizeof(runtime),&runtime)==PB_OK);
    NP_CONFIG host={0};host.Mode=NP_LATE_SUCCESS;host.NdisVersion=0x00060059;
    for(DWORD i=0;i<NP_EVENT_COUNT;++i){host.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(host.Events[i]);}
    HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(entered&&gate);
    initialize(&host);PB_CONFIG config={sizeof(config),1,4,4,0,0};PB_SESSION *session;
    CHECK(api.Open(L"hosted-retained-owner",&config,&session)==PB_OK);configure(session);configureDiagnostic(entered,gate);
    DIAGNOSTIC call={.Mark=mark,.Life=&runtime,.Result=PB_INVALID};HANDLE diagnostic=CreateThread(NULL,0,Diagnostic,&call,0,NULL);CHECK(diagnostic);Await(entered);
    RO_SNAPSHOT state;read(&state);
    CHECK(state.Core && state.DiagnosticHeld && state.Sessions==1 && state.Bytes && state.Regions==1 && state.Pins==2 && !state.Callbacks);
    uint64_t sessionId=state.SessionId;BOOL missing=state.References==2;
    CHECK(state.References==(missing?2:3) && (!missing || control));
    if(missing) {
        /* Paired Retain/Drop mutation leaves pub+registry alive. Observe refs2
         * here, then finish/join the unpinned call BEFORE consuming public.
         * No repair Retain, missing-drop underflow or freed-shell access occurs. */
        ++MissingPins;CHECK(SetEvent(gate));JoinDiagnostic(diagnostic,&call,sessionId,TRUE);diagnostic=NULL;
        read(&state);CHECK(state.Core && state.References==2 && !state.DiagnosticHeld && !state.Closed && !state.DestroyThread);
    }
    runtime.ReleaseSession(session);session=NULL;
    Await(host.Events[NP_PARK_ENTERED]);setActive(0);CHECK(SetEvent(host.Events[NP_PARK_GATE]));Await(host.Events[NP_FINAL_ENTERED]);
    read(&state);CHECK(state.Core && state.References==(missing?2:3) && state.Closed && state.Sessions==1 && !state.Regions && state.Pins==1 && state.Events==2);
    CHECK(SetEvent(host.Events[NP_FINAL_GATE]));
    if(!missing) {
        ULONGLONG deadline=GetTickCount64()+5000;
        /* Registry removal, reference release and callback counters are
         * separate observations. Wait for the complete stable predicate. */
        do {read(&state);if(!state.Active && state.Finished==state.Callbacks && !state.Sessions && state.References==1 && !state.Registered)break;
            CHECK(GetTickCount64()<deadline);SwitchToThread();}while(1);
        CHECK(state.Core && state.DiagnosticHeld && state.Closed && !state.Bytes && !state.Regions && state.Allocations==1);
        CHECK(state.Events==2 && state.Works==1 && state.Pools==1 && state.Pins==1 && !state.DestroyThread && !state.PoolCloseThread && !state.CoreUnpinThread);
        PB_RUNTIME_STATUS status;CHECK(runtime.GetRuntimeStatus(&status)==PB_OK && !status.Sessions && !status.RetainedBytes);
        PB_CLOSE_PROGRESS empty;memset(&empty,0x5a,sizeof(empty));PB_CLOSE_PROGRESS sentinel=empty;
        CHECK(runtime.GetRetained(0,&empty)==PB_EMPTY && !memcmp(&empty,&sentinel,sizeof(empty)));
        CHECK(SetEvent(gate));JoinDiagnostic(diagnostic,&call,sessionId,FALSE);diagnostic=NULL;
        read(&state);CHECK(!state.Core && (DWORD)state.DestroyThread==call.ThreadId && (DWORD)state.PoolCloseThread==call.ThreadId &&
            (DWORD)state.CoreUnpinThread==call.ThreadId && (DWORD)state.CallbackThread!=call.ThreadId);
    }
    /* In the mutation, normal shutdown now runs without any diagnostic caller.
     * Its core dies on the callback; join the same actual callback OS threads.
     * The positive diagnostic Drop has already closed its work/pool here. */
    for(DWORD i=0;i<NP_MAX_CALLBACKS;++i)if(host.Threads[i]){Await(host.Threads[i]);CHECK(CloseHandle(host.Threads[i]));++JoinedCallbacks;}
    read(&state);CHECK(!state.Core && !state.Allocations && !state.Regions && !state.Events && !state.Works && !state.Pools && !state.Pins && !state.Sessions && !state.Bytes);
    CHECK(!diagnostic && !state.Active && state.Finished==state.Callbacks);
    if(missing)CHECK(state.DestroyThread && state.DestroyThread==state.CallbackThread && state.PoolCloseThread==state.CallbackThread && state.CoreUnpinThread==state.CallbackThread);
    CHECK(FreeLibrary(library));Await(host.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(DWORD i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(host.Events[i]));CHECK(CloseHandle(entered)&&CloseHandle(gate));++Scenarios;
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2 || (argc==3 && !wcscmp(argv[2],L"111")));BOOL control=argc==3;
    if(control)Scenario(argv[1],TRUE);
    else {
        for(DWORD i=0;i<16;++i)Scenario(argv[1],FALSE);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(DWORD i=0;i<16;++i)Scenario(argv[1],FALSE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
        printf("PASS: complete retained-owner warmup/measured process handles %lu/%lu\n",before,after);
    }
    printf("PASS: %lu actual GetRetained owner schedules; %lu joined diagnostic callers and %lu callback-thread handle joins; Closed shell, final core destructor/pool/module pin owner and actual DLL unload/resource convergence\n",Scenarios,JoinedDiagnostics,JoinedCallbacks);
    if(MissingPins){CHECK(control && MissingPins==1);puts("DIAGNOSTIC_LOOKUP_PIN_MISSING: refs=2 witnessed while public+registry live; unpinned diagnostic completed/joined before public release, normal cleanup and DLL unload finished with zero owned residue");return 111;}
    return 0;
}
