/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);} } while(0)
#define NT_ASSERT CHECK
#define IO_NO_INCREMENT 0
typedef struct { HANDLE Native; } KEVENT,*PKEVENT,MDL,EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
typedef struct { KSPIN_LOCK *Lock; } KLOCK_QUEUE_HANDLE;
static VOID KeSetEvent(PKEVENT event,int priority,BOOLEAN wait) { (void)priority;(void)wait;CHECK(SetEvent(event->Native)); }
static VOID KeClearEvent(PKEVENT event) { CHECK(ResetEvent(event->Native)); }
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle) { CHECK(!*lock);*lock=1;handle->Lock=lock; }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle) { CHECK(*handle->Lock);*handle->Lock=0; }
typedef struct _TUN_CTX { struct { LONG V2SessionActive; } Probe; } TUN_CTX;
#include "../../driver/v2/session_fault.h"
#include "../../driver/v2/session_references.h"
#include "../../driver/v2/session_completion.h"
int main(void)
{
    PB_KERNEL_SESSION s={0}; PB_SHARED_LIFECYCLE lifecycle={0}; TUN_CTX ctx={0};
    KEVENT progress={CreateEventW(NULL,FALSE,FALSE,NULL)},user={CreateEventW(NULL,FALSE,FALSE,NULL)};
    KEVENT kernel={CreateEventW(NULL,FALSE,FALSE,NULL)};
    CHECK(progress.Native&&user.Native&&kernel.Native);
    s.Ctx=&ctx;s.Lifecycle=&lifecycle;s.ProgressEvent=&progress;s.Layout.QueueCount=1;
    s.Queues[0].UserEvent=&user;s.Queues[0].KernelEvent=&kernel;
    PbV2Progress(&s,FALSE);PbV2Progress(&s,FALSE);
    CHECK(lifecycle.Sequence==2&&!lifecycle.Terminal);
    CHECK(WaitForSingleObject(progress.Native,0)==WAIT_OBJECT_0&&WaitForSingleObject(progress.Native,0)==WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(user.Native,0)==WAIT_TIMEOUT); /* Never steal the data owner's event. */
    PbV2Fault(&s);CHECK(lifecycle.Sequence==3&&lifecycle.Terminal);
    CHECK(WaitForSingleObject(user.Native,0)==WAIT_OBJECT_0&&WaitForSingleObject(progress.Native,0)==WAIT_OBJECT_0);
    PbV2Fault(&s);CHECK(lifecycle.Sequence==3&&WaitForSingleObject(progress.Native,0)==WAIT_TIMEOUT);
    s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s.AllReturned.Native);
    PbV2RetainReceives(&s,2,2);PbV2ReleaseReference(&s);
    CHECK(WaitForSingleObject(progress.Native,0)==WAIT_TIMEOUT);
    PbV2ReleaseReference(&s);CHECK(lifecycle.Sequence==4&&lifecycle.Terminal&&WaitForSingleObject(progress.Native,0)==WAIT_OBJECT_0);
    PB_LAYOUT layout;CHECK(PbCalculateLayout(1,4,4,&layout)==PB_OK);
    /* The remaining check is a separate normal completion, not post-fault delivery. */
    PB_KERNEL_SESSION normal={0};normal.Ctx=&ctx;normal.Lifecycle=&lifecycle;normal.ProgressEvent=&progress;normal.Draining=1;
    void *region=VirtualAlloc(NULL,layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(region);
    PB_DESCRIPTOR pending[4]={0};PB_KERNEL_QUEUE *q=normal.Queues;
    normal.Layout=layout;q->Session=&normal;q->Pending=pending;q->PendingCount=q->Credits=1;
    CHECK(PbAttachQueue(&q->Rings[PB_QUEUE_RX_COMPLETE],region,&layout,0,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);
    PbV2FlushLocked(q);CHECK(!q->Credits&&!q->PendingCount&&normal.Completions==1);
    CHECK(lifecycle.Sequence==5&&WaitForSingleObject(progress.Native,0)==WAIT_OBJECT_0);
    CHECK(VirtualFree(region,0,MEM_RELEASE)&&CloseHandle(progress.Native)&&CloseHandle(user.Native)&&CloseHandle(kernel.Native)&&CloseHandle(s.AllReturned.Native));
    puts("PASS: production lifecycle hints, coalesced independent event, sticky terminal/fault, last receive reference and completion publication progress.");return 0;
}
