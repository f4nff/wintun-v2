/* SPDX-License-Identifier: GPL-2.0 */
/* Production lifecycle in a real, unloadable DLL. Only native device I/O is
 * hosted; all references, SRW locks, work objects and module pins are real. */
#define PB_API_TESTS 1
#include "guard_memory.h"
#include "../model/callback_handoff_fixture.h"
#include "../../api/v2/adapter.h"
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
static PB_SESSION *Current;
static CH_CONFIG *Host;
static volatile LONG Allocations,Regions,Events,Works,Pools,Pins;
static volatile LONG Active,Started,Finished,Peak,CallbackThread,IdleArmed,IdleHeld,FinalHeld,TailTransfers,FailWait;
static LONG Repairs;
static VOID WINAPI ObservedUnlock(PSRWLOCK);
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
static VOID CALLBACK Dispatch(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
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
    if(!p)return;
    if(p==Current)Current=NULL;
    CHECK(InterlockedDecrement(&Allocations)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID *WINAPI TrackedRegion(VOID *address,SIZE_T bytes,DWORD type,DWORD protection)
{
    CHECK(!address && type==(MEM_COMMIT|MEM_RESERVE) && protection==PAGE_READWRITE);
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(bytes);memset(p,0,bytes);
#else
    VOID *p=VirtualAlloc(NULL,bytes,type,protection);
#endif
    CHECK(p);InterlockedIncrement(&Regions);return p;
}
static BOOL WINAPI TrackedUnmap(VOID *p,SIZE_T bytes,DWORD type)
{
    CHECK(!bytes && type==MEM_RELEASE && InterlockedDecrement(&Regions)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);return TRUE;
#else
    return VirtualFree(p,0,MEM_RELEASE);
#endif
}
static HANDLE WINAPI TrackedEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(security,manual,initial,name);CHECK(event);InterlockedIncrement(&Events);return event;}
static BOOL WINAPI TrackedClose(HANDLE event)
{CHECK(InterlockedDecrement(&Events)>=0);CHECK(CloseHandle(event));return TRUE;}
static PTP_POOL WINAPI TrackedPool(PVOID reserved)
{PTP_POOL pool=CreateThreadpool(reserved);CHECK(pool);InterlockedIncrement(&Pools);return pool;}
static VOID WINAPI TrackedPoolClose(PTP_POOL pool)
{CHECK(InterlockedDecrement(&Pools)>=0);CloseThreadpool(pool);}
static PTP_WORK WINAPI TrackedWork(PTP_WORK_CALLBACK callback,PVOID context,PTP_CALLBACK_ENVIRON environment)
{
    CHECK(callback && context && environment);PTP_WORK work=CreateThreadpoolWork(Dispatch,context,environment);
    CHECK(work);InterlockedIncrement(&Works);return work;
}
static VOID WINAPI TrackedWorkClose(PTP_WORK work)
{CHECK(InterlockedDecrement(&Works)>=0);CloseThreadpoolWork(work);}
static BOOL WINAPI TrackedPin(DWORD flags,LPCWSTR address,HMODULE *module)
{CHECK(GetModuleHandleExW(flags,address,module));InterlockedIncrement(&Pins);return TRUE;}
static BOOL WINAPI TrackedUnpin(HMODULE module)
{CHECK(InterlockedDecrement(&Pins)>=0);return FreeLibrary(module);}
static VOID WINAPI TrackedTail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{
    CHECK(Host && Current && InterlockedIncrement(&TailTransfers)==1);
    CHECK(InterlockedDecrement(&Pins)==1);
    FreeLibraryWhenCallbackReturns(instance,module);
    InterlockedExchange(&FinalHeld,1);CHECK(SetEvent(Host->Events[CH_FINAL_ENTERED]));
    CHECK(WaitForSingleObject(Host->Events[CH_FINAL_GATE],10000)==WAIT_OBJECT_0);
    InterlockedExchange(&FinalHeld,0);
}
static BOOL WINAPI HostedIo(HANDLE device,DWORD code,VOID *input,DWORD inSize,VOID *output,DWORD outSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    (void)input;(void)inSize;(void)output;(void)outSize;
    CHECK(device==INVALID_HANDLE_VALUE && !overlapped);
    CHECK(code==PB_PROBE_IOCTL_POLICY || code==PB_V2_IOCTL_DRAIN || code==PB_PROBE_IOCTL_QUERY);
    *returned=0;return TRUE;
}
#define calloc TrackedCalloc
#define free TrackedFree
#define VirtualAlloc TrackedRegion
#define VirtualFree TrackedUnmap
#define CreateEventW TrackedEvent
#define CloseHandle TrackedClose
#define CreateThreadpool TrackedPool
#define CloseThreadpool TrackedPoolClose
#define CreateThreadpoolWork TrackedWork
#define CloseThreadpoolWork TrackedWorkClose
#define GetModuleHandleExW TrackedPin
#define FreeLibrary TrackedUnpin
#define FreeLibraryWhenCallbackReturns TrackedTail
#define DeviceIoControl HostedIo
#define ReleaseSRWLockExclusive ObservedUnlock
#define SleepConditionVariableSRW ObservedSleep
#ifndef PB_CALLBACK_SESSION_SOURCE
#define PB_CALLBACK_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_CALLBACK_SESSION_SOURCE
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"fixture requires the shipping source fingerprint layout");
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CloseHandle
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef DeviceIoControl
#undef ReleaseSRWLockExclusive
#undef SleepConditionVariableSRW
static VOID WINAPI ObservedUnlock(PSRWLOCK lock)
{
    /* Test the predicate while State is still held, then block only after the
     * real unlock. New public requests can acquire State and queue real work. */
    BOOL gate=Current && GetCurrentThreadId()==(DWORD)ReadAcquire(&CallbackThread) && lock==&Current->State &&
        !Current->Scheduled && Current->Progress.Phase==PB_LIFE_RUNNING && Current->Progress.PolicyCompleted==1 &&
        InterlockedCompareExchange(&IdleArmed,0,1)==1;
    ReleaseSRWLockExclusive(lock);
    if(gate) {
        InterlockedExchange(&IdleHeld,1);CHECK(SetEvent(Host->Events[CH_IDLE_ENTERED]));
        CHECK(WaitForSingleObject(Host->Events[CH_IDLE_GATE],10000)==WAIT_OBJECT_0);
        InterlockedExchange(&IdleHeld,0);
    }
}
static BOOL WINAPI ObservedSleep(PCONDITION_VARIABLE changed,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    if(InterlockedExchange(&FailWait,0)) {
        CHECK(Current && changed==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);
        SetLastError(ERROR_OPERATION_ABORTED);return FALSE;
    }
    return SleepConditionVariableSRW(changed,lock,timeout,flags);
}
static VOID CALLBACK Dispatch(PTP_CALLBACK_INSTANCE instance,PVOID opaque,PTP_WORK work)
{
    CHECK(Host && opaque==Current);
    LONG active=InterlockedIncrement(&Active),ordinal=InterlockedIncrement(&Started);
    if(active>Peak)InterlockedExchange(&Peak,active);
    CHECK(active==1 && ordinal>0 && (ULONG)ordinal<=CH_MAX_CALLBACKS);
    Host->ThreadIds[ordinal-1]=GetCurrentThreadId();
    Host->Threads[ordinal-1]=OpenThread(SYNCHRONIZE,FALSE,GetCurrentThreadId());CHECK(Host->Threads[ordinal-1]);
    InterlockedExchange(&CallbackThread,(LONG)GetCurrentThreadId());
    LifecycleWorker(instance,opaque,work);
    /* The final worker may already have destroyed its core. Touch globals
     * only; the real deferred tail pin protects these instructions. */
    InterlockedIncrement(&Finished);CHECK(InterlockedDecrement(&Active)==0);
}
__declspec(dllexport) VOID WINAPI Configure(PB_SESSION *s,CH_CONFIG *config)
{CHECK(!Current && s && config && !Started);Current=s;Host=config;InterlockedExchange(&IdleArmed,1);}
__declspec(dllexport) VOID WINAPI InjectWaitFailure(VOID)
{CHECK(IdleHeld);InterlockedExchange(&FailWait,1);}
__declspec(dllexport) VOID WINAPI RepairMissingWorkPin(VOID)
{
#ifdef PB_CALLBACK_NEGATIVE
    /* Negative-only safe teardown: the observer has already witnessed the
     * missing work owner. Restore exactly that deleted pin, then let actual
     * shutdown free everything. This is never enabled in positive fixtures. */
    CHECK(Current && IdleHeld && Started==1 && !Finished && !Repairs && Current->References==2);
    Retain(Current);++Repairs;
#else
    CHECK(FALSE);
#endif
}
__declspec(dllexport) VOID WINAPI Observe(CH_SNAPSHOT *out)
{
    CHECK(out && Current && Host && Active==1);
    /* Each observation is made while a real callback is fixed at a host gate.
     * Its work/registry pins, rather than an extra observer Retain, own core. */
    CHECK(IdleHeld || FinalHeld ||
        (WaitForSingleObject(Host->Events[CH_POLICY_ENTERED],0)==WAIT_OBJECT_0 &&
         WaitForSingleObject(Host->Events[CH_POLICY_GATE],0)==WAIT_TIMEOUT));
    PB_SESSION *s=Current;AcquireSRWLockShared(&s->State);
    *out=(CH_SNAPSHOT){.References=ReadAcquire(&s->References),.Active=Active,.Started=Started,.Finished=Finished,.Peak=Peak,
        .Allocations=Allocations,.Regions=Regions,.Events=Events,.Works=Works,.Pools=Pools,.Pins=Pins,.TailTransfers=TailTransfers,
        .Sessions=RuntimeCount,.Phase=s->Progress.Phase,.Bytes=RuntimeBytes,.Requested=s->Progress.PolicyRequested,.Completed=s->Progress.PolicyCompleted,
        .Registered=s->Registered,.Scheduled=s->Scheduled,.Occupied=s->PolicyOccupied,.Queued=s->PolicyQueued,.Abandoned=s->PolicyAbandoned,
        .Closed=s->ResourcesClosed,.ModulePresent=s->Module!=NULL,.TailPresent=s->TailModule!=NULL,.IdleHeld=IdleHeld,.FinalHeld=FinalHeld};
    ReleaseSRWLockShared(&s->State);
}
PB_RESULT WINAPI PbAdapterCreate(const WCHAR *name,const WCHAR *instance,PB_ADAPTER **out)
{(void)name;(void)instance;*out=NULL;return PB_UNSUPPORTED;}
VOID WINAPI PbAdapterRelease(PB_ADAPTER *adapter){CHECK(!adapter);}
VOID PbAdapterRetain(PB_ADAPTER *adapter){CHECK(!adapter);}
const WCHAR *PbAdapterPath(PB_ADAPTER *adapter){CHECK(!adapter);return NULL;}
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter,uint64_t *luid){(void)adapter;(void)luid;return PB_UNSUPPORTED;}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,PVOID reserved)
{
    (void)module;
    if(reason==DLL_PROCESS_DETACH && !reserved) {
        CHECK(Host && !Current && !Active && Started>=2 && Finished==Started && Peak==1 && TailTransfers==1);
        CHECK(!Allocations && !Regions && !Events && !Works && !Pools && !Pins && !RuntimeCount && !RuntimeBytes && !RuntimeAdapterCount);
#ifdef PB_CALLBACK_NEGATIVE
        CHECK(Repairs==1);
#else
        CHECK(!Repairs);
#endif
        CHECK(SetEvent(Host->Events[CH_DETACHED]));
    }
    return TRUE;
}
