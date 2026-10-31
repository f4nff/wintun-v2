/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Open/adapter references/lifecycle, real protected OS handles and work.
 * Device IO and SwDeviceClose are hosted; no device is installed or loaded. */
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_version.h"
#include <swdevice.h>
#include <stdlib.h>
#include <string.h>
#define CHECK SAFETY_CHECK
enum { FAIL_NONE,FAIL_PROBE,FAIL_REGISTER,FAIL_POLICY,FAIL_FILE,FAIL_ALLOC };
static ULONG Failure;
enum { QUERY_ACK_OK,QUERY_ACK_SHORT,QUERY_ACK_LONG,QUERY_ACK_SIZE,QUERY_ACK_EPOCH,QUERY_IO_FAILURE };
static volatile LONG QueryInjection;
static DWORD QueryStaleError;
static BOOL Protect;
static volatile LONG Allocations,Regions,Events,Works,WorkPools,Modules;
static volatile LONG DeviceOwned,CloseCalls,CloseFailures,CloseSuccesses,Drains,Queries,SwCloses,Parks,Order;
static LONG DeviceCloseOrder,SwCloseOrder;
static DWORD FirstCloseError;
static volatile LONG NativeRegistered,Unavailable;
static HANDLE Device,Progress,ParkEntered,ParkGate;
static PB_SESSION *Observed;
static PB_INTERFACE_V2 Api;
static PB_LIFECYCLE_INTERFACE Life;
static const WCHAR Path[]=L"packetbridge-hosted-close-retry";
static VOID *TrackedCalloc(SIZE_T count,SIZE_T size)
{
    if(Failure==FAIL_ALLOC) {Failure=FAIL_NONE;return NULL;}
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
    if(!p) return;
    if(p==Observed) Observed=NULL;
    CHECK(InterlockedDecrement(&Allocations)>=0);
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
    VOID *p=VirtualAlloc(address,size,type,protection);
#endif
    CHECK(p);InterlockedIncrement(&Regions);return p;
}
static BOOL WINAPI TrackedUnmap(VOID *p,SIZE_T size,DWORD type)
{
    CHECK(p && !size && type==MEM_RELEASE && InterlockedDecrement(&Regions)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);return TRUE;
#else
    return VirtualFree(p,size,type);
#endif
}
static HANDLE WINAPI TrackedEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name)
{HANDLE event=CreateEventW(attributes,manual,initial,name);CHECK(event);InterlockedIncrement(&Events);return event;}
static BOOL WINAPI TrackedModule(DWORD flags,LPCWSTR address,HMODULE *module)
{BOOL result=GetModuleHandleExW(flags,address,module);CHECK(result);InterlockedIncrement(&Modules);return result;}
static BOOL WINAPI TrackedLibrary(HMODULE module)
{CHECK(InterlockedDecrement(&Modules)>=0);return FreeLibrary(module);}
static VOID WINAPI TrackedTail(PTP_CALLBACK_INSTANCE instance,HMODULE module)
{CHECK(InterlockedDecrement(&Modules)>=0);FreeLibraryWhenCallbackReturns(instance,module);}
static PTP_POOL WINAPI TrackedPool(VOID *reserved)
{PTP_POOL pool=CreateThreadpool(reserved);CHECK(pool);InterlockedIncrement(&WorkPools);return pool;}
static VOID WINAPI TrackedPoolClose(PTP_POOL pool)
{CHECK(InterlockedDecrement(&WorkPools)>=0);CloseThreadpool(pool);}
static PTP_WORK WINAPI TrackedWork(PTP_WORK_CALLBACK callback,VOID *context,PTP_CALLBACK_ENVIRON environment)
{Observed=context;PTP_WORK work=CreateThreadpoolWork(callback,context,environment);CHECK(work);InterlockedIncrement(&Works);return work;}
static VOID WINAPI TrackedWorkClose(PTP_WORK work)
{CHECK(InterlockedDecrement(&Works)>=0);CloseThreadpoolWork(work);}
static VOID WINAPI HostedSwClose(HSWDEVICE);
static HANDLE WINAPI HostedFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL WINAPI HostedIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static BOOL WINAPI TrackedClose(HANDLE);
static DWORD WINAPI GatedWait(HANDLE,DWORD);
#define calloc TrackedCalloc
#define free TrackedFree
#define SwDeviceClose HostedSwClose
#include "../../api/v2/adapter.c"
#undef SwDeviceClose
#define VirtualAlloc TrackedRegion
#define VirtualFree TrackedUnmap
#define CreateEventW TrackedEvent
#define CloseHandle TrackedClose
#define GetModuleHandleExW TrackedModule
#define FreeLibrary TrackedLibrary
#define FreeLibraryWhenCallbackReturns TrackedTail
#define CreateThreadpool TrackedPool
#define CloseThreadpool TrackedPoolClose
#define CreateThreadpoolWork TrackedWork
#define CloseThreadpoolWork TrackedWorkClose
#define CreateFileW HostedFile
#define DeviceIoControl HostedIo
#define WaitForSingleObject GatedWait
#ifndef PB_CLOSE_RETRY_SESSION_SOURCE
#define PB_CLOSE_RETRY_SESSION_SOURCE "../../api/v2/session.c"
#endif
#include PB_CLOSE_RETRY_SESSION_SOURCE
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CloseHandle
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef CreateThreadpool
#undef CloseThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef CreateFileW
#undef DeviceIoControl
#undef WaitForSingleObject
static VOID WINAPI HostedSwClose(HSWDEVICE device)
{CHECK(device==(HSWDEVICE)(ULONG_PTR)0x1234);InterlockedIncrement(&SwCloses);SwCloseOrder=InterlockedIncrement(&Order);}
static HANDLE WINAPI HostedFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templateFile)
{
    CHECK(!wcscmp(path,Path) && access==(GENERIC_READ|GENERIC_WRITE) && share==(FILE_SHARE_READ|FILE_SHARE_WRITE) &&
        !security && creation==OPEN_EXISTING && !flags && !templateFile && Observed);
    Progress=Observed->ProgressEvent;
    if(Failure==FAIL_FILE) {SetLastError(ERROR_FILE_NOT_FOUND);return INVALID_HANDLE_VALUE;}
    Device=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Device);DeviceOwned=1;
    if(Protect) CHECK(SetHandleInformation(Device,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE));
    return Device;
}
static BOOL WINAPI TrackedClose(HANDLE handle)
{
    if(handle==Device && DeviceOwned) {
        InterlockedIncrement(&CloseCalls);BOOL result=CloseHandle(handle);DWORD error=GetLastError();
        if(result) {DeviceOwned=0;InterlockedIncrement(&CloseSuccesses);DeviceCloseOrder=InterlockedIncrement(&Order);}
        else {InterlockedIncrement(&CloseFailures);if(!FirstCloseError) FirstCloseError=error;CHECK(error);}
        SetLastError(error);return result;
    }
    BOOL result=CloseHandle(handle);CHECK(result && InterlockedDecrement(&Events)>=0);return result;
}
static BOOL WINAPI HostedIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlapped)
{
    CHECK(device==Device && DeviceOwned && !overlapped);*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY) {
        CHECK(!input && !inputSize && outputSize==sizeof(PB_PROBE_STATUS));
        if(Failure==FAIL_PROBE) {SetLastError(ERROR_REVISION_MISMATCH);return FALSE;}
        PB_PROBE_STATUS *status=output;memset(status,0,sizeof(*status));status->Size=sizeof(*status);status->Magic=PB_PROBE_MAGIC;
        status->Schema=PB_PROBE_SCHEMA;status->DataAbi=PB_ABI_MAJOR;status->FileVersion=PB_VERSION_PACKED;
        memcpy(status->SourceFingerprint,PB_SOURCE_STRING,sizeof(status->SourceFingerprint));*returned=sizeof(*status);return TRUE;
    }
    if(code==PB_V2_IOCTL_REGISTER) {
        CHECK(inputSize==sizeof(PB_REGISTER_V2) && !outputSize);
        if(Failure==FAIL_REGISTER) {SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}
        InterlockedExchange(&NativeRegistered,1);return TRUE;
    }
    if(code==PB_PROBE_IOCTL_POLICY) {CHECK(Failure==FAIL_POLICY);SetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
    if(code==PB_V2_IOCTL_DRAIN) {
        InterlockedIncrement(&Drains);if(!NativeRegistered || Unavailable) {SetLastError(ERROR_NOT_READY);return FALSE;}return TRUE;
    }
    CHECK(code==PB_V2_IOCTL_QUERY && outputSize==sizeof(PB_V2_STATUS));InterlockedIncrement(&Queries);
    if(!NativeRegistered || Unavailable) {SetLastError(ERROR_NOT_READY);return FALSE;}
    LONG injection=ReadAcquire(&QueryInjection);
    if(injection==QUERY_IO_FAILURE) {SetLastError(ERROR_DEVICE_NOT_CONNECTED);return FALSE;}
    PB_V2_STATUS *status=output;memset(status,0,sizeof(*status));status->Size=sizeof(*status);status->Epoch=Observed->Epoch;
    status->QueueCount=Observed->Layout.QueueCount;status->State=PB_SESSION_STOPPED;*returned=sizeof(*status);
    if(injection==QUERY_ACK_SHORT) --*returned;
    else if(injection==QUERY_ACK_LONG) ++*returned; /* Report only; never write past the output buffer. */
    else if(injection==QUERY_ACK_SIZE) --status->Size;
    else if(injection==QUERY_ACK_EPOCH) status->Epoch^=1;
    SetLastError(QueryStaleError);return TRUE;
}
static DWORD WINAPI GatedWait(HANDLE event,DWORD timeout)
{
    if(event==Progress && timeout==1000) {
        InterlockedIncrement(&Parks);CHECK(SetEvent(ParkEntered) && WaitForSingleObject(ParkGate,5000)==WAIT_OBJECT_0);
    }
    return WaitForSingleObject(event,timeout);
}
static PB_ADAPTER *Adapter(VOID)
{
    PB_ADAPTER *adapter=TrackedCalloc(1,sizeof(*adapter));CHECK(PbRuntimeReserveAdapter());
    adapter->References=1;adapter->Admitted=TRUE;adapter->Device=(HSWDEVICE)(ULONG_PTR)0x1234;
    adapter->Path=TrackedCalloc(1,sizeof(Path));memcpy(adapter->Path,Path,sizeof(Path));return adapter;
}
static VOID Begin(ULONG failure,BOOL protect)
{
    CHECK(!Allocations && !Regions && !Events && !Works && !WorkPools && !Modules && !DeviceOwned && !RuntimeCount && !RuntimeAdapterCount && !RuntimeBytes);
    Failure=FAIL_NONE;Protect=protect;Observed=NULL;Device=Progress=NULL;FirstCloseError=0;
    CloseCalls=CloseFailures=CloseSuccesses=Drains=Queries=SwCloses=Parks=Order=DeviceCloseOrder=SwCloseOrder=0;
    NativeRegistered=Unavailable=0;
    QueryInjection=QUERY_ACK_OK;QueryStaleError=0;
    ParkEntered=CreateEventW(NULL,FALSE,FALSE,NULL);ParkGate=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(ParkEntered && ParkGate);
    Failure=failure;
}
static VOID End(VOID)
{
    CHECK(!Observed && !Allocations && !Regions && !Events && !Works && !WorkPools && !Modules && !DeviceOwned && !RuntimeCount && !RuntimeAdapterCount && !RuntimeBytes);
    CHECK(CloseHandle(ParkEntered) && CloseHandle(ParkGate));ParkEntered=ParkGate=NULL;
}
static PB_RESULT CallOpen(PB_ADAPTER *adapter,PB_SESSION **s)
{
    PB_CONFIG config={sizeof(config),1,4,4,0,0};
    if(Failure==FAIL_POLICY) {config.Flags=PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS;config.Offloads=PB_TX_IP4_CHECKSUM;}
    return adapter?Api.StartAdapterSession(adapter,&config,s):Api.Open(Path,&config,s);
}
static VOID AwaitPark(VOID)
{CHECK(WaitForSingleObject(ParkEntered,3000)==WAIT_OBJECT_0 && ResetEvent(Progress));}
static VOID Recover(PB_SESSION *s,BOOL published,BOOL hasAdapter)
{
    CHECK(s==Observed && s->Region && !s->ResourcesClosed && DeviceOwned && FirstCloseError && CloseFailures);
    LONG closes=CloseCalls,drains=Drains,queries=Queries;Unavailable=1;
    CHECK(SetHandleInformation(Device,HANDLE_FLAG_PROTECT_FROM_CLOSE,0) && SetEvent(Progress) && SetEvent(ParkGate));
#ifdef PB_CLOSE_RETRY_BASELINE
    AwaitPark();CHECK(CloseCalls==closes && !CloseSuccesses && !s->ResourcesClosed && s->Region &&
        (Drains>drains || Queries>queries));
    /* Explicit IO availability recovery only for frozen-code teardown. */
    NativeRegistered=1;Unavailable=0;CHECK(SetEvent(Progress) && SetEvent(ParkGate));
#endif
    WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    CHECK(s->ResourcesClosed && !s->Region && CloseSuccesses==1 && !DeviceOwned && !RuntimeCount);
#ifndef PB_CLOSE_RETRY_BASELINE
    CHECK(CloseCalls==closes+1 && Drains==drains && Queries==queries);
#endif
    if(published) CHECK(Api.Close(s,2000)==PB_OK);
    if(hasAdapter) {
        CHECK(SwCloses==1);
#ifdef PB_CLOSE_RETRY_BASELINE
        CHECK(published?SwCloseOrder>DeviceCloseOrder:SwCloseOrder<DeviceCloseOrder);
#else
        CHECK(SwCloseOrder>DeviceCloseOrder);
#endif
    }
    DropReference(s);CHECK(!Observed);
}
static VOID FailedOpen(ULONG failure,BOOL adapter,BOOL protect)
{
    Begin(FAIL_NONE,protect);PB_ADAPTER *a=adapter?Adapter():NULL;Failure=failure;
    PB_SESSION *out=(PB_SESSION *)(ULONG_PTR)1;CHECK(CallOpen(a,&out)==PB_UNSUPPORTED && !out);
    if(protect) {
        PB_SESSION *s=Observed;CHECK(s);Retain(s);AwaitPark();
        PB_CLOSE_PROGRESS progress;CHECK(Life.GetCloseProgress(s,&progress)==PB_OK && progress.RetainedBytes && !progress.UserLeases && !progress.Published &&
            progress.Started && progress.LastProgress>=progress.Started);
#ifndef PB_CLOSE_RETRY_BASELINE
        CHECK(progress.Phase==PB_LIFE_RELEASING && progress.BlockStage==PB_BLOCK_DEVICE_CLOSE && progress.LastError==FirstCloseError && !Drains && !Queries);
#endif
        if(a) Api.CloseAdapter(a);
#ifdef PB_CLOSE_RETRY_BASELINE
        CHECK(SwCloses==(LONG)adapter);
#else
        CHECK(!SwCloses && RuntimeAdapterCount==(ULONG)adapter);
#endif
        Recover(s,FALSE,adapter);
    } else {
        CHECK(!Observed && CloseSuccesses==1 && !CloseFailures && !RuntimeCount);
        if(a) {CHECK(!SwCloses);Api.CloseAdapter(a);CHECK(SwCloses==1 && SwCloseOrder>DeviceCloseOrder);}
    }
    End();
}
static VOID PublishedClose(BOOL adapter,BOOL loan)
{
    Begin(FAIL_NONE,TRUE);PB_ADAPTER *a=adapter?Adapter():NULL;PB_SESSION *s;
    CHECK(CallOpen(a,&s)==PB_OK && s==Observed);Retain(s);
    PB_USER_QUEUE *q=NULL;PB_LEASE lease={0};uint32_t n;
    if(loan) CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.ReserveBatch(q,&lease,1,&n)==PB_OK && n==1);
    CHECK(Api.Close(s,0)==(loan?PB_BUSY:PB_QUARANTINED));AwaitPark();
    if(loan) {
        CHECK(!CloseCalls && !s->ResourcesClosed && s->Progress.Phase<PB_LIFE_PREPARED && q->Loans==1);
        lease.Data[0]=0x5a;CHECK(Api.CancelBatch(q,&lease.Descriptor.Token,1)==PB_OK && SetEvent(ParkGate));AwaitPark();
    }
    CHECK(s->Progress.Phase==PB_LIFE_RELEASING && s->Progress.LastError==FirstCloseError && Drains==1 && Queries==1);
    if(a) {Api.CloseAdapter(a);CHECK(!SwCloses);}
    Recover(s,TRUE,adapter);End();
}
static VOID EarlyFailure(ULONG failure,BOOL adapter)
{
    Begin(FAIL_NONE,FALSE);PB_ADAPTER *a=adapter?Adapter():NULL;Failure=failure;
    PB_SESSION *s=(PB_SESSION *)(ULONG_PTR)1;
    CHECK(CallOpen(a,&s)==(failure==FAIL_ALLOC?PB_FULL:PB_UNSUPPORTED) && !s && !Observed && !RuntimeCount && !DeviceOwned);
    if(a) {CHECK(!SwCloses);Api.CloseAdapter(a);CHECK(SwCloses==1);}
    End();
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    for(ULONG a=0;a<2;++a) {
        for(ULONG protect=0;protect<2;++protect) for(ULONG failure=FAIL_PROBE;failure<=FAIL_POLICY;++failure) FailedOpen(failure,(BOOL)a,(BOOL)protect);
        PublishedClose((BOOL)a,FALSE);PublishedClose((BOOL)a,TRUE);EarlyFailure(FAIL_FILE,(BOOL)a);EarlyFailure(FAIL_ALLOC,(BOOL)a);
    }
#ifdef PB_CLOSE_RETRY_BASELINE
    puts("PASS: 20 actual old Open/close scenarios: protected-handle cleanup stalls behind unavailable DRAIN/QUERY after close repair; failed StartAdapterSession drops adapter before the retained device barrier; explicit hosted IO recovery permits balanced teardown.");
#else
    puts("PASS: 20 actual Open/close scenarios: protected OS-handle close retries finish with DRAIN/QUERY unavailable, failed adapter-start retains adapter until device closure, ordinary failures/success and outstanding user loans preserve barriers; zero tracked allocations/regions/events/modules/work/session/adapter budget.");
#endif
    return 0;
}
