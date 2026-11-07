/* SPDX-License-Identifier: GPL-2.0 */
/* Actual legacy facade plus native session. Reuse the native ledger once;
 * guards cover native objects/map, while legacy rings use VirtualAlloc. */
#ifdef PB_API_TESTS
#error Facade stop fixture requires shipping native session paths
#endif
#ifdef PB_COMPAT_OPEN
#error Facade stop fixture must use the default production PB_COMPAT_OPEN
#endif
#include "guard_memory.h"
#include "../model/facade_stop_fixture.h"
static FS_CONFIG *FacadeConfig;
static volatile LONG WaitArmed=1,QueryFails,WaitCalls,CommitStopped,Cancelled,RepairLoans,PublicReturns,CloseCalls,CloseResult;
static BOOL WINAPI FacadeIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static DWORD WINAPI FacadeWait(DWORD,const HANDLE *,BOOL,DWORD);
#ifndef PB_FACADE_NATIVE_SOURCE
#error Build through Build-FacadeStopTests for explicit source binding
#endif
#define DllMain NativeFixtureDllMain
#include "native_policy_fixture.c"
#undef DllMain
#include "../../api/adapter.h"
#include "../../api/logger.h"
HANDLE ModuleHeap;
static volatile LONG LegacyRuntime,LegacyHeap,LegacyRegions,LegacyHandles,LegacyPaths,LegacyPins,LegacyWakes;
static HANDLE LegacyThread,LegacyWake;
static PB_INTERFACE_V2 Raw;
static PB_LIFECYCLE_INTERFACE RawLife;
static PB_DESCRIPTOR Incoming[64];
static PB_RESULT WINAPI FacadeInterface(uint32_t,uint32_t,PB_INTERFACE_V2 *);
static PB_RESULT WINAPI FacadeLifecycle(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
BOOL MainRuntimeAcquire(VOID){InterlockedIncrement(&LegacyRuntime);return TRUE;}
VOID MainRuntimeRelease(VOID){CHECK(InterlockedDecrement(&LegacyRuntime)>=0);}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args){(void)format;(void)args;SetLastError(error);return error;}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args){(void)level;(void)format;(void)args;return GetLastError();}
static VOID *LegacyAlloc(SIZE_T bytes)
{VOID *p=HeapAlloc(ModuleHeap,HEAP_ZERO_MEMORY,bytes);CHECK(p);InterlockedIncrement(&LegacyHeap);return p;}
static VOID LegacyFree(VOID *p)
{CHECK(p && HeapFree(ModuleHeap,0,p) && InterlockedDecrement(&LegacyHeap)>=0);}
static VOID *WINAPI LegacyMap(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{VOID *p=VirtualAlloc(address,size,type,protection);CHECK(p);InterlockedIncrement(&LegacyRegions);return p;}
static BOOL WINAPI LegacyUnmap(VOID *address,SIZE_T size,DWORD type)
{CHECK(VirtualFree(address,size,type) && InterlockedDecrement(&LegacyRegions)>=0);return TRUE;}
static WCHAR *LegacyPath(const WCHAR *path)
{WCHAR *p=_wcsdup(path);CHECK(p);InterlockedIncrement(&LegacyPaths);return p;}
static VOID LegacyPathFree(VOID *p){if(p){CHECK(InterlockedDecrement(&LegacyPaths)>=0);free(p);}}
static HANDLE WINAPI LegacyEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(security,manual,initial,name);CHECK(event);InterlockedIncrement(&LegacyHandles);return event;}
static HANDLE WINAPI LegacyStart(LPSECURITY_ATTRIBUTES security,SIZE_T stack,LPTHREAD_START_ROUTINE routine,VOID *opaque,DWORD flags,DWORD *id)
{HANDLE thread=CreateThread(security,stack,routine,opaque,flags,id);CHECK(thread && !LegacyThread);LegacyThread=thread;InterlockedIncrement(&LegacyHandles);return thread;}
static BOOL WINAPI LegacyClose(HANDLE handle)
{CHECK(CloseHandle(handle) && InterlockedDecrement(&LegacyHandles)>=0);return TRUE;}
static BOOL WINAPI LegacyPin(DWORD flags,LPCWSTR address,HMODULE *module)
{CHECK(GetModuleHandleExW(flags,address,module));InterlockedIncrement(&LegacyPins);return TRUE;}
static BOOL WINAPI LegacyUnpin(HMODULE module)
{CHECK(InterlockedDecrement(&LegacyPins)>=0);return FreeLibrary(module);}
static BOOL WINAPI LegacySignal(HANDLE event)
{if(event==LegacyWake)InterlockedIncrement(&LegacyWakes);return SetEvent(event);}
#undef Zalloc
#define Zalloc LegacyAlloc
#define Free LegacyFree
#define free LegacyPathFree
#define _wcsdup LegacyPath
#define VirtualAlloc LegacyMap
#define VirtualFree LegacyUnmap
#define CreateEventW LegacyEvent
#define CloseHandle LegacyClose
#define GetModuleHandleExW LegacyPin
#define FreeLibrary LegacyUnpin
#define CreateThread LegacyStart
#define SetEvent LegacySignal
#define PbGetInterfaceV2 FacadeInterface
#define PbGetLifecycleInterface FacadeLifecycle
#ifndef PB_FACADE_LEGACY_SOURCE
#error Explicit legacy session source required
#endif
/* The actual worker supplies default PB_COMPAT_OPEN=(s)->Api.Open. */
#include PB_FACADE_LEGACY_SOURCE
#undef Zalloc
#undef Free
#undef free
#undef _wcsdup
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CloseHandle
#undef GetModuleHandleExW
#undef FreeLibrary
#undef CreateThread
#undef SetEvent
#undef PbGetInterfaceV2
#undef PbGetLifecycleInterface
static TUN_SESSION *Facade;
static PB_RESULT WINAPI ObservedOpen(const WCHAR *path,const PB_CONFIG *config,PB_SESSION **out)
{PB_RESULT result=Raw.Open(path,config,out);CHECK(result==PB_OK && Facade);Configure(*out);return result;}
static PB_RESULT WINAPI ObservedCommit(PB_USER_QUEUE *queue,const PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    CHECK(FacadeConfig->Direction==FS_SEND && queue==Current->Queues && count==8 && LoansNow()==8);
    CHECK(SetEvent(FacadeConfig->Events[FS_PUMP_ENTERED]) && WaitForSingleObject(FacadeConfig->Events[FS_PUMP_GATE],10000)==WAIT_OBJECT_0);
    PB_RESULT result=Raw.CommitBatch(queue,leases,count,done);
    CHECK(result==PB_STOPPED && !*done && LoansNow()==8);InterlockedIncrement(&CommitStopped);return result;
}
static PB_RESULT WINAPI ObservedCancel(PB_USER_QUEUE *queue,const PB_TOKEN *tokens,uint32_t count)
{
    CHECK(count==8 && LoansNow()==8);PB_RESULT result=Raw.CancelBatch(queue,tokens,count);
    CHECK(result==PB_OK && !LoansNow());InterlockedAdd(&Cancelled,(LONG)count);return result;
}
static PB_RESULT WINAPI ObservedClose(PB_SESSION *session,uint32_t timeout)
{
    CHECK(session==Current && timeout==5000 && InterlockedIncrement(&CloseCalls)==1);
    if(LoansNow()) {
        /* Negative teardown only: first witness the missing production return,
         * then this SAME queue owner retires the scripted producer's known
         * slots1..63 before entering real Close or ReleaseSession. */
        CHECK(FacadeConfig->Direction==FS_RECEIVE && LoansNow()==63 && Current->Queues[0].Owner==(LONG)GetCurrentThreadId() && Current->CloseRequested);
        PB_TOKEN tokens[63];for(ULONG i=0;i<63;++i)tokens[i]=Incoming[i+1].Token;uint32_t accepted=0;
        CHECK(Raw.ReleaseBatch(Current->Queues,tokens,63,&accepted)==PB_OK && accepted==63 && !LoansNow());InterlockedAdd(&RepairLoans,63);
    }
    CHECK(!LoansNow() && SetEvent(FacadeConfig->Events[FS_CLOSE_ENTERED]));
    PB_RESULT result=Raw.Close(session,timeout);InterlockedExchange(&CloseResult,(LONG)result);return result;
}
static PB_RESULT WINAPI ObservedBeginStop(PB_SESSION *session)
{
    PB_RESULT result=RawLife.BeginStop(session);CHECK(result==PB_OK);
    if(FacadeConfig->Direction==FS_RECEIVE) {
        /* Callback submission precedes production CompatReturnPending. Force
         * its legal early LEASES park before allowing the owner to retire. */
        CHECK(SetEvent(FacadeConfig->Events[FS_STOP_ENTERED]));
        CHECK(WaitForSingleObject(FacadeConfig->Events[FS_STOP_GATE],10000)==WAIT_OBJECT_0);
    }
    return result;
}
static PB_RESULT WINAPI FacadeInterface(uint32_t major,uint32_t size,PB_INTERFACE_V2 *table)
{
    PB_RESULT result=PbGetInterfaceV2(major,size,table);CHECK(result==PB_OK && !Facade);Raw=*table;
    Facade=(TUN_SESSION *)((BYTE *)table - offsetof(TUN_SESSION,Api));LegacyWake=Facade->Wake;
    table->Open=ObservedOpen;table->CommitBatch=ObservedCommit;table->CancelBatch=ObservedCancel;table->Close=ObservedClose;return result;
}
static PB_RESULT WINAPI FacadeLifecycle(uint32_t major,uint32_t size,PB_LIFECYCLE_INTERFACE *table)
{
    PB_RESULT result=PbGetLifecycleInterface(major,size,table);CHECK(result==PB_OK);RawLife=*table;
    table->BeginStop=ObservedBeginStop;return result;
}
static BOOL WINAPI FacadeIo(HANDLE handle,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlap)
{
    if(code==PB_V2_IOCTL_QUERY && ReadAcquire(&QueryFails)){SetLastError(ERROR_DEVICE_NOT_CONNECTED);return FALSE;}
    return HostedIo(handle,code,input,inputSize,output,outputSize,returned,overlap);
}
static DWORD WINAPI FacadeWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    if(count==3 && InterlockedCompareExchange(&WaitArmed,0,1)==1) {
        CHECK(!all && timeout==INFINITE && SetEvent(FacadeConfig->Events[FS_WAIT_ENTERED]));
        CHECK(WaitForSingleObject(FacadeConfig->Events[FS_WAIT_GATE],10000)==WAIT_OBJECT_0);
    }
    if(count==3 && InterlockedIncrement(&WaitCalls)==2 && FacadeConfig->Direction==FS_RECEIVE) {
        CHECK(LoansNow()==63 && SetEvent(FacadeConfig->Events[FS_PUMP_ENTERED]));
        CHECK(WaitForSingleObject(FacadeConfig->Events[FS_PUMP_GATE],10000)==WAIT_OBJECT_0);
    }
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);
    if(count==3)CHECK(result==WAIT_OBJECT_0+(FacadeConfig->Direction==FS_SEND?2:1));return result;
}
__declspec(dllexport) VOID WINAPI PrepareFacade(FS_CONFIG *config)
{CHECK(config && !FacadeConfig);FacadeConfig=config;ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);}
__declspec(dllexport) VOID *WINAPI StartFacade(VOID)
{
    WINTUN_ADAPTER adapter={0};adapter.InterfaceFilename=L"hosted-actual-facade";
    VOID *session=WintunStartSession(&adapter,WINTUN_MIN_RING_CAPACITY);CHECK(session && session==Facade);return session;
}
__declspec(dllexport) VOID WINAPI ProduceFacade(VOID *session)
{
    BYTE *packets[8];for(ULONG i=0;i<8;++i){packets[i]=WintunAllocateSendPacket(session,32);CHECK(packets[i]);memset(packets[i],(int)i,32);}
    for(ULONG i=8;i--;)WintunSendPacket(session,packets[i]);
}
__declspec(dllexport) VOID WINAPI PublishFacadeIncoming(VOID)
{
    CHECK(Current && !WaitCalls && Current->Layout.SlotCount==64 && Current->Layout.RingCapacity==128);
    /* A single hosted native publication, no slot reuse or fake FULL replies. */
    for(ULONG i=0;i<64;++i){Incoming[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,0};Incoming[i].Length=65535;
        Incoming[i].DataOffset=Current->Layout.Queues[0].TxData+i*PB_BLOCK_SIZE;memset(Current->Region+Incoming[i].DataOffset,(int)i,65535);}
    PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,0,PB_QUEUE_TX_READY,TRUE)==PB_OK);
    uint32_t count,notify;CHECK(PbQueuePush(&producer,Incoming,64,&count,&notify)==PB_OK && count==64 && notify);CHECK(SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) BYTE *WINAPI ReceiveFacadeCopy(VOID *session)
{DWORD size;BYTE *packet=WintunReceivePacket(session,&size);CHECK(packet && size==65535 && !packet[0] && !packet[65534]);return packet;}
__declspec(dllexport) VOID WINAPI ReturnFacadeCopy(VOID *session,BYTE *packet)
{
    CHECK(session==Facade && WaitForSingleObject(LegacyThread,0)==WAIT_OBJECT_0);
    WintunReleaseReceivePacket(session,packet);CHECK(!Facade->Send.PacketsToRelease && Facade->Descriptor.Send.Ring->Head==Facade->Send.Head && ++PublicReturns==1);
}
__declspec(dllexport) VOID WINAPI StopFacadeNative(VOID)
{
    CHECK(Current && LoansNow()==(FacadeConfig->Direction==FS_SEND?8:63));
    InterlockedExchange((LONG *)&Current->SharedLifecycle->Terminal,1);InterlockedExchange(&QueryFails,FacadeConfig->QueryFailure);
    if(FacadeConfig->Direction==FS_RECEIVE)CHECK(SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) VOID WINAPI WaitFacadeExit(VOID)
{CHECK(Facade && WaitForSingleObject(LegacyThread,10000)==WAIT_OBJECT_0 && Current && !LoansNow() && Current->PublicReleased);}
__declspec(dllexport) VOID WINAPI EndFacade(VOID *session)
{
    CHECK(session==Facade);WintunEndSession(session);Facade=NULL;
    CHECK(!LegacyRuntime && !LegacyHeap && !LegacyRegions && !LegacyHandles && !LegacyPaths && !LegacyPins && !CompatRingBytes && LegacyWakes==2);
    CHECK(FacadeConfig->Direction==FS_SEND?(CommitStopped==1 && Cancelled==8 && !PublicReturns && !RepairLoans):(!CommitStopped && !Cancelled && PublicReturns==1));
    CHECK(HeapDestroy(ModuleHeap));ModuleHeap=NULL;
}
__declspec(dllexport) VOID WINAPI RecoverFacadeNative(VOID)
{InterlockedExchange(&QueryFails,0);SetNativeActive(0);}
__declspec(dllexport) VOID WINAPI ReadFacadeStop(FS_SNAPSHOT *out)
{
    CHECK(out);memset(out,0,sizeof(*out));PB_SESSION *session=Current;
    if(session){CHECK(ParkHeld || FinalHeld || (Facade && !session->PublicReleased));out->Core=1;out->Loans=LoansNow();out->Published=session->Queues[0].Published;
        out->NativeClosed=session->ResourcesClosed;out->PublicReleased=session->PublicReleased;out->Registered=session->Registered;}
    out->NativeActive=NativeActive;out->Runtime=LegacyRuntime;out->Heap=LegacyHeap;out->Regions=LegacyRegions;out->Handles=LegacyHandles;
    out->Paths=LegacyPaths;out->Pins=LegacyPins;out->WakeSignals=LegacyWakes;out->WaitCalls=WaitCalls;out->CommitStopped=CommitStopped;
    out->Cancelled=Cancelled;out->RepairLoans=RepairLoans;out->PublicReturns=PublicReturns;out->LegacyBytes=(uint64_t)CompatRingBytes;
    out->NativeAllocations=Allocations;out->NativeRegions=Regions;out->NativeEvents=Events;out->NativeWorks=Works;out->NativePools=Pools;
    out->NativePins=Pins;out->Callbacks=CallbackCount;out->Finished=Finished;out->Active=ActiveCallbacks;out->DeviceCloses=DeviceCloses;
    out->CloseCalls=CloseCalls;out->CloseResult=CloseResult;
    AcquireSRWLockShared(&RuntimeLock);out->Sessions=RuntimeCount;out->Bytes=RuntimeBytes;ReleaseSRWLockShared(&RuntimeLock);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,VOID *reserved)
{
    BOOL result=NativeFixtureDllMain(module,reason,reserved);
    if(reason==DLL_PROCESS_DETACH && !reserved)CHECK(!Facade && !ModuleHeap && !LegacyRuntime && !LegacyHeap && !LegacyRegions && !LegacyHandles && !LegacyPaths && !LegacyPins);
    return result;
}
