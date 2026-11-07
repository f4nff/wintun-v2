/* SPDX-License-Identifier: GPL-2.0 */
/* Actual non-test-shim DLL lifecycle/control/status. Device I/O is the only
 * driver boundary; pools, handles, module pins, queues and ownership are real. */
#ifdef PB_API_TESTS
#error Native policy tests must not compile the PB_API_TESTS shortcut
#endif
#include "guard_memory.h"
#include "../model/native_policy_fixture.h"
#include "../../api/v2/adapter.h"
#include "../../common/platform.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
static NP_CONFIG *Host;
static PB_SESSION *Current;
static HANDLE Device,Progress;
static uint64_t NativeEpoch;
static uint32_t NativeQueues;
static volatile LONG Allocations,Regions,Events,Works,Pools,Pins;
static volatile LONG CallbackCount,Finished,ActiveCallbacks,Peak,CallbackThread;
static volatile LONG ControlArmed,IoArmed,ControlHeld,IoHeld,ParkHeld,FinalHeld,TailTransfers;
static volatile LONG PolicyCalls,AfterStopCalls,Drains,Queries,DeviceCloses,NativeActive,NativeRegistered,NativeDrained;
static DWORD LastPolicyMask,LastPolicyError,AllowedMask;
static VOID WINAPI ObservedAcquire(PSRWLOCK);
static DWORD WINAPI ObservedWait(HANDLE,DWORD);
static VOID CALLBACK Dispatch(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
static VOID CALLBACK LifecycleWorker(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
static BOOL WINAPI HostedIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static VOID *TrackedCalloc(SIZE_T count,SIZE_T size)
{
    CHECK(!size || count<=SIZE_MAX/size);
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(count*size);memset(p,0,count*size);
#else
    VOID *p=calloc(count,size);
#endif
    CHECK(p);InterlockedIncrement(&Allocations);return p;
}
static VOID TrackedFree(VOID *p)
{
    if(!p)return;if(p==Current)Current=NULL;CHECK(InterlockedDecrement(&Allocations)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID *WINAPI TrackedRegion(VOID *address,SIZE_T size,DWORD type,DWORD protection)
{
    CHECK(!address && type==(MEM_COMMIT|MEM_RESERVE) && protection==PAGE_READWRITE);
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(size);memset(p,0,size);
#else
    VOID *p=VirtualAlloc(NULL,size,type,protection);
#endif
    CHECK(p);InterlockedIncrement(&Regions);return p;
}
static BOOL WINAPI TrackedUnmap(VOID *p,SIZE_T size,DWORD type)
{
    CHECK(!size && type==MEM_RELEASE && InterlockedDecrement(&Regions)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);return TRUE;
#else
    return VirtualFree(p,0,MEM_RELEASE);
#endif
}
static HANDLE WINAPI TrackedEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(security,manual,initial,name);CHECK(event);InterlockedIncrement(&Events);return event;}
static HANDLE WINAPI HostedFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template)
{
    CHECK(Host && !Device && path && access==(GENERIC_READ|GENERIC_WRITE) && share==(FILE_SHARE_READ|FILE_SHARE_WRITE));
    CHECK(!security && creation==OPEN_EXISTING && !flags && !template);
    Device=TrackedEvent(NULL,FALSE,FALSE,NULL);return Device;
}
static BOOL WINAPI TrackedClose(HANDLE handle);
static PTP_POOL WINAPI TrackedPool(VOID *reserved)
{PTP_POOL pool=CreateThreadpool(reserved);CHECK(pool);InterlockedIncrement(&Pools);return pool;}
static VOID WINAPI TrackedPoolClose(PTP_POOL pool){CHECK(InterlockedDecrement(&Pools)>=0);CloseThreadpool(pool);}
static PTP_WORK WINAPI TrackedWork(PTP_WORK_CALLBACK callback,VOID *opaque,PTP_CALLBACK_ENVIRON environment)
{CHECK(callback==LifecycleWorker && opaque && environment);PTP_WORK work=CreateThreadpoolWork(Dispatch,opaque,environment);CHECK(work);InterlockedIncrement(&Works);return work;}
static VOID WINAPI TrackedWorkClose(PTP_WORK work){CHECK(InterlockedDecrement(&Works)>=0);CloseThreadpoolWork(work);}
static BOOL WINAPI TrackedPin(DWORD flags,LPCWSTR address,HMODULE *module)
{CHECK(GetModuleHandleExW(flags,address,module));InterlockedIncrement(&Pins);return TRUE;}
static BOOL WINAPI TrackedUnpin(HMODULE module){CHECK(InterlockedDecrement(&Pins)>=0);return FreeLibrary(module);}
static VOID WINAPI TrackedTail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{
    CHECK(Host && Current && InterlockedIncrement(&TailTransfers)==1 && InterlockedDecrement(&Pins)==1);
    FreeLibraryWhenCallbackReturns(instance,module);InterlockedExchange(&FinalHeld,1);
    CHECK(SetEvent(Host->Events[NP_FINAL_ENTERED]) && WaitForSingleObject(Host->Events[NP_FINAL_GATE],10000)==WAIT_OBJECT_0);
    InterlockedExchange(&FinalHeld,0);
}
#define calloc TrackedCalloc
#define free TrackedFree
#define VirtualAlloc TrackedRegion
#define VirtualFree TrackedUnmap
#define CreateEventW TrackedEvent
#define CreateFileW HostedFile
#define CloseHandle TrackedClose
#define CreateThreadpool TrackedPool
#define CloseThreadpool TrackedPoolClose
#define CreateThreadpoolWork TrackedWork
#define CloseThreadpoolWork TrackedWorkClose
#define GetModuleHandleExW TrackedPin
#define FreeLibrary TrackedUnpin
#define FreeLibraryWhenCallbackReturns TrackedTail
#define DeviceIoControl HostedIo
#define AcquireSRWLockExclusive ObservedAcquire
#define WaitForSingleObject ObservedWait
#ifndef PB_NATIVE_POLICY_SESSION_SOURCE
#define PB_NATIVE_POLICY_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_NATIVE_POLICY_SESSION_SOURCE
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"native policy fixture requires the shipping fingerprint layout");
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CreateFileW
#undef CloseHandle
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef DeviceIoControl
#undef AcquireSRWLockExclusive
#undef WaitForSingleObject
static LONG LoansNow(VOID)
{LONG n=0;for(uint32_t i=0;i<Current->Layout.QueueCount;++i)n+=ReadAcquire(&Current->Queues[i].Loans);return n;}
static BOOL WINAPI TrackedClose(HANDLE handle)
{
    if(handle==Device) {
        CHECK(Current && NativeRegistered && NativeDrained && !NativeActive && !LoansNow());
        InterlockedIncrement(&DeviceCloses);NativeRegistered=0;Device=NULL;
    }
    CHECK(InterlockedDecrement(&Events)>=0 && CloseHandle(handle));return TRUE;
}
static VOID WINAPI ObservedAcquire(PSRWLOCK lock)
{
    /* Lifecycle already sampled stop=false and calls real PolicyIo. Block
     * before Control acquisition without owning State or Control; Lifetime
     * remains pinned exactly as production requires. */
    if(Current && GetCurrentThreadId()==(DWORD)ReadAcquire(&CallbackThread) && lock==&Current->Control &&
        InterlockedCompareExchange(&ControlArmed,0,1)==1) {
        InterlockedExchange(&ControlHeld,1);CHECK(SetEvent(Host->Events[NP_CONTROL_ENTERED]));
        CHECK(WaitForSingleObject(Host->Events[NP_CONTROL_GATE],10000)==WAIT_OBJECT_0);InterlockedExchange(&ControlHeld,0);
    }
    AcquireSRWLockExclusive(lock);
}
static DWORD WINAPI ObservedWait(HANDLE event,DWORD timeout)
{
    if(Host && event==Progress && timeout==1000) {
        InterlockedExchange(&ParkHeld,1);CHECK(SetEvent(Host->Events[NP_PARK_ENTERED]));
        CHECK(WaitForSingleObject(Host->Events[NP_PARK_GATE],10000)==WAIT_OBJECT_0);InterlockedExchange(&ParkHeld,0);
    }
    return WaitForSingleObject(event,timeout);
}
static VOID CALLBACK Dispatch(PTP_CALLBACK_INSTANCE instance,VOID *opaque,PTP_WORK work)
{
    CHECK(Host && opaque==Current);LONG active=InterlockedIncrement(&ActiveCallbacks),ordinal=InterlockedIncrement(&CallbackCount);
    if(active>Peak)InterlockedExchange(&Peak,active);
    CHECK(active==1 && ordinal>0 && (ULONG)ordinal<=NP_MAX_CALLBACKS);
    Host->ThreadIds[ordinal-1]=GetCurrentThreadId();Host->Threads[ordinal-1]=OpenThread(SYNCHRONIZE,FALSE,GetCurrentThreadId());CHECK(Host->Threads[ordinal-1]);
    InterlockedExchange(&CallbackThread,(LONG)GetCurrentThreadId());LifecycleWorker(instance,opaque,work);
    /* Core may already be gone; real deferred module release covers globals. */
    InterlockedIncrement(&Finished);CHECK(InterlockedDecrement(&ActiveCallbacks)==0);
}
static BOOL WINAPI HostedIo(HANDLE device,DWORD code,VOID *input,DWORD inSize,VOID *output,DWORD outSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    CHECK(Host && Device && device==Device && !overlapped && returned);*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY) {
        CHECK(!input && !inSize && output && outSize==sizeof(PB_PROBE_STATUS));PB_PROBE_STATUS *p=output;memset(p,0,sizeof(*p));
        p->Size=sizeof(*p);p->Magic=PB_PROBE_MAGIC;p->Schema=PB_PROBE_SCHEMA;p->FileVersion=PB_VERSION_PACKED;p->DataAbi=PB_ABI_MAJOR;
        p->NdisVersion=Host->NdisVersion;p->ImplementedMask=PbPlatformOffloads(Host->NdisVersion);p->AllowedMask=AllowedMask;
        p->NdisMask=p->ImplementedMask;p->EffectiveMask=AllowedMask;p->SessionRegistered=(uint32_t)NativeRegistered;
        memcpy(p->SourceFingerprint,PB_SOURCE_STRING,sizeof(PB_SOURCE_STRING));*returned=sizeof(*p);return TRUE;
    }
    if(code==PB_V2_IOCTL_REGISTER) {
        CHECK(input && inSize==sizeof(PB_REGISTER_V2) && !output && !outSize && !NativeRegistered);
        PB_REGISTER_V2 *r=input;CHECK(r->Size==sizeof(*r) && r->Major==PB_ABI_MAJOR && r->Minor==PB_ABI_MINOR && r->Epoch &&
            r->QueueCount && r->QueueCount<=PB_MAX_QUEUES && r->RegionAddress && r->RegionSize && r->ProgressEvent &&
            !memcmp(r->SourceFingerprint,PB_SOURCE_STRING,sizeof(PB_SOURCE_STRING)));
        NativeEpoch=r->Epoch;NativeQueues=r->QueueCount;Progress=(HANDLE)(ULONG_PTR)r->ProgressEvent;NativeRegistered=1;return TRUE;
    }
    CHECK(Current && NativeRegistered);
    if(code==PB_PROBE_IOCTL_POLICY) {
        CHECK(input && inSize==sizeof(PB_PROBE_POLICY) && !output && !outSize);const PB_PROBE_POLICY *p=input;
        CHECK(p->Size==sizeof(*p) && p->Schema==PB_PROBE_SCHEMA && !p->Reserved && !(p->AllowedMask&~PB_PROBE_SUPPORTED));
        InterlockedIncrement(&PolicyCalls);LastPolicyMask=p->AllowedMask;
        if(p->AllowedMask && ReadAcquire(&Current->CloseRequested))InterlockedIncrement(&AfterStopCalls);
        if(p->AllowedMask && InterlockedCompareExchange(&IoArmed,0,1)==1) {
            InterlockedExchange(&IoHeld,1);CHECK(SetEvent(Host->Events[NP_IO_ENTERED]));
            CHECK(WaitForSingleObject(Host->Events[NP_IO_GATE],10000)==WAIT_OBJECT_0);InterlockedExchange(&IoHeld,0);
        }
        DWORD error=p->AllowedMask&~PbPlatformOffloads(Host->NdisVersion)?ERROR_NOT_SUPPORTED:
            p->AllowedMask && Host->Mode==NP_LATE_FAILURE?Host->PolicyError:ERROR_SUCCESS;
        LastPolicyError=error;if(error){SetLastError(error);return FALSE;}
        AllowedMask=p->AllowedMask;SetLastError(ERROR_SUCCESS);return TRUE;
    }
    if(code==PB_V2_IOCTL_DRAIN) {
        CHECK(!input && !inSize && !output && !outSize);InterlockedIncrement(&Drains);NativeDrained=1;AllowedMask=0;return TRUE;
    }
    CHECK(code==PB_V2_IOCTL_QUERY && !input && !inSize && output && outSize==sizeof(PB_V2_STATUS));InterlockedIncrement(&Queries);
    PB_V2_STATUS *p=output;memset(p,0,sizeof(*p));p->Size=sizeof(*p);p->Epoch=NativeEpoch;p->QueueCount=NativeQueues;p->ActiveReferences=(uint32_t)ReadAcquire(&NativeActive);
    p->State=NativeDrained?(p->ActiveReferences?PB_SESSION_DRAINING:PB_SESSION_STOPPED):PB_SESSION_RUNNING;*returned=sizeof(*p);return TRUE;
}
__declspec(dllexport) VOID WINAPI Initialize(NP_CONFIG *config){CHECK(!Host && config);Host=config;NativeActive=1;}
__declspec(dllexport) VOID WINAPI Configure(PB_SESSION *s)
{CHECK(Host && !Current && s && s->Device==Device);Current=s;ControlArmed=Host->Mode==NP_BEFORE_CONTROL;IoArmed=Host->Mode!=NP_BEFORE_CONTROL;}
__declspec(dllexport) VOID WINAPI SetNativeActive(LONG active)
{CHECK(Current && ParkHeld && (active==0 || active==1));InterlockedExchange(&NativeActive,active);CHECK(SetEvent(Progress));}
__declspec(dllexport) VOID WINAPI Observe(NP_SNAPSHOT *out)
{
    CHECK(out && Current && (ControlHeld || IoHeld || ParkHeld || FinalHeld || !Current->PublicReleased));
    PB_SESSION *s=Current;AcquireSRWLockShared(&s->State);
    *out=(NP_SNAPSHOT){.References=ReadAcquire(&s->References),.Callbacks=CallbackCount,.Finished=Finished,.ActiveCallbacks=ActiveCallbacks,.Peak=Peak,
        .Allocations=Allocations,.Regions=Regions,.Events=Events,.Works=Works,.Pools=Pools,.Pins=Pins,.TailTransfers=TailTransfers,
        .PolicyCalls=PolicyCalls,.AfterStopCalls=AfterStopCalls,.Drains=Drains,.Queries=Queries,.DeviceCloses=DeviceCloses,.NativeActive=NativeActive,.Loans=LoansNow(),
        .LastPolicyMask=LastPolicyMask,.LastPolicyError=LastPolicyError,.AllowedMask=AllowedMask,.Phase=s->Progress.Phase,.BlockStage=s->Progress.BlockStage,
        .Requested=s->Progress.PolicyRequested,.Completed=s->Progress.PolicyCompleted,.Occupied=s->PolicyOccupied,.Queued=s->PolicyQueued,.Abandoned=s->PolicyAbandoned,
        .Registered=s->Registered,.ResourcesClosed=s->ResourcesClosed,.PublicReleased=s->PublicReleased,.CloseRequested=s->CloseRequested};
    ReleaseSRWLockShared(&s->State);AcquireSRWLockShared(&RuntimeLock);out->Sessions=RuntimeCount;out->Bytes=RuntimeBytes;ReleaseSRWLockShared(&RuntimeLock);
}
PB_RESULT WINAPI PbAdapterCreate(const WCHAR *name,const WCHAR *instance,PB_ADAPTER **out){(void)name;(void)instance;*out=NULL;return PB_UNSUPPORTED;}
VOID WINAPI PbAdapterRelease(PB_ADAPTER *adapter){CHECK(!adapter);}
VOID PbAdapterRetain(PB_ADAPTER *adapter){CHECK(!adapter);}
const WCHAR *PbAdapterPath(PB_ADAPTER *adapter){CHECK(!adapter);return NULL;}
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter,uint64_t *luid){(void)adapter;(void)luid;return PB_UNSUPPORTED;}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,VOID *reserved)
{
    (void)module;if(reason==DLL_PROCESS_DETACH && !reserved) {
        CHECK(Host && !Current && !Device && !NativeRegistered && !NativeActive && !AllowedMask && DeviceCloses==1);
        CHECK(!Allocations && !Regions && !Events && !Works && !Pools && !Pins && !RuntimeCount && !RuntimeBytes && !RuntimeAdapterCount);
        CHECK(!ActiveCallbacks && CallbackCount && Finished==CallbackCount && Peak==1 && TailTransfers==1);CHECK(SetEvent(Host->Events[NP_DETACHED]));
    }
    return TRUE;
}
