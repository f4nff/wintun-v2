/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Register/worker/Free early-stop handshake, no packet processing.
 * Kernel DDIs are hosted with real SDK events/locks and joined OS threads. */
#ifdef PB_API_TESTS
#error Worker startup requires shipping kernel paths
#endif
#ifndef NDEBUG
#error Worker startup must exercise shipping NDEBUG paths
#endif
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include "../../common/v2_core.h"
static volatile LONG UnexpectedBody=1;
static HANDLE Selected[PB_MAX_QUEUES],Gate[PB_MAX_QUEUES],WaitEntry[PB_MAX_QUEUES];
static HANDLE FailureEntered,FailureGate,FreeJoinEntered,Registered,CleanupGate;
static volatile LONG StartCalls[PB_MAX_QUEUES],ReturnedCalls[PB_MAX_QUEUES],Terminated[PB_MAX_QUEUES];
static volatile LONG FailureCalls,JoinProbes;
static __declspec(thread) ULONG WorkerIndex;
static ULONG Stage,ExpectedWorkers,HarnessHandles,HarnessThreads,Completed,Rescues;
static BOOLEAN SuccessCase;
static LONG Outcome;
#include PB_WORKER_START_HOST
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"worker startup requires shipping source fingerprint");
static ULONG WorkerJoins;
static VOID Matrix(VOID);
static HANDLE MakeEvent(VOID){HANDLE h=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(h);++HarnessHandles;return h;}
static VOID DropEvent(HANDLE h){CHECK(h&&CloseHandle(h)&&HarnessHandles);--HarnessHandles;}
static VOID HostBeforeWorker(PVOID pointer)
{
    PB_KERNEL_QUEUE *q=pointer;WorkerIndex=q->Index;CHECK(WorkerIndex<ExpectedWorkers&&q->Session==Current);
    if(!Stage){CHECK(SetEvent(Selected[WorkerIndex]));Await(Gate[WorkerIndex]);}
}
static VOID HostStartWait(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    CHECK(Current&&event==&Current->StartEvent&&!reason&&!mode&&!alertable&&!timeout&&InterlockedIncrement(StartCalls+WorkerIndex)==1);
    if(!SuccessCase)CHECK(WaitForSingleObject(event->Native,0)==(DWORD)(Stage?WAIT_TIMEOUT:WAIT_OBJECT_0));
    CHECK(SetEvent(WaitEntry[WorkerIndex]));
    if(Stage==1){CHECK(SetEvent(Selected[WorkerIndex]));Await(Gate[WorkerIndex]);}
    KeWaitForSingleObject(event,reason,mode,alertable,timeout);CHECK(InterlockedIncrement(ReturnedCalls+WorkerIndex)==1);
    if(Stage==2){CHECK(SetEvent(Selected[WorkerIndex]));Await(Gate[WorkerIndex]);}
}
static VOID HostClear(KEVENT *event){(void)event;CHECK(!UnexpectedBody);}
static PB_RESULT HostRecycle(PB_KERNEL_QUEUE *q){CHECK(!UnexpectedBody);return PbV2RecycleLocked(q);}
static VOID HostFlush(PB_KERNEL_QUEUE *q){CHECK(!UnexpectedBody);PbV2FlushLocked(q);}
static PB_RESULT HostArm(PB_QUEUE_VIEW *view,uint32_t *ready){CHECK(!UnexpectedBody);return PbQueueArm(view,ready);}
static VOID HostMultipleWait(ULONG n,VOID **events,int type,int reason,int mode,BOOLEAN alertable,VOID *timeout,VOID *blocks)
{(void)n;(void)events;(void)type;(void)reason;(void)mode;(void)alertable;(void)timeout;(void)blocks;CHECK(!UnexpectedBody);}
static VOID HostTerminate(NTSTATUS status)
{
    CHECK(status==STATUS_SUCCESS&&Current&&Current->Stopping&&StartCalls[WorkerIndex]==1&&ReturnedCalls[WorkerIndex]==1&&InterlockedIncrement(Terminated+WorkerIndex)==1);
    InterlockedIncrement((LONG *)&ThreadsExited);InterlockedDecrement(&Workers);
}
static VOID FailureBoundary(VOID)
{CHECK(!SuccessCase&&Current&&InterlockedIncrement(&FailureCalls)==1&&SetEvent(FailureEntered));Await(FailureGate);}
static VOID *ObservedAllocate(ULONG kind,SIZE_T bytes,ULONG tag){VOID *p=ExAllocatePoolZero(kind,bytes,tag);if(!p)FailureBoundary();return p;}
static NTSTATUS ObservedReference(HANDLE handle,ACCESS_MASK access,VOID *type,UCHAR mode,PKEVENT *out,VOID *info)
{NTSTATUS s=ObReferenceObjectByHandle(handle,access,type,mode,out,info);if(!NT_SUCCESS(s))FailureBoundary();return s;}
static NTSTATUS ObservedCreate(HANDLE *thread,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,HANDLE process,VOID *id,VOID (*routine)(PVOID),PVOID input)
{NTSTATUS s=PsCreateSystemThread(thread,access,attributes,process,id,routine,input);if(!NT_SUCCESS(s))FailureBoundary();return s;}
static VOID ObservedJoin(HANDLE thread,BOOLEAN alertable,VOID *timeout)
{CHECK(Current&&Current->Stopping&&WaitForSingleObject(Current->StopEvent.Native,0)==WAIT_OBJECT_0);if(InterlockedIncrement(&JoinProbes)==1)CHECK(SetEvent(FreeJoinEntered));ZwWaitForSingleObject(thread,alertable,timeout);}
static DWORD WINAPI Caller(VOID *unused)
{(void)unused;Outcome=Register();if(SuccessCase){CHECK(NT_SUCCESS(Outcome)&&SetEvent(Registered));Await(CleanupGate);}Cleanup();return 0;}
static VOID RunCase(ULONG queues,ULONG failure,ULONG stage,BOOLEAN manual,BOOLEAN negative)
{
    SuccessCase=!failure;Stage=stage;ExpectedWorkers=SuccessCase?queues:queues-1;FailureCalls=JoinProbes=0;
    memset((VOID *)StartCalls,0,sizeof(StartCalls));memset((VOID *)ReturnedCalls,0,sizeof(ReturnedCalls));memset((VOID *)Terminated,0,sizeof(Terminated));
    Begin(queues,manual);Context.Probe.RunningEvent.Native=MakeEvent();
    FailureEntered=MakeEvent();FailureGate=MakeEvent();FreeJoinEntered=MakeEvent();Registered=MakeEvent();CleanupGate=MakeEvent();
    for(ULONG i=0;i<ExpectedWorkers;++i){Selected[i]=MakeEvent();Gate[i]=MakeEvent();WaitEntry[i]=MakeEvent();}
    if(failure==1)FailPool=3+6*(queues-1);else if(failure==2)FailOb=1+2*queues;else if(failure==3)FailThread=queues;
    HANDLE caller=CreateThread(NULL,0,Caller,NULL,0,NULL);CHECK(caller);++HarnessHandles;++HarnessThreads;
    if(SuccessCase)Await(Registered);else Await(FailureEntered);
    for(ULONG i=0;i<ExpectedWorkers;++i)Await(Stage==2?WaitEntry[i]:Selected[i]);
    CHECK(Current&&!Current->Stopping&&Workers==(LONG)ExpectedWorkers&&ThreadsMade==ExpectedWorkers);
    CHECK(SuccessCase?(Context.V2==Current&&Starts==1&&EverPublished):(!Context.V2&&!Starts&&!EverPublished&&!StartBeforePublish));
    CHECK(SetEvent(SuccessCase?CleanupGate:FailureGate));
    if(ExpectedWorkers){
        Await(FreeJoinEntered);CHECK(Current&&Current->Stopping&&Workers==(LONG)ExpectedWorkers&&Locked==1&&Mapped==1&&Mdls==1);
        if(negative){CHECK(Stage==2&&!SuccessCase&&WaitForSingleObject(Current->StartEvent.Native,0)==WAIT_TIMEOUT);KeSetEvent(&Current->StartEvent,0,FALSE);++Rescues;}
        CHECK(WaitForSingleObject(Current->StartEvent.Native,0)==WAIT_OBJECT_0);
        for(ULONG i=0;i<ExpectedWorkers;++i){Await(Selected[i]);CHECK(!Terminated[i]&&StartCalls[i]==(LONG)(Stage!=0)&&ReturnedCalls[i]==(LONG)(Stage==2));}
        CHECK(!ThreadsExited&&!CleanupJoins);for(ULONG i=0;i<ExpectedWorkers;++i)CHECK(SetEvent(Gate[i]));
    }
    Await(caller);DropEvent(caller);--HarnessThreads;
    CHECK(Outcome==(failure==2?STATUS_ACCESS_DENIED:failure?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS));
    CHECK(!Current&&!Context.V2&&!Pools&&!Mdls&&!Locked&&!Mapped&&!References&&!WorkerHandles&&!Workers&&!Handles&&ThreadsMade==ExpectedWorkers&&ThreadsExited==ExpectedWorkers&&CleanupJoins==ExpectedWorkers&&JoinProbes==(LONG)ExpectedWorkers);
    for(ULONG i=0;i<ExpectedWorkers;++i){CHECK(StartCalls[i]==1&&ReturnedCalls[i]==1&&Terminated[i]==1);DropEvent(Selected[i]);DropEvent(Gate[i]);DropEvent(WaitEntry[i]);}
    DropEvent(Context.Probe.RunningEvent.Native);Context.Probe.RunningEvent.Native=NULL;DropEvent(FailureEntered);DropEvent(FailureGate);DropEvent(FreeJoinEntered);DropEvent(Registered);DropEvent(CleanupGate);
    CHECK(!HarnessHandles&&!HarnessThreads&&FailureCalls==(LONG)!SuccessCase);++Completed;WorkerJoins+=ExpectedWorkers;
}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"131");CHECK(argc==1||selected);
#ifdef PB_WORKER_START_NEGATIVE
    BOOLEAN negative=TRUE;CHECK(selected);
