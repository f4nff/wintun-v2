#pragma once
/* SPDX-License-Identifier: GPL-2.0 */
/* Production NBL allocation/return/completion paths with tracked hosted DDI substitutes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PB_GUARD_TESTS
#include "../safety/guard_memory.h"
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
#define NT_ASSERT CHECK
typedef struct { HANDLE Native; } KEVENT, *PKEVENT;
typedef struct MDL { struct MDL *Next; VOID *Address; ULONG Length; BOOLEAN Partial; } MDL;
typedef struct { LONG References; SRWLOCK Lock; } EX_RUNDOWN_REF;
#include "../../driver/v2/session_state.h"
#include "../../common/ethernet.h"
#include "../../common/return_diagnostics.h"
typedef UCHAR KIRQL;
typedef struct _TUN_CTX {
    SRWLOCK TransitionLock; volatile LONG Running,Halting; PB_KERNEL_SESSION *V2;
    VOID *NblPool,*MiniportAdapterHandle;
    struct { SRWLOCK RegistrationLock; KEVENT Disconnected; } Device;
    struct {
        UCHAR LocalMac[6],PeerMac[6]; volatile LONG V2SessionActive,AllowedMask; KEVENT RunningEvent;
        EX_RUNDOWN_REF ReturnRundown;
        volatile LONG64 RscPackets,RscBytes,RscEvents,RscAborts,UroPackets,UroEvents;
        volatile LONG64 LsoNbls,UsoNbls,ChecksumNbls;
        KSPIN_LOCK ReturnDiagnosticsLock;
        PB_RETURN_HISTORY ReturnDiagnostics;
    } Probe;
    struct { LONG64 ifHCInOctets,ifHCInUcastOctets,ifHCInUcastPkts,ifInErrors,ifInDiscards;
        LONG64 ifHCOutOctets,ifHCOutUcastOctets,ifHCOutUcastPkts; } Statistics;
} TUN_CTX;
typedef union { struct { USHORT CoalescedSegCount; } Info; VOID *Value; } NDIS_RSC_NBL_INFO;
typedef union { struct { USHORT SegCount,SegSize; } Receive; VOID *Value; } NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO;
typedef union {
    struct { ULONG TcpChecksumFailed:1,UdpChecksumFailed:1,IpChecksumFailed:1,TcpChecksumSucceeded:1,
        UdpChecksumSucceeded:1,IpChecksumSucceeded:1,Loopback:1,TcpChecksumValueInvalid:1,IpChecksumValueInvalid:1; } Receive;
    VOID *Value;
} NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO;
enum { NetBufferListFrameType,TcpRecvSegCoalesceInfo,UdpRecvSegCoalesceOffloadInfo,TcpIpChecksumNetBufferListInfo,RscTcpTimestampDelta,Ieee8021QNetBufferListInfo,InfoCount };
typedef struct NET_BUFFER_LIST { struct NET_BUFFER_LIST *Next; VOID *Reserved[2],*SourceHandle,*Info[InfoCount]; ULONG Flags,Status; MDL *FirstMdl; ULONG Length; } NET_BUFFER_LIST;
#define NET_BUFFER_LIST_NEXT_NBL_EX(n) ((n)->Reserved[1])
#define NET_BUFFER_LIST_OFFSET(n) ((n)->Reserved[0])
#define NET_BUFFER_LIST_STATUS(n) ((n)->Status)
#define NET_BUFFER_LIST_NEXT_NBL(n) ((n)->Next)
#define NET_BUFFER_LIST_INFO(n,i) ((n)->Info[i])
#define NDIS_NBL_FLAGS_IS_IPV4 1u
#define NDIS_NBL_FLAGS_IS_IPV6 2u
#define NDIS_NBL_FLAGS_IS_TCP 4u
#define NDIS_NBL_FLAGS_IS_UDP 8u
#define NDIS_ETH_TYPE_IPV4 0x0800u
#define NDIS_ETH_TYPE_IPV6 0x86ddu
#define NDIS_STATUS_SUCCESS 0u
#define TUN_MEMORY_TAG 0u
#define POOL_FLAG_NON_PAGED 0u
#define NonPagedPool 0u
#define ExAllocatePoolZero ExAllocatePool2
#define IO_NO_INCREMENT 0
#define MAXULONG 0xffffffffu
#define HTONS(x) ((USHORT)((((x)&0xffu)<<8)|(((x)>>8)&0xffu)))
typedef struct { SRWLOCK *Lock; } KLOCK_QUEUE_HANDLE;
static ULONG Failure,Acquisitions;
static volatile LONG Pools,Mdls,Nbls;
static volatile LONG FixtureLocks,FixtureRundownAcquires,FixtureRundownReleases;
static volatile LONG FixtureDiagnosticLocks;
static VOID (*BeforePoolFree)(VOID *);
static VOID (*AfterRundownAcquire)(EX_RUNDOWN_REF *);
static VOID (*BeforeRundownRelease)(EX_RUNDOWN_REF *);
static VOID (*OnSetEvent)(KEVENT *);
static VOID (*AfterNblAllocate)(VOID);
static VOID (*BeforeNblFree)(NET_BUFFER_LIST *);
static VOID (*AfterQueueUnlock)(KSPIN_LOCK *);
static VOID (*AfterQueueLock)(KSPIN_LOCK *);
static VOID (*BeforeDiagnosticLock)(KSPIN_LOCK *);
static BOOLEAN Fail(VOID) { return ++Acquisitions==Failure; }
static VOID *ExAllocatePool2(ULONG flags,SIZE_T size,ULONG tag)
{
    (void)flags; (void)tag; if(Fail()) return NULL;
#ifdef PB_GUARD_TESTS
    VOID *p=PbTestGuardAlloc(size); memset(p,0,size);
#else
    VOID *p=calloc(1,size);
#endif
    CHECK(p); InterlockedIncrement(&Pools); return p;
}
static VOID ExFreePoolWithTag(VOID *p,ULONG tag)
{
    (void)tag; CHECK(p && Pools>0); InterlockedDecrement(&Pools);
    if(BeforePoolFree) BeforePoolFree(p);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
}
static MDL *IoAllocateMdl(VOID *address,ULONG length,BOOLEAN secondary,BOOLEAN quota,VOID *irp)
{
    (void)secondary; (void)quota; (void)irp; if(Fail()) return NULL;
#ifdef PB_GUARD_TESTS
    MDL *mdl=PbTestGuardAlloc(sizeof(*mdl)); memset(mdl,0,sizeof(*mdl));
#else
    MDL *mdl=calloc(1,sizeof(*mdl));
#endif
    CHECK(mdl); mdl->Address=address; mdl->Length=length; InterlockedIncrement(&Mdls); return mdl;
}
static VOID IoFreeMdl(MDL *mdl)
{
    CHECK(mdl && Mdls>0); InterlockedDecrement(&Mdls);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(mdl);
#else
    free(mdl);
#endif
}
static VOID *MmGetMdlVirtualAddress(MDL *mdl) { return mdl->Address; }
static VOID MmBuildMdlForNonPagedPool(MDL *mdl) { CHECK(mdl && !mdl->Partial); }
static VOID IoBuildPartialMdl(MDL *parent,MDL *child,VOID *address,ULONG length)
{
    CHECK((UCHAR *)address>=(UCHAR *)parent->Address &&
        (UCHAR *)address+length<=(UCHAR *)parent->Address+parent->Length);
    child->Partial=TRUE;
}
static NET_BUFFER_LIST *NdisAllocateNetBufferAndNetBufferList(VOID *pool,ULONG csize,ULONG backfill,MDL *mdl,ULONG offset,ULONG length)
{
    (void)pool; CHECK(!csize && !backfill && !offset && mdl);
    if(Fail()) return NULL;
#ifdef PB_GUARD_TESTS
    NET_BUFFER_LIST *nbl=PbTestGuardAlloc(sizeof(*nbl)); memset(nbl,0,sizeof(*nbl));
#else
    NET_BUFFER_LIST *nbl=calloc(1,sizeof(*nbl));
#endif
    CHECK(nbl); InterlockedIncrement(&Nbls); nbl->FirstMdl=mdl; nbl->Length=length;
    nbl->Flags=15; for(ULONG i=0;i<InfoCount;++i) nbl->Info[i]=(VOID *)(ULONG_PTR)~0u;
    if(AfterNblAllocate) AfterNblAllocate();
    return nbl;
}
static VOID NdisFreeNetBufferList(NET_BUFFER_LIST *nbl)
{
    CHECK(nbl && Nbls>0); if(BeforeNblFree) BeforeNblFree(nbl); InterlockedDecrement(&Nbls);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(nbl);
#else
    free(nbl);
#endif
}
static VOID NdisClearNblFlag(NET_BUFFER_LIST *nbl,ULONG flags) { nbl->Flags&=~flags; }
static VOID NdisSetNblFlag(NET_BUFFER_LIST *nbl,ULONG flags) { nbl->Flags|=flags; }
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *refs)
{
    AcquireSRWLockShared(&refs->Lock); InterlockedIncrement(&refs->References); InterlockedIncrement(&FixtureRundownAcquires);
    if(AfterRundownAcquire) AfterRundownAcquire(refs);
    return TRUE;
}
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *refs)
{ CHECK(refs->References>0); if(BeforeRundownRelease) BeforeRundownRelease(refs); InterlockedDecrement(&refs->References); InterlockedIncrement(&FixtureRundownReleases); ReleaseSRWLockShared(&refs->Lock); }
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{ handle->Lock=(SRWLOCK *)lock; AcquireSRWLockExclusive(handle->Lock); InterlockedIncrement(&FixtureLocks); if(AfterQueueLock) AfterQueueLock(lock); }
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle)
{ ReleaseSRWLockExclusive(handle->Lock); if(AfterQueueUnlock) AfterQueueUnlock((KSPIN_LOCK *)handle->Lock); }
static VOID KeAcquireSpinLock(KSPIN_LOCK *lock,KIRQL *irql)
{ *irql=0; if(BeforeDiagnosticLock) BeforeDiagnosticLock(lock); AcquireSRWLockExclusive((SRWLOCK *)lock); InterlockedIncrement(&FixtureDiagnosticLocks); }
static VOID KeReleaseSpinLock(KSPIN_LOCK *lock,KIRQL irql)
{ (void)irql; ReleaseSRWLockExclusive((SRWLOCK *)lock); }
static VOID KeClearEvent(KEVENT *event) { CHECK(ResetEvent(event->Native)); }
static VOID KeSetEvent(KEVENT *event,int priority,BOOLEAN wait)
{ (void)priority; (void)wait; if(OnSetEvent) OnSetEvent(event); CHECK(SetEvent(event->Native)); }
#include "../../driver/v2/session_fault.h"
#ifndef PB_REFERENCE_SOURCE
#define PB_REFERENCE_SOURCE "../../driver/v2/session_references.h"
#endif
#include PB_REFERENCE_SOURCE
#include "../../driver/v2/session_completion.h"
#ifndef PB_NBL_SOURCE
#define PB_NBL_SOURCE "../../driver/v2/session_nbl.h"
#endif
#include PB_NBL_SOURCE
