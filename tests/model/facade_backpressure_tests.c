/* SPDX-License-Identifier: GPL-2.0 */
/* Actual facade backpressure with a hosted native ring producer and real SDK waits. */
#ifdef PB_API_TESTS
#error Shipping paths required
#endif
#ifndef NDEBUG
#error Facade backpressure requires the shipping NDEBUG branch
#endif
#include "../safety/guard_memory.h"
#include "facade_stop_fixture.h"
#include <string.h>
#define CHECK SAFETY_CHECK
enum { BP_FIRST,BP_SECOND,BP_LATER,BP_ARM,BP_FIRST_GO,BP_SECOND_GO,BP_LATER_GO,BP_ARM_GO,BP_EVENTS };
typedef struct BP_CONFIG {NP_CONFIG Native;FS_CONFIG Legacy;HANDLE Events[BP_EVENTS];ULONG Length;BOOL Wrap;} BP_CONFIG;
typedef struct BP_STATE {LONG Loans,Waits,ReadyArms,Issued,Repairs;ULONG Cursor,Head,Tail;LONG NativeRegions,NativePins,LegacyRegions,LegacyHandles;} BP_STATE;
#ifdef BP_FIXTURE
static BP_CONFIG *BackHost;
static DWORD WINAPI BackWait(DWORD,const HANDLE *,BOOL,DWORD);
static PB_RESULT BackArm(PB_QUEUE_VIEW *,uint32_t *);
#include PB_BACKPRESSURE_BASE
static PB_QUEUE_VIEW BackReady,BackRecycle;
static volatile LONG BackWaits,BackArms,Republished;
static VOID Await(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static DWORD WINAPI BackWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(count==3 && !all && timeout==INFINITE && events[2]==LegacyWake);
    LONG call=InterlockedIncrement(&BackWaits);ULONG entered=call==1?BP_FIRST:call==2?BP_SECOND:BP_LATER;
    ULONG gate=call==1?BP_FIRST_GO:call==2?BP_SECOND_GO:BP_LATER_GO;
    CHECK(SetEvent(BackHost->Events[entered]));Await(BackHost->Events[gate]);
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result>=WAIT_OBJECT_0 && result<WAIT_OBJECT_0+3);return result;
}
static PB_RESULT BackArm(PB_QUEUE_VIEW *view,uint32_t *ready)
{
    PB_RESULT result=PbQueueArm(view,ready);
    if(Current && Republished && view==Current->Queues[0].Rings+PB_QUEUE_TX_READY && result==PB_OK && *ready) {
        ULONG span=TUN_ALIGN(sizeof(TUN_PACKET)+BackHost->Length),space=TUN_RING_WRAP(Facade->Descriptor.Send.Ring->Head-Facade->Descriptor.Send.Ring->Tail-TUN_ALIGNMENT,Facade->Capacity);
        CHECK(LoansNow()>0 && space<span && InterlockedIncrement(&BackArms)<=2);
        CHECK(SetEvent(BackHost->Events[BP_ARM]));Await(BackHost->Events[BP_ARM_GO]);
    }
    return result;
}
__declspec(dllexport) VOID WINAPI BackPrepare(BP_CONFIG *config)
{
    BackHost=config;CHECK(config);Initialize(&config->Native);PrepareFacade(&config->Legacy);(void)&FacadeWait;
}
__declspec(dllexport) VOID *WINAPI BackStart(VOID)
{return StartFacade();}
__declspec(dllexport) VOID WINAPI BackPublish(ULONG stage)
{
    CHECK(Current && Facade && BackHost);
    if(!stage) {
        ULONG base=BackHost->Wrap?Facade->Capacity-TUN_ALIGNMENT:0;
        Facade->Send.Head=Facade->Send.HeadRelease=base;Facade->Descriptor.Send.Ring->Head=Facade->Descriptor.Send.Ring->Tail=base;
        CHECK(PbAttachQueue(&BackReady,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK && PbAttachQueue(&BackRecycle,Current->Region,&Current->Layout,0,PB_QUEUE_TX_RECYCLE,FALSE)==PB_OK);
        for(ULONG i=0;i<64;++i){Incoming[i]=(PB_DESCRIPTOR){.Token={Current->Epoch,1,(USHORT)i,0},.DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE,.Length=BackHost->Length};memset(Current->Region+Incoming[i].DataOffset,(int)i,BackHost->Length);}
        uint32_t count,notify;CHECK(PbQueuePush(&BackReady,Incoming,64,&count,&notify)==PB_OK && count==64 && notify);CHECK(SetEvent(Current->Queues[0].UserEvent));
    }else {
        CHECK(stage==1 && BackWaits==2 && LoansNow()==63 && !Republished);
        PB_TOKEN token;uint32_t count,notify;CHECK(PbQueuePopTokens(&BackRecycle,&token,1,&count)==PB_OK && count==1 && token.Slot==0 && token.Generation==1);
        CHECK(Current->Queues[0].TxSlots[0].State==PB_SLOT_FREE && Current->Queues[0].TxSlots[0].Generation==1);
        PB_DESCRIPTOR next=Incoming[0];next.Token=token;++next.Token.Generation;memset(Current->Region+next.DataOffset,0xa5,next.Length);
        CHECK(PbQueuePush(&BackReady,&next,1,&count,&notify)==PB_OK && count==1);Republished=1;if(notify)CHECK(SetEvent(Current->Queues[0].UserEvent));
    }
}
__declspec(dllexport) BYTE *WINAPI BackReceive(VOID *session,ULONG expected)
{DWORD bytes;BYTE *packet=WintunReceivePacket(session,&bytes);CHECK(packet && bytes==BackHost->Length && packet[0]==(BYTE)expected && packet[bytes-1]==(BYTE)expected);return packet;}
__declspec(dllexport) VOID WINAPI BackReturn(VOID *session,BYTE *packet)
{WintunReleaseReceivePacket(session,packet);CHECK(!Facade->Send.PacketsToRelease);}
__declspec(dllexport) BOOL WINAPI BackStopping(VOID)
{CHECK(Facade);return ReadAcquire(&Facade->Stopping)!=0;}
__declspec(dllexport) VOID WINAPI BackEnd(VOID *session)
{
    CHECK(session==Facade && !Facade->Send.PacketsToRelease);WintunEndSession(session);Facade=NULL;
    CHECK(!LegacyRuntime&&!LegacyHeap&&!LegacyRegions&&!LegacyHandles&&!LegacyPaths&&!LegacyPins&&!CompatRingBytes&&!RepairLoans && LegacyWakes==4);
    CHECK(HeapDestroy(ModuleHeap));ModuleHeap=NULL;
}
__declspec(dllexport) VOID WINAPI BackRead(BP_STATE *out)
{
    CHECK(out);memset(out,0,sizeof(*out));out->Waits=BackWaits;out->ReadyArms=BackArms;out->Repairs=RepairLoans;
    if(Current){out->Loans=LoansNow();out->Cursor=(ULONG)Current->Queues[0].Rings[PB_QUEUE_TX_READY].Cursor;CHECK(BackReady.Cursor==65);}
    if(Facade){out->Issued=Facade->Send.PacketsToRelease;out->Head=Facade->Descriptor.Send.Ring->Head;out->Tail=Facade->Descriptor.Send.Ring->Tail;}
    out->NativeRegions=Regions;out->NativePins=Pins;out->LegacyRegions=LegacyRegions;out->LegacyHandles=LegacyHandles;
}
#else
typedef VOID (WINAPI *PREPARE)(BP_CONFIG *);
typedef VOID *(WINAPI *START)(VOID);
typedef VOID (WINAPI *PUBLISH)(ULONG);
typedef BYTE *(WINAPI *RECEIVE)(VOID *,ULONG);
typedef VOID (WINAPI *RETURN)(VOID *,BYTE *);
typedef BOOL (WINAPI *STOPPING)(VOID);
typedef VOID (WINAPI *END)(VOID *);
typedef VOID (WINAPI *READ)(BP_STATE *);
typedef struct END_CALL {END End;VOID *Session;} END_CALL;
static DWORD WINAPI EndCaller(VOID *opaque){END_CALL *call=opaque;call->End(call->Session);return 0;}
static VOID AwaitHost(HANDLE event){CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);}
static ULONG Cases,CallbackJoins,ReadyCases;
static BOOL Negative;
static VOID Scenario(LPCWSTR path,ULONG length,BOOL wrap)
{
    CHECK(!GetModuleHandleW(path));HMODULE library=LoadLibraryW(path);CHECK(library);PREPARE prepare;START start;PUBLISH publish;RECEIVE receive;RETURN release;END end;READ read;STOPPING stopping;FS_ACTION recover;
#define GET(f,n) do{*(FARPROC *)&(f)=GetProcAddress(library,n);CHECK(f);}while(0)
    GET(prepare,"BackPrepare");GET(start,"BackStart");GET(publish,"BackPublish");GET(receive,"BackReceive");GET(release,"BackReturn");GET(end,"BackEnd");GET(read,"BackRead");GET(stopping,"BackStopping");GET(recover,"RecoverFacadeNative");
#undef GET
    BP_CONFIG config={0};config.Native.Mode=NP_LATE_SUCCESS;config.Native.NdisVersion=0x60059;config.Legacy.Direction=FS_RECEIVE;config.Length=length;config.Wrap=wrap;
    for(ULONG i=0;i<NP_EVENT_COUNT;++i){config.Native.Events[i]=CreateEventW(NULL,i!=NP_PARK_ENTERED&&i!=NP_PARK_GATE,FALSE,NULL);CHECK(config.Native.Events[i]);}
    for(ULONG i=0;i<FS_EVENT_COUNT;++i){config.Legacy.Events[i]=CreateEventW(NULL,TRUE,i==FS_STOP_GATE,NULL);CHECK(config.Legacy.Events[i]);}
    for(ULONG i=0;i<BP_EVENTS;++i){config.Events[i]=CreateEventW(NULL,i!=BP_LATER&&i!=BP_ARM&&i!=BP_LATER_GO&&i!=BP_ARM_GO,FALSE,NULL);CHECK(config.Events[i]);}
    prepare(&config);VOID *session=start();AwaitHost(config.Events[BP_FIRST]);publish(0);CHECK(SetEvent(config.Events[BP_FIRST_GO]));AwaitHost(config.Events[BP_SECOND]);
    publish(1);BYTE *packet=receive(session,0);release(session,packet);CHECK(SetEvent(config.Events[BP_SECOND_GO]));
    HANDLE choices[]={config.Events[BP_LATER],config.Events[BP_ARM]};BP_STATE state;
    for(ULONG i=1;i<=2;++i){CHECK(WaitForMultipleObjects(2,choices,FALSE,10000)==WAIT_OBJECT_0+(Negative?1u:0u));read(&state);
        CHECK(state.Loans==(LONG)(63-i) && state.Cursor==64 && !state.Issued && !state.Repairs && state.NativeRegions==1 && state.LegacyRegions==1);
        CHECK(state.Waits==(Negative?2:(LONG)(i+2)) && state.ReadyArms==(Negative?(LONG)i:0));packet=receive(session,i);release(session,packet);
        if(i==1)CHECK(SetEvent(config.Events[Negative?BP_ARM_GO:BP_LATER_GO]));
    }
    END_CALL call={end,session};HANDLE endThread=CreateThread(NULL,0,EndCaller,&call,0,NULL);CHECK(endThread);ULONGLONG deadline=GetTickCount64()+10000;
    while(!stopping()){CHECK(GetTickCount64()<deadline);SwitchToThread();}
    CHECK(SetEvent(config.Events[Negative?BP_ARM_GO:BP_LATER_GO]));AwaitHost(config.Native.Events[NP_PARK_ENTERED]);recover();CHECK(SetEvent(config.Native.Events[NP_PARK_GATE]));AwaitHost(config.Native.Events[NP_FINAL_ENTERED]);
    AwaitHost(endThread);CHECK(CloseHandle(endThread));session=NULL;read(&state);CHECK(!state.Loans&&!state.LegacyRegions&&!state.LegacyHandles&&!state.NativeRegions&&!state.Repairs);
    CHECK(SetEvent(config.Native.Events[NP_FINAL_GATE]));
    for(ULONG i=0;i<NP_MAX_CALLBACKS;++i)if(config.Native.Threads[i]){AwaitHost(config.Native.Threads[i]);CHECK(CloseHandle(config.Native.Threads[i]));++CallbackJoins;}
    CHECK(FreeLibrary(library));AwaitHost(config.Native.Events[NP_DETACHED]);CHECK(!GetModuleHandleW(path));
    for(ULONG i=0;i<NP_EVENT_COUNT;++i)CHECK(CloseHandle(config.Native.Events[i]));for(ULONG i=0;i<FS_EVENT_COUNT;++i)CHECK(CloseHandle(config.Legacy.Events[i]));for(ULONG i=0;i<BP_EVENTS;++i)CHECK(CloseHandle(config.Events[i]));
    ++Cases;ReadyCases+=Negative;
}
int wmain(int argc,WCHAR **argv)
{
    BOOL selected=argc==3&&!wcscmp(argv[2],L"129");CHECK(argc==2||selected);DWORD before=0,after;
#ifdef BP_NEGATIVE
    Negative=TRUE;CHECK(selected);
#endif
    if(selected){Scenario(argv[1],65535,TRUE);Scenario(argv[1],65535,TRUE);CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Scenario(argv[1],65535,TRUE);}
    else for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));for(ULONG size=0;size<2;++size)for(ULONG wrap=0;wrap<2;++wrap)Scenario(argv[1],size?65531:65535,wrap);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);printf("PASS: %lu facade backpressure cases, callback joins%lu ready cases%lu, handles%lu/%lu; NDEBUG real wait, legal slot generation reuse, three public copy returns, actual End5000 and unload\n",Cases,CallbackJoins,ReadyCases,before,after);
    if(Negative){puts("FACADE_BACKPRESSURE_ARMED_UNCONSUMABLE_READY: bounded readiness observer, actual public packet returns/End/close/joins/unload complete");return 129;}return 0;
}
#endif
