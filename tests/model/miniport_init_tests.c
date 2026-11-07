/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Initialize/Halt and context gate. Concrete NDIS formats are SDK/WDK;
 * kernel objects/DDIs and initial adapter state are hosted, no native device. */
#ifdef PB_API_TESTS
#error Miniport init requires shipping kernel paths
#endif
#ifndef NDEBUG
#error Miniport init must exercise shipping NDEBUG paths
#endif
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#define NDIS_SUPPORT_NDIS6 1
#define NDIS_SUPPORT_NDIS61 1
#define NDIS_SUPPORT_NDIS620 1
#define NDIS_SUPPORT_NDIS630 1
#define NDIS_SUPPORT_NDIS650 1
#define NDIS_SUPPORT_NDIS670 1
#define NDIS_SUPPORT_NDIS683 1
#define NDIS_SUPPORT_NDIS685 1
#define NDIS_SUPPORT_NDIS687 1
#define NDIS_SUPPORT_NDIS689 1
#pragma warning(push)
#pragma warning(disable:4324)
#include <ntddndis.h>
#include <ndis/types.h>
#include <ndis/status.h>
#include <ndis/miniportgeneralattributes.h>
#include PB_INIT_TYPES
#pragma warning(pop)
#include "../safety/guard_memory.h"
#define CHECK SAFETY_CHECK
#define ASSERT(x) ((void)0)
#define NT_ASSERT(x) ((void)0)
typedef LONG NTSTATUS;
#define NT_SUCCESS(status) (((NTSTATUS)(status))>=0)
typedef UCHAR KIRQL;
typedef SRWLOCK EX_SPIN_LOCK,FAST_MUTEX;
typedef struct {HANDLE Native;} KEVENT,*PKEVENT;
typedef struct {SRWLOCK Lock;BOOLEAN Initialized;} ERESOURCE;
typedef struct {SRWLOCK Lock;volatile LONG References;BOOLEAN Initialized,Closing;} EX_RUNDOWN_REF;
typedef struct _FILE_OBJECT FILE_OBJECT;
typedef struct {VOID *Reserved;} DEVICE_OBJECT;
typedef struct {VOID *Unused;} MDL;
#include "../../driver/v2/probe_state.h"
#include "../../common/platform.h"
#include PB_INIT_CONSTANTS
#include PB_INIT_CONTEXT
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"miniport init requires shipping source fingerprint");
_Static_assert(sizeof(KSPIN_LOCK)==sizeof(SRWLOCK),"hosted spin lock storage");
typedef NDIS_STATUS MINIPORT_INITIALIZE(NDIS_HANDLE,NDIS_HANDLE,PNDIS_MINIPORT_INIT_PARAMETERS);
typedef VOID MINIPORT_HALT(NDIS_HANDLE,NDIS_HALT_ACTION);
static ERESOURCE TunDispatchCtxGuard={SRWLOCK_INIT,TRUE},TunDispatchDeviceListLock={SRWLOCK_INIT,TRUE};
static UINT NdisVersion;
enum {SUCCEED,FAIL_CONTEXT,NO_FDO,FAIL_RESOURCE,FAIL_NBL_POOL,FAIL_REGISTRATION,FAIL_GENERAL,FAIL_OFFLOAD};
typedef struct INIT_JOB {
    DEVICE_OBJECT Device;NDIS_MINIPORT_INIT_PARAMETERS Parameters;TUN_CTX *Context;VOID *NdisContext,*Pool;
    ULONG Failure,Allocations,Properties,ResourceInits,ResourceDeletes,PoolRequests,PoolDeletes,Attributes,Publications,Unregisters,ControlWaits,ReturnWaits;
    BOOLEAN Initialized,HaltStarted;NDIS_STATUS Result;HANDLE FreeEntered,FreeRescue,HaltWaiting;ULONG Observed,Repairs;
} INIT_JOB;
static __declspec(thread) INIT_JOB *Job;
static __declspec(thread) LONG Critical;
static __declspec(thread) ERESOURCE *Resources[4];
static __declspec(thread) BOOLEAN Exclusive[4];
static __declspec(thread) ULONG ResourceDepth;
static volatile LONG Contexts,NblPools,ResourceOwners,Handles,ActiveThreads;
static ULONG Cases,HaltJoins,InitJoins,Compensations;
static HANDLE Event(VOID){HANDLE h=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static VOID Close(HANDLE h){CHECK(h&&CloseHandle(h)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE h){CHECK(WaitForSingleObject(h,10000)==WAIT_OBJECT_0);}
static VOID KeEnterCriticalRegion(VOID){++Critical;}
static VOID KeLeaveCriticalRegion(VOID){CHECK(Critical>0);--Critical;}
static VOID ExAcquireResourceSharedLite(ERESOURCE *r,BOOLEAN wait)
{CHECK(r->Initialized&&wait&&Critical>0&&ResourceDepth<4);AcquireSRWLockShared(&r->Lock);Resources[ResourceDepth]=r;Exclusive[ResourceDepth++]=FALSE;}
static VOID ExAcquireResourceExclusiveLite(ERESOURCE *r,BOOLEAN wait)
{CHECK(r->Initialized&&wait&&Critical>0&&ResourceDepth<4);AcquireSRWLockExclusive(&r->Lock);Resources[ResourceDepth]=r;Exclusive[ResourceDepth++]=TRUE;}
static VOID ExReleaseResourceLite(ERESOURCE *r)
{
    CHECK(ResourceDepth&&Resources[ResourceDepth-1]==r);--ResourceDepth;
    if(Job&&r==&TunDispatchCtxGuard&&Exclusive[ResourceDepth]&&Job->Device.Reserved){
        CHECK(Job->Device.Reserved==Job->Context&&Job->Attributes==3&&Job->Context->Device.RegistrationLock.Initialized&&Job->Pool&&!Job->Context->Running);
        ++Job->Publications;
    }
    if(Exclusive[ResourceDepth])ReleaseSRWLockExclusive(&r->Lock);else ReleaseSRWLockShared(&r->Lock);
}
static NTSTATUS ExInitializeResourceLite(ERESOURCE *r)
{CHECK(Job&&r==&Job->Context->Device.RegistrationLock&&!r->Initialized);++Job->ResourceInits;if(Job->Failure==FAIL_RESOURCE)return STATUS_INSUFFICIENT_RESOURCES;
 InitializeSRWLock(&r->Lock);r->Initialized=TRUE;InterlockedIncrement(&ResourceOwners);return STATUS_SUCCESS;}
static VOID ExDeleteResourceLite(ERESOURCE *r)
{CHECK(Job&&r==&Job->Context->Device.RegistrationLock&&r->Initialized&&!ResourceDepth);r->Initialized=FALSE;++Job->ResourceDeletes;CHECK(InterlockedDecrement(&ResourceOwners)>=0);}
static VOID ExInitializeRundownProtection(EX_RUNDOWN_REF *r){CHECK(!r->Initialized);InitializeSRWLock(&r->Lock);r->Initialized=TRUE;}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *r)
{CHECK(r->Initialized&&!r->Closing);AcquireSRWLockShared(&r->Lock);InterlockedIncrement(&r->References);return TRUE;}
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *r)
{CHECK(r->Initialized&&InterlockedDecrement(&r->References)>=0);ReleaseSRWLockShared(&r->Lock);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r)
{
    CHECK(Job&&Job->HaltStarted&&r->Initialized&&!ResourceDepth&&!Critical);r->Closing=TRUE;
    if(r==&Job->Context->ControlRundown){++Job->ControlWaits;CHECK(!Job->Device.Reserved&&r->References==1&&SetEvent(Job->HaltWaiting));}
    else{CHECK(r==&Job->Context->Probe.ReturnRundown&&!r->References);++Job->ReturnWaits;}
    AcquireSRWLockExclusive(&r->Lock);CHECK(!r->References);ReleaseSRWLockExclusive(&r->Lock);
}
#include "../../driver/context_gate.h"
static VOID KeInitializeEvent(KEVENT *event,ULONG kind,BOOLEAN signal)
{CHECK(!kind&&!event->Native);event->Native=Event();if(signal)CHECK(SetEvent(event->Native));}
#define NotificationEvent 0u
#define IO_NO_INCREMENT 0
static VOID KeClearEvent(KEVENT *event){CHECK(event->Native&&ResetEvent(event->Native));}
static VOID KeInitializeSpinLock(KSPIN_LOCK *lock){memset(lock,0,sizeof(*lock));}
static VOID ExInitializeFastMutex(FAST_MUTEX *lock){InitializeSRWLock(lock);}
static KIRQL ExAcquireSpinLockExclusive(EX_SPIN_LOCK *lock){AcquireSRWLockExclusive(lock);return 0;}
static VOID ExReleaseSpinLockExclusive(EX_SPIN_LOCK *lock,KIRQL irql){CHECK(!irql);ReleaseSRWLockExclusive(lock);}
static VOID *ExAllocatePoolZero(ULONG kind,SIZE_T bytes,ULONG tag)
{
    CHECK(Job&&!kind&&tag==TUN_MEMORY_TAG&&bytes==sizeof(TUN_CTX)&&!Job->Context);++Job->Allocations;
    if(Job->Failure==FAIL_CONTEXT)return NULL;
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(bytes);memset(p,0,bytes);
#else
    VOID *p=calloc(1,bytes);
#endif
    CHECK(p);Job->Context=p;InterlockedIncrement(&Contexts);return p;
}
#define NonPagedPool 0u
static VOID ExFreePoolWithTag(VOID *p,ULONG tag)
{
    CHECK(Job&&p==Job->Context&&tag==TUN_MEMORY_TAG&&!Job->Device.Reserved&&!Job->Pool&&!Job->Context->V2);
    TUN_CTX *ctx=p;
    if(ctx->Device.RegistrationLock.Initialized){
        CHECK(!Job->HaltStarted&&Job->Failure==FAIL_GENERAL&&!Job->Observed++&&!Job->Repairs&&SetEvent(Job->FreeEntered));Await(Job->FreeRescue);
        CHECK(!ctx->Device.RegistrationLock.Initialized&&Job->Repairs==1);
    }
    CHECK(!ctx->ControlRundown.References&&!ctx->Probe.ReturnRundown.References);
    KEVENT *events[]={&ctx->Probe.RunningEvent,&ctx->Probe.IndicationIdle[0],&ctx->Probe.IndicationIdle[1],&ctx->Device.Disconnected};
    for(ULONG i=0;i<4;++i)if(events[i]->Native)Close(events[i]->Native);
    Job->Context=NULL;CHECK(InterlockedDecrement(&Contexts)>=0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static VOID NdisMGetDeviceProperty(NDIS_HANDLE adapter,DEVICE_OBJECT **a,DEVICE_OBJECT **fdo,DEVICE_OBJECT **b,VOID *c,VOID *d)
{CHECK(Job&&adapter==Job&&!a&&fdo&&!b&&!c&&!d&&!Job->Device.Reserved);++Job->Properties;*fdo=Job->Failure==NO_FDO?NULL:&Job->Device;}
static VOID *NdisAllocateNetBufferListPool(NDIS_HANDLE adapter,const NET_BUFFER_LIST_POOL_PARAMETERS *p)
{
    CHECK(Job&&adapter==Job&&!Job->Device.Reserved&&Job->Context->Device.RegistrationLock.Initialized&&!Job->Pool);++Job->PoolRequests;
    CHECK(p->Header.Type==NDIS_OBJECT_TYPE_DEFAULT&&p->Header.Revision==NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1&&p->Header.Size==NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1);
    CHECK(p->ProtocolId==NDIS_PROTOCOL_ID_DEFAULT&&p->fAllocateNetBuffer&&!p->ContextSize&&p->PoolTag==TUN_MEMORY_TAG&&!p->DataSize&&!p->Flags);
    if(Job->Failure==FAIL_NBL_POOL)return NULL;Job->Pool=malloc(1);CHECK(Job->Pool);InterlockedIncrement(&NblPools);return Job->Pool;
}
static VOID NdisFreeNetBufferListPool(VOID *pool)
{CHECK(Job&&pool==Job->Pool);free(pool);Job->Pool=NULL;++Job->PoolDeletes;CHECK(InterlockedDecrement(&NblPools)>=0);}
static NDIS_STATUS NdisMSetMiniportAttributes(NDIS_HANDLE adapter,PNDIS_MINIPORT_ADAPTER_ATTRIBUTES raw)
{
    CHECK(Job&&adapter==Job&&!Job->Device.Reserved&&Job->Pool&&Job->Context->Device.RegistrationLock.Initialized);NDIS_OBJECT_HEADER *h=(NDIS_OBJECT_HEADER *)raw;++Job->Attributes;
    if(Job->Attributes==1){
        NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES *p=(VOID *)raw;
        CHECK(h->Type==NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES&&h->Revision==(NdisVersion<PB_NDIS_VERSION(30)?1:2)&&h->Size==NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1);
        CHECK(p->MiniportAdapterContext==Job->Context&&p->AttributeFlags==(NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND|NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK)&&p->InterfaceType==NdisInterfaceInternal&&!p->CheckForHangTimeInSeconds);
        if(Job->Failure==FAIL_REGISTRATION)return NDIS_STATUS_BAD_VERSION;Job->NdisContext=p->MiniportAdapterContext;
    }else if(Job->Attributes==2){
        NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES *p=(VOID *)raw;NDIS_PM_CAPABILITIES *pm=p->PowerManagementCapabilitiesEx;
        CHECK(h->Type==NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES&&h->Revision==2&&h->Size==NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2&&Job->NdisContext==Job->Context);
        CHECK(p->MediaType==NdisMedium802_3&&p->MtuSize==TUN_MAX_IP_PACKET_SIZE&&p->MacAddressLength==6&&!memcmp(p->PermanentMacAddress,Job->Context->Probe.LocalMac,6)&&!memcmp(p->CurrentMacAddress,p->PermanentMacAddress,6));
        CHECK(p->SupportedOidList&&p->SupportedOidListLength==24*sizeof(NDIS_OID)&&!p->PowerManagementCapabilities&&pm&&pm->Header.Revision==(NdisVersion<PB_NDIS_VERSION(30)?1:2));
        CHECK(pm->Header.Size==(NdisVersion<PB_NDIS_VERSION(30)?NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1:NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_2));
        CHECK(!pm->MinMagicPacketWakeUp&&!pm->MinPatternWakeUp&&!pm->MinLinkChangeWakeUp);
        if(Job->Failure==FAIL_GENERAL)return NDIS_STATUS_BAD_VERSION;
    }else{
        NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES *p=(VOID *)raw;
        CHECK(Job->Attributes==3&&h->Type==NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES&&h->Revision==1&&h->Size==NDIS_SIZEOF_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1);
        CHECK(p->DefaultOffloadConfiguration->Header.Revision==PbPlatformOffloadRevision(NdisVersion)&&p->HardwareOffloadCapabilities->Header.Revision==PbPlatformOffloadRevision(NdisVersion));
        CHECK(p->HardwareOffloadCapabilities->Rsc.IPv4.Enabled==(ULONG)(NdisVersion>=PB_NDIS_VERSION(30))&&p->HardwareOffloadCapabilities->UdpRsc.Enabled==(ULONG)(NdisVersion>=PB_NDIS_VERSION(89)));
        CHECK(!p->DefaultOffloadConfiguration->Checksum.IPv4Transmit.IpChecksum&&!p->DefaultOffloadConfiguration->Rsc.IPv4.Enabled&&!p->DefaultOffloadConfiguration->UdpRsc.Enabled);
        CHECK(p->DefaultTcpConnectionOffloadConfiguration&&p->TcpConnectionOffloadHardwareCapabilities==p->DefaultTcpConnectionOffloadConfiguration);
        if(Job->Failure==FAIL_OFFLOAD)return NDIS_STATUS_BAD_VERSION;
    }
    return NDIS_STATUS_SUCCESS;
}
#include "../../driver/v2/offload_format.h"
#include PB_INIT_OFFLOAD
static volatile LONG UnexpectedSession=1;
static VOID RemoveEntryList(LIST_ENTRY *entry){(void)entry;CHECK(!UnexpectedSession);}
static VOID TunIndicateStatus(NDIS_HANDLE adapter,NDIS_MEDIA_CONNECT_STATE state){(void)adapter;(void)state;CHECK(!UnexpectedSession);}
static VOID PbV2Unregister(TUN_CTX *ctx){(void)ctx;CHECK(!UnexpectedSession);}
#include PB_INIT_COMMON
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_INIT_SELECTED
#include PB_INIT_HALT
#pragma warning(pop)
static DWORD WINAPI InitThread(VOID *opaque){Job=opaque;Job->Result=TunInitializeEx(Job,NULL,&Job->Parameters);return 0;}
static DWORD WINAPI HaltThread(VOID *opaque){Job=opaque;Job->HaltStarted=TRUE;TunHaltEx(Job->Context,NdisHaltDeviceDisabled);return 0;}
static HANDLE Start(LPTHREAD_START_ROUTINE routine,INIT_JOB *job){HANDLE h=CreateThread(NULL,0,routine,job,0,NULL);CHECK(h);InterlockedIncrement(&Handles);InterlockedIncrement(&ActiveThreads);return h;}
static VOID Join(HANDLE h){Await(h);Close(h);CHECK(InterlockedDecrement(&ActiveThreads)>=0);}
static VOID Scenario(ULONG version,ULONG failure,ULONG identity,BOOLEAN negative)
{
    CHECK(!Contexts&&!NblPools&&!ResourceOwners&&!Handles&&!ActiveThreads);INIT_JOB job={0};Job=&job;job.Failure=failure;
    job.Parameters.Header=(NDIS_OBJECT_HEADER){NDIS_OBJECT_TYPE_MINIPORT_INIT_PARAMETERS,NDIS_MINIPORT_INIT_PARAMETERS_REVISION_1,NDIS_SIZEOF_MINIPORT_INIT_PARAMETER_REVISION_1};
    job.Parameters.IfIndex=1;job.Parameters.NetLuid.Info.IfType=IF_TYPE_ETHERNET_CSMACD;job.Parameters.NetLuid.Info.NetLuidIndex=identity;NdisVersion=PB_NDIS_VERSION(version);
    if(negative){
        CHECK(failure==FAIL_GENERAL);job.FreeEntered=Event();job.FreeRescue=Event();HANDLE t=Start(InitThread,&job);Await(job.FreeEntered);
        CHECK(job.Context&&job.Context->Device.RegistrationLock.Initialized&&!job.Pool&&!job.Device.Reserved&&Contexts==1&&ResourceOwners==1&&!job.Repairs);
        ExDeleteResourceLite(&job.Context->Device.RegistrationLock);++job.Repairs;CHECK(SetEvent(job.FreeRescue));Join(t);++InitJoins;Close(job.FreeEntered);Close(job.FreeRescue);
    }else job.Result=TunInitializeEx(&job,NULL,&job.Parameters);
    BOOLEAN success=failure==SUCCEED||failure==FAIL_OFFLOAD;
    CHECK(job.Result==(success?NDIS_STATUS_SUCCESS:failure==FAIL_RESOURCE?STATUS_INSUFFICIENT_RESOURCES:NDIS_STATUS_FAILURE));
    CHECK(job.Allocations==1&&job.ResourceInits==(ULONG)(failure!=FAIL_CONTEXT&&failure!=NO_FDO));
    CHECK(job.Properties==(ULONG)(failure!=FAIL_CONTEXT)&&job.PoolRequests==(ULONG)(success||failure>=FAIL_NBL_POOL));
    CHECK(job.Attributes==(success?3u:failure==FAIL_GENERAL?2u:failure==FAIL_REGISTRATION?1u:0u));
    if(success){
        CHECK(job.Context&&job.Device.Reserved==job.Context&&job.NdisContext==job.Context&&job.Publications==1&&job.Attributes==3&&!job.Context->Running&&!job.Context->V2);
        CHECK(WaitForSingleObject(job.Context->Probe.RunningEvent.Native,0)==WAIT_TIMEOUT&&WaitForSingleObject(job.Context->Device.Disconnected.Native,0)==WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(job.Context->Probe.IndicationIdle[0].Native,0)==WAIT_OBJECT_0&&WaitForSingleObject(job.Context->Probe.IndicationIdle[1].Native,0)==WAIT_OBJECT_0);
        UCHAR mac[]={2,0x50,(UCHAR)identity,(UCHAR)(identity>>8),(UCHAR)(identity>>16),(UCHAR)(identity>>24)};CHECK(!memcmp(mac,job.Context->Probe.LocalMac,6));mac[1]^=0x80;CHECK(!memcmp(mac,job.Context->Probe.PeerMac,6));
        CHECK(job.Context->Probe.RegistrationStatus==(failure==FAIL_OFFLOAD?NDIS_STATUS_BAD_VERSION:NDIS_STATUS_SUCCESS)&&!job.Context->Probe.AllowedMask&&!job.Context->Probe.EncapsulationMask);
        CHECK((ULONG)job.Context->Probe.NdisMask==(failure==FAIL_OFFLOAD?0:PbPlatformOffloads(NdisVersion)));
        TUN_CTX *held=TunReferenceDeviceCtx(&job.Device);CHECK(held==job.Context);job.HaltWaiting=Event();HANDLE halt=Start(HaltThread,&job);Await(job.HaltWaiting);
        CHECK(!job.Device.Reserved&&!TunReferenceDeviceCtx(&job.Device)&&job.Context==held&&held->Halting&&Contexts==1&&NblPools==1&&ResourceOwners==1);
        TunDereferenceCtx(held);Join(halt);++HaltJoins;Close(job.HaltWaiting);CHECK(job.ControlWaits==1&&job.ReturnWaits==1&&job.ResourceDeletes==1&&job.PoolDeletes==1);
    }else CHECK(!job.Context&&!job.Device.Reserved&&!job.Publications&&!job.HaltStarted);
    CHECK(job.PoolDeletes==(ULONG)(success||failure==FAIL_GENERAL||failure==FAIL_REGISTRATION));
    CHECK(job.ResourceDeletes==(ULONG)(success||failure>=FAIL_NBL_POOL));
    CHECK(!Contexts&&!NblPools&&!ResourceOwners&&!Handles&&!ActiveThreads&&!Critical&&!ResourceDepth&&job.Repairs==(ULONG)negative);
    Compensations+=job.Repairs;++Cases;Job=NULL;
}
static VOID Matrix(VOID){const ULONG versions[]={20,29,30,89};for(ULONG v=0;v<4;++v)for(ULONG failure=0;failure<8;++failure)for(ULONG max=0;max<2;++max)Scenario(versions[v],failure,max?0xffffffu:0,FALSE);}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"130");CHECK(argc==1||selected);
#ifdef PB_INIT_WITHOUT_RESOURCE_DELETE
    BOOLEAN negative=TRUE;CHECK(selected);
#else
    BOOLEAN negative=FALSE;
#endif
    if(selected){Scenario(20,FAIL_GENERAL,0xffffffu,negative);Scenario(20,FAIL_GENERAL,0xffffffu,negative);}else Matrix();
    ULONG warmed=Cases,warmRepairs=Compensations;Cases=Compensations=HaltJoins=InitJoins=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Scenario(20,FAIL_GENERAL,0xffffffu,negative);else Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    CHECK(Compensations==(ULONG)negative&&warmRepairs==(negative?warmed:0));
    printf("PASS: %lu actual Miniport Initialize/initial-Paused Halt cases plus %lu checked warmup; required rollback/optional plain fallback, native NDIS formats and publication, %lu Halt/%lu Init thread joins; zero owned resources; handles %lu/%lu; compensation%lu/%lu\n",Cases,warmed,HaltJoins,InitJoins,before,after,Compensations,warmRepairs);
    if(negative){puts("MINIPORT_INIT_FAILURE_RESOURCE_NOT_DELETED: pre-Ctx-free observer held live registration resource; one actual Delete compensation before original free, initializer joined and all resources reclaimed before130");return 130;}return 0;
}
