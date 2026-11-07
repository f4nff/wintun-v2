/* SPDX-License-Identifier: GPL-2.0 */
/* Actual NDEBUG DLL completion turns; legal peer publications and native status are hosted. */
#ifdef PB_API_TESTS
#error Actual native paths required
#endif
#ifndef NDEBUG
#error Shipping NDEBUG required
#endif
#include "../safety/guard_memory.h"
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
#include <string.h>
#define CHECK SAFETY_CHECK
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"shipping fingerprint required");
typedef struct CT_CONFIG {NP_CONFIG Native;ULONG Queues,Heavy;BOOL Wrap;} CT_CONFIG;
typedef struct CT_STATE {LONG Published[2],Calls[2],Popped[2],Waits,Wakes,Timeouts,EventState;ULONG64 Cursor[2],Producer[2];LONG Loans,NativeActive,Regions,Closed;} CT_STATE;
#ifdef CT_FIXTURE
static CT_CONFIG *TurnHost;
static PB_RESULT TurnPop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t *);
static DWORD WINAPI TurnWait(HANDLE,DWORD);
static VOID WINAPI TurnAcquire(PSRWLOCK);
#include "../safety/native_policy_fixture.c"
static PB_DESCRIPTOR Script[2][512];
static PB_QUEUE_VIEW SubmitPeer[2],CompletePeer[2];
static volatile LONG TurnCalls[2],TurnPopped[2],TurnWaits,TurnWakes,TurnTimeouts;
static uint64_t Base;
typedef LONG (NTAPI *QUERY_EVENT)(HANDLE,ULONG,VOID *,ULONG,ULONG *);
static VOID WINAPI TurnAcquire(PSRWLOCK lock)
{
    ObservedAcquire(lock);
    if(Current && (LONG)GetCurrentThreadId()==CallbackThread && lock==&Current->DataGate)
        for(ULONG i=0;i<TurnHost->Queues;++i)TurnCalls[i]=TurnPopped[i]=0;
}
static PB_RESULT TurnPop(PB_QUEUE_VIEW *view,PB_DESCRIPTOR *items,uint32_t capacity,uint32_t *count)
{
    PB_RESULT result=PbQueuePop(view,items,capacity,count);
    if(Current && (LONG)GetCurrentThreadId()==CallbackThread)for(ULONG q=0;q<TurnHost->Queues;++q)if(view==Current->Queues[q].Rings+PB_QUEUE_RX_COMPLETE){CHECK(capacity==64 && InterlockedIncrement(TurnCalls+q)<=4);InterlockedAdd(TurnPopped+q,(LONG)*count);}
    return result;
}
static DWORD WINAPI TurnWait(HANDLE event,DWORD timeout)
{
    if(Current && event==Progress && timeout==1000){
        InterlockedIncrement(&TurnWaits);DWORD result=ObservedWait(event,timeout);
        if(result==WAIT_OBJECT_0)InterlockedIncrement(&TurnWakes);else if(result==WAIT_TIMEOUT)InterlockedIncrement(&TurnTimeouts);else CHECK(FALSE);
        return result;
    }
    return ObservedWait(event,timeout);
}
__declspec(dllexport) VOID WINAPI PrepareTurns(CT_CONFIG *config)
{CHECK(config && !TurnHost);TurnHost=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI ConfigureTurns(PB_SESSION *session)
{
    Configure(session);Base=TurnHost->Wrap?UINT64_MAX-127:0;
    for(ULONG q=0;q<TurnHost->Queues;++q){PB_USER_QUEUE *queue=Current->Queues+q;
        for(ULONG r=PB_QUEUE_RX_SUBMIT;r<=PB_QUEUE_RX_COMPLETE;++r){PB_QUEUE_VIEW *view=queue->Rings+r;view->Cursor=view->PeerCursor=Base;view->Shared->Producer=view->Shared->Consumer=Base;}
        CHECK(PbAttachQueue(SubmitPeer+q,Current->Region,&Current->Layout,q,PB_QUEUE_RX_SUBMIT,FALSE)==PB_OK && PbAttachQueue(CompletePeer+q,Current->Region,&Current->Layout,q,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);
        SubmitPeer[q].Cursor=SubmitPeer[q].PeerCursor=CompletePeer[q].Cursor=CompletePeer[q].PeerCursor=Base;
    }
}
__declspec(dllexport) VOID WINAPI CollectTurnSubmissions(VOID)
{
    for(ULONG q=0;q<TurnHost->Queues;++q){ULONG total=q==TurnHost->Heavy?512:1,done=0;while(done<total){uint32_t count;CHECK(PbQueuePop(SubmitPeer+q,Script[q]+done,min(64,total-done),&count)==PB_OK && count);done+=count;}CHECK(Current->Queues[q].Published==(LONG)total);}
}
static VOID TurnPublish(ULONG queue,ULONG first,ULONG count)
{
    ULONG done=0;while(done<count){uint32_t accepted,notify;ULONG n=min(64,count-done);CHECK(PbQueuePush(CompletePeer+queue,Script[queue]+first+done,n,&accepted,&notify)==PB_OK && accepted==n);if(notify)CHECK(SetEvent(Current->Queues[queue].UserEvent));done+=accepted;}
}
__declspec(dllexport) VOID WINAPI PublishTurnCompletions(ULONG stage)
{
    CHECK(Current && ParkHeld && NativeActive==1);
    if(!stage){CHECK(TurnWaits==2 && TurnWakes==1);for(ULONG q=0;q<TurnHost->Queues;++q)TurnPublish(q,0,q==TurnHost->Heavy?511:1);CHECK(SetEvent(Progress));}
    else{CHECK(stage==1);TurnPublish(TurnHost->Heavy,511,1);SetNativeActive(0);}
}
__declspec(dllexport) VOID WINAPI ReadTurns(CT_STATE *out)
{
    CHECK(Current && ParkHeld && out);memset(out,0,sizeof(*out));out->Waits=TurnWaits;out->Wakes=TurnWakes;out->Timeouts=TurnTimeouts;out->Loans=LoansNow();out->NativeActive=NativeActive;out->Regions=Regions;out->Closed=Current->ResourcesClosed;
    for(ULONG q=0;q<TurnHost->Queues;++q){out->Calls[q]=TurnCalls[q];out->Popped[q]=TurnPopped[q];out->Published[q]=Current->Queues[q].Published;out->Cursor[q]=Current->Queues[q].Rings[PB_QUEUE_RX_COMPLETE].Cursor-Base;out->Producer[q]=CompletePeer[q].Cursor-Base;}
    QUERY_EVENT query;*(FARPROC *)&query=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryEvent");CHECK(query);struct {LONG Type,State;} basic;
    CHECK(query(Progress,0,&basic,sizeof(basic),NULL)>=0 && basic.Type==1 && (basic.State==0 || basic.State==1));out->EventState=basic.State;
}
__declspec(dllexport) ULONG WINAPI WaitPreparedTurn(VOID)
{
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(Current->Progress.Phase==PB_LIFE_PREPARED && !Current->Scheduled && !ActiveCallbacks && Finished==CallbackCount &&
        (CallbackCount==1 || CallbackCount==2) && !Current->ResourcesClosed && Regions==1 && !LoansNow() && !NativeActive);
    /* Test-only inspection after the real callback: the shipping NDEBUG
     * ValidOwnership path never performs this slot/index scan. */
    for(ULONG q=0;q<TurnHost->Queues;++q){PB_USER_QUEUE *queue=Current->Queues+q;CHECK(!queue->Published);
        for(ULONG i=0;i<512;++i)CHECK(queue->RxSlots[i].State==PB_SLOT_FREE);
        for(ULONG i=0;i<512/32;++i)CHECK(queue->Rx.FreeBits[i]==UINT32_MAX);
    }
    return (ULONG)CallbackCount;
}
#else
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef VOID (WINAPI *PREPARE)(CT_CONFIG *);
typedef VOID (WINAPI *ACTION)(VOID);
typedef ULONG (WINAPI *PREPARED)(VOID);
typedef VOID (WINAPI *CONFIGURE)(PB_SESSION *);
typedef VOID (WINAPI *PUBLISH)(ULONG);
typedef VOID (WINAPI *READ)(CT_STATE *);
typedef struct OWNER {PB_INTERFACE_V2 *Api;PB_SESSION *Session;ULONG Queue,Total;} OWNER;
static DWORD WINAPI Owner(VOID *opaque)
{
    OWNER *owner=opaque;PB_USER_QUEUE *queue;CHECK(owner->Api->OpenQueue(owner->Session,owner->Queue,&queue)==PB_OK);
    for(ULONG done=0;done<owner->Total;){PB_LEASE leases[64];uint32_t count;ULONG n=min(64,owner->Total-done);CHECK(owner->Api->ReserveBatch(queue,leases,n,&count)==PB_OK && count==n);
        for(ULONG i=0;i<n;++i){memset(leases[i].Data,0,20);leases[i].Data[0]=0x45;leases[i].Data[3]=20;leases[i].Descriptor.Length=20;}
        CHECK(owner->Api->CommitBatch(queue,leases,n,&count)==PB_OK && count==n);done+=count;}
    return 0;
}
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static ULONG Cases,OwnerJoins,PrepCallbacks,FinalCallbacks,CallbackJoins,ForcedPrepCases,Missing;
static BOOL Negative;
static VOID Scenario(LPCWSTR path,ULONG queues,ULONG heavy,BOOL wrap)
{
    CHECK(!GetModuleHandleW(path));HMODULE module=LoadLibraryW(path);CHECK(module);GET_API get;GET_LIFE getLife;PREPARE prepare;CONFIGURE configure;ACTION collect;PREPARED prepared;PUBLISH publish;READ read;
#define GET(f,n) do{*(FARPROC *)&f=GetProcAddress(module,n);CHECK(f);}while(0)
    GET(get,"PbGetInterfaceV2");GET(getLife,"PbGetLifecycleInterface");GET(prepare,"PrepareTurns");GET(configure,"ConfigureTurns");GET(collect,"CollectTurnSubmissions");GET(publish,"PublishTurnCompletions");GET(read,"ReadTurns");GET(prepared,"WaitPreparedTurn");
#undef GET
    PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;CHECK(get(2,sizeof(api),&api)==PB_OK && getLife(1,sizeof(life),&life)==PB_OK);
    CT_CONFIG config={0};config.Queues=queues;config.Heavy=heavy;config.Wrap=wrap;config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60014;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}
    prepare(&config);PB_CONFIG opening={sizeof(opening),queues,512,512,0,0};PB_SESSION *session;CHECK(api.Open(L"hosted-completion-turn",&opening,&session)==PB_OK);configure(session);
    OWNER jobs[2];HANDLE owners[2];for(ULONG q=0;q<queues;++q){jobs[q]=(OWNER){&api,session,q,q==heavy?512:1};owners[q]=CreateThread(NULL,0,Owner,jobs+q,0,NULL);CHECK(owners[q]);}
    for(ULONG q=0;q<queues;++q){Await(owners[q]);CHECK(CloseHandle(owners[q]));++OwnerJoins;}collect();CHECK(life.BeginStop(session)==PB_OK);
    CT_STATE s;Await(config.Native.Events[NP_PARK_ENTERED]);read(&s);CHECK(s.Waits==1 && !s.Wakes && s.EventState==1 && !s.Timeouts && !s.Loans);
    CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);read(&s);CHECK(s.Waits==2 && s.Wakes==1 && !s.EventState && !s.Timeouts && s.Published[heavy]==512);
    publish(0);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);read(&s);
    CHECK(s.Waits==3 && s.Wakes==2 && !s.Timeouts && s.Calls[heavy]==4 && s.Popped[heavy]==256 && s.Published[heavy]==256 && s.Cursor[heavy]==256 && s.Producer[heavy]==511 && s.NativeActive==1 && s.Regions==1 && !s.Closed);
    for(ULONG q=0;q<queues;++q)if(q!=heavy)CHECK(s.Popped[q]==1 && s.Published[q]==0 && s.Cursor[q]==1);
    CHECK(s.EventState==(Negative?0:1));
    if(Negative)++Missing;
    else{
        CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);read(&s);CHECK(s.Wakes==3 && !s.Timeouts && s.Popped[heavy]==255 && s.Published[heavy]==1 && s.Cursor[heavy]==511 && s.EventState==1);
        CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));Await(config.Native.Events[NP_PARK_ENTERED]);read(&s);CHECK(s.Wakes==4 && !s.Timeouts && !s.Popped[heavy] && s.Calls[heavy]==1 && s.Published[heavy]==1 && !s.EventState);
    }
    /* Last real hosted native completion/reference notification rescues the
     * negative naturally; no event-state repair or timeout proof is used. */
    publish(1);CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));
    /* Finishing the first callback before PrepareClose deterministically tests
     * its legal second preparation callback. Other shapes retain the race:
     * PrepareClose can reuse the running callback or submit one after it ends. */
    if(wrap)CHECK(prepared()==1);
    CHECK(api.PrepareClose(session,5000)==PB_OK);ULONG prep=prepared();CHECK(wrap?prep==2:(prep==1 || prep==2));
    CHECK(api.Close(session,5000)==PB_OK);session=NULL;Await(config.Native.Events[NP_FINAL_ENTERED]);
    CHECK(FreeLibrary(module));CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));Await(config.Native.Events[NP_DETACHED]);
    ULONG joined=0;BOOL gap=FALSE;
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i){
        if(config.Native.Threads[i]){CHECK(!gap && i<prep+1 && config.Native.ThreadIds[i]);Await(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++joined;++CallbackJoins;}
        else{CHECK(!config.Native.ThreadIds[i]);gap=TRUE;}
    }
    CHECK(joined==prep+1 && !GetModuleHandleW(path));for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));
    PrepCallbacks+=prep;++FinalCallbacks;if(wrap)++ForcedPrepCases;++Cases;
    printf("CASE: completion turn queues%lu heavy%lu wrap%u preparation%lu final1 joins%lu missing%u\n",queues,heavy,(unsigned)wrap,prep,joined,(unsigned)Negative);
}
static VOID Matrix(LPCWSTR path){for(ULONG wrap=0;wrap<2;++wrap){Scenario(path,1,0,wrap);Scenario(path,2,0,wrap);Scenario(path,2,1,wrap);}}
int wmain(int argc,WCHAR **argv)
{
    BOOL selected=argc==3 && !wcscmp(argv[2],L"132");CHECK(argc==2||selected);DWORD before,after;
#ifdef CT_NEGATIVE
    Negative=TRUE;CHECK(selected);
#endif
    if(selected){Scenario(argv[1],2,0,TRUE);Scenario(argv[1],2,0,TRUE);}else Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Scenario(argv[1],2,0,TRUE);else Matrix(argv[1]);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    CHECK(FinalCallbacks==Cases && CallbackJoins==PrepCallbacks+FinalCallbacks);
    printf("PASS: %lu completion turn cases, owner joins%lu preparation callbacks%lu final callbacks%lu callback joins%lu forced prepare%lu missing%lu handles%lu/%lu; NDEBUG bounded256 per queue, direct event-state witness, actual SDK wakes and final native progress\n",Cases,OwnerJoins,PrepCallbacks,FinalCallbacks,CallbackJoins,ForcedPrepCases,Missing,before,after);
    if(Negative){puts("COMPLETION_TURN_SELF_WAKE_MISSING: non-consuming event query observed no continuation after budget while ready completions remained; final genuine native progress rescued normal cleanup before132");return 132;}return 0;
}
#endif
