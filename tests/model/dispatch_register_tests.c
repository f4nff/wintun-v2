/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Register host/ledger, actual dispatch/context gate/Halt. Native
 * security/IRPs/NDIS/page locking/IRQL are hosted; every case starts paused. */
#ifdef PB_API_TESTS
#error Dispatch/register must not enable API test shortcuts
#endif
#define PB_DISPATCH_HOST_TYPES \
typedef struct {VOID *Reserved;} DEVICE_OBJECT; \
typedef VOID *NDIS_HANDLE;typedef ULONG NDIS_HALT_ACTION; \
typedef VOID MINIPORT_HALT(NDIS_HANDLE,NDIS_HALT_ACTION); \
typedef struct _TUN_CTX {SRWLOCK TransitionLock;PB_KERNEL_SESSION *V2; \
 struct {SRWLOCK RegistrationLock;FILE_OBJECT *OwningFileObject;HANDLE OwningProcessId;LIST_ENTRY Entry;KEVENT Disconnected;} Device; \
 struct {volatile LONG V2SessionActive,AllowedMask;EX_RUNDOWN_REF ReturnRundown;KEVENT RunningEvent;SRWLOCK OffloadMutex;} Probe; \
 VOID *MiniportAdapterHandle,*NblPool;volatile LONG Running,Halting;EX_RUNDOWN_REF ControlRundown;DEVICE_OBJECT *FunctionalDeviceObject;} TUN_CTX; \
typedef struct {FILE_OBJECT *FileObject;struct {struct {ULONG InputBufferLength,OutputBufferLength,IoControlCode;} DeviceIoControl;} Parameters;} IO_STACK_LOCATION; \
typedef struct {BOOLEAN Is32;UCHAR RequestorMode;struct {VOID *SystemBuffer;} AssociatedIrp;IO_STACK_LOCATION Stack; \
 struct {NTSTATUS Status;ULONG_PTR Information;} IoStatus;BOOLEAN Allow;ULONG Captured,Released,Checked,Completed,Forwarded;NTSTATUS Result;} IRP; \
