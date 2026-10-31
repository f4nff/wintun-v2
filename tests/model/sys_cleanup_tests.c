/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted DDI stand-ins execute the driver's actual cleanup routine, without loading a SYS. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while (0)
typedef struct _MOCK_EVENT { HANDLE Native; LONG References; } KEVENT, *PKEVENT;
typedef struct _MOCK_MDL { BOOLEAN Locked; UCHAR *Mapping; } MDL;
typedef struct _MOCK_RUNDOWN { SRWLOCK Lock; } EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
#define TUN_MEMORY_TAG 0
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
static volatile LONG Pools, ObjectReferences, ThreadHandles, Workers, LockedPages, Mdls;
static LONG WakeCalls;
static PB_KERNEL_SESSION *CurrentSession;
static HANDLE ReturnedTail, ReleaseTail, CleanupDone, CleanupAtRundown;
static VOID KeSetEvent(KEVENT *event, int priority, BOOLEAN wait)
{ (void)priority; (void)wait; WakeCalls++; CHECK(SetEvent(event->Native)); }
static VOID KeWaitForSingleObject(KEVENT *event, int reason, int mode, BOOLEAN alertable, VOID *timeout)
{ (void)reason; (void)mode; (void)alertable; (void)timeout; CHECK(WaitForSingleObject(event->Native,5000)==WAIT_OBJECT_0); }
static VOID ZwWaitForSingleObject(HANDLE thread, BOOLEAN alertable, VOID *timeout)
{ (void)alertable; (void)timeout; CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); }
static VOID ZwClose(HANDLE thread) { CHECK(CloseHandle(thread)); InterlockedDecrement(&ThreadHandles); }
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *refs)
{ if (CleanupAtRundown) SetEvent(CleanupAtRundown); AcquireSRWLockExclusive(&refs->Lock); ReleaseSRWLockExclusive(&refs->Lock); }
static VOID ObDereferenceObject(PKEVENT event)
{
    CHECK(event->References > 0); InterlockedDecrement(&ObjectReferences);
    if (!--event->References) { CHECK(CloseHandle(event->Native)); free(event); }
}
static VOID MmUnlockPages(MDL *mdl)
{ CHECK(mdl && mdl->Locked); mdl->Locked=FALSE; free(mdl->Mapping); mdl->Mapping=NULL; InterlockedDecrement(&LockedPages); }
static VOID IoFreeMdl(MDL *mdl)
{ CHECK(!mdl->Locked && !mdl->Mapping); free(mdl); InterlockedDecrement(&Mdls); }
static VOID ExFreePoolWithTag(VOID *pointer, ULONG tag)
{
    (void)tag;
    if (pointer == CurrentSession) {
        CHECK(!Workers && !ObjectReferences && !ThreadHandles && !LockedPages && !Mdls && Pools == 1);
        CHECK(CloseHandle(CurrentSession->StartEvent.Native));
        CHECK(CloseHandle(CurrentSession->StopEvent.Native));
        CHECK(CloseHandle(CurrentSession->AllReturned.Native));
        CurrentSession = NULL;
    }
    free(pointer); InterlockedDecrement(&Pools);
}
struct _TUN_CTX { struct { volatile LONG V2SessionActive; } Probe; };
#include "../../driver/v2/session_fault.h"
#include "../../driver/v2/session_cleanup.h"
static VOID *Pool(SIZE_T size) { VOID *p=calloc(1,size); CHECK(p); InterlockedIncrement(&Pools); return p; }
static PKEVENT ObjectEvent(VOID)
{
    PKEVENT e=calloc(1,sizeof(*e)); CHECK(e); e->Native=CreateEventW(NULL,FALSE,FALSE,NULL); CHECK(e->Native);
    e->References=1; InterlockedIncrement(&ObjectReferences); return e;
}
static DWORD WINAPI Worker(VOID *arg)
{
    PB_KERNEL_SESSION *s=arg;
    CHECK(WaitForSingleObject(s->StartEvent.Native,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(s->StopEvent.Native,5000)==WAIT_OBJECT_0);
    CHECK(s->Stopping); InterlockedDecrement(&Workers); return 0;
}
/* Every acquisition prefix represents a registration failure at the next operation. */
static PB_KERNEL_SESSION *AcquirePrefix(ULONG limit)
{
    CHECK(!CurrentSession && !Pools && !ThreadHandles && !Workers && !LockedPages && !Mdls && !ObjectReferences);
    PB_KERNEL_SESSION *s=Pool(sizeof(*s)); CurrentSession=s; s->Layout.QueueCount=8;
    s->StartEvent.Native=CreateEventW(NULL,TRUE,FALSE,NULL); s->StopEvent.Native=CreateEventW(NULL,TRUE,FALSE,NULL);
    s->AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(s->StartEvent.Native && s->StopEvent.Native && s->AllReturned.Native);
    InitializeSRWLock(&s->Returns.Lock);
    ULONG step=0;
#define STEP(code) do { if(step++ >= limit) return s; code; } while(0)
    STEP(s->Mdl=calloc(1,sizeof(*s->Mdl)); CHECK(s->Mdl); InterlockedIncrement(&Mdls));
    STEP(s->Mdl->Locked=TRUE; s->PagesLocked=TRUE; InterlockedIncrement(&LockedPages));
    STEP(s->Mdl->Mapping=calloc(1,4096); CHECK(s->Mdl->Mapping); s->Region=s->Mdl->Mapping);
    STEP(s->ProgressEvent=ObjectEvent(); s->Lifecycle=(PB_SHARED_LIFECYCLE *)(s->Region+320));
    for (ULONG i=0;i<s->Layout.QueueCount;++i) {
        PB_KERNEL_QUEUE *q=s->Queues+i; q->Session=s;
        STEP(q->KernelEvent=ObjectEvent()); STEP(q->UserEvent=ObjectEvent());
        STEP(q->TxSlots=Pool(8)); STEP(q->RxSlots=Pool(8)); STEP(q->Pending=Pool(8));
        STEP(q->Scratch=Pool(65536)); STEP(q->CoalesceBuffers[0]=Pool(65536)); STEP(q->CoalesceBuffers[1]=Pool(65536));
        STEP(InterlockedIncrement(&Workers); q->Thread=CreateThread(NULL,0,Worker,s,0,NULL); CHECK(q->Thread); InterlockedIncrement(&ThreadHandles));
    }
#undef STEP
    return s;
}
static DWORD WINAPI LastReturn(VOID *arg)
{
    PB_KERNEL_SESSION *s=arg; AcquireSRWLockShared(&s->Returns.Lock);
    InterlockedExchange(&s->Active,0); CHECK(SetEvent(s->AllReturned.Native)); CHECK(SetEvent(ReturnedTail));
    CHECK(WaitForSingleObject(ReleaseTail,5000)==WAIT_OBJECT_0);
    ReleaseSRWLockShared(&s->Returns.Lock); return 0;
}
static DWORD WINAPI Cleanup(VOID *arg) { PbV2Free(arg); CHECK(SetEvent(CleanupDone)); return 0; }
int main(VOID)
{
    DWORD initialHandles, finalHandles; CHECK(GetProcessHandleCount(GetCurrentProcess(),&initialHandles));
    /* Includes MDL allocation only, locked but mapping failed, every object/allocation/thread failure. */
    for (ULONG prefix=0;prefix<=76;++prefix) {
        PbV2Free(AcquirePrefix(prefix)); CHECK(!CurrentSession && !Pools && !Workers && !ThreadHandles && !ObjectReferences && !Mdls && !LockedPages);
    }
    PB_KERNEL_SESSION *s=AcquirePrefix(76);
    /* A failed aliased-event registration still owns two references to the same object. */
    ObDereferenceObject(s->Queues[0].UserEvent);
    s->Queues[0].UserEvent=s->Queues[0].KernelEvent;
    s->Queues[0].KernelEvent->References++; InterlockedIncrement(&ObjectReferences);
    PbV2Free(s); CHECK(!Pools && !ObjectReferences);
    s=AcquirePrefix(76);
    ObDereferenceObject(s->ProgressEvent);
    s->ProgressEvent=s->Queues[0].KernelEvent;
    s->ProgressEvent->References++; InterlockedIncrement(&ObjectReferences);
    PbV2Free(s); CHECK(!Pools && !ObjectReferences);
    s=AcquirePrefix(76);
    struct _TUN_CTX context={0}; context.Probe.V2SessionActive=1; s->Ctx=&context;
    LONG wakes=WakeCalls; PbV2Fault(s);
    CHECK(s->Faulted && s->Draining && !context.Probe.V2SessionActive && WakeCalls-wakes==17);
    CHECK(s->Lifecycle->Terminal && s->Lifecycle->Sequence==1);
    PbV2Fault(s); CHECK(WakeCalls-wakes==17); /* A repeated corrupt-ring observation cannot self-wake forever. */
    PbV2Free(s);
    s=AcquirePrefix(76); s->Active=1; CHECK(ResetEvent(s->AllReturned.Native));
    ReturnedTail=CreateEventW(NULL,TRUE,FALSE,NULL); ReleaseTail=CreateEventW(NULL,TRUE,FALSE,NULL); CleanupDone=CreateEventW(NULL,TRUE,FALSE,NULL);
    CleanupAtRundown=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(ReturnedTail && ReleaseTail && CleanupDone && CleanupAtRundown);
    HANDLE callback=CreateThread(NULL,0,LastReturn,s,0,NULL); CHECK(callback);
    CHECK(WaitForSingleObject(ReturnedTail,5000)==WAIT_OBJECT_0);
    HANDLE cleanup=CreateThread(NULL,0,Cleanup,s,0,NULL); CHECK(cleanup);
    CHECK(WaitForSingleObject(CleanupAtRundown,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(CleanupDone,40)==WAIT_TIMEOUT);
    CHECK(CurrentSession==s && s->Mdl->Locked); /* AllReturned is set, but callback still uses the session. */
    CHECK(SetEvent(ReleaseTail)); CHECK(WaitForSingleObject(CleanupDone,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(callback,5000)==WAIT_OBJECT_0 && WaitForSingleObject(cleanup,5000)==WAIT_OBJECT_0);
    CloseHandle(callback); CloseHandle(cleanup); CloseHandle(ReturnedTail); CloseHandle(ReleaseTail); CloseHandle(CleanupDone);
    CloseHandle(CleanupAtRundown); CleanupAtRundown=NULL;
    CHECK(!Pools && !Workers && !ThreadHandles && !ObjectReferences && !Mdls && !LockedPages);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&finalHandles)); CHECK(finalHandles==initialHandles);
    puts("PASS: actual SYS cleanup, 77 acquisition prefixes, progress-event/queue alias rollback, one-shot fault wake, 8 workers joined, delayed return rundown, zero tracked resources and handle delta.");
    return 0;
}
