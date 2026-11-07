/* SPDX-License-Identifier: GPL-2.0 */
/* Actual SYS offload handlers and Report; real Windows synchronization.
 * SDK/WDK formats stay native. IRQL is a contract model, not kernel execution. */
#ifdef PB_API_TESTS
#error Offload handler must not enable API test shortcuts
#endif
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
typedef LONG NTSTATUS;
#define NDIS_SUPPORT_NDIS6 1
#define NDIS_SUPPORT_NDIS61 1
#define NDIS_SUPPORT_NDIS630 1
#define NDIS_SUPPORT_NDIS650 1
#define NDIS_SUPPORT_NDIS670 1
#define NDIS_SUPPORT_NDIS683 1
#define NDIS_SUPPORT_NDIS685 1
#define NDIS_SUPPORT_NDIS689 1
#pragma warning(push)
#pragma warning(disable:4324)
#include <ntddndis.h>
#include <ndis/oidrequest.h>
#include <ndis/encapsulationconfig.h>
#include <ndis/status.h>
#pragma warning(pop)
#include PB_OFFLOAD_STATUS_TYPE
#include "../safety/guard_memory.h"
#define CHECK SAFETY_CHECK
#define NT_ASSERT CHECK
typedef UCHAR KIRQL;
typedef SRWLOCK FAST_MUTEX;
typedef struct {HANDLE Native;} KEVENT,*PKEVENT;
typedef struct {LONG Ref;SRWLOCK Lock;} EX_RUNDOWN_REF;
typedef struct {VOID *Address;} MDL;
#include "../../driver/v2/probe_state.h"
#include "../../common/offload_policy.h"
#include "../../include/packetbridge_pool_pressure.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"offload handler requires shipping source fingerprint");
_Static_assert(sizeof(KSPIN_LOCK)==sizeof(SRWLOCK),"hosted spin-lock storage must fit SRWLOCK");
typedef struct _TUN_CTX {SRWLOCK TransitionLock;PB_PROBE_STATE Probe;PB_KERNEL_SESSION *V2;VOID *MiniportAdapterHandle;} TUN_CTX;
typedef struct {struct {struct {ULONG InputBufferLength,OutputBufferLength;} DeviceIoControl;} Parameters;} IO_STACK_LOCATION;
typedef struct {IO_STACK_LOCATION Stack;struct {VOID *SystemBuffer;} AssociatedIrp;struct {ULONG_PTR Information;} IoStatus;} IRP;
static IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(IRP *irp){return &irp->Stack;}
static ULONG NdisVersion;
static TUN_CTX *CurrentCtx;
static PB_KERNEL_SESSION PresentSession;
static VOID BindPresence(TUN_CTX *ctx){PresentSession.Ctx=ctx;}
#define Ctx (*CurrentCtx)
static volatile LONG Handles,Memory,Reports,MutexAttempts,MutexAcquires,Waits;
static __declspec(thread) LONG TransitionHeld,MutexHeld,Role;
static __declspec(thread) KIRQL ModelLevel;
static __declspec(thread) BOOLEAN StatusHeld;
static HANDLE FirstWait,SecondWait,SecondAttempt,FirstDone,SecondDone;
static ULONG LastReport;
static HANDLE Event(VOID){HANDLE h=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static VOID Close(HANDLE h){CHECK(CloseHandle(h)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE h){CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static VOID *MemoryAlloc(SIZE_T bytes)
{
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(bytes);memset(p,0,bytes);
#else
    VOID *p=calloc(1,bytes);
#endif
    CHECK(p);InterlockedIncrement(&Memory);return p;
}
static VOID MemoryFree(VOID *p)
{
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
    CHECK(InterlockedDecrement(&Memory)>=0);
}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *lock){CHECK(!TransitionHeld&&ModelLevel<=2);KIRQL saved=ModelLevel;ModelLevel=2;AcquireSRWLockExclusive(lock);TransitionHeld=2;return saved;}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *lock,KIRQL saved){CHECK(saved<=2&&TransitionHeld==2&&ModelLevel==2);TransitionHeld=0;ReleaseSRWLockExclusive(lock);ModelLevel=saved;}
static KIRQL ExAcquireSpinLockShared(SRWLOCK *lock){CHECK(!TransitionHeld&&ModelLevel<=2);KIRQL saved=ModelLevel;ModelLevel=2;AcquireSRWLockShared(lock);TransitionHeld=1;return saved;}
static VOID ExReleaseSpinLockShared(SRWLOCK *lock,KIRQL saved){CHECK(saved<=2&&TransitionHeld==1&&ModelLevel==2);TransitionHeld=0;ReleaseSRWLockShared(lock);ModelLevel=saved;}
static VOID ExAcquireFastMutex(SRWLOCK *lock){CHECK(!MutexHeld&&!TransitionHeld&&!ModelLevel);InterlockedIncrement(&MutexAttempts);if(Role==2)CHECK(SetEvent(SecondAttempt));AcquireSRWLockExclusive(lock);MutexHeld=1;ModelLevel=1;InterlockedIncrement(&MutexAcquires);}
static VOID ExReleaseFastMutex(SRWLOCK *lock){CHECK(MutexHeld==1&&!TransitionHeld&&ModelLevel==1);MutexHeld=0;ReleaseSRWLockExclusive(lock);ModelLevel=0;}
typedef struct {SRWLOCK *Lock;KIRQL Level;BOOLEAN Status;} KLOCK_QUEUE_HANDLE;
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *h){h->Lock=(SRWLOCK *)lock;h->Level=ModelLevel;ModelLevel=2;h->Status=lock==&Ctx.Probe.StatusLock;AcquireSRWLockExclusive(h->Lock);if(h->Status){CHECK(!StatusHeld);StatusHeld=TRUE;}}
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *h){if(h->Status){CHECK(StatusHeld);StatusHeld=FALSE;}ReleaseSRWLockExclusive(h->Lock);ModelLevel=h->Level;}
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
static VOID KeClearEvent(KEVENT *event){CHECK(ResetEvent(event->Native));}
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait){CHECK(!priority&&!wait&&SetEvent(event->Native));}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{CHECK(!reason&&!mode&&!alertable&&!timeout&&!TransitionHeld&&MutexHeld==1&&ModelLevel==1);InterlockedIncrement(&Waits);if(Role==1)CHECK(SetEvent(FirstWait));if(Role==2)CHECK(SetEvent(SecondWait));Await(event->Native);}
#include "../../driver/v2/indication_gate.h"
#include "../../driver/v2/offload_format.h"
#include PB_OFFLOAD_EFFECTIVE_SOURCE
static VOID NdisMIndicateStatusEx(VOID *adapter,NDIS_STATUS_INDICATION *indication)
{
    CHECK(adapter==&Ctx&&MutexHeld==1&&!TransitionHeld&&StatusHeld&&ModelLevel==2);
    CHECK(indication->Header.Revision==NDIS_STATUS_INDICATION_REVISION_1&&indication->Header.Size==NDIS_SIZEOF_STATUS_INDICATION_REVISION_1&&indication->StatusCode==NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG);
    NDIS_OFFLOAD *status=indication->StatusBuffer;LastReport=PbEffectiveMask(&Ctx);CHECK(status->Header.Revision==PbPlatformOffloadRevision(NdisVersion)&&indication->StatusBufferSize==status->Header.Size&&!(LastReport&~PbPlatformOffloads(NdisVersion)));InterlockedIncrement(&Reports);
    /* ProtocolStatusEx runs at modeled DISPATCH here. A regular OID callback
     * is PASSIVE-only; this host never fabricates recursive regular dispatch. */
}
#include PB_OFFLOAD_REPORT_SOURCE
static NTSTATUS PbQueryPoolPressure(TUN_CTX *ctx,VOID *output,ULONG outSize,ULONG inSize,ULONG_PTR *done)
{(void)ctx;(void)output;(void)outSize;(void)inSize;(void)done;CHECK(FALSE);return STATUS_NOT_SUPPORTED;}
#define PbQueryReturnDiagnostics PbQueryPoolPressure
#define PbQuerySendDiagnostics PbQueryPoolPressure
#include PB_OFFLOAD_HANDLERS_SOURCE
static const USHORT ParameterSizes[]={0,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_1,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_2,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_4,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_5,NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_6};
static VOID Initialize(ULONG minor)
{
    CHECK(!Handles&&!Memory&&!CurrentCtx);CurrentCtx=MemoryAlloc(sizeof(*CurrentCtx));memset(&PresentSession,0,sizeof(PresentSession));BindPresence(&Ctx);
    NdisVersion=PB_NDIS_VERSION(minor);Ctx.V2=&PresentSession;Ctx.MiniportAdapterHandle=&Ctx;
    Ctx.Probe.RegistrationStatus=NDIS_STATUS_SUCCESS;Ctx.Probe.V2SessionActive=1;Ctx.Probe.AllowedMask=PB_PROBE_SUPPORTED;Ctx.Probe.NdisMask=PbPlatformOffloads(NdisVersion);Ctx.Probe.EncapsulationMask=PB_PROBE_SUPPORTED;
    for(ULONG i=0;i<2;++i){Ctx.Probe.IndicationIdle[i].Native=Event();CHECK(SetEvent(Ctx.Probe.IndicationIdle[i].Native));}
    Reports=MutexAttempts=MutexAcquires=Waits=0;LastReport=0;
}
static VOID End(VOID)
{for(ULONG i=0;i<2;++i){CHECK(!Ctx.Probe.IndicationCount[i]);Close(Ctx.Probe.IndicationIdle[i].Native);}MemoryFree(CurrentCtx);CurrentCtx=NULL;CHECK(!Handles&&!Memory&&!MutexHeld&&!TransitionHeld&&!ModelLevel&&!StatusHeld&&MutexAttempts==MutexAcquires);}
static NTSTATUS Policy(ULONG mask)
{PB_PROBE_POLICY policy={sizeof(policy),PB_PROBE_SCHEMA,mask,0};IRP irp={.AssociatedIrp={&policy}};irp.Stack.Parameters.DeviceIoControl.InputBufferLength=sizeof(policy);return PbProbeIoctl(&Ctx,&irp,PB_PROBE_IOCTL_POLICY);}
static VOID Oid(NDIS_OID_REQUEST *request,NDIS_OID oid,VOID *buffer,ULONG size)
{
    memset(request,0,sizeof(*request));request->Header.Type=NDIS_OBJECT_TYPE_OID_REQUEST;
    request->Header.Revision=NdisVersion>=PB_NDIS_VERSION(50)?NDIS_OID_REQUEST_REVISION_2:NDIS_OID_REQUEST_REVISION_1;
    request->Header.Size=(USHORT)(request->Header.Revision==NDIS_OID_REQUEST_REVISION_2?NDIS_SIZEOF_OID_REQUEST_REVISION_2:NDIS_SIZEOF_OID_REQUEST_REVISION_1);
    request->RequestType=NdisRequestSetInformation;request->DATA.SET_INFORMATION.Oid=oid;request->DATA.SET_INFORMATION.InformationBuffer=buffer;request->DATA.SET_INFORMATION.InformationBufferLength=size;
}
static NDIS_STATUS Parameters(ULONG revision,ULONG *read)
{
    NDIS_OFFLOAD_PARAMETERS *p=MemoryAlloc(ParameterSizes[revision]);p->Header=(NDIS_OBJECT_HEADER){NDIS_OBJECT_TYPE_DEFAULT,(UCHAR)revision,ParameterSizes[revision]};
    NDIS_OID_REQUEST request;Oid(&request,OID_TCP_OFFLOAD_PARAMETERS,p,p->Header.Size);
    NDIS_STATUS result=PbSetOffloadParameters(&Ctx,&request);*read=request.DATA.SET_INFORMATION.BytesRead;MemoryFree(p);return result;
}
static NDIS_STATUS Encapsulation(ULONG header,ULONG *read)
{
    NDIS_OFFLOAD_ENCAPSULATION *p=MemoryAlloc(sizeof(*p));p->Header=(NDIS_OBJECT_HEADER){NDIS_OBJECT_TYPE_OFFLOAD_ENCAPSULATION,NDIS_OFFLOAD_ENCAPSULATION_REVISION_1,sizeof(*p)};
    p->IPv4.Enabled=p->IPv6.Enabled=NDIS_OFFLOAD_SET_ON;p->IPv4.EncapsulationType=p->IPv6.EncapsulationType=NDIS_ENCAPSULATION_IEEE_802_3;p->IPv4.HeaderSize=p->IPv6.HeaderSize=header;
    NDIS_OID_REQUEST request;Oid(&request,OID_OFFLOAD_ENCAPSULATION,p,sizeof(*p));
    NDIS_STATUS result=PbSetEncapsulation(&Ctx,&request);*read=request.DATA.SET_INFORMATION.BytesRead;MemoryFree(p);return result;
}
static ULONG Cases;
static VOID Features(VOID)
{
    const ULONG bits[]={PB_RX_RSC4,PB_TX_USO4,PB_RX_URO},revisions[]={3,5,6};
    ULONG caps=PbPlatformOffloads(NdisVersion);
    for(ULONG i=0;i<_countof(bits);++i){
        ULONG old=(ULONG)Ctx.Probe.AllowedMask;LONG reports=Reports,attempts=MutexAttempts;BOOLEAN supported=!!(caps&bits[i]);
        CHECK(Policy(bits[i])==(supported?STATUS_SUCCESS:STATUS_NOT_SUPPORTED));
        if(!supported)CHECK((ULONG)Ctx.Probe.AllowedMask==old&&Reports==reports&&MutexAttempts==attempts);
        ++Cases;if(!supported)continue;
        Ctx.Probe.AllowedMask=PB_PROBE_SUPPORTED;Ctx.Probe.NdisMask=(LONG)(caps&~bits[i]);
        NDIS_OFFLOAD_PARAMETERS *p=MemoryAlloc(ParameterSizes[revisions[i]]);p->Header=(NDIS_OBJECT_HEADER){NDIS_OBJECT_TYPE_DEFAULT,(UCHAR)revisions[i],ParameterSizes[revisions[i]]};
        if(i==0)p->RscIPv4=NDIS_OFFLOAD_PARAMETERS_RSC_ENABLED;
        else if(i==1)p->UdpSegmentation.IPv4=NDIS_OFFLOAD_PARAMETERS_USO_ENABLED;
        else p->UdpRsc.Enabled=NDIS_OFFLOAD_PARAMETERS_UDP_RSC_ENABLED;
        NDIS_OID_REQUEST request;Oid(&request,OID_TCP_OFFLOAD_PARAMETERS,p,p->Header.Size);
        CHECK(PbSetOffloadParameters(&Ctx,&request)==NDIS_STATUS_SUCCESS&&request.DATA.SET_INFORMATION.BytesRead==p->Header.Size&&(ULONG)Ctx.Probe.NdisMask==caps&&(LastReport&bits[i]));
        MemoryFree(p);++Cases;
    }
    Ctx.Probe.AllowedMask=PB_PROBE_SUPPORTED;
}
static VOID Basic(ULONG minor)
{
    Initialize(minor);CHECK(Policy(PB_TX_IP4_CHECKSUM)==STATUS_SUCCESS&&Ctx.Probe.AllowedMask==PB_TX_IP4_CHECKSUM);++Cases;
    Ctx.Probe.V2SessionActive=0;LONG reports=Reports;CHECK(Policy(PB_TX_IP4_CHECKSUM)==STATUS_DEVICE_NOT_READY&&Reports==reports);++Cases;
    CHECK(Policy(0)==STATUS_SUCCESS&&!Ctx.Probe.AllowedMask&&!LastReport);++Cases;
    Ctx.Probe.V2SessionActive=1;Ctx.Probe.AllowedMask=PB_PROBE_SUPPORTED;
    for(ULONG revision=1;revision<=6;++revision){ULONG read=MAXDWORD;NDIS_STATUS result=Parameters(revision,&read);BOOLEAN supported=revision<=PbPlatformParameterRevision(NdisVersion);CHECK(result==(supported?NDIS_STATUS_SUCCESS:NDIS_STATUS_INVALID_LENGTH)&&read==(ULONG)(supported?ParameterSizes[revision]:0));++Cases;}
    Features();
    Ctx.Probe.RegistrationStatus=NDIS_STATUS_NOT_SUPPORTED;reports=Reports;CHECK(Policy(PB_TX_IP4_CHECKSUM)==STATUS_NOT_SUPPORTED&&Reports==reports);++Cases;
    CHECK(Policy(0)==STATUS_SUCCESS&&!Ctx.Probe.AllowedMask&&Reports==reports);++Cases;Ctx.Probe.RegistrationStatus=NDIS_STATUS_SUCCESS;
    ULONG read;CHECK(Encapsulation(14,&read)==NDIS_STATUS_SUCCESS&&read==sizeof(NDIS_OFFLOAD_ENCAPSULATION));++Cases;
    CHECK(Encapsulation(18,&read)==NDIS_STATUS_SUCCESS&&read==sizeof(NDIS_OFFLOAD_ENCAPSULATION));++Cases;
    reports=Reports;CHECK(Encapsulation(16,&read)==NDIS_STATUS_NOT_SUPPORTED&&!read&&Reports==reports);++Cases;End();
}
static ULONG SecondKind;
static DWORD WINAPI First(VOID *unused){(void)unused;Role=1;CHECK(Policy(PB_TX_IP4_CHECKSUM)==STATUS_SUCCESS&&!TransitionHeld&&!MutexHeld&&!ModelLevel&&!StatusHeld&&SetEvent(FirstDone));return 0;}
static DWORD WINAPI Second(VOID *unused){(void)unused;Role=2;ULONG read;NDIS_STATUS result=SecondKind?Parameters(PbPlatformParameterRevision(NdisVersion),&read):Encapsulation(14,&read);CHECK(result==NDIS_STATUS_SUCCESS&&!TransitionHeld&&!MutexHeld&&!ModelLevel&&!StatusHeld&&SetEvent(SecondDone));return 0;}
static VOID Serialized(ULONG minor,ULONG kind)
{
    Initialize(minor);SecondKind=kind;FirstWait=Event();SecondWait=Event();SecondAttempt=Event();FirstDone=Event();SecondDone=Event();
    KIRQL saved=ExAcquireSpinLockShared(&Ctx.TransitionLock);ULONG bank=PbBeginIndication(&Ctx);ExReleaseSpinLockShared(&Ctx.TransitionLock,saved);CHECK(bank==0);
    HANDLE first=CreateThread(NULL,0,First,NULL,0,NULL);CHECK(first);InterlockedIncrement(&Handles);Await(FirstWait);
    CHECK(MutexAcquires==1&&Waits==1&&!Reports&&Ctx.Probe.IndicationBank==1&&TryAcquireSRWLockShared(&Ctx.TransitionLock));ReleaseSRWLockShared(&Ctx.TransitionLock);
    saved=ExAcquireSpinLockShared(&Ctx.TransitionLock);ULONG next=PbBeginIndication(&Ctx);ExReleaseSpinLockShared(&Ctx.TransitionLock,saved);CHECK(next==1);
    HANDLE second=CreateThread(NULL,0,Second,NULL,0,NULL);CHECK(second);InterlockedIncrement(&Handles);Await(SecondAttempt);
    CHECK(MutexAcquires==1&&Ctx.Probe.IndicationBank==1&&WaitForSingleObject(SecondWait,0)==WAIT_TIMEOUT);
    PbEndIndication(&Ctx,bank);Await(FirstDone);Await(first);Close(first);Await(SecondWait);
    CHECK(MutexAcquires==2&&Waits==2&&Reports==1&&Ctx.Probe.IndicationBank==0&&Ctx.Probe.IndicationCount[1]==1&&WaitForSingleObject(SecondDone,0)==WAIT_TIMEOUT&&TryAcquireSRWLockShared(&Ctx.TransitionLock));ReleaseSRWLockShared(&Ctx.TransitionLock);
    PbEndIndication(&Ctx,next);Await(SecondDone);Await(second);Close(second);CHECK(Reports==2);
    Close(FirstWait);Close(SecondWait);Close(SecondAttempt);Close(FirstDone);Close(SecondDone);End();++Cases;
}
static VOID MissingWait(BOOLEAN negative)
{
    Initialize(89);FirstWait=Event();FirstDone=Event();
    KIRQL saved=ExAcquireSpinLockShared(&Ctx.TransitionLock);ULONG bank=PbBeginIndication(&Ctx);ExReleaseSpinLockShared(&Ctx.TransitionLock,saved);
    HANDLE first=CreateThread(NULL,0,First,NULL,0,NULL);CHECK(first);InterlockedIncrement(&Handles);
    HANDLE events[]={FirstWait,FirstDone};DWORD observed=WaitForMultipleObjects(2,events,FALSE,5000);CHECK(observed<WAIT_OBJECT_0+2);
    CHECK(observed==WAIT_OBJECT_0+(negative?1u:0u)&&Ctx.Probe.IndicationCount[bank]==1);
    CHECK(WaitForSingleObject(Ctx.Probe.IndicationIdle[bank].Native,0)==WAIT_TIMEOUT);
    if(negative)CHECK(!Waits&&Reports==1&&WaitForSingleObject(FirstDone,0)==WAIT_OBJECT_0);
    else CHECK(Waits==1&&!Reports&&WaitForSingleObject(FirstDone,0)==WAIT_TIMEOUT);
    /* Observe the unsafe logical acknowledgement, never free a held bank.
     * Rescue all valid owners and join the real caller before any teardown. */
    PbEndIndication(&Ctx,bank);Await(FirstDone);Await(first);Close(first);Close(FirstWait);Close(FirstDone);End();++Cases;
}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"120");CHECK(argc==1||selected);
#ifdef PB_OFFLOAD_WITHOUT_POLICY_WAIT
    BOOLEAN negative=TRUE;CHECK(selected);
#else
    BOOLEAN negative=FALSE;
#endif
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)MissingWait(negative);
    else{const ULONG versions[]={20,30,83,89};for(ULONG i=0;i<_countof(versions);++i){Basic(versions[i]);for(ULONG kind=0;kind<2;++kind)Serialized(versions[i],kind);}}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual SYS offload POLICY/OID parameters/encapsulation checks; real WDK OID request/status + SDK exact old-revision spans; serialized mutex, transition released during prior-bank wait, V2Active and zero disable; modeled status DISPATCH, regular OID PASSIVE; handles %lu/%lu\n",Cases,before,after);
    if(negative){puts("OFFLOAD_POLICY_SKIPPED_BANK_WAIT: actual POLICY returned while prior bank remained owned; owner released and caller joined before context/event free; zero owned resources");return 120;}
    return 0;
}