typedef NTSTATUS DRIVER_DISPATCH_PAGED(DEVICE_OBJECT *,IRP *);
#ifndef PB_DISPATCH_REGISTER_HOST
#error Build through Build-DispatchRegisterTests
#endif
#define main RegisterOwnerBaseMain
#include PB_DISPATCH_REGISTER_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"dispatch/register requires shipping source fingerprint");
#include <winioctl.h>
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_return_diagnostics.h"
#include "../../include/packetbridge_send_diagnostics.h"
#include "../../include/packetbridge_pool_pressure.h"
#include PB_DISPATCH_LEGACY_SOURCE
#define NDIS_STATUS_ADAPTER_NOT_READY STATUS_NDIS_ADAPTER_NOT_READY
#define MediaConnectStateDisconnected 0u
typedef struct {ULONG_PTR Cookie;} SECURITY_SUBJECT_CONTEXT;
static SECURITY_DESCRIPTOR Descriptor;
static SECURITY_DESCRIPTOR *TunDispatchSecurityDescriptor=&Descriptor;
static GENERIC_MAPPING GenericMap;
static SRWLOCK TunDispatchCtxGuard;
static DEVICE_OBJECT Device;
static __declspec(thread) ULONG CriticalDepth,Role,ResourceDepth;
static __declspec(thread) SECURITY_SUBJECT_CONTEXT *Subject;
static __declspec(thread) IRP *CallingIrp;
static __declspec(thread) struct {SRWLOCK *Lock;BOOLEAN Exclusive;} DispatchResources[8];
enum {ROLE_CALLER=1,ROLE_HALT=2};
static HANDLE AdmissionEntered,AdmissionGate,TailEntered,TailGate,PinDropped,CompleteGate,ControlWaitEntered,UnsafeEntered,UnsafeGate,CtxFreed;
static volatile LONG ExtraHandles,ControlReaders,ControlWaits,AdmissionArmed,PoolLive,ContextLive,UnsafeFree,ProbeCallsNew,Completions,Dispatches;
static ULONG Measured;
static BOOLEAN Gates;
static HANDLE ExtraEvent(BOOLEAN manual)
{HANDLE h=CreateEventW(NULL,manual,FALSE,NULL);CHECK(h);InterlockedIncrement(&ExtraHandles);return h;}
static VOID ExtraClose(HANDLE h) {CHECK(h&&CloseHandle(h)&&InterlockedDecrement(&ExtraHandles)>=0);}
static HANDLE Start(LPTHREAD_START_ROUTINE entry,VOID *input)
{HANDLE h=CreateThread(NULL,0,entry,input,0,NULL);CHECK(h);InterlockedIncrement(&ExtraHandles);return h;}
static VOID Join(HANDLE h) {Await(h);ExtraClose(h);}
static VOID KeEnterCriticalRegion(VOID) {++CriticalDepth;}
static VOID KeLeaveCriticalRegion(VOID)
{
    CHECK(CriticalDepth&&!ResourceDepth);--CriticalDepth;
    /* Lookup released the global guard but the outer dispatch still owns its
     * Control pin. Never park while CtxGuard is held. */
    if(Role==ROLE_CALLER&&CriticalDepth==1&&InterlockedCompareExchange(&AdmissionArmed,0,1)==1){
        CHECK(Gates&&ControlReaders==1&&SetEvent(AdmissionEntered));Await(AdmissionGate);
    }
}
static VOID AcquireResource(SRWLOCK *lock,BOOLEAN wait,BOOLEAN exclusive)
{
    CHECK(wait&&CriticalDepth&&ResourceDepth<_countof(DispatchResources));
    if(exclusive)AcquireSRWLockExclusive(lock);else AcquireSRWLockShared(lock);
    DispatchResources[ResourceDepth].Lock=lock;DispatchResources[ResourceDepth++].Exclusive=exclusive;
}
static VOID DispatchExclusive(SRWLOCK *lock,BOOLEAN wait) {AcquireResource(lock,wait,TRUE);}
static VOID DispatchShared(SRWLOCK *lock,BOOLEAN wait) {AcquireResource(lock,wait,FALSE);}
static VOID DispatchResourceRelease(SRWLOCK *lock)
{CHECK(ResourceDepth&&DispatchResources[ResourceDepth-1].Lock==lock);if(DispatchResources[--ResourceDepth].Exclusive)ReleaseSRWLockExclusive(lock);else ReleaseSRWLockShared(lock);}
static BOOLEAN DispatchAcquire(EX_RUNDOWN_REF *refs)
{CHECK(HostedCtxPointer&&refs==&Context.ControlRundown);AcquireSRWLockShared(&refs->Lock);InterlockedIncrement(&ControlReaders);return TRUE;}
static VOID DispatchRelease(EX_RUNDOWN_REF *refs)
{
    CHECK(HostedCtxPointer&&refs==&Context.ControlRundown&&ControlReaders>0);BOOLEAN gated=Gates&&Role==ROLE_CALLER;
    if(gated){CHECK(CriticalDepth==1&&!ResourceHeld&&!ResourceDepth&&SetEvent(TailEntered));Await(TailGate);}
    CHECK(InterlockedDecrement(&ControlReaders)>=0);ReleaseSRWLockShared(&refs->Lock);
    /* No access to refs/Ctx after unlock: Halt can free them immediately. */
    if(gated){CHECK(SetEvent(PinDropped));Await(CompleteGate);}
}
static VOID DispatchWait(EX_RUNDOWN_REF *refs)
{
    CHECK(HostedCtxPointer);
    if(refs==&Context.ControlRundown){InterlockedIncrement(&ControlWaits);
        if(!TryAcquireSRWLockExclusive(&refs->Lock)){CHECK(SetEvent(ControlWaitEntered));AcquireSRWLockExclusive(&refs->Lock);}
        CHECK(!ControlReaders);ReleaseSRWLockExclusive(&refs->Lock);
    }else{CHECK(refs==&Context.Probe.ReturnRundown);ExWaitForRundownProtectionRelease(refs);}
}
#define ExAcquireResourceExclusiveLite DispatchExclusive
#define ExAcquireResourceSharedLite DispatchShared
#define ExReleaseResourceLite DispatchResourceRelease
#define ExAcquireRundownProtection DispatchAcquire
#define ExReleaseRundownProtection DispatchRelease
#define ExWaitForRundownProtectionRelease DispatchWait
#include PB_DISPATCH_CONTEXT_SOURCE
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceLite
#undef ExAcquireRundownProtection
#undef ExReleaseRundownProtection
#undef ExWaitForRundownProtectionRelease
static VOID DispatchStatus(VOID *adapter,ULONG status)
{CHECK(HostedCtxPointer&&adapter==Context.MiniportAdapterHandle&&status==MediaConnectStateDisconnected&&!Context.Device.OwningFileObject);}
static VOID NdisFreeNetBufferListPool(VOID *pool)
{
    CHECK(HostedCtxPointer&&pool==Context.NblPool&&PoolLive==1);
    if(ControlReaders){CHECK(!InterlockedExchange(&UnsafeFree,1)&&SetEvent(UnsafeEntered));Await(UnsafeGate);}
    CHECK(!ControlReaders&&!Current&&!Context.V2);HostFree(pool);Context.NblPool=NULL;InterlockedDecrement(&PoolLive);
}
static VOID DispatchDeleteResource(SRWLOCK *lock) {CHECK(HostedCtxPointer&&lock==&Context.Device.RegistrationLock&&!ResourceDepth&&!ResourceHeld);}
static VOID DispatchContextFree(VOID *memory,ULONG tag)
{
    CHECK(HostedCtxPointer&&memory==HostedCtxPointer&&!tag&&!PoolLive&&!ControlReaders&&!Device.Reserved&&!Current&&!Context.V2);
    /* Halt finished Unregister. Reuse caller-region/event teardown while Ctx
     * remains alive, then physically free the guarded/ordinary Ctx. */
    Cleanup();CHECK(!Handles&&!Pools&&!Workers&&!WorkerHandles&&!References&&!Mdls&&!Locked&&!Mapped);
    ExtraClose(Context.Probe.RunningEvent.Native);HostFree(memory);HostedCtxPointer=NULL;InterlockedDecrement(&ContextLive);CHECK(SetEvent(CtxFreed));
}
#define ExAcquireResourceExclusiveLite DispatchExclusive
#define ExReleaseResourceLite DispatchResourceRelease
#define TunIndicateStatus DispatchStatus
#pragma warning(push)
#pragma warning(disable:4100 4459)
#include PB_DISPATCH_COMMON_SOURCE
#undef TunIndicateStatus
#define ExWaitForRundownProtectionRelease DispatchWait
#define ExFreePoolWithTag DispatchContextFree
#define ExDeleteResourceLite DispatchDeleteResource
#include PB_DISPATCH_HALT_SOURCE
#pragma warning(pop)
#undef ExAcquireResourceExclusiveLite
#undef ExReleaseResourceLite
#undef ExWaitForRundownProtectionRelease
#undef ExFreePoolWithTag
#undef ExDeleteResourceLite
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock) {AcquireSRWLockShared(lock);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL saved) {CHECK(!saved);ReleaseSRWLockShared(lock);}
static ULONG64 KeQueryInterruptTime(VOID) {return GetTickCount64()*10000u;}
#include "../../driver/v2/session_query.h"
#include "../../driver/v2/session_drain.h"
static VOID ExAcquireFastMutex(SRWLOCK *lock) {AcquireSRWLockExclusive(lock);}
static VOID ExReleaseFastMutex(SRWLOCK *lock) {ReleaseSRWLockExclusive(lock);}
static ULONG PbRotateIndications(TUN_CTX *ctx) {CHECK(ctx==HostedCtxPointer&&!ctx->Running);return 0;}
static VOID PbWaitIndications(TUN_CTX *ctx,ULONG bank) {CHECK(ctx==HostedCtxPointer&&!ctx->Running&&!bank);}
#include PB_DISPATCH_CONTROL_SOURCE
static IO_STACK_LOCATION *DispatchStack(IRP *irp) {CHECK(irp);return &irp->Stack;}
static GENERIC_MAPPING *IoGetFileObjectGenericMapping(VOID) {return &GenericMap;}
static VOID SeCaptureSubjectContext(SECURITY_SUBJECT_CONTEXT *subject)
{CHECK(CallingIrp&&!Subject);Subject=subject;subject->Cookie=GetCurrentThreadId();++CallingIrp->Captured;}
static BOOLEAN SeAccessCheck(SECURITY_DESCRIPTOR *descriptor,SECURITY_SUBJECT_CONTEXT *subject,BOOLEAN locked,ACCESS_MASK wanted,ACCESS_MASK previous,
    VOID *privileges,GENERIC_MAPPING *mapping,UCHAR mode,ACCESS_MASK *granted,NTSTATUS *status)
{
    CHECK(CallingIrp&&Subject==subject&&descriptor==TunDispatchSecurityDescriptor&&!locked&&wanted==FILE_WRITE_DATA&&!previous&&!privileges&&mapping==&GenericMap&&mode==UserMode&&granted&&status);
    CHECK(subject->Cookie==GetCurrentThreadId());++CallingIrp->Checked;*granted=CallingIrp->Allow?wanted:0;*status=CallingIrp->Allow?STATUS_SUCCESS:STATUS_ACCESS_DENIED;return CallingIrp->Allow;
}
static VOID SeReleaseSubjectContext(SECURITY_SUBJECT_CONTEXT *subject)
{CHECK(CallingIrp&&Subject==subject);Subject=NULL;++CallingIrp->Released;}
static VOID IoCompleteRequest(IRP *irp,ULONG priority)
{CHECK(CallingIrp==irp&&!priority&&!Subject&&!CriticalDepth&&!ResourceDepth&&!irp->Completed);++irp->Completed;InterlockedIncrement(&Completions);}
static NTSTATUS PbProbeIoctl(TUN_CTX *ctx,IRP *irp,ULONG code)
{CHECK(ctx==HostedCtxPointer&&irp==CallingIrp&&code!=PB_V2_IOCTL_REGISTER&&ControlReaders);InterlockedIncrement(&ProbeCallsNew);return STATUS_NOT_SUPPORTED;}
static NTSTATUS ForwardedDispatch(DEVICE_OBJECT *device,IRP *irp)
{CHECK(device==&Device&&irp==CallingIrp&&!Subject&&!CriticalDepth&&!irp->Captured);++irp->Forwarded;irp->IoStatus.Status=STATUS_NOT_SUPPORTED;irp->IoStatus.Information=0x55;IoCompleteRequest(irp,0);return STATUS_NOT_SUPPORTED;}
static DRIVER_DISPATCH_PAGED *NdisDispatchDeviceControl=ForwardedDispatch;
#define IoGetCurrentIrpStackLocation DispatchStack
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_DISPATCH_DISPATCH_SOURCE
#pragma warning(pop)
#undef IoGetCurrentIrpStackLocation
static NTSTATUS Dispatch(IRP *irp)
{
    CHECK(!CallingIrp&&!Subject&&!CriticalDepth&&!ResourceDepth);CallingIrp=irp;InterlockedIncrement(&Dispatches);
    NTSTATUS status=TunDispatchDeviceControl(&Device,irp);CHECK(!Subject&&!CriticalDepth&&!ResourceDepth&&irp->Completed==1&&irp->IoStatus.Status==status);
    CallingIrp=NULL;return status;
}
static VOID Initialize(ULONG queues,BOOLEAN manual)
{
    CHECK(!HostedCtxPointer&&!ContextLive&&!PoolLive&&!ExtraHandles&&!ControlReaders);HostedCtxPointer=HostAlloc(sizeof(*HostedCtxPointer));InterlockedIncrement(&ContextLive);
    Begin(queues,manual);Context.Running=FALSE;Context.Probe.RunningEvent.Native=ExtraEvent(TRUE);Context.FunctionalDeviceObject=&Device;
    Context.NblPool=HostAlloc(32);PoolLive=1;Device.Reserved=HostedCtxPointer;ControlWaits=AdmissionArmed=UnsafeFree=0;Gates=FALSE;
    AdmissionEntered=ExtraEvent(TRUE);AdmissionGate=ExtraEvent(TRUE);TailEntered=ExtraEvent(TRUE);TailGate=ExtraEvent(TRUE);PinDropped=ExtraEvent(TRUE);CompleteGate=ExtraEvent(TRUE);
    ControlWaitEntered=ExtraEvent(TRUE);UnsafeEntered=ExtraEvent(TRUE);UnsafeGate=ExtraEvent(TRUE);CtxFreed=ExtraEvent(TRUE);
    Request.Allow=TRUE;Request.Stack.Parameters.DeviceIoControl.IoControlCode=PB_V2_IOCTL_REGISTER;Request.IoStatus.Information=~(ULONG_PTR)0;
    CHECK(!Context.Running&&!Current&&!Context.V2&&WaitForSingleObject(Context.Probe.RunningEvent.Native,0)==WAIT_TIMEOUT);
}
static VOID Finish(VOID)
{
    CHECK(!HostedCtxPointer&&!ContextLive&&!PoolLive&&!Device.Reserved&&!Current&&!ControlReaders&&!Handles);
    ExtraClose(AdmissionEntered);ExtraClose(AdmissionGate);ExtraClose(TailEntered);ExtraClose(TailGate);ExtraClose(PinDropped);ExtraClose(CompleteGate);
    ExtraClose(ControlWaitEntered);ExtraClose(UnsafeEntered);ExtraClose(UnsafeGate);ExtraClose(CtxFreed);Gates=FALSE;CHECK(!ExtraHandles);++Measured;
}
static DWORD WINAPI Caller(VOID *input) {(void)input;Role=ROLE_CALLER;Request.Result=Dispatch(&Request);CHECK(!ResourceHeld);return 0;}
static DWORD WINAPI Halter(VOID *input)
{Role=ROLE_HALT;TUN_CTX *ctx=input;CHECK(!ctx->Running&&(!ctx->V2||!ctx->V2->Active));TunHaltEx(ctx,0);CHECK(!CriticalDepth&&!ResourceDepth&&!ResourceHeld);return 0;}
static VOID MissingLookup(VOID)
{
    IRP irp={0};irp.Allow=TRUE;irp.RequestorMode=UserMode;irp.Stack.Parameters.DeviceIoControl.IoControlCode=PB_V2_IOCTL_REGISTER;irp.IoStatus.Information=~(ULONG_PTR)0;
    CHECK(Dispatch(&irp)==STATUS_DEVICE_NOT_READY&&!irp.IoStatus.Information&&irp.Captured==1&&irp.Released==1&&irp.Checked==1&&ControlReaders==1);
}
static VOID Concurrent(ULONG queues,BOOLEAN manual,ULONG failure,BOOLEAN control)
{
    Initialize(queues,manual);NTSTATUS expected=STATUS_INSUFFICIENT_RESOURCES;
    if(failure==0)FailPool=1;else if(failure==1)FailMap=TRUE;else if(failure==2){FailOb=1+2*queues;expected=ObError;}
    else if(failure==3)FailThread=queues;else{CHECK(failure==4);expected=STATUS_SUCCESS;}
    Gates=TRUE;AdmissionArmed=1;HANDLE caller=Start(Caller,NULL);Await(AdmissionEntered);
    TUN_CTX *ctx=HostedCtxPointer;CHECK(ControlReaders==1&&!Current&&!Context.Running);HANDLE halt=Start(Halter,ctx);
    HANDLE waits[]={ControlWaitEntered,UnsafeEntered};DWORD observed=WaitForMultipleObjects(2,waits,FALSE,10000);CHECK(observed<WAIT_OBJECT_0+2);
    BOOLEAN missing=observed==WAIT_OBJECT_0+1;CHECK(!missing||control);CHECK(ctx->Halting&&!Device.Reserved&&ContextLive&&ControlReaders==1);
    CHECK(SetEvent(AdmissionGate));Await(TailEntered);
    CHECK(!ResourceHeld&&ControlReaders==1&&Request.Completed==0&&Request.Captured==1&&Request.Checked==1&&Request.Released==1);
    if(failure<4)CHECK(!Current&&!Context.V2&&!Pools&&!Mdls&&!Locked&&!Mapped&&!References&&!WorkerHandles&&!Workers&&!Context.Device.OwningFileObject&&!EverPublished);
    else CHECK(Current==Context.V2&&Workers==(LONG)queues&&Context.Device.OwningFileObject==&Owner&&!StartBeforePublish);
    if(failure==3)CHECK(ThreadCalls==queues&&ThreadsMade==queues-1&&ThreadsExited==ThreadsMade&&CleanupJoins==ThreadsMade);
    MissingLookup();CHECK(SetEvent(TailGate));Await(PinDropped);
    if(missing){CHECK(SetEvent(CompleteGate));Join(caller);CHECK(SetEvent(UnsafeGate));Join(halt);}
    else{Await(CtxFreed);Join(halt);CHECK(!Request.Completed&&SetEvent(CompleteGate));Join(caller);}
    CHECK(Request.Result==expected&&Request.IoStatus.Information==0&&Request.Completed==1&&ThreadsMade==ThreadsExited&&ThreadsMade==CleanupJoins);
    CHECK(missing?(!ControlWaits&&UnsafeFree):(ControlWaits==1&&!UnsafeFree));Finish();
}
static VOID Surface(VOID)
{
    Initialize(1,FALSE);const ULONG codes[]={PB_V2_IOCTL_REGISTER,PB_V2_IOCTL_DRAIN,PB_V2_IOCTL_QUERY,PB_PROBE_IOCTL_QUERY,PB_PROBE_IOCTL_POLICY,PB_RETURN_DIAG_IOCTL,PB_SEND_DIAG_IOCTL,PB_POOL_PRESSURE_IOCTL,TUN_IOCTL_REGISTER_RINGS};
    for(ULONG i=0;i<_countof(codes);++i){Request.Allow=FALSE;Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;Request.IoStatus.Information=~(ULONG_PTR)0;Request.Stack.Parameters.DeviceIoControl.IoControlCode=codes[i];
        CHECK(Dispatch(&Request)==STATUS_ACCESS_DENIED&&!Request.IoStatus.Information&&Request.Captured==1&&Request.Checked==1&&Request.Released==1&&!ControlReaders&&!PoolCalls);}
    Request.Allow=TRUE;
    for(ULONG i=0;i<_countof(codes);++i){Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;Request.IoStatus.Information=~(ULONG_PTR)0;Request.Stack.Parameters.DeviceIoControl.IoControlCode=codes[i];FailPool=1;
        NTSTATUS expected=i==0?STATUS_INSUFFICIENT_RESOURCES:i==1?STATUS_DEVICE_NOT_READY:i==2?STATUS_BUFFER_TOO_SMALL:STATUS_NOT_SUPPORTED;
        CHECK(Dispatch(&Request)==expected&&!Request.IoStatus.Information&&Request.Captured==1&&Request.Released==1&&!ControlReaders);}
    Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;Request.Stack.Parameters.DeviceIoControl.IoControlCode=0x12345678;
    CHECK(Dispatch(&Request)==STATUS_NOT_SUPPORTED&&Request.Forwarded==1&&Request.IoStatus.Information==0x55&&!Request.Captured);
    /* Also exercise a successful positive-length buffered output through the
     * actual router/query, rather than validating only failure Information=0. */
    PoolCalls=FailPool=0;Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;
    Request.Stack.Parameters.DeviceIoControl.IoControlCode=PB_V2_IOCTL_REGISTER;
    CHECK(Dispatch(&Request)==STATUS_SUCCESS&&!Request.IoStatus.Information&&Current==Context.V2&&!Context.Running);
    PB_V2_STATUS status;memset(&status,0x5a,sizeof(status));Request.AssociatedIrp.SystemBuffer=&status;
    Request.Stack.Parameters.DeviceIoControl.IoControlCode=PB_V2_IOCTL_QUERY;Request.Stack.Parameters.DeviceIoControl.OutputBufferLength=sizeof(status);
    Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;
    CHECK(Dispatch(&Request)==STATUS_SUCCESS&&Request.IoStatus.Information==sizeof(status)&&status.Size==sizeof(status)&&
        status.Epoch==42&&status.State==PB_SESSION_RUNNING&&!status.ActiveReferences);
    Join(Start(Halter,HostedCtxPointer));
    for(ULONG i=0;i<_countof(codes);++i){Request.Captured=Request.Checked=Request.Released=Request.Completed=Request.Forwarded=0;Request.IoStatus.Information=~(ULONG_PTR)0;Request.Stack.Parameters.DeviceIoControl.IoControlCode=codes[i];
        CHECK(Dispatch(&Request)==(i==8?NDIS_STATUS_ADAPTER_NOT_READY:STATUS_DEVICE_NOT_READY)&&!Request.IoStatus.Information&&Request.Captured==1&&Request.Released==1);}
    Finish();
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);if(argc==2){CHECK(!strcmp(argv[1],"1"));Concurrent(2,FALSE,3,TRUE);
        if(UnsafeFree){puts("DISPATCH_HALT_SKIPPED_CONTROL_WAIT: unsafe free parked; actual Register/Dispatch joined before native pool/Ctx free; zero owned resources");return 112;}
        puts("PASS: actual dispatcher/register Control tail barrier control, every thread joined and owner reclaimed");return 0;}
    Surface();Measured=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    const ULONG queues[]={1,2,8};for(ULONG i=0;i<_countof(queues);++i)for(ULONG manual=0;manual<2;++manual)for(ULONG failure=0;failure<5;++failure)Concurrent(queues[i],(BOOLEAN)manual,failure,FALSE);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual DeviceControl/Register/Halt schedules plus checked dispatch surface; %ld IRP completions across %ld dispatches, paused boundary, security/forwarding/status, partial rollback and Control tail; all workers/callers joined, owned resources zero, process handles %lu/%lu\n",Measured,Completions,Dispatches,before,after);return 0;
}
