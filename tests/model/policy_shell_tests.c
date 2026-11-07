/* SPDX-License-Identifier: GPL-2.0 */
/* Public shell reference survives actual physical close until all policy readers join. */
#ifdef PB_API_TESTS
#error Actual native DLL required
#endif
#include "../safety/guard_memory.h"
#include "policy_shell_fixture.h"
#include "../../common/platform.h"
#include <string.h>
#define CHECK SAFETY_CHECK
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef struct READER {PS_MARK Mark;PB_LIFECYCLE_INTERFACE *Life;PB_SESSION *Session;ULONG Index;PB_RESULT Result,Operation;} READER;
static HANDLE Event(BOOL manual){HANDLE h=CreateEventW(NULL,manual,FALSE,NULL);CHECK(h);return h;}
static VOID Await(HANDLE h){CHECK(WaitForSingleObject(h,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI Poll(VOID *opaque){READER *r=opaque;r->Mark(r->Index);r->Operation=PB_INVALID;r->Result=r->Life->PollPolicy(r->Session,1,INFINITE,&r->Operation);return 0;}
static ULONG Cases,ReaderJoins,CallbackJoins,RepeatConfirmations;
static BOOL Negative;
static VOID Scenario(LPCWSTR path,ULONG count,ULONG retire,BOOL failPolicy)
{
    PS_CONFIG config={0};config.Pollers=count;config.Native.Mode=failPolicy?NP_LATE_FAILURE:NP_LATE_SUCCESS;
    config.Native.PolicyError=ERROR_ACCESS_DENIED;config.Native.NdisVersion=PB_NDIS_VERSION(89);
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)config.Native.Events[i]=Event(i!=NP_PARK_ENTERED && i!=NP_PARK_GATE);
    for(ULONG i=0;i<count;++i){config.Waiting[i]=Event(TRUE);config.Resumed[i]=Event(TRUE);config.Gates[i]=Event(TRUE);}
    CHECK(!GetModuleHandleW(path));HMODULE module=LoadLibraryW(path);CHECK(module);
    GET_API getApi;GET_LIFE getLife;PS_PREPARE prepare;PS_MARK mark;NP_CONFIGURE configure;NP_SET_ACTIVE active;NP_OBSERVE observe;PS_WAIT_CALLBACK waitCallback;
#define GET(field,name) do {*(FARPROC *)&field=GetProcAddress(module,name);CHECK(field);} while(0)
    GET(getApi,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PreparePolicyShell");GET(mark,"MarkPolicyReader");GET(configure,"Configure");GET(active,"SetNativeActive");GET(observe,"Observe");GET(waitCallback,"WaitPolicyShellCallback");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(getApi(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);prepare(&config);
    PB_SESSION *session;PB_CONFIG opening={sizeof(opening),1,4,4,0,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS};
    CHECK(api.Open(L"hosted-policy-shell",&opening,&session)==PB_OK);configure(session);
    uint64_t ticket;CHECK(life.BeginSetPolicy(session,PB_TX_IP4_CHECKSUM,&ticket)==PB_OK && ticket==1);Await(config.Native.Events[NP_IO_ENTERED]);
    /* Close0 returns non-success BEFORE the new public readers start. */
    CHECK(api.Close(session,0)==PB_QUARANTINED);
    READER readers[8];HANDLE threads[8];
    for(ULONG i=0;i<count;++i){readers[i]=(READER){mark,&life,session,i,PB_INVALID,PB_INVALID};threads[i]=CreateThread(NULL,0,Poll,readers+i,0,NULL);CHECK(threads[i]);Await(config.Waiting[i]);}
    CHECK(SetEvent(config.Native.Events[NP_IO_GATE]));
    for(ULONG i=0;i<count;++i)Await(config.Resumed[i]);Await(config.Native.Events[NP_PARK_ENTERED]);
    NP_SNAPSHOT s;observe(&s);CHECK(s.NativeActive==1 && s.Occupied && s.Completed==1 && !s.ResourcesClosed && !s.PublicReleased);
    active(0);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_FINAL_ENTERED]);
    CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));waitCallback();observe(&s);
    CHECK(s.ResourcesClosed && s.Phase==PB_LIFE_CLOSED && s.References==1 && !s.Registered && !s.Sessions && !s.Bytes && s.Allocations==1 && !s.Regions && s.Events==2 && s.Works==1 && s.Pools==1 && s.Pins==1 && s.TailTransfers==1 && !s.PublicReleased);
    uint64_t rejected=UINT64_MAX;CHECK(life.BeginSetPolicy(session,0,&rejected)==PB_STOPPED && !rejected);
    for(ULONG i=0;i<count;++i)CHECK(WaitForSingleObject(threads[i],0)==WAIT_TIMEOUT);
    if(retire==1){PB_RESULT operation=PB_INVALID;CHECK(life.PollPolicy(session,1,0,&operation)==PB_OK && operation==(failPolicy?PB_UNSUPPORTED:PB_OK));}
    else if(retire==2)life.ForgetPolicy(session,1);
    for(ULONG i=0;i<count;++i){CHECK(SetEvent(config.Gates[i]));Await(threads[i]);CHECK(CloseHandle(threads[i]));++ReaderJoins;
        BOOL winner=!retire && !i;BOOL acknowledged=winner || Negative;
        CHECK(readers[i].Result==(acknowledged?PB_OK:PB_STALE) && readers[i].Operation==(acknowledged?(failPolicy?PB_UNSUPPORTED:PB_OK):PB_INVALID));
        if(Negative && !winner)++RepeatConfirmations;}
    observe(&s);CHECK(!s.Occupied && s.References==1 && s.Pins==1 && s.ResourcesClosed && !s.PublicReleased);
    CHECK(api.Close(session,0)==PB_OK);session=NULL;
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;}
    CHECK(FreeLibrary(module) && WaitForSingleObject(config.Native.Events[NP_DETACHED],0)==WAIT_OBJECT_0 && !GetModuleHandleW(path));
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));
    for(ULONG i=0;i<count;++i)CHECK(CloseHandle(config.Waiting[i]) && CloseHandle(config.Resumed[i]) && CloseHandle(config.Gates[i]));++Cases;
}
int wmain(int argc,WCHAR **argv)
{
    BOOL selected=argc==3 && !wcscmp(argv[2],L"126");CHECK(argc==2 || selected);
    const ULONG readers[]={1,2,8};DWORD before=0,after;
#ifdef PB_POLICY_SHELL_WITHOUT_OCCUPANCY
    Negative=TRUE;CHECK(selected);
#endif
    for(ULONG pass=0;pass<2;++pass){
        if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        if(selected){Scenario(argv[1],8,1,FALSE);Scenario(argv[1],8,2,TRUE);}
        else for(ULONG i=0;i<3;++i)for(ULONG retire=0;retire<3;++retire)for(ULONG failure=0;failure<2;++failure)Scenario(argv[1],readers[i],retire,failure!=0);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("PASS: %lu actual nonPB policy Closed-shell schedules; %lu poller joins, %lu callback OS-thread joins; pub-only ref/pin preserved through physical close/runtime removal, winner/consume/forget outputs, final Close strictly after reader joins, DLL unload; handles %lu/%lu; repeat confirmations%lu\n",Cases,ReaderJoins,CallbackJoins,before,after,RepeatConfirmations);
    if(Negative){CHECK(RepeatConfirmations==32);puts("CLOSED_POLICY_ALREADY_RETIRED_ACKNOWLEDGED: occupancy guard omitted; completed ticket acknowledged after peer consume/forget, all pollers joined before finalClose and actual unload, every owned resource closed");return 126;}return 0;
}
