/* SPDX-License-Identifier: GPL-2.0 */
/* Actual namespace control flow; no project namespace/device is created.
 * Descriptor/SID, mutex, event, lock and thread objects use real Win7 APIs. */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../api/logger.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static BOOL WINAPI TestSid(WELL_KNOWN_SID_TYPE,PSID,PSID,DWORD *);
static HANDLE WINAPI TestBoundary(LPCWSTR,ULONG);
static BOOL WINAPI TestAddSid(HANDLE *,PSID);
static VOID WINAPI TestDeleteBoundary(HANDLE);
static HANDLE WINAPI TestCreateNamespace(LPSECURITY_ATTRIBUTES,LPVOID,LPCWSTR);
static HANDLE WINAPI TestOpenNamespace(LPVOID,LPCWSTR);
static BOOLEAN WINAPI TestCloseNamespace(HANDLE,ULONG);
static HANDLE WINAPI TestMutex(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR);
static BOOL WINAPI TestClose(HANDLE);
static ULONGLONG WINAPI TestTick(VOID);
static DWORD CaptureLog(DWORD);
#define CreateWellKnownSid TestSid
#define CreateBoundaryDescriptorW TestBoundary
#define AddSIDToBoundaryDescriptor TestAddSid
#define DeleteBoundaryDescriptor TestDeleteBoundary
#define CreatePrivateNamespaceW TestCreateNamespace
#define OpenPrivateNamespaceW TestOpenNamespace
#define ClosePrivateNamespace TestCloseNamespace
#define CreateMutexW TestMutex
#define CloseHandle TestClose
#define GetTickCount64 TestTick
#ifndef PB_NAMESPACE_SOURCE
#define PB_NAMESPACE_SOURCE "../../api/namespace.c"
#endif
#include PB_NAMESPACE_SOURCE
#undef CreateWellKnownSid
#undef CreateBoundaryDescriptorW
#undef AddSIDToBoundaryDescriptor
#undef DeleteBoundaryDescriptor
#undef CreatePrivateNamespaceW
#undef OpenPrivateNamespaceW
#undef ClosePrivateNamespace
#undef CreateMutexW
#undef CloseHandle
#undef GetTickCount64
SECURITY_ATTRIBUTES SecurityAttributes={sizeof(SecurityAttributes),NULL,FALSE};
BOOL IsLocalSystem;
static volatile LONG DescriptorOwned,NamespaceOwned,MutexOwned,ThreadsOwned;
static DWORD FailAt,FailureError,CreateCalls,OpenCalls,DelayCalls,DelayTotal,LogCalls,Scenarios,MutexCalls;
static DWORD Races,CreateLatency,OpenError,MutexError,WaitError,OperationError,StopAt;
static BOOL ForceCreateFailure,LogProbe,LogUnlocked,BlockDelay,ForceOperationFailure,ForceWaitFailure;
static DWORD OperationBudget;
static volatile LONG64 Clock;
static HANDLE StopEvent,CreateEntered,CreateGate,DelayEntered;
static HANDLE LiveBoundary;
static ULONGLONG WINAPI TestTick(VOID){return (ULONGLONG)InterlockedCompareExchange64(&Clock,0,0);}
static BOOL Fail(DWORD step)
{if(FailAt!=step)return FALSE;SetLastError(FailureError);return TRUE;}
static BOOL WINAPI TestSid(WELL_KNOWN_SID_TYPE type,PSID domain,PSID sid,DWORD *size)
{CHECK(type==(IsLocalSystem?WinLocalSystemSid:WinBuiltinAdministratorsSid)&&!domain);return Fail(1)?FALSE:CreateWellKnownSid(type,domain,sid,size);}
static HANDLE WINAPI TestBoundary(LPCWSTR name,ULONG flags)
{
    CHECK(!wcscmp(name,L"PacketBridgeV2")&&!flags&&!DescriptorOwned);
    if(Fail(2))return NULL;LiveBoundary=CreateBoundaryDescriptorW(name,flags);CHECK(LiveBoundary);InterlockedIncrement(&DescriptorOwned);return LiveBoundary;
}
static BOOL WINAPI TestAddSid(HANDLE *descriptor,PSID sid)
{CHECK(*descriptor==LiveBoundary&&DescriptorOwned==1&&IsValidSid(sid));if(Fail(3))return FALSE;BOOL okay=AddSIDToBoundaryDescriptor(descriptor,sid);CHECK(okay);LiveBoundary=*descriptor;return TRUE;}
static VOID WINAPI TestDeleteBoundary(HANDLE descriptor)
{CHECK(descriptor==LiveBoundary&&DescriptorOwned==1);DeleteBoundaryDescriptor(descriptor);LiveBoundary=NULL;InterlockedDecrement(&DescriptorOwned);SetLastError(ERROR_INVALID_HANDLE);}
static HANDLE WINAPI TestCreateNamespace(LPSECURITY_ATTRIBUTES security,LPVOID boundary,LPCWSTR name)
{
    CHECK(security==&SecurityAttributes&&boundary==LiveBoundary&&!wcscmp(name,L"PacketBridgeV2"));
    ++CreateCalls;CHECK(CreateCalls<1000);InterlockedAdd64(&Clock,CreateLatency);
    if(StopAt==1)CHECK(SetEvent(StopEvent));
    if(CreateGate){CHECK(SetEvent(CreateEntered)&&WaitForSingleObject(CreateGate,5000)==WAIT_OBJECT_0);}
    if(Fail(4))return NULL;
    if(CreateCalls<=Races){SetLastError(ERROR_ALREADY_EXISTS);return NULL;}
    if(ForceCreateFailure){SetLastError(ERROR_ACCESS_DENIED);return NULL;}
    HANDLE result=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(result&&!NamespaceOwned);InterlockedIncrement(&NamespaceOwned);return result;
}
static HANDLE WINAPI TestOpenNamespace(LPVOID boundary,LPCWSTR name)
{
    CHECK(boundary==LiveBoundary&&!wcscmp(name,L"PacketBridgeV2"));++OpenCalls;
    if(StopAt==2)CHECK(SetEvent(StopEvent));
    if(OpenError){SetLastError(OpenError);return NULL;}
    HANDLE result=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(result&&!NamespaceOwned);InterlockedIncrement(&NamespaceOwned);return result;
}
static BOOLEAN WINAPI TestCloseNamespace(HANDLE space,ULONG flags)
{CHECK(space&&NamespaceOwned==1&&!flags&&CloseHandle(space));InterlockedDecrement(&NamespaceOwned);SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static HANDLE WINAPI TestMutex(LPSECURITY_ATTRIBUTES security,BOOL owned,LPCWSTR name)
{
    CHECK(security==&SecurityAttributes&&!owned&&name);InterlockedIncrement((LONG *)&MutexCalls);
    if(MutexError){SetLastError(MutexError);return NULL;}
    HANDLE result=CreateMutexW(NULL,FALSE,NULL);CHECK(result);InterlockedIncrement(&MutexOwned);return result;
}
static BOOL WINAPI TestClose(HANDLE handle)
{CHECK(MutexOwned>0&&CloseHandle(handle));InterlockedDecrement(&MutexOwned);SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
BOOL MainOperationSleep(DWORD delay)
{
    if(OperationError||ForceOperationFailure){SetLastError(OperationError);return FALSE;}
    if(OperationBudget) {
        if(TestTick()>=OperationBudget){SetLastError(ERROR_TIMEOUT);return FALSE;}
        delay=min(delay,OperationBudget-(DWORD)TestTick());
    }
    if(delay){++DelayCalls;DelayTotal+=delay;InterlockedAdd64(&Clock,delay);}
    if(OperationBudget&&TestTick()>=OperationBudget){SetLastError(ERROR_TIMEOUT);return FALSE;}
    return TRUE;
}
BOOL MainWaitForObject(HANDLE object,HANDLE stop,DWORD timeout)
{
    if(OperationError){SetLastError(OperationError);return FALSE;}
    if(WaitError||ForceWaitFailure){SetLastError(WaitError);return FALSE;}
    if(stop&&WaitForSingleObject(stop,0)==WAIT_OBJECT_0){SetLastError(ERROR_CANCELLED);return FALSE;}
    if(object==StopEvent) {
        if(timeout){++DelayCalls;DelayTotal+=timeout;InterlockedAdd64(&Clock,timeout);}
        if(BlockDelay&&timeout){CHECK(SetEvent(DelayEntered));DWORD waited=WaitForSingleObject(object,5000);CHECK(waited==WAIT_OBJECT_0);return TRUE;}
        if(WaitForSingleObject(object,0)==WAIT_OBJECT_0)return TRUE;
        SetLastError(ERROR_TIMEOUT);return FALSE;
    }
    CHECK(object&&MutexOwned>0);DWORD waited=WaitForSingleObject(object,0);
    if(waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED)return TRUE;
    SetLastError(ERROR_TIMEOUT);return FALSE;
}
static DWORD WINAPI ProbeLock(VOID *opaque)
{BOOL *okay=opaque;*okay=TryEnterCriticalSection(&Initializing);if(*okay)LeaveCriticalSection(&Initializing);return 0;}
static DWORD CaptureLog(DWORD error)
{
    ++LogCalls;
    if(LogProbe) {
        BOOL unlocked=FALSE;HANDLE thread=CreateThread(NULL,0,ProbeLock,&unlocked,0,NULL);CHECK(thread);++ThreadsOwned;
        CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&CloseHandle(thread));--ThreadsOwned;LogUnlocked=unlocked;
    }
    SetLastError(ERROR_INVALID_DATA);return error;
}
/* Keep logger.h's real LOG_LAST_ERROR wrapper: the callback may overwrite the
 * thread error, but LoggerLastErrorV restores the captured API failure. */
_Use_decl_annotations_
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{(void)format;(void)args;return CaptureLog(error);}
static VOID Begin(VOID)
{
    CHECK(!DescriptorOwned&&!NamespaceOwned&&!MutexOwned&&!ThreadsOwned);
    /* Old failed initialization leaves a deleted descriptor in this private
     * field. Clear only the test shell after its tracked owner is gone. */
    PrivateNamespace=BoundaryDescriptor=NULL;
    FailAt=CreateCalls=OpenCalls=DelayCalls=DelayTotal=LogCalls=MutexCalls=0;
    Races=CreateLatency=OpenError=MutexError=WaitError=OperationError=StopAt=0;
    FailureError=ERROR_NOT_ENOUGH_MEMORY;ForceCreateFailure=LogProbe=LogUnlocked=BlockDelay=FALSE;
    ForceOperationFailure=ForceWaitFailure=FALSE;OperationBudget=0;
    Clock=0;StopEvent=CreateEntered=CreateGate=DelayEntered=NULL;IsLocalSystem=FALSE;NamespaceInit();
}
static VOID End(VOID)
{NamespaceDone();CHECK(!DescriptorOwned&&!NamespaceOwned&&!MutexOwned&&!ThreadsOwned);++Scenarios;}
static HANDLE Take(HANDLE stop)
{return NamespaceTakeDeviceInstallationMutexCancelable(stop);}
static VOID Release(HANDLE mutex)
{CHECK(mutex);NamespaceReleaseMutex(mutex);CHECK(!MutexOwned);}
static VOID Control(DWORD kind)
{
    CHECK(kind>=1&&kind<=6);
    Begin();
    if(kind<=2){Races=kind==1?2:1;OpenError=ERROR_PATH_NOT_FOUND;ForceCreateFailure=TRUE;}
    if(kind==2){StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);StopAt=1;}
    if(kind==3){FailAt=4;LogProbe=TRUE;}
    if(kind==4)MutexError=ERROR_ACCESS_DENIED;
    if(kind==5){Races=1;StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);StopAt=1;}
    if(kind==6){Races=1;CreateLatency=15000;}
    HANDLE mutex=Take(StopEvent);DWORD error=GetLastError();if(mutex)Release(mutex);
    BOOL failed=kind==1?CreateCalls>1&&!DelayCalls:kind==2?CreateCalls>1||error!=ERROR_CANCELLED:kind==3?!LogUnlocked:kind==4?mutex!=NULL||error!=ERROR_ACCESS_DENIED||LogCalls!=1:OpenCalls!=0;
    if(StopEvent)CHECK(CloseHandle(StopEvent));End();
    if(failed){const char *markers[]={"","NAMESPACE_RETRY_WITHOUT_BACKOFF","EXPLICIT_STOP_IGNORED_DURING_NAMESPACE_RETRY","LOGGER_CALLED_UNDER_NAMESPACE_INIT_LOCK","MUTEX_CREATE_ERROR_NOT_PRESERVED","OPEN_ENTERED_AFTER_STOP","OPEN_ENTERED_AFTER_NAMESPACE_DEADLINE"};fprintf(stderr,"%s\n",markers[kind]);ExitProcess(70+kind);}
    if(kind==4)puts("MUTEX_CREATE_ERROR_PRESERVED");
}
static VOID Matrix(VOID)
{
    const DWORD errors[]={0,ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_TOO_MANY_OPEN_FILES};
    for(DWORD system=0;system<2;++system)for(DWORD step=1;step<=4;++step)for(DWORD e=0;e<ARRAYSIZE(errors);++e) {
        Begin();IsLocalSystem=system;FailAt=step;FailureError=errors[e];CHECK(!Take(NULL));
        CHECK(GetLastError()==(errors[e]?errors[e]:ERROR_GEN_FAILURE)&&!PrivateNamespace&&!BoundaryDescriptor&&!MutexCalls);
        FailAt=0;HANDLE retry=Take(NULL);Release(retry);CHECK(PrivateNamespace&&BoundaryDescriptor&&NamespaceOwned==1&&DescriptorOwned==1);End();
    }
    for(DWORD races=0;races<4;++races) {
        Begin();Races=races;OpenError=ERROR_PATH_NOT_FOUND;HANDLE mutex=Take(NULL);Release(mutex);
        CHECK(CreateCalls==races+1&&OpenCalls==races&&DelayCalls==races&&DelayTotal==50*races&&DescriptorOwned==1&&NamespaceOwned==1);
        mutex=NamespaceTakeDriverInstallationMutex();Release(mutex);CHECK(CreateCalls==races+1);End();
    }
    Begin();Races=1;HANDLE mutex=Take(NULL);Release(mutex);CHECK(CreateCalls==1&&OpenCalls==1&&!DelayCalls);End();
    const DWORD openErrors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INVALID_HANDLE};
    for(DWORD i=0;i<ARRAYSIZE(openErrors);++i){Begin();Races=1;OpenError=openErrors[i];CHECK(!Take(NULL)&&GetLastError()==openErrors[i]&&!DelayCalls);End();}
    for(DWORD wrap=0;wrap<3;++wrap) {
        Begin();Clock=wrap==1?(LONG64)MAXDWORD-100:wrap==2?-100:0;Races=10000;OpenError=ERROR_PATH_NOT_FOUND;
        CHECK(!Take(NULL)&&GetLastError()==ERROR_TIMEOUT&&CreateCalls==300&&DelayCalls==300&&DelayTotal==15000);End();
    }
    Begin();Races=10000;OpenError=ERROR_PATH_NOT_FOUND;CreateLatency=10000;
    CHECK(!Take(NULL)&&GetLastError()==ERROR_TIMEOUT&&CreateCalls==2&&DelayCalls==1);End();
    Begin();StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);StopAt=1;Races=1;
    CHECK(!Take(StopEvent)&&GetLastError()==ERROR_CANCELLED&&CreateCalls==1&&!OpenCalls&&!MutexCalls);CHECK(CloseHandle(StopEvent));End();
    Begin();Races=1;CreateLatency=15000;
    CHECK(!Take(NULL)&&GetLastError()==ERROR_TIMEOUT&&CreateCalls==1&&!OpenCalls&&!MutexCalls);End();
    Begin();Races=10000;OpenError=ERROR_PATH_NOT_FOUND;OperationBudget=75;
    CHECK(!Take(NULL)&&GetLastError()==ERROR_TIMEOUT&&CreateCalls==2&&DelayCalls==2&&DelayTotal==75);End();
    for(DWORD latency=14999;latency<=15001;++latency) {
        Begin();CreateLatency=latency;mutex=Take(NULL);
        if(latency==14999){Release(mutex);CHECK(NamespaceOwned==1);}
        else CHECK(!mutex&&GetLastError()==ERROR_TIMEOUT&&!PrivateNamespace&&!BoundaryDescriptor&&!MutexCalls);
        End();
    }
    Begin();mutex=Take(NULL);Release(mutex);StopEvent=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(StopEvent);
    CHECK(!Take(StopEvent)&&GetLastError()==ERROR_CANCELLED&&NamespaceOwned==1&&DescriptorOwned==1&&CreateCalls==1&&MutexCalls==1);
    CHECK(CloseHandle(StopEvent));End();
    for(DWORD at=0;at<3;++at) {
        Begin();StopEvent=CreateEventW(NULL,TRUE,at==0,NULL);CHECK(StopEvent);StopAt=at;
        if(at==2)Races=1;
        CHECK(!Take(StopEvent)&&GetLastError()==ERROR_CANCELLED&&!PrivateNamespace&&!BoundaryDescriptor&&!MutexCalls);
        CHECK(CreateCalls==(at?1u:0u));CHECK(CloseHandle(StopEvent));End();
    }
    Begin();StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);StopAt=2;Races=1;OpenError=ERROR_PATH_NOT_FOUND;
    CHECK(!Take(StopEvent)&&GetLastError()==ERROR_CANCELLED&&!PrivateNamespace&&!BoundaryDescriptor&&!MutexCalls);CHECK(CloseHandle(StopEvent));End();
    for(DWORD i=0;i<ARRAYSIZE(openErrors);++i) {
        Begin();OperationError=openErrors[i];CHECK(!Take(NULL)&&GetLastError()==openErrors[i]&&!CreateCalls);End();
        Begin();StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);WaitError=openErrors[i];
        CHECK(!Take(StopEvent)&&GetLastError()==openErrors[i]&&!CreateCalls);CHECK(CloseHandle(StopEvent));End();
    }
    Begin();ForceOperationFailure=TRUE;CHECK(!Take(NULL)&&GetLastError()==ERROR_GEN_FAILURE&&!CreateCalls);End();
    Begin();ForceWaitFailure=TRUE;StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent);
    CHECK(!Take(StopEvent)&&GetLastError()==ERROR_GEN_FAILURE&&!CreateCalls);CHECK(CloseHandle(StopEvent));End();
    Begin();ForceWaitFailure=TRUE;CHECK(!Take(NULL)&&GetLastError()==ERROR_GEN_FAILURE&&!MutexOwned);End();
    Begin();FailAt=4;LogProbe=TRUE;CHECK(!Take(NULL)&&GetLastError()==ERROR_NOT_ENOUGH_MEMORY&&LogUnlocked);End();
    for(DWORD phase=0;phase<2;++phase) {
        Begin();if(phase)WaitError=ERROR_ACCESS_DENIED;else MutexError=ERROR_ACCESS_DENIED;
        CHECK(!Take(NULL)&&GetLastError()==ERROR_ACCESS_DENIED&&NamespaceOwned==1&&DescriptorOwned==1&&!MutexOwned);
        WaitError=MutexError=0;mutex=Take(NULL);Release(mutex);CHECK(CreateCalls==1);End();
    }
}
typedef struct CALL {HANDLE Stop,Mutex;DWORD Error;} CALL;
static DWORD WINAPI TakeThread(VOID *opaque)
{CALL *call=opaque;call->Mutex=Take(call->Stop);call->Error=GetLastError();if(call->Mutex)NamespaceReleaseMutex(call->Mutex);return 0;}
static VOID Concurrent(BOOL cancel)
{
    Begin();StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CreateEntered=CreateEventW(NULL,TRUE,FALSE,NULL);CreateGate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent&&CreateEntered&&CreateGate);
    CALL calls[8]={0};HANDLE threads[8];
    for(DWORD i=0;i<8;++i){calls[i].Stop=StopEvent;threads[i]=CreateThread(NULL,0,TakeThread,calls+i,0,NULL);CHECK(threads[i]);++ThreadsOwned;}
    CHECK(WaitForSingleObject(CreateEntered,5000)==WAIT_OBJECT_0);if(cancel)CHECK(SetEvent(StopEvent));CHECK(SetEvent(CreateGate));
    CHECK(WaitForMultipleObjects(8,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(DWORD i=0;i<8;++i){CHECK(CloseHandle(threads[i]));--ThreadsOwned;CHECK(cancel?!calls[i].Mutex&&calls[i].Error==ERROR_CANCELLED:calls[i].Mutex!=NULL);}
    CHECK(CreateCalls==1&&!MutexOwned);if(cancel)CHECK(!NamespaceOwned&&!DescriptorOwned&&!MutexCalls);
    CHECK(CloseHandle(StopEvent)&&CloseHandle(CreateEntered)&&CloseHandle(CreateGate));End();
}
static VOID CancelBackoff(VOID)
{
    Begin();StopEvent=CreateEventW(NULL,TRUE,FALSE,NULL);DelayEntered=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(StopEvent&&DelayEntered);
    Races=10000;OpenError=ERROR_PATH_NOT_FOUND;BlockDelay=TRUE;CALL call={.Stop=StopEvent};HANDLE thread=CreateThread(NULL,0,TakeThread,&call,0,NULL);CHECK(thread);++ThreadsOwned;
    CHECK(WaitForSingleObject(DelayEntered,5000)==WAIT_OBJECT_0&&SetEvent(StopEvent)&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&CloseHandle(thread));--ThreadsOwned;
    CHECK(!call.Mutex&&call.Error==ERROR_CANCELLED&&CreateCalls==1&&DelayCalls==1);
    CHECK(CloseHandle(StopEvent)&&CloseHandle(DelayEntered));End();
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc<=2);if(argc==2)Control(wcstoul(argv[1],NULL,10));
    else {
        DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(DWORD pass=0;pass<3;++pass){Matrix();Concurrent(FALSE);Concurrent(TRUE);CancelBackoff();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);}
    }
    printf("PASS: %lu production namespace initialization schedules; bounded race retry, explicit/runtime stop, private rollback, no logger under init lock, all descriptors/namespace/mutex resources returned and all test threads joined.\n",Scenarios);return 0;
}