#else
    BOOLEAN negative=FALSE;
#endif
    if(selected){RunCase(8,3,2,FALSE,negative);RunCase(8,3,2,FALSE,negative);}else Matrix();
    ULONG warmed=Completed,warmRescues=Rescues;Completed=Rescues=WorkerJoins=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)RunCase(8,3,2,FALSE,negative);else Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&Scenarios==warmed+Completed);
    CHECK(Rescues==(negative?Completed:0)&&warmRescues==(negative?warmed:0));
    printf("PASS: %lu actual worker/Register/Free startup cases plus %lu checked warmup; %lu worker starts/terminations/joins, zero clear/recycle/flush/receive/arm/multiple-wait, every caller joined and owned resources zero; handles%lu/%lu; rescues%lu/%lu\n",Completed,warmed,WorkerJoins,before,after,Rescues,warmRescues);
    if(negative){puts("WORKER_ROLLBACK_START_WAKE_MISSING: Free published Stopping and StopEvent but Start remained unsignaled; one original Start-event rescue per negative case, actual worker early exit and complete joins/reclamation before131");return 131;}return 0;
}
static VOID Matrix(VOID)
{const ULONG queues[]={1,2,8};for(ULONG q=0;q<3;++q)for(ULONG f=0;f<4;++f)for(ULONG stage=0;stage<3;++stage)for(ULONG manual=0;manual<2;++manual){if(queues[q]==1&&f&&stage)continue;RunCase(queues[q],f,stage,(BOOLEAN)manual,FALSE);}}
