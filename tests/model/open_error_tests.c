/* SPDX-License-Identifier: GPL-2.0 */
#ifdef PB_API_TESTS
#error Open rollback tests require actual native session paths
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../api/v2/adapter.h"
#include "../../common/platform.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s error=%lu\n",__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static unsigned Mode;
static uint64_t RegisteredEpoch;
static HANDLE PoolClosed;
static volatile LONG LiveEvents,LiveMaps,LiveHeap,LivePools,LiveWorks,LivePins;
enum { CONSTRUCTION_SUCCESS=10,CONSTRUCTION_LOW_CAPABILITY,CONSTRUCTION_IO_FAILURE,TERMINAL_REGISTER,TERMINAL_POLICY,TERMINAL_BEFORE_POLICY };
static PB_SESSION *Constructing;
static HANDLE PolicyEntered,PolicyGate,ConstructionDevice;
static volatile LONG ConstructionIo,DiagnosticSamples;
static ULONG ConstructionCases,ConstructorJoins,DiagnosticJoins,PolicySamples,CapturedJoins,TestHandles;
static volatile LONG CallbackStarts,CallbackFinishes,ActiveCallbacks;
static HANDLE CallbackThreads[8];
static PB_SESSION *CallbackContext;
static volatile LONG ConstructionRegistered,ConstructionDeviceCloses;
static volatile LONG TerminalInjections,EarlyWaitEntries;
static ULONG EarlyCases,EarlyOwnerJoins,EarlyConstructorJoins,EarlyRejectedCases,EarlyRejectedJoins;
static VOID CALLBACK LifecycleWorker(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
static VOID CALLBACK TrackDispatch(PTP_CALLBACK_INSTANCE,PVOID,PTP_WORK);
static HANDLE WINAPI TrackEvent(LPSECURITY_ATTRIBUTES a,BOOL m,BOOL s,LPCWSTR n)
{HANDLE h=CreateEventW(a,m,s,n);if(h)InterlockedIncrement(&LiveEvents);return h;}
static void *TrackCalloc(size_t n,size_t size)
{
#ifdef PB_GUARD_TESTS
    CHECK(!size || n<=SIZE_MAX/size);void *p=PbTestGuardAlloc(n*size);memset(p,0,n*size);
#else
    void *p=calloc(n,size);
#endif
    if(p)InterlockedIncrement(&LiveHeap);return p;
}
static void TrackFree(void *p)
{
    if(!p)return;CHECK(InterlockedDecrement(&LiveHeap)>=0);
    if(p==Constructing)Constructing=NULL;if(p==CallbackContext)CallbackContext=NULL;
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static void *WINAPI TrackMap(void *p,SIZE_T size,DWORD type,DWORD protect)
{
#ifdef PB_GUARD_TESTS
    CHECK(!p && type==(MEM_COMMIT|MEM_RESERVE) && protect==PAGE_READWRITE);void *r=PbTestGuardAlloc(size);memset(r,0,size);
#else
    void *r=VirtualAlloc(p,size,type,protect);
#endif
    if(r)InterlockedIncrement(&LiveMaps);return r;
}
static BOOL WINAPI TrackUnmap(void *p,SIZE_T size,DWORD type)
{
#ifdef PB_GUARD_TESTS
    CHECK(p && !size && type==MEM_RELEASE);PbTestGuardFree(p);BOOL r=TRUE;
#else
    BOOL r=VirtualFree(p,size,type);
#endif
    CHECK(r&&InterlockedDecrement(&LiveMaps)>=0);return r;
}
static PTP_POOL WINAPI TrackPool(void *reserved) {PTP_POOL p=CreateThreadpool(reserved);if(p)InterlockedIncrement(&LivePools);return p;}
static PTP_WORK WINAPI TrackWork(PTP_WORK_CALLBACK cb,void *p,PTP_CALLBACK_ENVIRON env)
{
    CHECK(cb==LifecycleWorker && !CallbackContext && !ActiveCallbacks && CallbackStarts==CallbackFinishes);
    for(ULONG i=0;i<8;++i)CHECK(!CallbackThreads[i]);CallbackStarts=CallbackFinishes=0;CallbackContext=p;
    PTP_WORK w=CreateThreadpoolWork(TrackDispatch,p,env);
    if(w){InterlockedIncrement(&LiveWorks);if(Mode>=CONSTRUCTION_SUCCESS){CHECK(!Constructing);Constructing=p;}}return w;
}
static void WINAPI TrackCloseWork(PTP_WORK w) {CloseThreadpoolWork(w);CHECK(InterlockedDecrement(&LiveWorks)>=0);}
static BOOL WINAPI TrackPin(DWORD flags,LPCWSTR address,HMODULE *out)
{BOOL r=GetModuleHandleExW(flags,address,out);if(r)InterlockedIncrement(&LivePins);return r;}
static BOOL WINAPI TrackUnpin(HMODULE m) {CHECK(InterlockedDecrement(&LivePins)>=0);return FreeLibrary(m);}
static void WINAPI TrackUnpinOnReturn(PTP_CALLBACK_INSTANCE i,HMODULE m)
{CHECK(InterlockedDecrement(&LivePins)>=0);FreeLibraryWhenCallbackReturns(i,m);}
static void WINAPI FakeClosePool(PTP_POOL pool)
{
    CloseThreadpool(pool);CHECK(InterlockedDecrement(&LivePools)>=0);CHECK(SetEvent(PoolClosed));
}
static HANDLE WINAPI FakeFile(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template)
{
    (void)name;(void)access;(void)share;(void)security;(void)creation;(void)flags;(void)template;
    if(Mode==1){SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    HANDLE event=TrackEvent(NULL,FALSE,FALSE,NULL);CHECK(event);if(Mode>=CONSTRUCTION_SUCCESS)ConstructionDevice=event;return event;
}
static BOOL WINAPI FakeClose(HANDLE h)
{
    BOOL device=Mode>=CONSTRUCTION_SUCCESS && h==ConstructionDevice;
    BOOL r=CloseHandle(h);CHECK(r&&InterlockedDecrement(&LiveEvents)>=0);
    if(device){CHECK(InterlockedExchange(&ConstructionRegistered,0)==1);ConstructionDevice=NULL;InterlockedIncrement(&ConstructionDeviceCloses);}
    SetLastError(ERROR_GEN_FAILURE);return r;
}
static BOOL WINAPI FakeIo(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static DWORD WINAPI ObserveMultipleWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{if(Mode>=TERMINAL_REGISTER)InterlockedIncrement(&EarlyWaitEntries);return WaitForMultipleObjects(count,events,all,timeout);}
#define CreateFileW FakeFile
#define CloseHandle FakeClose
#define DeviceIoControl FakeIo
#define CloseThreadpool FakeClosePool
#define CreateEventW TrackEvent
#define calloc TrackCalloc
#define free TrackFree
#define VirtualAlloc TrackMap
#define VirtualFree TrackUnmap
#define CreateThreadpool TrackPool
#define CreateThreadpoolWork TrackWork
#define CloseThreadpoolWork TrackCloseWork
#define GetModuleHandleExW TrackPin
#define FreeLibrary TrackUnpin
#define FreeLibraryWhenCallbackReturns TrackUnpinOnReturn
#define WaitForMultipleObjects ObserveMultipleWait
#include "../../api/v2/session.c"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"open rollback requires shipping fingerprint");
#undef CreateFileW
#undef CloseHandle
#undef DeviceIoControl
#undef CloseThreadpool
#undef CreateEventW
#undef calloc
#undef free
#undef VirtualAlloc
#undef VirtualFree
#undef CreateThreadpool
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#undef GetModuleHandleExW
#undef FreeLibrary
#undef FreeLibraryWhenCallbackReturns
#undef WaitForMultipleObjects
static VOID CALLBACK TrackDispatch(PTP_CALLBACK_INSTANCE instance,PVOID context,PTP_WORK work)
{
    CHECK(context==CallbackContext && InterlockedIncrement(&ActiveCallbacks)==1);
    LONG ordinal=InterlockedIncrement(&CallbackStarts);CHECK(ordinal>0 && ordinal<=8);
    CallbackThreads[ordinal-1]=OpenThread(SYNCHRONIZE,FALSE,GetCurrentThreadId());CHECK(CallbackThreads[ordinal-1]);
    LifecycleWorker(instance,context,work);
    /* The core may be gone. Only the host's still-live callback ledger is used. */
    InterlockedIncrement(&CallbackFinishes);CHECK(InterlockedDecrement(&ActiveCallbacks)==0);
}
static VOID JoinCallbacks(VOID)
{
    CHECK(WaitForSingleObject(PoolClosed,5000)==WAIT_OBJECT_0);
    ULONG captured=0;BOOL gap=FALSE;
    for(ULONG i=0;i<8;++i){if(CallbackThreads[i]){CHECK(!gap && WaitForSingleObject(CallbackThreads[i],5000)==WAIT_OBJECT_0 && CloseHandle(CallbackThreads[i]));CallbackThreads[i]=NULL;++captured;++CapturedJoins;}else gap=TRUE;}
    CHECK(!ActiveCallbacks && CallbackStarts==CallbackFinishes && captured==(ULONG)CallbackStarts && !CallbackContext);
}
PB_RESULT WINAPI PbAdapterCreate(const wchar_t *n,const wchar_t *id,PB_ADAPTER **out)
{(void)n;(void)id;*out=NULL;return PB_UNSUPPORTED;}
void WINAPI PbAdapterRelease(PB_ADAPTER *a) {CHECK(!a);}
void PbAdapterRetain(PB_ADAPTER *a) {(void)a;}
const wchar_t *PbAdapterPath(PB_ADAPTER *a) {(void)a;return L"hosted-device";}
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *a,uint64_t *luid) {(void)a;(void)luid;return PB_UNSUPPORTED;}
static VOID PublishEarlyTerminal(VOID)
{
    PB_SESSION *s=Constructing;CHECK(s && ConstructionRegistered && !s->Registered && !CallbackStarts && InterlockedIncrement(&TerminalInjections)==1);
    /* Native DDI completion timing is modeled; these are real shared stores
     * and SDK events. Terminal is only a stop hint, never a native close ACK. */
    InterlockedExchange((volatile LONG *)&s->SharedLifecycle->Terminal,1);
    InterlockedIncrement64((volatile LONG64 *)&s->SharedLifecycle->Sequence);
    CHECK(SetEvent(s->ProgressEvent));
    for(ULONG q=0;q<s->Layout.QueueCount;++q)CHECK(SetEvent(s->Queues[q].UserEvent));
    for(ULONG q=0;q<s->Layout.QueueCount;++q)CHECK(SetEvent(s->Queues[q].KernelEvent));
}
static BOOL WINAPI FakeIo(HANDLE handle,DWORD code,LPVOID input,DWORD inSize,LPVOID output,DWORD outSize,LPDWORD returned,LPOVERLAPPED overlapped)
{
    (void)handle;(void)inSize;(void)outSize;(void)overlapped;*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY) {
        if(Mode==2){SetLastError(ERROR_NOT_READY);return FALSE;}
        PB_PROBE_STATUS *s=output;memset(s,0,sizeof(*s));s->Size=sizeof(*s);s->Magic=PB_PROBE_MAGIC;
        s->Schema=PB_PROBE_SCHEMA;s->DataAbi=PB_ABI_MAJOR;s->FileVersion=PB_VERSION_PACKED;
        if(Mode>=CONSTRUCTION_SUCCESS){s->NdisVersion=Mode==CONSTRUCTION_LOW_CAPABILITY?PB_NDIS_VERSION(20):PB_NDIS_VERSION(83);s->ImplementedMask=PbPlatformOffloads(s->NdisVersion);}
        memcpy(s->SourceFingerprint,PB_SOURCE_STRING,sizeof(PB_SOURCE_STRING));*returned=sizeof(*s);
        if(Mode==3)*returned=1;
        if(Mode==4)s->Magic^=1;
        if(Mode==5)s->DataAbi++;
        if(Mode==6)s->SourceFingerprint[0]^=1;
        return TRUE;
    }
    if(code==PB_V2_IOCTL_REGISTER) {
        if(Mode==7){SetLastError(ERROR_INVALID_DATA);return FALSE;}
        RegisteredEpoch=((PB_REGISTER_V2 *)input)->Epoch;
        if(Mode>=CONSTRUCTION_SUCCESS)CHECK(InterlockedExchange(&ConstructionRegistered,1)==0);
        if(Mode==TERMINAL_REGISTER || Mode==TERMINAL_BEFORE_POLICY)PublishEarlyTerminal();return TRUE;
    }
    if(code==PB_PROBE_IOCTL_POLICY&&Mode==8){SetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
    if(code==PB_PROBE_IOCTL_POLICY && Mode>=CONSTRUCTION_SUCCESS){
        const PB_PROBE_POLICY *policy=input;CHECK(Constructing && ConstructionRegistered && handle==ConstructionDevice && !overlapped &&
            input && inSize==sizeof(*policy) && !output && !outSize && policy->Size==sizeof(*policy) && policy->Schema==PB_PROBE_SCHEMA &&
            policy->AllowedMask==PB_TX_USO4 && !policy->Reserved && InterlockedIncrement(&ConstructionIo)==1);
        CHECK(SetEvent(PolicyEntered) && WaitForSingleObject(PolicyGate,10000)==WAIT_OBJECT_0);
        /* Native device I/O is hosted. The pure shipping capability helper
         * supplies the downlevel mask; access denied is an SDK-boundary input,
         * not a claim that this host has exercised NDIS or Windows 7. */
        DWORD runtime=Mode==CONSTRUCTION_LOW_CAPABILITY?PB_NDIS_VERSION(20):PB_NDIS_VERSION(83);
        /* The rejected stage models a faulted native endpoint before policy
         * acceptance. ERROR_NOT_READY is an injected SDK failure, not a test
         * of the kernel NTSTATUS-to-Win32 error translation. */
        DWORD error=Mode==TERMINAL_BEFORE_POLICY?ERROR_NOT_READY:policy->AllowedMask&~PbPlatformOffloads(runtime)?ERROR_NOT_SUPPORTED:Mode==CONSTRUCTION_IO_FAILURE?ERROR_ACCESS_DENIED:ERROR_SUCCESS;
        /* The supported policy completed successfully before terminal arrived;
         * only delivery of that successful SDK result is still outstanding.
         * Fault-before-policy-admission could instead fail Open and is not
         * modeled as a successful policy here. */
        if(Mode==TERMINAL_POLICY){CHECK(error==ERROR_SUCCESS);PublishEarlyTerminal();}
        SetLastError(error);return error==ERROR_SUCCESS;
    }
    if(code==PB_V2_IOCTL_QUERY){PB_V2_STATUS *s=output;memset(s,0,sizeof(*s));s->Size=sizeof(*s);s->Epoch=RegisteredEpoch;s->State=PB_SESSION_STOPPED;s->QueueCount=Constructing?Constructing->Layout.QueueCount:1;*returned=sizeof(*s);}
    return TRUE;
}
static void Run(void)
{
    CHECK(ResetEvent(PoolClosed));
    PB_CONFIG c={sizeof(c),1,4,4,Mode==8?PB_TX_LSO4:0,Mode==8?3:0};PB_SESSION *s=(void *)1;
    PB_RESULT r=Open(L"hosted-device",&c,&s);
    const DWORD errors[]={0,ERROR_ACCESS_DENIED,ERROR_NOT_READY,ERROR_REVISION_MISMATCH,ERROR_REVISION_MISMATCH,
        ERROR_REVISION_MISMATCH,ERROR_REVISION_MISMATCH,ERROR_INVALID_DATA,ERROR_NOT_SUPPORTED};
    if(Mode<=8){CHECK(r==PB_UNSUPPORTED&&!s);CHECK(GetLastError()==errors[Mode]);}
    else CHECK(r==PB_OK&&Close(s,5000)==PB_OK);
    JoinCallbacks();
    PB_RUNTIME_STATUS status;CHECK(GetRuntimeStatus(&status)==PB_OK&&!status.Sessions&&!status.RetainedBytes);
    CHECK(!LiveEvents&&!LiveMaps&&!LiveHeap&&!LivePools&&!LiveWorks&&!LivePins);
}
typedef struct CONSTRUCTOR {
    PB_CONFIG Config;PB_SESSION *Session;PB_RESULT Result;DWORD Error;
} CONSTRUCTOR;
static HANDLE TestEvent(VOID)
{HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(event);++TestHandles;return event;}
static VOID JoinTestThread(HANDLE thread)
{CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0 && CloseHandle(thread) && TestHandles);--TestHandles;}
static DWORD WINAPI Construct(VOID *opaque)
{
    CONSTRUCTOR *job=opaque;SetLastError(ERROR_SHARING_VIOLATION);
    job->Result=Open(L"hosted-construction-policy",&job->Config,&job->Session);job->Error=GetLastError();return 0;
}
static DWORD WINAPI DiagnoseConstruction(VOID *unused)
{
    (void)unused;
    for(ULONG sample=0;sample<8;++sample){
        PB_SESSION *s=Constructing;CHECK(s && ConstructionRegistered && ConstructionIo==1 && !ConstructionDeviceCloses);
        /* PolicyIo owns Lifetime shared and Control exclusive across hosted I/O.
         * This internal observation takes no session reference. The constructor
         * is hard-gated and owns the only reference until real Open returns. */
        CHECK(!TryAcquireSRWLockExclusive(&s->Lifetime) && !TryAcquireSRWLockShared(&s->Control));
        AcquireSRWLockShared(&s->State);
        CHECK(ReadAcquire(&s->References)==1 && s->Admitted && !s->Registered && !s->Scheduled && !s->PublicReleased &&
            !s->CloseRequested && !s->ResourcesClosed && s->Progress.Phase==PB_LIFE_RUNNING && s->Device==ConstructionDevice &&
            s->Region && s->Work && s->Pool && s->Module && s->TailModule && !CallbackStarts && !ActiveCallbacks);
        ReleaseSRWLockShared(&s->State);
        PB_RUNTIME_STATUS runtime;CHECK(GetRuntimeStatus(&runtime)==PB_OK && runtime.Sessions==1 && runtime.RetainedBytes==s->Layout.RegionSize && !runtime.Adapters);
        PB_CLOSE_PROGRESS progress,unchanged;memset(&progress,0xa5,sizeof(progress));memcpy(&unchanged,&progress,sizeof(progress));
        CHECK(GetRetained(0,&progress)==PB_EMPTY && !memcmp(&progress,&unchanged,sizeof(progress)));
        CHECK(LivePins==2 && LiveMaps==1 && LivePools==1 && LiveWorks==1 && LiveHeap==(LONG)(1+2*s->Layout.QueueCount) && LiveEvents==(LONG)(3+2*s->Layout.QueueCount));
        InterlockedIncrement(&DiagnosticSamples);
    }
    return 0;
}
static VOID ConstructionPolicy(ULONG queues,ULONG mode)
{
    CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && !TestHandles && ResetEvent(PoolClosed));
    Mode=mode;ConstructionIo=DiagnosticSamples=ConstructionDeviceCloses=0;PolicyEntered=TestEvent();PolicyGate=TestEvent();
    CONSTRUCTOR job={{sizeof(PB_CONFIG),queues,4,4,PB_TX_USO4,PB_OPEN_EXPERIMENTAL_OFFLOADS|PB_OPEN_CHECKED_SEGMENTS},(PB_SESSION *)(ULONG_PTR)1,PB_INVALID,MAXDWORD};
    HANDLE constructor=CreateThread(NULL,0,Construct,&job,0,NULL);CHECK(constructor);++TestHandles;
    CHECK(WaitForSingleObject(PolicyEntered,10000)==WAIT_OBJECT_0 && !job.Session && !CallbackStarts);
    HANDLE diagnostics[PB_MAX_QUEUES];
    for(ULONG i=0;i<queues;++i){diagnostics[i]=CreateThread(NULL,0,DiagnoseConstruction,NULL,0,NULL);CHECK(diagnostics[i]);++TestHandles;}
    for(ULONG i=0;i<queues;++i){JoinTestThread(diagnostics[i]);++DiagnosticJoins;}
    CHECK(DiagnosticSamples==(LONG)(8*queues) && !ConstructionDeviceCloses && SetEvent(PolicyGate));PolicySamples+=(ULONG)DiagnosticSamples;
    JoinTestThread(constructor);++ConstructorJoins;
    if(mode==CONSTRUCTION_SUCCESS){
        CHECK(job.Result==PB_OK && job.Session==Constructing && job.Session && ConstructionRegistered && !ConstructionDeviceCloses);
        PB_CLOSE_PROGRESS progress;CHECK(GetRetained(0,&progress)==PB_OK && progress.SessionId==job.Session->Epoch);
        CHECK(Close(job.Session,5000)==PB_OK);job.Session=NULL;
    }else{
        CHECK(job.Result==PB_UNSUPPORTED && !job.Session && job.Error==(DWORD)(mode==CONSTRUCTION_LOW_CAPABILITY?ERROR_NOT_SUPPORTED:ERROR_ACCESS_DENIED));
    }
    JoinCallbacks();CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && ConstructionDeviceCloses==1 && ConstructionIo==1);
    PB_RUNTIME_STATUS runtime;PB_CLOSE_PROGRESS progress;CHECK(GetRuntimeStatus(&runtime)==PB_OK && !runtime.Sessions && !runtime.RetainedBytes && GetRetained(0,&progress)==PB_EMPTY);
    CHECK(!LiveEvents && !LiveMaps && !LiveHeap && !LivePools && !LiveWorks && !LivePins);
    CHECK(CloseHandle(PolicyEntered) && CloseHandle(PolicyGate) && TestHandles==2);TestHandles-=2;PolicyEntered=PolicyGate=NULL;++ConstructionCases;
    printf("PASS: construction policy queues%lu outcome%lu; private refs1/pins2/admission1, %lu joined diagnostic callers, actual Open return and close, all owners zero\n",queues,mode,queues);
}
static VOID ConstructionMatrix(VOID)
{const ULONG queues[]={1,2,8};for(ULONG q=0;q<3;++q)for(ULONG mode=CONSTRUCTION_SUCCESS;mode<=CONSTRUCTION_IO_FAILURE;++mode)ConstructionPolicy(queues[q],mode);}
typedef struct EARLY_OWNER {PB_USER_QUEUE *Queue;ULONG First;} EARLY_OWNER;
static DWORD WINAPI EarlyOwner(VOID *opaque)
{
    EARLY_OWNER *owner=opaque;
    for(ULONG step=0;step<4;++step){
        PB_LEASE lease;PB_DESCRIPTOR completed;uint32_t count=MAXDWORD;PB_RESULT result;
        switch((owner->First+step)%4){
        case 0:result=ReserveBatch(owner->Queue,&lease,1,&count);CHECK(result==PB_STOPPED && !count);break;
        case 1:result=ReceiveBatch(owner->Queue,&lease,1,&count);CHECK(result==PB_STOPPED && !count);break;
        case 2:CHECK(Wait(owner->Queue,1000)==PB_STOPPED);break;
        default:result=PollCompletions(owner->Queue,&completed,1,&count);CHECK(result==PB_EMPTY && !count);break;
        }
    }
    return 0;
}
static VOID EarlyTerminal(ULONG queues,ULONG mode,ULONG first)
{
    CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && !TestHandles && ResetEvent(PoolClosed));
    Mode=mode;ConstructionIo=ConstructionDeviceCloses=TerminalInjections=EarlyWaitEntries=0;PolicyEntered=TestEvent();PolicyGate=TestEvent();
    CONSTRUCTOR job={{sizeof(PB_CONFIG),queues,4,4,mode==TERMINAL_POLICY?PB_TX_USO4:0,mode==TERMINAL_POLICY?PB_OPEN_KNOWN_FLAGS:0},(PB_SESSION *)(ULONG_PTR)1,PB_INVALID,MAXDWORD};
    HANDLE constructor=CreateThread(NULL,0,Construct,&job,0,NULL);CHECK(constructor);++TestHandles;
    if(mode==TERMINAL_POLICY){
        CHECK(WaitForSingleObject(PolicyEntered,10000)==WAIT_OBJECT_0 && Constructing && !job.Session && !TerminalInjections && Constructing->References==1);
        CHECK(SetEvent(PolicyGate));
    }
    JoinTestThread(constructor);++EarlyConstructorJoins;
    PB_SESSION *s=job.Session;CHECK(job.Result==PB_OK && s && s==Constructing && TerminalInjections==1 && ConstructionRegistered && !ConstructionDeviceCloses && !s->CloseRequested &&
        ReadAcquire((volatile LONG *)&s->SharedLifecycle->Terminal) && InterlockedCompareExchange64((volatile LONG64 *)&s->SharedLifecycle->Sequence,0,0)>0 && !CallbackStarts);
    CHECK(ConstructionIo==(LONG)(mode==TERMINAL_POLICY));
    /* OpenQueue does not consume the terminal hint. Obtain every borrowed
     * queue before its unique owner performs the first finite operation. */
    EARLY_OWNER jobs[PB_MAX_QUEUES];HANDLE owners[PB_MAX_QUEUES];
    for(ULONG q=0;q<queues;++q){PB_USER_QUEUE *queue=NULL;CHECK(OpenQueue(s,q,&queue)==PB_OK && queue);jobs[q]=(EARLY_OWNER){queue,first};}
    for(ULONG q=0;q<queues;++q){owners[q]=CreateThread(NULL,0,EarlyOwner,jobs+q,0,NULL);CHECK(owners[q]);++TestHandles;}
    for(ULONG q=0;q<queues;++q){JoinTestThread(owners[q]);++EarlyOwnerJoins;}
    CHECK(s->CloseRequested && !s->Fatal && !EarlyWaitEntries && ConstructionRegistered && !ConstructionDeviceCloses && !CallbackStarts);
    for(ULONG q=0;q<queues;++q){PB_USER_QUEUE *queue=(VOID *)(ULONG_PTR)1;CHECK(OpenQueue(s,q,&queue)==PB_STOPPED && !queue && !s->Queues[q].Loans && !s->Queues[q].Published);}
    /* The stop hint does not prove native reclamation. Only after all public
     * owners have joined may actual Close perform DRAIN/QUERY/device close. */
    CHECK(Close(s,5000)==PB_OK);s=NULL;job.Session=NULL;JoinCallbacks();
    CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && ConstructionDeviceCloses==1 && !LiveEvents && !LiveMaps && !LiveHeap && !LivePools && !LiveWorks && !LivePins);
    CHECK(CloseHandle(PolicyEntered) && CloseHandle(PolicyGate) && TestHandles==2);TestHandles-=2;PolicyEntered=PolicyGate=NULL;++EarlyCases;
    printf("PASS: early terminal queues%lu stage%lu first%lu; actual Open success, stopped admission/Wait and empty retirement, owners joined before close, native hint alone never freed resources\n",queues,mode,first);
}
static VOID EarlyMatrix(VOID)
{const ULONG queues[]={1,2,8};for(ULONG q=0;q<3;++q)for(ULONG mode=TERMINAL_REGISTER;mode<=TERMINAL_POLICY;++mode)for(ULONG first=0;first<4;++first)EarlyTerminal(queues[q],mode,first);}
static VOID EarlyRejectedPolicy(ULONG queues)
{
    CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && !TestHandles && ResetEvent(PoolClosed));
    Mode=TERMINAL_BEFORE_POLICY;ConstructionIo=ConstructionDeviceCloses=TerminalInjections=EarlyWaitEntries=0;PolicyEntered=TestEvent();PolicyGate=TestEvent();
    CONSTRUCTOR job={{sizeof(PB_CONFIG),queues,4,4,PB_TX_USO4,PB_OPEN_KNOWN_FLAGS},(PB_SESSION *)(ULONG_PTR)1,PB_INVALID,MAXDWORD};
    HANDLE constructor=CreateThread(NULL,0,Construct,&job,0,NULL);CHECK(constructor);++TestHandles;
    CHECK(WaitForSingleObject(PolicyEntered,10000)==WAIT_OBJECT_0 && Constructing && !job.Session && TerminalInjections==1 && Constructing->References==1 && !Constructing->Registered);
    PB_RUNTIME_STATUS runtime;PB_CLOSE_PROGRESS progress;CHECK(GetRuntimeStatus(&runtime)==PB_OK && runtime.Sessions==1 && runtime.RetainedBytes && GetRetained(0,&progress)==PB_EMPTY);
    CHECK(SetEvent(PolicyGate));JoinTestThread(constructor);++EarlyRejectedJoins;
    CHECK(job.Result==PB_UNSUPPORTED && !job.Session && job.Error==ERROR_NOT_READY);JoinCallbacks();
    CHECK(!Constructing && !ConstructionDevice && !ConstructionRegistered && ConstructionDeviceCloses==1 && ConstructionIo==1 && !CallbackStarts && !EarlyWaitEntries &&
        !LiveEvents && !LiveMaps && !LiveHeap && !LivePools && !LiveWorks && !LivePins);
    CHECK(GetRuntimeStatus(&runtime)==PB_OK && !runtime.Sessions && !runtime.RetainedBytes && GetRetained(0,&progress)==PB_EMPTY);
    CHECK(CloseHandle(PolicyEntered) && CloseHandle(PolicyGate) && TestHandles==2);TestHandles-=2;PolicyEntered=PolicyGate=NULL;++EarlyRejectedCases;
    printf("PASS: early terminal rejected initial policy queues%lu; actual Open failed with original SDK error, no public session or callback, constructor joined and owned resources zero\n",queues);
}
static VOID EarlyRejectedMatrix(VOID)
{const ULONG queues[]={1,2,8};for(ULONG q=0;q<3;++q)EarlyRejectedPolicy(queues[q]);}
int main(void)
{
    PoolClosed=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(PoolClosed);
    /* Exercise every path before measuring process-wide CNG/SRW/loader caches. */
    for(unsigned warmup=0;warmup<5;++warmup)for(Mode=1;Mode<=9;++Mode)Run();
    ConstructionMatrix();ULONG warmed=ConstructionCases;
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(unsigned repeat=0;repeat<50;++repeat)for(Mode=1;Mode<=9;++Mode)Run();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
    /* Windows owns additional process-wide threadpool/CNG caches. Account all
     * application-owned objects above after joining the actual closing thread. */
    printf("Process handle samples (including Windows caches): before=%lu after=%lu\n",before,after);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));ConstructionMatrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    CHECK(ConstructionCases==2*warmed && ConstructorJoins==ConstructionCases && !TestHandles);
    printf("PASS: %lu construction-policy warmup/%lu measured cases; constructor joins%lu diagnostic joins%lu samples%lu, every actual callback captured/joined (%lu including prior rollback cycles), process handles%lu/%lu\n",warmed,ConstructionCases-warmed,ConstructorJoins,DiagnosticJoins,PolicySamples,CapturedJoins,before,after);
    EarlyMatrix();ULONG earlyWarm=EarlyCases;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));EarlyMatrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after && !TestHandles);
    printf("PASS: %lu early-terminal warmup/%lu measured cases; constructor joins%lu owner joins%lu, actual queue waits never entered SDK wait after terminal, every callback captured/joined (%lu total), process handles%lu/%lu\n",earlyWarm,EarlyCases-earlyWarm,EarlyConstructorJoins,EarlyOwnerJoins,CapturedJoins,before,after);
    EarlyRejectedMatrix();ULONG rejectedWarm=EarlyRejectedCases;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));EarlyRejectedMatrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after && !TestHandles && EarlyRejectedJoins==EarlyRejectedCases);
    printf("PASS: %lu early-terminal rejected-policy warmup/%lu measured cases; constructor joins%lu, SDK first error preserved through synchronous rollback, process handles%lu/%lu\n",rejectedWarm,EarlyRejectedCases-rejectedWarm,EarlyRejectedJoins,before,after);
    CHECK(CloseHandle(PoolClosed));
    puts("PASS: actual v2 open failures preserve Windows errors across destructive cleanup; 450 rollback/close cycles, joined callbacks, zero owned events/maps/heap/pools/work/module pins and native admission residue.");return 0;
}
