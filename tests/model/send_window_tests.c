/* SPDX-License-Identifier: GPL-2.0 */
/* Execute production send/recycle/reference code with tracked DDI stand-ins.
 * These counters measure critical-section work, not kernel IRQL or elapsed DPC time. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
#define PbPoolReserve ProductionPoolReserve
#define PbPoolTransition ProductionPoolTransition
#include "../../common/v2_core.c"
#undef PbPoolReserve
#undef PbPoolTransition
#include "../../common/ethernet.h"
#include "../../common/send_diagnostics.h"
#define PbEthernetInspect ActualEthernetInspect
#define PbEthernetControl ActualEthernetControl
#define PbEthernetHeader ActualEthernetHeader
#include "../../common/ethernet.c"
#undef PbEthernetInspect
#undef PbEthernetControl
#undef PbEthernetHeader
#define PbSelectQueue ActualSelectQueue
#include "../../common/flow.c"
#undef PbSelectQueue
typedef struct { LONG Value; } KEVENT,*PKEVENT,EX_RUNDOWN_REF;
typedef struct READ_MDL {struct READ_MDL *Next;UCHAR *Address;ULONG Length;BOOLEAN Failed;} MDL;
#include "../../driver/v2/session_state.h"
typedef UCHAR KIRQL;
typedef LONG NDIS_STATUS;
typedef VOID *NDIS_HANDLE;
typedef struct NB { struct NB *Next; ULONG Length,Queue,Kind,Reject; const UCHAR *Frame;MDL *Mdl;ULONG MdlOffset; } NET_BUFFER;
typedef struct NBL { struct NBL *Next; NET_BUFFER *Nb; LONG Status; VOID *Info[4]; } NET_BUFFER_LIST;
typedef struct _TUN_CTX {
    LONG TransitionLock; volatile LONG Running; PB_KERNEL_SESSION *V2; NDIS_HANDLE MiniportAdapterHandle;
    struct { volatile LONG64 ifHCOutOctets,ifHCOutUcastOctets,ifHCOutUcastPkts; } Statistics;
    struct { UCHAR PeerMac[6]; volatile LONG64 LsoNbls,UsoNbls,ChecksumNbls,OffloadRejected;
        KSPIN_LOCK SendDiagnosticsLock; PB_SEND_HISTORY SendDiagnostics; } Probe;
} TUN_CTX;
typedef struct { KSPIN_LOCK *Lock; ULONG First, FirstRead; BOOLEAN Tx; } KLOCK_QUEUE_HANDLE;
#define NET_BUFFER_LIST_NEXT_NBL(n) ((n)->Next)
#define NET_BUFFER_LIST_FIRST_NB(n) ((n)->Nb)
#define NET_BUFFER_NEXT_NB(n) ((n)->Next)
#define NET_BUFFER_DATA_LENGTH(n) ((n)->Length)
#define NET_BUFFER_CURRENT_MDL(n) ((n)->Mdl)
#define NET_BUFFER_CURRENT_MDL_OFFSET(n) ((n)->MdlOffset)
#define NET_BUFFER_LIST_STATUS(n) ((n)->Status)
#define NET_BUFFER_LIST_INFO(n,k) ((n)->Info[k])
#define TcpLargeSendNetBufferListInfo 0
#define UdpSegmentationOffloadInfo 1
#define TcpIpChecksumNetBufferListInfo 2
#define NDIS_STATUS_SUCCESS 0
#define NDIS_STATUS_PAUSED 1
#define NDIS_STATUS_RESOURCES 2
#define NDIS_STATUS_MEDIA_DISCONNECTED 3
#define NDIS_STATUS_INVALID_DATA 4
#define NDIS_STATUS_INVALID_LENGTH 5
#define IO_NO_INCREMENT 0
#define NT_ASSERT CHECK
static ULONG Reads,Reserves,Describes,Completed,Replies,Windows,MaxWork,MaxBytes,MaxQueueWork,Work,Bytes;
static ULONG Trigger,Action,Reenter,PoisonOnComplete,Indications,ExpectedPin,CorruptAt,AfterPinRelease;
static ULONG Controls,RecycleAttempts,Faults,LastReplyLength;
static ULONG TxLockAcquires,MaxQueueReads,RecyclePolls,ReadySpaceReads,ReadyAtRead,ReadyAction,UserSignals;
static ULONG64 RecycleSnapshotBytes;
static VOID (*AfterRecyclePop)(PB_QUEUE_VIEW *,uint32_t);
static BOOLEAN RealEthernet,RealMdl,ReadingPayload;
static ULONG PrefixCalls,PrefixMaps,PayloadMaps,MdlVisits;
static ULONG64 PrefixBytes,PayloadBytes;
static UCHAR LastReply[PB_ETH_CONTROL_MAX];
static TUN_CTX *Current;
static PB_KERNEL_SESSION *Session;
static PB_QUEUE_VIEW Consumers[PB_MAX_QUEUES],Recyclers[PB_MAX_QUEUES];
static ULONG ReturnAtReserve, ReturnAtRead;
static PB_DESCRIPTOR LateReturn, LateReadItems[PB_MAX_BATCH];
static ULONG LateReadCount, CorruptReadProducer, LateReadDrip;
static ULONG ReturnOnWake, WakeReturned;
static ULONG ReturnOnMask, CorruptOnMask;
static PB_DESCRIPTOR MaskReturn;
static ULONG ObserveCapture, CaptureReadOrder, PublishAfterReturnRead;
static ULONG CaptureReturnReads, CaptureReadyReads;
static VOID OnWindowEnd(VOID);
static VOID OnUserWake(VOID);
static VOID AssertUnlocked(VOID)
{
    CHECK(!Current->TransitionLock);
    for(ULONG i=0;i<Session->Layout.QueueCount;++i) CHECK(!Session->Queues[i].TxLock);
}
static KIRQL ExAcquireSpinLockShared(LONG *lock)
{ CHECK(!*lock); *lock=1; Work=Bytes=0; Windows++; return 0; }
static VOID ExReleaseSpinLockShared(LONG *lock,KIRQL level)
{
    (void)level; CHECK(*lock==1); *lock=0;
    if(Work>MaxWork) MaxWork=Work; if(Bytes>MaxBytes) MaxBytes=Bytes; OnWindowEnd();
}
static VOID KeAcquireInStackQueuedSpinLock(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{
    CHECK(!*lock); *lock=1; handle->Lock=lock; handle->First=Reserves;handle->FirstRead=Reads;handle->Tx=FALSE;
    for(ULONG i=0;i<Session->Layout.QueueCount;++i) if(lock==&Session->Queues[i].TxLock) {handle->Tx=TRUE;TxLockAcquires++;}
}
static VOID KeReleaseInStackQueuedSpinLock(KLOCK_QUEUE_HANDLE *handle)
{
    CHECK(*handle->Lock); *handle->Lock=0; if(Reserves-handle->First>MaxQueueWork) MaxQueueWork=Reserves-handle->First;
    if(handle->Tx && Reads-handle->FirstRead>MaxQueueReads) MaxQueueReads=Reads-handle->FirstRead;
}
static VOID KeAcquireSpinLock(KSPIN_LOCK *lock,KIRQL *irql) { CHECK(!*lock); *lock=1; *irql=0; }
static VOID KeReleaseSpinLock(KSPIN_LOCK *lock,KIRQL irql) { (void)irql; CHECK(*lock); *lock=0; }
static ULONG PbEffectiveMask(TUN_CTX *ctx)
{
    (void)ctx;
    if(ReturnOnMask) {uint32_t n,notify;ReturnOnMask=0;CHECK(PbQueuePush(Recyclers,&MaskReturn,1,&n,&notify)==PB_OK && n==1);}
    if(CorruptOnMask) {CorruptOnMask=0;Recyclers[0].Shared->Producer+=Recyclers[0].Capacity+1;}
    return 0x1ffff;
}
static VOID KeSetEvent(KEVENT *event,LONG priority,BOOLEAN wait)
{
    (void)priority; (void)wait; event->Value=1;if(event==&Session->StartEvent) UserSignals++;
    if(ReturnOnWake && event==Session->Queues[0].UserEvent) OnUserWake();
}
static VOID KeClearEvent(KEVENT *event) { event->Value=0; }
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *r) { CHECK(r->Value>=0); r->Value++; return TRUE; }
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *r)
{ CHECK(r->Value>0 && Session->Active==r->Value-1 && !!Session->AllReturned.Value==(r->Value==1)); r->Value--; AfterPinRelease++; }
static ULONG PbBeginIndication(TUN_CTX *ctx) { CHECK(ctx->TransitionLock); return ++Indications; }
static VOID PbEndIndication(TUN_CTX *ctx,ULONG bank) { (void)ctx; AssertUnlocked(); CHECK(bank && Indications); Indications--; }
static VOID PbV2Fault(PB_KERNEL_SESSION *s) { Faults++;s->Faulted=1; s->Draining=1; }
static PB_RESULT PbPoolTransition(PB_POOL *pool,const PB_TOKEN *token,uint32_t before,uint32_t after)
{if(before==PB_SLOT_PUBLISHED && after==PB_SLOT_FREE) RecycleAttempts++;return ProductionPoolTransition(pool,token,before,after);}
static PB_RESULT PbPoolReserve(PB_POOL *pool,PB_TOKEN *token)
{
    CHECK(Current->TransitionLock && Session->Queues[pool->Queue].TxLock); Reserves++;
    if(ReturnAtReserve==Reserves) {
        uint32_t accepted,notify;ReturnAtReserve=0;
        CHECK(PbQueuePush(Recyclers+pool->Queue,&LateReturn,1,&accepted,&notify)==PB_OK && accepted==1);
    }
    return ProductionPoolReserve(pool,token);
}
#define NormalPagePriority 0u
#define POOL_NX_OPTIN 1
static ULONG MmGetMdlByteCount(MDL *mdl) {MdlVisits++;return mdl->Length;}
static UCHAR *MmGetSystemAddressForMdlSafe(MDL *mdl,ULONG priority)
{(void)priority;if(ReadingPayload) PayloadMaps++;else PrefixMaps++;return mdl->Failed?NULL:mdl->Address;}
static VOID ReadCopy(VOID *dst,const VOID *src,SIZE_T length)
{if(ReadingPayload) PayloadBytes+=length;else PrefixBytes+=length;memcpy(dst,src,length);}
#pragma push_macro("RtlCopyMemory")
#undef RtlCopyMemory
#define RtlCopyMemory ReadCopy
#define PbV2Read ActualRead
#include "../../driver/v2/session_read.h"
#undef PbV2Read
#pragma pop_macro("RtlCopyMemory")
static BOOLEAN PbV2Read(NET_BUFFER *nb,ULONG offset,UCHAR *bytes,ULONG length)
{
    CHECK(offset<=nb->Length && length<=nb->Length-offset && Current->TransitionLock && (ULONG)Session->Active==ExpectedPin && (ULONG)Session->Returns.Value==ExpectedPin);
    if(!ReadingPayload) PrefixCalls++;
    if(!offset) {
        Reads++;Work++;Bytes+=min(nb->Length,PB_BLOCK_SIZE);
        if(ReadyAtRead==Reads) {
            ReadyAtRead=0;
            if(ReadyAction==3) Recyclers[nb->Queue].Shared->Producer+=Recyclers[nb->Queue].Capacity+1;
            else if(ReadyAction==4) Consumers[nb->Queue].Shared->Consumer=Consumers[nb->Queue].Cursor+3;
            else {
                PB_DESCRIPTOR d;uint32_t n,notify;
                CHECK(PbQueuePop(Consumers+nb->Queue,&d,1,&n)==PB_OK && n==1);
                if(ReadyAction==1) CHECK(PbQueuePush(Recyclers+nb->Queue,&d,1,&n,&notify)==PB_OK && n==1);
            }
        }
        if(LateReadDrip && Reads>=3 && Reads-3<LateReadCount) {
            uint32_t accepted,notify;
            CHECK(PbQueuePush(Recyclers+nb->Queue,LateReadItems+Reads-3,1,&accepted,&notify)==PB_OK && accepted==1);
        }
        if(ReturnAtRead==Reads) {
            uint32_t accepted,notify;ReturnAtRead=0;
            ULONG count=LateReadCount?LateReadCount:1;
            CHECK(PbQueuePush(Recyclers+nb->Queue,LateReadCount?LateReadItems:&LateReturn,count,&accepted,&notify)==PB_OK && accepted==count);
            if(CorruptReadProducer) Recyclers[nb->Queue].Shared->Producer+=Recyclers[nb->Queue].Capacity+1;
        }
    }
    if(RealMdl) return ActualRead(nb,offset,bytes,length);
    CHECK(length<=PB_ETH_CONTROL_MAX);memset(bytes,0,length);
    if(nb->Frame) {memcpy(bytes,nb->Frame+offset,length);return TRUE;}
    if(!offset && length>16) {bytes[14]=0x45;bytes[15]=(UCHAR)nb->Queue;bytes[16]=(UCHAR)nb->Kind;}
    return TRUE;
}
PB_ETH_RESULT PbEthernetInspect(const uint8_t *frame,uint32_t length,uint32_t *offset)
{if(RealEthernet) return ActualEthernetInspect(frame,length,offset); *offset=14; return frame[16]?PB_ETH_DROP:PB_ETH_IP; }
PB_ETH_RESULT PbEthernetControl(const uint8_t *frame,uint32_t length,const uint8_t peer[6],uint8_t reply[PB_ETH_CONTROL_MAX],uint32_t *replyLength)
{Controls++;if(RealEthernet) return ActualEthernetControl(frame,length,peer,reply,replyLength); *replyLength=0; return frame[16]==1?PB_ETH_REPLY:PB_ETH_IP; }
uint32_t PbSelectQueue(const uint8_t *ip,uint32_t length,uint32_t count)
{if(RealEthernet) return ActualSelectQueue(ip,length,count);CHECK(ip[1]<count);return ip[1];}
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)
{
    (void)ctx; (void)nbl; (void)offset; (void)bytes; Describes++; d->Length=20;
    if(RealMdl) {
        CHECK(offset==14 || offset==18);ReadingPayload=TRUE;
        BOOLEAN read=PbV2Read(nb,offset,bytes,nb->Length-offset);ReadingPayload=FALSE;
        if(!read) {failure->Reason=PB_SEND_PAYLOAD_READ;return NDIS_STATUS_RESOURCES;}
        CHECK(!memcmp(bytes,nb->Frame+offset,nb->Length-offset));d->Length=nb->Length-offset;
    }
    if(CorruptAt && Describes==CorruptAt) Session->Queues[nb->Queue].Rings[PB_QUEUE_TX_READY].Shared->Consumer=1;
    if(nb->Reject) {failure->Reason=PB_SEND_GSO_PLAN;return NDIS_STATUS_INVALID_DATA;} return NDIS_STATUS_SUCCESS;
}
#ifndef PB_SEND_DIAGNOSTICS_SOURCE
#define PB_SEND_DIAGNOSTICS_SOURCE "../../driver/v2/send_diagnostics.h"
#endif
static __inline PB_RESULT ObserveReturnProducer(PB_QUEUE_VIEW *view,uint64_t *producer)
{
    CaptureReturnReads++;
    if(ObserveCapture){CHECK(CaptureReadOrder==0);CaptureReadOrder=1;}
    PB_RESULT result=PbQueueSnapshotProducer(view,producer);
    if(PublishAfterReturnRead) {
        PB_DESCRIPTOR item;uint32_t n,notify;PublishAfterReturnRead=0;
        CHECK(PbQueuePop(Consumers,&item,1,&n)==PB_OK && n==1);
        CHECK(PbQueuePush(Recyclers,&item,1,&n,&notify)==PB_OK && n==1);
    }
    return result;
}
static __inline PB_RESULT ObserveReadyConsumer(PB_QUEUE_VIEW *view,uint32_t *space)
{
    CaptureReadyReads++;
    if(ObserveCapture){CHECK(CaptureReadOrder==1);CaptureReadOrder=2;}
    return PbQueueSpace(view,space);
}
#define PbQueueSnapshotProducer ObserveReturnProducer
#define PbQueueSpace ObserveReadyConsumer
#include PB_SEND_DIAGNOSTICS_SOURCE
#undef PbQueueSpace
#undef PbQueueSnapshotProducer
static BOOLEAN PbV2Send(TUN_CTX *,NET_BUFFER_LIST *);
static NDIS_STATUS PbV2ControlReply(TUN_CTX *ctx,const UCHAR *reply,ULONG length)
{
    (void)ctx; AssertUnlocked(); Replies++;CHECK(length<=sizeof(LastReply));LastReplyLength=length;memcpy(LastReply,reply,length);
    CHECK(Session->Active==1 && Session->Returns.Value==1 && !Session->AllReturned.Value);
    if(Reenter) {
        Reenter=0; NET_BUFFER nb={.Length=64}; NET_BUFFER_LIST nbl={.Nb=&nb}; ExpectedPin++;
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS); ExpectedPin--;
        CHECK(Session->Active==1 && Session->Returns.Value==1 && !Session->AllReturned.Value); Current->Running=0;
    }
    return NDIS_STATUS_SUCCESS;
}
static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls) { (void)nbls; AssertUnlocked(); }
static VOID PbCompleteSend(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbls)
{
    AssertUnlocked(); CHECK(adapter && Indications==(ExpectedPin?ExpectedPin-1:0) && (ULONG)Session->Active==ExpectedPin &&
        (ULONG)Session->Returns.Value==ExpectedPin && !!Session->AllReturned.Value==!ExpectedPin);
    for(;nbls;) {
        NET_BUFFER_LIST *next=nbls->Next; Completed++;
        if(PoisonOnComplete) { DWORD old; CHECK(VirtualProtect(nbls,sizeof(*nbls),PAGE_NOACCESS,&old)); }
        nbls=next;
    }
}
#ifndef PB_REFERENCE_SOURCE
#define PB_REFERENCE_SOURCE "../../driver/v2/session_references.h"
#endif
#include PB_REFERENCE_SOURCE
#ifndef PB_RECYCLE_SOURCE
#define PB_RECYCLE_SOURCE "../../driver/v2/session_recycle.h"
#endif
#if defined(PB_RECYCLE_TOKEN_BASELINE) || defined(PB_SEND_FAULT_BASELINE)
static PB_RESULT CountRecyclePop(PB_QUEUE_VIEW *v,PB_DESCRIPTOR *items,uint32_t count,uint32_t *received)
{RecyclePolls++;PB_RESULT r=PbQueuePop(v,items,count,received);RecycleSnapshotBytes+=(ULONG64)*received*sizeof(*items);if(r==PB_OK && AfterRecyclePop) AfterRecyclePop(v,*received);return r;}
#define PbQueuePop CountRecyclePop
#else
static PB_RESULT CountRecycleTokens(PB_QUEUE_VIEW *v,PB_TOKEN *items,uint32_t count,uint32_t *received)
{RecyclePolls++;PB_RESULT r=PbQueuePopTokens(v,items,count,received);RecycleSnapshotBytes+=(ULONG64)*received*sizeof(*items);if(r==PB_OK && AfterRecyclePop) AfterRecyclePop(v,*received);return r;}
#define PbQueuePopTokens CountRecycleTokens
#endif
#include PB_RECYCLE_SOURCE
#undef PbQueuePop
#undef PbQueuePopTokens
#ifndef PB_SEND_SOURCE
#define PB_SEND_SOURCE "../../driver/v2/session_send.h"
#endif
#if defined(PB_SEND_FAULT_BASELINE) || defined(PB_SEND_PREFIX_BASELINE)
/* Keep historical Send bodies unchanged. Their Describe call predates the
 * diagnostic output; status, payload, ownership and cost still use this fixture. */
static NDIS_STATUS LegacyDescribe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes)
{PB_SEND_FAILURE ignored={0};return PbV2Describe(ctx,nbl,nb,offset,d,bytes,&ignored);}
#define PbV2Describe LegacyDescribe
#endif
static PB_RESULT CountReadySpace(PB_QUEUE_VIEW *view,uint32_t *space)
{ReadySpaceReads++;return PbQueueSpace(view,space);}
#define PbQueueSpace CountReadySpace
#include PB_SEND_SOURCE
#undef PbQueueSpace
#undef PbV2Describe
static VOID Recycle(VOID)
{
    AssertUnlocked();
    for(ULONG i=0;i<Session->Layout.QueueCount;++i) {
        PB_DESCRIPTOR d[PB_MAX_BATCH]; uint32_t n,accepted,notify;
        for(;;) {
            PB_RESULT r=PbQueuePop(Consumers+i,d,PB_MAX_BATCH,&n); if(r==PB_EMPTY) break; CHECK(r==PB_OK);
            CHECK(PbQueuePush(Recyclers+i,d,n,&accepted,&notify)==PB_OK && accepted==n);
        }
    }
}
static VOID OnWindowEnd(VOID)
{
    if(!Trigger || Reads<Trigger || !Action) return;
    CHECK(Session->Active==1 && Session->Returns.Value==1 && !Session->AllReturned.Value);
    ULONG action=Action; Action=0;
    if(action==1) Current->Running=0;
    else if(action==2) Current->V2=NULL;
    else if(action==3) { Recycle(); Action=3; Trigger=Reads+32; }
    else { CHECK(action==4); PbV2Fault(Session); }
}
/* The user owns a separate consumer/return producer and need not acquire
 * TxLock. Execute that legal response to the actual armed publication wake. */
static VOID OnUserWake(VOID)
{
    PB_DESCRIPTOR items[PB_MAX_BATCH];uint32_t n,accepted,notify,ready;
    for(ULONG q=0;q<Session->Layout.QueueCount;++q) {
        PB_RESULT result=PbQueuePop(Consumers+q,items,PB_MAX_BATCH,&n);
        CHECK(result==PB_OK || result==PB_EMPTY);
        if(n) {
            if(ReturnOnWake==2) items[0].Token.Epoch++;
            CHECK(PbQueuePush(Recyclers+q,items,n,&accepted,&notify)==PB_OK && accepted==n);
            WakeReturned+=n;
        }
        CHECK(PbQueueArm(Consumers+q,&ready)==PB_OK && !ready);
    }
}
static VOID Setup(ULONG queues,ULONG slots,ULONG capacity)
{
    static TUN_CTX ctx; memset(&ctx,0,sizeof(ctx)); Current=&ctx;
    Session=calloc(1,sizeof(*Session)); CHECK(Session); Session->Ctx=&ctx; Session->Epoch=42; Session->AllReturned.Value=1;
    CHECK(PbCalculateLayout(queues,slots,capacity,&Session->Layout)==PB_OK);
    Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(Session->Region);
    ctx.Running=1; ctx.V2=Session; ctx.MiniportAdapterHandle=&ctx;
    for(ULONG i=0;i<queues;++i) {
        PB_KERNEL_QUEUE *q=Session->Queues+i; q->Session=Session; q->Index=i; q->UserEvent=&Session->StartEvent;
        q->TxSlots=calloc(slots,sizeof(*q->TxSlots)); CHECK(q->TxSlots);
        CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,slots,42,(uint16_t)i)==PB_OK);
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_TX_READY,Session->Region,&Session->Layout,i,PB_QUEUE_TX_READY,1)==PB_OK);
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_TX_RECYCLE,Session->Region,&Session->Layout,i,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
        CHECK(PbAttachQueue(Consumers+i,Session->Region,&Session->Layout,i,PB_QUEUE_TX_READY,0)==PB_OK);
        CHECK(PbAttachQueue(Recyclers+i,Session->Region,&Session->Layout,i,PB_QUEUE_TX_RECYCLE,1)==PB_OK);
    }
    Reads=Reserves=Describes=Completed=Replies=Windows=MaxWork=MaxBytes=MaxQueueWork=Work=Bytes=0;
    Trigger=Action=Reenter=PoisonOnComplete=Indications=CorruptAt=AfterPinRelease=0; ExpectedPin=1;
    Controls=RecycleAttempts=Faults=LastReplyLength=0;RecycleSnapshotBytes=0;RealEthernet=RealMdl=ReadingPayload=FALSE;
    TxLockAcquires=MaxQueueReads=RecyclePolls=ReadySpaceReads=ReadyAtRead=ReadyAction=UserSignals=0;
    AfterRecyclePop=NULL;
    PrefixCalls=PrefixMaps=PayloadMaps=MdlVisits=0;PrefixBytes=PayloadBytes=0;
    ReturnAtReserve=ReturnAtRead=LateReadCount=CorruptReadProducer=LateReadDrip=0;
    ReturnOnWake=WakeReturned=0;
    ReturnOnMask=CorruptOnMask=0;memset(&MaskReturn,0,sizeof(MaskReturn));
    ObserveCapture=CaptureReadOrder=PublishAfterReturnRead=0;
    CaptureReturnReads=CaptureReadyReads=0;
    memset(&LateReturn,0,sizeof(LateReturn));memset(LateReadItems,0,sizeof(LateReadItems));
}
static VOID Destroy(VOID)
{
    AssertUnlocked(); CHECK(!Session->Active && !Session->Returns.Value && Session->AllReturned.Value);
    /* Admission attempts remain <=32 and <=128 KiB. A newly returned loan
     * can authorize one reserve retry, so reserve calls are separately <=2
     * per attempt; no-progress full pools scan once until ownership changes. */
    CHECK(MaxWork<=PB_TX_ATTEMPT_BUDGET && MaxQueueWork<=2*PB_TX_ATTEMPT_BUDGET && MaxBytes<=PB_TX_BYTE_BUDGET);
    for(ULONG i=0;i<Session->Layout.QueueCount;++i) {
        for(ULONG j=0;j<Session->Layout.SlotCount;++j) CHECK(Session->Queues[i].TxSlots[j].State!=PB_SLOT_RESERVED);
        free(Session->Queues[i].TxSlots);
    }
    CHECK(VirtualFree(Session->Region,0,MEM_RELEASE)); free(Session); Session=NULL;
}
static VOID Chain(ULONG count,ULONG length,ULONG reject,ULONG empty,ULONG queues,ULONG full)
{
    ULONG slots=queues==1?256:128; Setup(queues,slots,512);
    NET_BUFFER_LIST *nbls=calloc(count,sizeof(*nbls)); NET_BUFFER *nbs=calloc(count,sizeof(*nbs)); CHECK(nbls && nbs);
    for(ULONG i=0;i<queues && full;++i) for(ULONG j=0;j<slots;++j) {
        PB_TOKEN token;CHECK(ProductionPoolReserve(&Session->Queues[i].TxPool,&token)==PB_OK);
        CHECK(ProductionPoolTransition(&Session->Queues[i].TxPool,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    for(ULONG i=0;i<count;++i) {
        nbls[i].Next=i+1<count?nbls+i+1:NULL; nbls[i].Nb=empty?NULL:nbs+i;
        nbs[i]=(NET_BUFFER){.Length=length,.Queue=i%queues,.Reject=reject};
    }
    CHECK(PbV2Send(Current,nbls) && Completed==count && AfterPinRelease==1);
    CHECK(Reads==(empty?0:count) && Windows>=1+(count+PB_TX_ATTEMPT_BUDGET-1)/PB_TX_ATTEMPT_BUDGET);
#ifdef PB_SEND_FAULT_BASELINE
    CHECK(Controls==(empty?0:count));
#else
    CHECK(!Controls);
#endif
    for(ULONG i=0;i<count;++i) CHECK(nbls[i].Status==(empty?NDIS_STATUS_INVALID_LENGTH:full?NDIS_STATUS_RESOURCES:reject?NDIS_STATUS_INVALID_DATA:NDIS_STATUS_SUCCESS));
    if(!empty && length<=4096) CHECK(MaxWork==min(count,PB_TX_ATTEMPT_BUDGET));
    if(full && length<=4096) {
#if defined(PB_SEND_FAULT_BASELINE) || defined(PB_SEND_PREFIX_BASELINE)
        CHECK(Reserves==(count+PB_MAX_BATCH-1)/PB_MAX_BATCH); /* Historical per-window proof. */
#else
        CHECK(Reserves==queues); /* No ownership progress: one scan per queue. */
#endif
    }
    if(!empty && length>=PB_BLOCK_SIZE) CHECK(MaxWork==2 && MaxBytes==PB_TX_BYTE_BUDGET);
    printf("PASS: send chain=%lu length=%lu queues=%lu full=%lu rejected=%lu empty=%lu, max transition attempts=%lu bytes=%lu, pool reserve calls=%lu max/TxLock=%lu control calls=%lu.\n",
        count,length,queues,full,reject,empty,MaxWork,MaxBytes,Reserves,MaxQueueWork,Controls);
    free(nbls); free(nbs); Destroy();
}
static VOID Boundary(ULONG action)
{
    const ULONG total=3*PB_TX_ATTEMPT_BUDGET,capacity=2*PB_TX_ATTEMPT_BUDGET;
    Setup(1,capacity,capacity); NET_BUFFER_LIST nbls[3*PB_TX_ATTEMPT_BUDGET]={0}; NET_BUFFER nbs[3*PB_TX_ATTEMPT_BUDGET]={0};
    for(ULONG i=0;i<total;++i) { nbls[i].Next=i+1<total?nbls+i+1:NULL; nbls[i].Nb=nbs+i; nbs[i].Length=64; }
    Trigger=PB_TX_ATTEMPT_BUDGET; Action=action; CHECK(PbV2Send(Current,nbls) && Completed==total && AfterPinRelease==1);
    CHECK(Reads==(action==3?total:PB_TX_ATTEMPT_BUDGET));
    for(ULONG i=0;i<total;++i) CHECK(nbls[i].Status==((i<PB_TX_ATTEMPT_BUDGET || action==3)?NDIS_STATUS_SUCCESS:action==2?NDIS_STATUS_MEDIA_DISCONNECTED:NDIS_STATUS_PAUSED));
    if(action==3) {
        ULONG freeSlots=0; for(ULONG i=0;i<capacity;++i) freeSlots+=Session->Queues[0].TxSlots[i].State==PB_SLOT_FREE;
        CHECK(Session->TxObjects==total && freeSlots==PB_TX_ATTEMPT_BUDGET);
    }
    Destroy();
}
static VOID MultiBufferAndCallbacks(VOID)
{
    Setup(1,2*PB_TX_ATTEMPT_BUDGET,2*PB_TX_ATTEMPT_BUDGET); NET_BUFFER_LIST nbl={0}; NET_BUFFER nbs[2*PB_TX_ATTEMPT_BUDGET]={0}; nbl.Nb=nbs;
    for(ULONG i=0;i<2*PB_TX_ATTEMPT_BUDGET;++i) { nbs[i].Next=i+1<2*PB_TX_ATTEMPT_BUDGET?nbs+i+1:NULL; nbs[i].Length=64; }
    CorruptAt=PB_TX_ATTEMPT_BUDGET; CHECK(PbV2Send(Current,&nbl) && Reads==PB_TX_ATTEMPT_BUDGET && Describes==PB_TX_ATTEMPT_BUDGET && Completed==1 && nbl.Status==NDIS_STATUS_RESOURCES && Session->Faulted);
    CHECK(!Session->TxObjects); Destroy();
    Setup(1,4,4); NET_BUFFER data={.Length=64},reply={.Length=64,.Kind=1},next={.Length=64}; data.Next=&reply; nbl.Nb=&data;
    CorruptAt=1; CHECK(PbV2Send(Current,&nbl) && Reads==2 && Describes==1 && !Replies && nbl.Status==NDIS_STATUS_RESOURCES && Session->Faulted); Destroy();
    Setup(1,4,4); reply.Next=&next; nbl.Nb=&reply; nbl.Next=NULL; Reenter=1;
    CHECK(PbV2Send(Current,&nbl) && Replies==1 && Reads==2 && nbl.Status==NDIS_STATUS_PAUSED && AfterPinRelease==2 && Completed==2); Destroy();
    Setup(1,4,4); NET_BUFFER drop={.Length=64,.Kind=2}; nbl.Nb=&drop;
    CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && !Reserves && Completed==1); Destroy();
    Setup(1,4,4); Current->Running=0; ExpectedPin=0; nbl.Nb=&next;
    CHECK(PbV2Send(Current,&nbl) && !Reads && !AfterPinRelease && nbl.Status==NDIS_STATUS_PAUSED); Destroy();
    Setup(1,4,4); Current->V2=NULL; CHECK(!PbV2Send(Current,&nbl) && !Completed && !AfterPinRelease); Destroy();
    Setup(1,4,4); NET_BUFFER_LIST *owned=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(owned); owned->Nb=&next; PoisonOnComplete=1;
    CHECK(PbV2Send(Current,owned) && Completed==1 && AfterPinRelease==1); CHECK(VirtualFree(owned,0,MEM_RELEASE)); Destroy();
}
static VOID PushRecycles(const PB_DESCRIPTOR *items,ULONG count)
{
    for(ULONG at=0;at<count;) {
        uint32_t n,notify;ULONG batch=min(count-at,PB_MAX_BATCH);
        CHECK(PbQueuePush(Recyclers,items+at,batch,&n,&notify)==PB_OK && n==batch);at+=n;
    }
}
static VOID RecycleFaultPrefix(VOID)
{
    Setup(1,4,256);PB_KERNEL_QUEUE *q=Session->Queues;PB_TOKEN first,last;
    CHECK(ProductionPoolReserve(&q->TxPool,&first)==PB_OK && ProductionPoolTransition(&q->TxPool,&first,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    CHECK(ProductionPoolReserve(&q->TxPool,&last)==PB_OK && ProductionPoolTransition(&q->TxPool,&last,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    PB_DESCRIPTOR items[128]={0};for(ULONG i=0;i<128;++i) items[i].Token=(PB_TOKEN){41,1,0,0};
    items[0].Token=first;items[2].Token=last;PushRecycles(items,128);
#ifdef PB_SEND_FAULT_BASELINE
    PbV2RecycleLocked(q);CHECK(RecycleAttempts==128 && Faults==126 && q->TxSlots[last.Slot].State==PB_SLOT_FREE);
#else
    CHECK(PbV2RecycleLocked(q)==PB_STALE && RecycleAttempts==2 && Faults==1 && q->TxSlots[last.Slot].State==PB_SLOT_PUBLISHED);
#endif
    CHECK(q->TxSlots[first.Slot].State==PB_SLOT_FREE && Session->Faulted);
    printf("PASS: recycle fault after valid prefix: transitions=%lu fault calls=%lu, prefix free, later token state=%lu.\n",RecycleAttempts,Faults,q->TxSlots[last.Slot].State);Destroy();
    Setup(1,8*PB_MAX_BATCH,8*PB_MAX_BATCH);q=Session->Queues;PB_DESCRIPTOR valid[5*PB_MAX_BATCH]={0};
    for(ULONG i=0;i<5*PB_MAX_BATCH;++i) {
        CHECK(ProductionPoolReserve(&q->TxPool,&valid[i].Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&valid[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    PushRecycles(valid,5*PB_MAX_BATCH);
#ifdef PB_SEND_FAULT_BASELINE
    PbV2RecycleLocked(q);
#else
    CHECK(PbV2RecycleLocked(q)==PB_OK);
#endif
    CHECK(RecycleAttempts==4*PB_MAX_BATCH && !Faults && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==4*PB_MAX_BATCH);
#if defined(PB_RECYCLE_TOKEN_BASELINE) || defined(PB_SEND_FAULT_BASELINE)
    CHECK(RecycleSnapshotBytes==4*PB_MAX_BATCH*sizeof(PB_DESCRIPTOR));
#else
    CHECK(RecycleSnapshotBytes==4*PB_MAX_BATCH*sizeof(PB_TOKEN));
#endif
    printf("PASS: actual %u-item recycle budget: snapshot_bytes=%llu, exact token retirement and remaining ring cursor.\n",4*PB_MAX_BATCH,RecycleSnapshotBytes);
#ifdef PB_SEND_FAULT_BASELINE
    PbV2RecycleLocked(q);
#else
    CHECK(PbV2RecycleLocked(q)==PB_OK);
#endif
    CHECK(RecycleAttempts==5*PB_MAX_BATCH && !Faults);
    for(ULONG i=0;i<8*PB_MAX_BATCH;++i) CHECK(q->TxSlots[i].State==PB_SLOT_FREE);Destroy();
}
#ifndef PB_SEND_FAULT_BASELINE
static VOID RecycleTokenValidation(VOID)
{
    for(ULONG bad=0;bad<6;++bad) {
        Setup(1,4,4);PB_KERNEL_QUEUE *q=Session->Queues;PB_DESCRIPTOR items[3];memset(items,0xa5,sizeof(items));
        for(ULONG i=0;i<3;++i) {
            CHECK(ProductionPoolReserve(&q->TxPool,&items[i].Token)==PB_OK);
            CHECK(ProductionPoolTransition(&q->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        }
        PB_TOKEN middle=items[1].Token,last=items[2].Token;
        if(bad==0) items[1].Token.Epoch++;
        if(bad==1) items[1].Token.Queue++;
        if(bad==2) items[1].Token.Slot=4;
        if(bad==3) items[1].Token.Generation++;
        if(bad==4) items[1].Token.Generation=0;
        if(bad==5) items[1].Token=items[0].Token;
        ULONG64 base=UINT64_MAX;
        q->Rings[PB_QUEUE_TX_RECYCLE].Cursor=q->Rings[PB_QUEUE_TX_RECYCLE].PeerCursor=base;
        Recyclers[0].Cursor=Recyclers[0].PeerCursor=base;
        Recyclers[0].Shared->Producer=Recyclers[0].Shared->Consumer=base;
        PushRecycles(items,3);
        CHECK(PbV2RecycleLocked(q)==(bad==5?PB_BUSY:PB_STALE));
        CHECK(RecycleAttempts==2 && Faults==1 && Session->Faulted && q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==base+3);
        CHECK(q->TxSlots[items[0].Token.Slot].State==PB_SLOT_FREE && q->TxSlots[middle.Slot].State==PB_SLOT_PUBLISHED && q->TxSlots[last.Slot].State==PB_SLOT_PUBLISHED);
        Destroy();
    }
    puts("PASS: actual recycle validates epoch/queue/slot/generation/duplicate tokens across ring and uint64 wrap; arbitrary descriptor metadata cannot authorize retirement.");
}
static PB_DESCRIPTOR SnapshotReuseItems[8];
static VOID OverwriteConsumedRing(PB_QUEUE_VIEW *view,uint32_t received)
{
    PB_KERNEL_QUEUE *q=Session->Queues;CHECK(view==q->Rings+PB_QUEUE_TX_RECYCLE && received==4 && q->TxPoolExhausted);
    CHECK(view->Shared->Consumer==view->Cursor);
    for(ULONG i=0;i<8;++i) CHECK(q->TxSlots[i].State==PB_SLOT_PUBLISHED);
    AfterRecyclePop=NULL;uint32_t n,notify;
    /* Consumer was published, but the private retirement loop has not started.
     * Fill all eight shared entries, overwriting all four just-consumed ones. */
    CHECK(PbQueuePush(Recyclers,SnapshotReuseItems,8,&n,&notify)==PB_OK && n==8 && !notify);
}
static VOID RecycleSnapshotReuse(VOID)
{
    for(ULONG wrap=0;wrap<2;++wrap) for(ULONG bad=0;bad<7;++bad) {
        Setup(1,8,8);PB_KERNEL_QUEUE *q=Session->Queues;PB_SLOT *original=q->TxSlots;
        PB_TEST_GUARD slots=PbTestGuardCreate(8*sizeof(PB_SLOT));
        q->TxSlots=(PB_SLOT *)PbTestGuardSpan(&slots,8*sizeof(PB_SLOT));memcpy(q->TxSlots,original,8*sizeof(PB_SLOT));q->TxPool.Slots=q->TxSlots;
        PB_QUEUE_VIEW consumer=q->Rings[PB_QUEUE_TX_RECYCLE],producer=Recyclers[0];
        PB_TEST_GUARD ring=PbTestGuardCreate(sizeof(PB_QUEUE_SHARED)+8*sizeof(PB_DESCRIPTOR));
        PB_QUEUE_SHARED *shared=(PB_QUEUE_SHARED *)PbTestGuardSpan(&ring,sizeof(PB_QUEUE_SHARED)+8*sizeof(PB_DESCRIPTOR));
        memset(shared,0,sizeof(*shared)+8*sizeof(PB_DESCRIPTOR));
        q->Rings[PB_QUEUE_TX_RECYCLE].Shared=Recyclers[0].Shared=shared;
        q->Rings[PB_QUEUE_TX_RECYCLE].Entries=Recyclers[0].Entries=(PB_DESCRIPTOR *)(shared+1);
        ULONG64 base=wrap?UINT64_MAX-3:0;
        q->Rings[PB_QUEUE_TX_RECYCLE].Cursor=q->Rings[PB_QUEUE_TX_RECYCLE].PeerCursor=base;
        Recyclers[0].Cursor=Recyclers[0].PeerCursor=shared->Producer=shared->Consumer=base;
        PB_DESCRIPTOR items[8];memset(items,0xa5,sizeof(items));
        for(ULONG i=0;i<8;++i) {
            CHECK(ProductionPoolReserve(&q->TxPool,&items[i].Token)==PB_OK);
            CHECK(ProductionPoolTransition(&q->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
            SnapshotReuseItems[i]=items[i];SnapshotReuseItems[i].Token.Epoch=41;
        }
        SnapshotReuseItems[0].Token=items[4].Token;SnapshotReuseItems[1].Token=items[5].Token;SnapshotReuseItems[7].Token=items[7].Token;
        if(bad==0) SnapshotReuseItems[1].Token.Epoch++;
        if(bad==1) SnapshotReuseItems[1].Token.Queue++;
        if(bad==2) SnapshotReuseItems[1].Token.Slot=8;
        if(bad==3) SnapshotReuseItems[1].Token.Generation++;
        if(bad==4) SnapshotReuseItems[1].Token=items[4].Token;
        if(bad==5) SnapshotReuseItems[1].Token=items[0].Token;
        if(bad==6) SnapshotReuseItems[1].Token.Generation=0;
        PushRecycles(items,4);q->TxPoolExhausted=TRUE;AfterRecyclePop=OverwriteConsumedRing;
        CHECK(PbV2RecycleLocked(q)==((bad==4 || bad==5)?PB_BUSY:PB_STALE));
        CHECK(!AfterRecyclePop && Faults==1 && Session->Faulted && RecycleAttempts==6 && !q->TxPoolExhausted);
        CHECK(q->Rings[PB_QUEUE_TX_RECYCLE].Cursor==base+12 && shared->Consumer==base+12);
        for(ULONG i=0;i<8;++i) CHECK(q->TxSlots[i].Generation==1 && q->TxSlots[i].State==(i<5?PB_SLOT_FREE:PB_SLOT_PUBLISHED));
        CHECK(q->TxPool.FreeBits[0]==0x1fu);
        memcpy(original,q->TxSlots,8*sizeof(PB_SLOT));q->TxSlots=q->TxPool.Slots=original;
        q->Rings[PB_QUEUE_TX_RECYCLE]=consumer;Recyclers[0]=producer;PbTestGuardDestroy(&slots);PbTestGuardDestroy(&ring);
        Destroy();
    }
    puts("PASS: 14 guarded actual recycle snapshot/reuse cases: shared entries overwritten after Consumer publication but before private retirement; frozen valid tokens still retire, late bad token stops at exact prefix, suffix stays published, cache/index/generations and uint64 wrap preserved.");
}
#endif
static VOID SendAfterRecycleFault(BOOLEAN corruptRing)
{
    Setup(1,4,256);PB_KERNEL_QUEUE *q=Session->Queues;
    if(corruptRing) q->Rings[PB_QUEUE_TX_RECYCLE].Shared->Producer=257;
    else {PB_DESCRIPTOR items[128]={0};for(ULONG i=0;i<128;++i) items[i].Token=(PB_TOKEN){41,1,0,0};PushRecycles(items,128);}
    NET_BUFFER nb[2]={{.Length=64},{.Length=64}};NET_BUFFER_LIST nbl[2]={{.Nb=nb,.Next=nbl+1},{.Nb=nb+1}};
    CHECK(PbV2Send(Current,nbl) && Completed==2 && Reads==1 && AfterPinRelease==1 && Session->Faulted);
#ifdef PB_SEND_FAULT_BASELINE
    CHECK(Describes==1 && Reserves==1 && Session->TxObjects==1 && nbl[0].Status==NDIS_STATUS_SUCCESS);
    CHECK(Faults==(corruptRing?1u:128u));
#else
    CHECK(!Describes && !Reserves && !Session->TxObjects && nbl[0].Status==NDIS_STATUS_PAUSED && Faults==1);
#endif
    CHECK(nbl[1].Status==NDIS_STATUS_PAUSED);
    printf("PASS: send recycle fault ring_corrupt=%u: fault calls=%lu describes=%lu published=%lld first_status=%ld; completion pin released once.\n",corruptRing,Faults,Describes,Session->TxObjects,nbl[0].Status);Destroy();
}
static ULONG EthernetFrame(UCHAR frame[128],ULONG which)
{
    BOOLEAN vlan=which==7 || which==8 || which==9,arp=which==3 || which==8;
    BOOLEAN nd=which==4 || which==5 || which==6 || which==9;
    BOOLEAN v6=which==1 || which==2 || nd || which==11 || which==13 || which==14;
    ULONG h=vlan?18:14,n=(arp?60:v6?86:64)+(vlan?4:0);memset(frame,0,128);
    const UCHAR mac[6]={2,1,2,3,4,5};memcpy(frame+6,mac,6);
    if(which==10) frame[6]|=1;
    put16(frame+12,vlan?0x8100:arp?0x0806:v6?0x86dd:0x0800);
    if(vlan) put16(frame+16,arp?0x0806:v6?0x86dd:0x0800);
    UCHAR *ip=frame+h;
    if(arp) {
        put16(ip,1);put16(ip+2,0x0800);ip[4]=6;ip[5]=4;put16(ip+6,1);memcpy(ip+8,mac,6);
        ip[14]=ip[24]=10;ip[17]=1;ip[27]=2;
    } else if(v6) {
        ip[0]=0x60;put16(ip+4,32);ip[6]=nd || which==2?58:which==13?0:6;ip[7]=255;
        ip[8]=ip[24]=0x20;ip[23]=1;ip[39]=2;
        if(nd) {
            UCHAR *ns=ip+40;ns[0]=which==5?133:135;ns[8]=0x20;ns[23]=2;ns[24]=ns[25]=1;memcpy(ns+26,mac,6);
            put16(ns+2,finish(sum(ns,32,sum(ip+8,32,32+58))));if(which==6) ns[2]^=1;
        } else if(which==2) ip[40]=128;
        if(which==11) n=15;
        if(which==14) ip[0]=0x45;
    } else {ip[0]=0x45;ip[9]=which==15?1:6;}
    if(which==12) put16(frame+12,0x88cc);
    return n;
}
static VOID EthernetClassification(VOID)
{
    for(ULONG which=0;which<16;++which) {
        Setup(1,4,4);RealEthernet=TRUE;UCHAR frame[128],reply[128];ULONG n=EthernetFrame(frame,which);
        UCHAR *guarded=PbTestGuardAlloc(n);memcpy(guarded,frame,n);uint32_t offset,length;
        PB_ETH_RESULT kind=ActualEthernetInspect(guarded,n,&offset);
        PB_ETH_RESULT control=ActualEthernetControl(guarded,n,Current->Probe.PeerMac,reply,&length);
        BOOLEAN neighbor=kind==PB_ETH_IP && guarded[offset]>>4==6 && n>=offset+41 && guarded[offset+6]==58 && guarded[offset+40]>=133 && guarded[offset+40]<=137;
        BOOLEAN forward=kind==PB_ETH_IP && control!=PB_ETH_REPLY && !neighbor;
        CHECK(control!=PB_ETH_REPLY || kind!=PB_ETH_IP || neighbor);
        NET_BUFFER nb={.Length=n,.Frame=guarded};NET_BUFFER_LIST nbl={.Nb=&nb};
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && Completed==1 && AfterPinRelease==1);
#ifdef PB_SEND_FAULT_BASELINE
        CHECK(Controls==1);
#else
        CHECK(Controls==(ULONG)(kind!=PB_ETH_IP || neighbor));
#endif
        CHECK(Describes==(ULONG)forward && Replies==(ULONG)(control==PB_ETH_REPLY));
        if(Replies) CHECK(LastReplyLength==length && !memcmp(LastReply,reply,length));
        printf("PASS: actual Ethernet case=%lu control calls=%lu forwards=%lu replies=%lu, guarded source and exact reply bytes.\n",which,Controls,Describes,Replies);
        PbTestGuardFree(guarded);Destroy();
    }
}
#include "send_prefix_cases.h"
static VOID SendFailureHistory(VOID)
{
    Setup(1,4,4);
    NET_BUFFER nb[2]={{.Length=64},{.Length=64,.Reject=1}}; nb[0].Next=nb+1;
    NET_BUFFER_LIST nbl={.Nb=nb};nbl.Info[TcpLargeSendNetBufferListInfo]=(VOID *)(ULONG_PTR)0xfedc1234;
    CorruptAt=1;CHECK(PbV2Send(Current,&nbl));
    CHECK(nbl.Status==NDIS_STATUS_INVALID_DATA); /* Failed publication cannot erase the first error. */
    CHECK(Current->Probe.SendDiagnostics.Sequence==1 && Current->Probe.OffloadRejected==1);
    PB_SEND_EVENT *e=Current->Probe.SendDiagnostics.Records;
    CHECK(e->NdisStatus==NDIS_STATUS_INVALID_DATA && e->Reason==PB_SEND_GSO_PLAN && e->Epoch==42);
    CHECK(e->LsoInfo==0xfedc1234 && e->NetBufferLength==64 && e->NetBufferScope==2);
    CHECK(!Session->TxObjects);Destroy();
    Setup(1,4,4);Current->Running=0;ExpectedPin=0;nbl.Nb=nb;nb[0].Next=NULL;
    CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_PAUSED);
    CHECK(Current->Probe.SendDiagnostics.Records[0].Epoch==0 && Current->Probe.SendDiagnostics.Records[0].Reason==PB_SEND_ADMISSION);
    Destroy();
    puts("PASS: rejection snapshots preserve first raw status/OOB, publication failure cannot overwrite it, and unpinned admission records no session dereference.");
}
static VOID ReturnDuringSendWindow(VOID)
{
    for(ULONG bad=0;bad<2;++bad) {
        Setup(1,2,4);PB_KERNEL_QUEUE *q=Session->Queues;
        CHECK(ProductionPoolReserve(&q->TxPool,&LateReturn.Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&LateReturn.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        if(bad) LateReturn.Token.Epoch++;
        ReturnAtReserve=2;
        NET_BUFFER nb[2]={{.Length=64},{.Length=64}};
        NET_BUFFER_LIST nbl[2]={{.Nb=nb,.Next=nbl+1},{.Nb=nb+1}};
        CHECK(PbV2Send(Current,nbl) && Completed==2 && AfterPinRelease==1);
        CHECK(nbl[0].Status==NDIS_STATUS_SUCCESS);
        CHECK(nbl[1].Status==(bad?NDIS_STATUS_PAUSED:NDIS_STATUS_SUCCESS));
        CHECK(RecycleAttempts==1 && Faults==bad && Reserves==(bad?2u:3u));
        CHECK(Current->Probe.OffloadRejected==bad);
        if(bad) CHECK(Current->Probe.SendDiagnostics.Records[0].Reason==PB_SEND_RECYCLE);
        else CHECK(Session->TxObjects==2 && q->TxSlots[LateReturn.Token.Slot].Generation==2);
        Destroy();
    }
    puts("PASS: returns arriving after entry recycle and before pool-full are retired once; stale token faults before reuse; no waiting or NBL replay.");
}
static VOID ReturnAfterFullObservation(VOID)
{
    for(ULONG bad=0;bad<3;++bad) {
        Setup(1,2,4);PB_KERNEL_QUEUE *q=Session->Queues;
        CHECK(ProductionPoolReserve(&q->TxPool,&LateReturn.Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&LateReturn.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        if(bad==1) LateReturn.Token.Epoch++;
        CorruptReadProducer=bad==2;
        /* One descriptor is held by the user, one fits the pending prefix.
         * Packet 2 sees real pressure. The independent producer returns the
         * old loan before packet 3; TxLock excludes the worker, not this push. */
        ReturnAtRead=3;
        NET_BUFFER nb[3]={{.Length=64},{.Length=64},{.Length=64}};
        NET_BUFFER_LIST nbl[3]={{.Nb=nb,.Next=nbl+1},{.Nb=nb+1,.Next=nbl+2},{.Nb=nb+2}};
        CHECK(PbV2Send(Current,nbl) && Completed==3 && AfterPinRelease==1);
        CHECK(nbl[0].Status==NDIS_STATUS_SUCCESS && nbl[1].Status==NDIS_STATUS_RESOURCES);
        CHECK(nbl[2].Status==(bad?NDIS_STATUS_PAUSED:NDIS_STATUS_SUCCESS));
        CHECK(RecycleAttempts==(bad==2?0u:1u) && Faults==(bad?1u:0u) && Reserves==(bad?2u:3u));
        CHECK(Current->Probe.OffloadRejected==1+!!bad);
        if(bad) CHECK(Current->Probe.SendDiagnostics.Records[1].Reason==PB_SEND_RECYCLE);
        else CHECK(Session->TxObjects==2 && q->TxSlots[LateReturn.Token.Slot].Generation==2);
        Destroy();
    }
    Setup(1,4,8);PB_KERNEL_QUEUE *q=Session->Queues;
    LateReadCount=3;
    for(ULONG i=0;i<3;++i) {
        CHECK(ProductionPoolReserve(&q->TxPool,&LateReadItems[i].Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&LateReadItems[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    ReturnAtRead=3;
    NET_BUFFER nb[6]={0};NET_BUFFER_LIST nbl[6]={0};
    for(ULONG i=0;i<6;++i) {nb[i].Length=64;nbl[i].Nb=nb+i;if(i<5)nbl[i].Next=nbl+i+1;}
    CHECK(PbV2Send(Current,nbl) && Completed==6 && AfterPinRelease==1);
    for(ULONG i=0;i<6;++i) CHECK(nbl[i].Status==(i==1 || i==5?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(Session->TxObjects==4 && RecycleAttempts==3 && Reserves==6 && !Faults && Current->Probe.OffloadRejected==2);
    Destroy();
    Setup(1,32,64);q=Session->Queues;
    LateReadCount=31;LateReadDrip=1;
    for(ULONG i=0;i<31;++i) {
        CHECK(ProductionPoolReserve(&q->TxPool,&LateReadItems[i].Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&LateReadItems[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
    }
    NET_BUFFER dripNb[32]={0};NET_BUFFER_LIST dripNbl[32]={0};
    for(ULONG i=0;i<32;++i) {dripNb[i].Length=64;dripNbl[i].Nb=dripNb+i;if(i<31)dripNbl[i].Next=dripNbl+i+1;}
    CHECK(PbV2Send(Current,dripNbl) && Completed==32 && AfterPinRelease==1);
    for(ULONG i=0;i<32;++i) CHECK(dripNbl[i].Status==(i==1?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(Session->TxObjects==31 && RecycleAttempts==30 && !Faults && Current->Probe.OffloadRejected==1);
    CHECK(MaxWork<=32 && MaxQueueWork<=64 && Reads==32 && Reserves==61);
    Destroy();
    puts("PASS: return after an earlier full observation is visible to the next NBL in the same lock window; stale tokens fault before reuse; true pressure and work budgets preserved.");
}
static VOID FullPoolWindows(ULONG firstQueue)
{
    for(ULONG queues=firstQueue;queues<=8;queues*=8) {
        Setup(queues,32,64);
        for(ULONG q=0;q<queues;++q) for(ULONG i=0;i<32;++i) {
            PB_TOKEN token;CHECK(ProductionPoolReserve(&Session->Queues[q].TxPool,&token)==PB_OK);
            CHECK(ProductionPoolTransition(&Session->Queues[q].TxPool,&token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        }
        NET_BUFFER nb[4096]={0};NET_BUFFER_LIST nbl[4096]={0};
        for(ULONG i=0;i<4096;++i) {nb[i].Length=64;nb[i].Queue=i%queues;nbl[i].Nb=nb+i;if(i<4095)nbl[i].Next=nbl+i+1;}
        CHECK(PbV2Send(Current,nbl) && Completed==4096 && Windows==1+(4096+PB_TX_ATTEMPT_BUDGET-1)/PB_TX_ATTEMPT_BUDGET && !Describes && !Faults);
        CHECK(Current->Probe.OffloadRejected==4096);
        printf("Measured full-pool reservations: queues=%lu send_windows=%lu attempts=%lu no returned tokens.\n",queues,Windows-1,Reserves);
        CHECK(Reserves==queues);
        for(ULONG i=0;i<4096;++i) CHECK(nbl[i].Status==NDIS_STATUS_RESOURCES);
        Destroy();
    }
    /* A cached full result must not survive a validated late return between
     * separate send calls, including one that arrives while the worker sleeps. */
    for(ULONG bad=0;bad<2;++bad) {
        Setup(1,1,2);PB_KERNEL_QUEUE *q=Session->Queues;PB_DESCRIPTOR item={0};
        CHECK(ProductionPoolReserve(&q->TxPool,&item.Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&item.Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        NET_BUFFER nb={.Length=64};NET_BUFFER_LIST nbl={.Nb=&nb};
        CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_RESOURCES && Reserves==1);
        if(bad)item.Token.Generation++;
        PushRecycles(&item,1);
        CHECK(PbV2Send(Current,&nbl) && Completed==2);
        CHECK(nbl.Status==(bad?NDIS_STATUS_PAUSED:NDIS_STATUS_SUCCESS));
        CHECK(Describes==(ULONG)!bad && Faults==bad && RecycleAttempts==1 && Reserves==(bad?1u:2u));
        Destroy();
    }
    puts("PASS: no-progress full pools scan once across 128 windows and eight queue switches; late validated returns recover immediately, bad generations fault before reuse.");
}
static VOID RingPressureIsNotPoolPressure(VOID)
{
    Setup(1,2,2);PB_KERNEL_QUEUE *q=Session->Queues;PB_DESCRIPTOR items[2]={0};uint32_t n,notify;
    for(ULONG i=0;i<2;++i) {
        CHECK(ProductionPoolReserve(&q->TxPool,&items[i].Token)==PB_OK);
        CHECK(ProductionPoolTransition(&q->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        items[i].Length=20;
    }
    CHECK(PbQueuePush(q->Rings+PB_QUEUE_TX_READY,items,2,&n,&notify)==PB_OK && n==2);
    /* Return and ready-ring cursors are independent shared inputs. A valid
     * token may be returned before the peer advances its ready consumer. */
    PushRecycles(items,1);
    NET_BUFFER nb={.Length=64};NET_BUFFER_LIST nbl={.Nb=&nb};
    CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_RESOURCES && !Reserves);
    CHECK(PbQueuePop(Consumers,items,1,&n)==PB_OK && n==1); /* Read without returning a token. */
    CHECK(PbV2Send(Current,&nbl) && nbl.Status==NDIS_STATUS_SUCCESS && Reserves==1 && Describes==1 && RecycleAttempts==1);
    Destroy();
    /* A failed publication frees pending reservations without a user return;
     * those slots cannot remain hidden behind a full-pool proof. */
    Setup(1,1,2);q=Session->Queues;CorruptAt=1;
    NET_BUFFER buffers[2]={{.Length=64},{.Length=64}};
    NET_BUFFER_LIST lists[2]={{.Nb=buffers,.Next=lists+1},{.Nb=buffers+1}};
    CHECK(PbV2Send(Current,lists) && !Session->TxObjects && q->TxSlots[0].State==PB_SLOT_FREE);
    CHECK(!q->TxPoolExhausted && Session->Faulted && Current->Probe.SendDiagnostics.Reasons[PB_SEND_POOL_FULL]==1);
    Destroy();
    puts("PASS: freeing ready-ring space admits an already-free slot without recycle; failed publication invalidates full-pool state and preserves the fault.");
}
static VOID PublishBeforeFurtherPressure(VOID)
{
    Setup(1,1,2);ReturnOnWake=1;uint32_t ready;
    CHECK(PbQueueArm(Consumers,&ready)==PB_OK && !ready);
    NET_BUFFER nb[32]={0};NET_BUFFER_LIST nbl[32]={0};
    for(ULONG i=0;i<32;++i) {nb[i].Length=64;nbl[i].Nb=nb+i;if(i<31)nbl[i].Next=nbl+i+1;}
    CHECK(PbV2Send(Current,nbl) && Completed==32 && !Faults && AfterPinRelease==1);
    printf("Measured pressure handoff: accepted=%lu rejected=%lld user_returned=%lu.\n",Describes,Current->Probe.OffloadRejected,WakeReturned);
    CHECK(Describes==16 && Current->Probe.OffloadRejected==16 && WakeReturned==16);
    for(ULONG i=0;i<32;++i) CHECK(nbl[i].Status==(i%2?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    Destroy();
    puts("PASS: first real pool pressure publishes the pending prefix before rejecting later NBLs; actual ring wake and validated return allow the next original, without retry or replay of the rejected original.");
}
static VOID PressureHandoffBoundaries(VOID)
{
    /* No consumer progress must remain finite, preserve actual drops, and
     * reuse the proven-full result rather than repeatedly scanning the pool. */
    Setup(1,1,2);NET_BUFFER nb[32]={0};NET_BUFFER_LIST nbl[32]={0};
    for(ULONG i=0;i<32;++i) {nb[i].Length=64;nbl[i].Nb=nb+i;if(i<31)nbl[i].Next=nbl+i+1;}
    CHECK(PbV2Send(Current,nbl) && Completed==32 && Describes==1 && Reserves==2 && Reads==32);
    CHECK(Current->Probe.OffloadRejected==31 && !Faults && Session->TxObjects==1);Destroy();
    for(ULONG action=1;action<=4;++action) {
        Setup(1,1,2);ReturnOnWake=action==3?2:1;uint32_t ready;
        CHECK(PbQueueArm(Consumers,&ready)==PB_OK && !ready);
        if(action!=3) {Trigger=2;Action=action;}
        CHECK(PbV2Send(Current,nbl) && Completed==32 && Describes==1 && WakeReturned==1 && Reads==(action==3?3u:2u));
        CHECK(nbl[0].Status==NDIS_STATUS_SUCCESS && nbl[1].Status==NDIS_STATUS_RESOURCES);
        for(ULONG i=2;i<32;++i) CHECK(nbl[i].Status==(action==2?NDIS_STATUS_MEDIA_DISCONNECTED:NDIS_STATUS_PAUSED));
        CHECK(Session->Queues[0].TxSlots[0].Generation==1 && AfterPinRelease==1);
        CHECK(Faults==(action>=3?1u:0u));
        if(action==3) CHECK(Current->Probe.SendDiagnostics.Reasons[PB_SEND_RECYCLE]==1);
        Destroy();
    }
    /* A failed NBL may own an already accepted NB prefix. Publish it once,
     * retain the first raw failure, and continue only with the next NBL. */
    Setup(1,1,2);ReturnOnWake=1;uint32_t ready;CHECK(PbQueueArm(Consumers,&ready)==PB_OK && !ready);
    NET_BUFFER buffers[4]={{.Length=64},{.Length=64},{.Length=64},{.Length=64}};buffers[0].Next=buffers+1;
    NET_BUFFER_LIST lists[3]={{.Nb=buffers,.Next=lists+1},{.Nb=buffers+2,.Next=lists+2},{.Nb=buffers+3}};
    lists[0].Info[TcpLargeSendNetBufferListInfo]=(VOID *)(ULONG_PTR)0x12345678;
    CHECK(PbV2Send(Current,lists) && Completed==3 && Describes==2 && WakeReturned==2 && !Faults);
    CHECK(lists[0].Status==NDIS_STATUS_RESOURCES && lists[1].Status==NDIS_STATUS_SUCCESS && lists[2].Status==NDIS_STATUS_RESOURCES);
    CHECK(Current->Probe.OffloadRejected==2 && Current->Probe.SendDiagnostics.Records[0].LsoInfo==0x12345678);
    CHECK(Current->Probe.SendDiagnostics.Records[0].Reason==PB_SEND_POOL_FULL && Current->Probe.SendDiagnostics.Records[0].NetBufferScope==2);
    Destroy();
    Setup(1,1,2);ReturnOnWake=1;CHECK(PbQueueArm(Consumers,&ready)==PB_OK && !ready);CorruptAt=1;
    CHECK(PbV2Send(Current,nbl) && Completed==32 && Describes==1 && !WakeReturned && !Session->TxObjects && Faults==1);
    CHECK(nbl[0].Status==NDIS_STATUS_RESOURCES && nbl[1].Status==NDIS_STATUS_RESOURCES);
    for(ULONG i=2;i<32;++i) CHECK(nbl[i].Status==NDIS_STATUS_PAUSED);
    CHECK(Current->Probe.SendDiagnostics.Reasons[PB_SEND_POOL_FULL]==1 && Current->Probe.SendDiagnostics.Reasons[PB_SEND_PUBLICATION]==1);
    CHECK(Session->Queues[0].TxSlots[0].State==PB_SLOT_FREE);Destroy();
    /* Independent queue proofs and wake/recycle ownership remain scoped. */
    Setup(2,1,2);ReturnOnWake=1;
    for(ULONG q=0;q<2;++q) CHECK(PbQueueArm(Consumers+q,&ready)==PB_OK && !ready);
    for(ULONG i=0;i<32;++i) nb[i].Queue=(i/2)%2;
    CHECK(PbV2Send(Current,nbl) && Completed==32 && Describes==16 && WakeReturned==16 && !Faults);
    for(ULONG i=0;i<32;++i) CHECK(nbl[i].Status==(i%2?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(Current->Probe.OffloadRejected==16);Destroy();
    puts("PASS: pressure handoff with stalled consumer, stale return, pause/detach/fault at unlocked boundary, partial multi-NB original/raw status, failed publication and two scoped queues; no retry, duplicate completion or reuse before validation.");
}
static VOID PressureObservation(VOID)
{
    for(ULONG which=0;which<11;++which) {
    Setup(1,4,8);
    PB_KERNEL_QUEUE *q=Session->Queues;
    if(which==6) {
        ULONG64 base=UINT64_MAX-2;
        for(ULONG r=0;r<2;++r) {
            PB_QUEUE_VIEW *v=q->Rings+(r?PB_QUEUE_TX_RECYCLE:PB_QUEUE_TX_READY),*peer=r?Recyclers:Consumers;
            v->Cursor=v->PeerCursor=peer->Cursor=peer->PeerCursor=base;v->Shared->Producer=v->Shared->Consumer=base;
        }
    }
    NET_BUFFER nb[4]={{.Length=64},{.Length=64},{.Length=64},{.Length=64}};
    NET_BUFFER_LIST nbl[4]={0};
    for(ULONG i=0;i<4;++i){nbl[i].Nb=nb+i;if(i<3)nbl[i].Next=nbl+i+1;}
    ULONG seed=which==3?3:4;nbl[seed-1].Next=NULL;
    if(which==4) for(ULONG i=0;i<4;++i) q->TxSlots[i].Generation=UINT32_MAX;
    else CHECK(PbV2Send(Current,nbl) && Session->TxObjects==seed);
    CHECK(!CaptureReturnReads && !CaptureReadyReads); /* No observation on successful publication. */
    if(which==1 || which==2 || which==3) {
        PB_DESCRIPTOR consumed[4];uint32_t n;
        CHECK(PbQueuePop(Consumers,consumed,which==2?2:seed,&n)==PB_OK && n==(which==2?2:seed));
        if(which==2) {MaskReturn=consumed[0];ReturnOnMask=1;}
    }
    if(which==5) CorruptOnMask=1;
    DWORD protection=0;
    if(which==8) CHECK(VirtualProtect(Session->Region,Session->Layout.RegionSize,PAGE_READONLY,&protection));
    if(which==9) {ObserveCapture=PublishAfterReturnRead=1;}
    NET_BUFFER failed={.Length=64};NET_BUFFER_LIST last={.Nb=&failed};
    if(which==3) {nbl[0].Next=&last;CHECK(PbV2Send(Current,nbl) && nbl[0].Status==NDIS_STATUS_SUCCESS);}
    else
    CHECK(PbV2Send(Current,&last) && last.Status==NDIS_STATUS_RESOURCES && !Faults);
    if(which==8) CHECK(VirtualProtect(Session->Region,Session->Layout.RegionSize,protection,&protection));
    PB_SEND_HISTORY *history=&Current->Probe.SendDiagnostics;
    printf("Measured pool-full=%llu; pressure observations=%llu.\n",history->Reasons[PB_SEND_POOL_FULL],history->PressureSequence);
    CHECK(history->PressureSequence==1);
    PB_POOL_PRESSURE_EVENT *e=history->Pressure;
    CHECK(e->Slots==4 && e->Send.NdisStatus==NDIS_STATUS_RESOURCES && e->Send.Sequence==1 && e->Send.Epoch==42);
    if(which==5) {
        CHECK(!(e->Flags&PB_POOL_COUNTS_VALID) && e->ReturnStatus==PB_CORRUPT && history->PressureClasses[5]==1);
        CHECK(!q->Rings[PB_QUEUE_TX_RECYCLE].PeerCursor && !Session->Faulted);
    } else {
        CHECK(e->Flags==15 && e->Pending==(which==3?1u:0u));
        CHECK(e->Unaccounted==(which==4?4u:0u) && e->ReturnQueued==(which==2?1u:0u));
        CHECK(e->UserHeld==(which==1?4u:which==2 || which==9?1u:which==3?3u:0u));
        CHECK(e->ReadyUnread==(which==2?2u:which==9?3u:which==0 || which==6 || which==7 || which==8 || which==10?4u:0u));
        CHECK(e->Outstanding==(which==3?3u:which==4?0u:4u));
        if(which==2) CHECK(!q->Rings[PB_QUEUE_TX_RECYCLE].PeerCursor && RecycleAttempts==0);
        if(which==9) CHECK(CaptureReadOrder==2 && Recyclers[0].Cursor==1 && !e->ReturnProducer && !RecycleAttempts);
    }
    if(which==7) {
        for(ULONG i=0;i<4096;++i) CHECK(PbV2Send(Current,&last) && last.Status==NDIS_STATUS_RESOURCES);
        CHECK(history->Reasons[PB_SEND_POOL_FULL]==4097 && history->PressureSequence==65 && history->PressureSamples==65);
        CHECK(history->PressureClasses[1]==65 && history->Pressure[0].Sequence==65 && history->Pressure[0].Send.Sequence==4097);
        CHECK(CaptureReturnReads==65 && CaptureReadyReads==65);
    }
    if(which==10) {
        for(ULONG i=0;i<63;++i) CHECK(PbV2Send(Current,&last) && last.Status==NDIS_STATUS_RESOURCES);
        PB_SLOT *saved=q->TxSlots;
        PB_SLOT *guard=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);CHECK(guard);
        q->TxSlots=q->TxPool.Slots=guard;
        CHECK(PbV2Send(Current,&last) && last.Status==NDIS_STATUS_RESOURCES);
        q->TxSlots=q->TxPool.Slots=saved;
        CHECK(VirtualFree(guard,0,MEM_RELEASE));
        CHECK(history->PressureSequence==2 && history->Pressure[1].Flags==15 && CaptureReturnReads==2 && CaptureReadyReads==2);
    }
    Destroy();
    }
    puts("PASS: sampled actual send pressure distinguishes pending/ready/user/late-return and unaccounted generation-retirement residuals, uint64 wrap, invalid shared snapshots and 1/64 retention; observation never advances real validation cursors or authorizes reuse.");
}
#include "../../driver/v2/pool_snapshot.h"
static VOID CurrentPoolObservation(VOID)
{
    Setup(1,4,8);PB_KERNEL_QUEUE *q=Session->Queues;
    PB_POOL_PRESSURE_SNAPSHOT *out=PbTestGuardAlloc(sizeof(*out));memset(out,0,sizeof(*out));
    PbSnapshotCurrentTxPools(Current,out);CHECK(out->CurrentQueueCount==1 && out->Current[0].Free==4 && out->Current[0].CountsValid);
    NET_BUFFER nb[3]={{.Length=64},{.Length=64},{.Length=64}};
    NET_BUFFER_LIST nbl[3]={{.Nb=nb,.Next=nbl+1},{.Nb=nb+1,.Next=nbl+2},{.Nb=nb+2}};
    CHECK(PbV2Send(Current,nbl));PB_DESCRIPTOR consumed[2];uint32_t n;
    CHECK(PbQueuePop(Consumers,consumed,2,&n)==PB_OK && n==2);PushRecycles(consumed,1);
    PB_KERNEL_QUEUE saved=*q;PB_SLOT slots[4];memcpy(slots,q->TxSlots,sizeof(slots));
    memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);
    CHECK(!memcmp(&saved,q,sizeof(saved)) && !memcmp(slots,q->TxSlots,sizeof(slots)));
    CHECK(out->Current[0].Free==1 && out->Current[0].Published==3 && out->Current[0].ReturnProducer==1 && !out->Current[0].ReturnConsumer);
    KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->TxLock,&lock);
#ifdef PB_SEND_FAULT_BASELINE
    PbV2RecycleLocked(q);
#else
    CHECK(PbV2RecycleLocked(q)==PB_OK);
#endif
    KeReleaseInStackQueuedSpinLock(&lock);
    q->TxPoolExhausted=TRUE; /* Deliberately inconsistent proof must be exposed, never repaired by inspection. */
    memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);
    CHECK(out->Current[0].Free==2 && out->Current[0].Published==2 && out->Current[0].PoolExhausted && q->TxPoolExhausted);
    q->TxPoolExhausted=FALSE;PB_TOKEN reserved;
    CHECK(ProductionPoolReserve(&q->TxPool,&reserved)==PB_OK);
    memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);CHECK(out->Current[0].Reserved==1);
    CHECK(ProductionPoolTransition(&q->TxPool,&reserved,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_OK);
    ULONG retire=0;while(q->TxSlots[retire].State!=PB_SLOT_FREE)retire++;
    q->TxSlots[retire].Generation=UINT32_MAX;q->TxPool.Next=retire;
    CHECK(ProductionPoolReserve(&q->TxPool,&reserved)==PB_OK && q->TxSlots[retire].State==PB_SLOT_RETIRED);
    CHECK(ProductionPoolTransition(&q->TxPool,&reserved,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_OK);
    memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);
    CHECK(out->Current[0].Retired==1 && out->Current[0].Free==1 && !out->Current[0].Reserved);
    q->TxPool.Count=5;memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);
    CHECK(!out->Current[0].CountsValid && out->Current[0].PoolSlots==5 && !out->Current[0].Free);q->TxPool.Count=4;
    Current->V2=NULL;memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);CHECK(!out->CurrentQueueCount && !out->CurrentStatus);
    Current->V2=Session;Session->Layout.QueueCount=9;memset(out,0,sizeof(*out));PbSnapshotCurrentTxPools(Current,out);
    CHECK(!out->CurrentQueueCount && out->CurrentStatus==PB_CORRUPT);Session->Layout.QueueCount=1;
    PbTestGuardFree(out);Destroy();
    puts("PASS: manual current-pool snapshot counts exact private states, exposes queued returns/cache inconsistencies/reserved and retired slots, rejects invalid private bounds and absent sessions; no state or cursor mutation.");
}
static VOID FillReadyRing(ULONG index,ULONG64 base)
{
    PB_KERNEL_QUEUE *q=Session->Queues+index;PB_DESCRIPTOR items[2]={0};uint32_t n,notify;
    CHECK(Session->Layout.SlotCount==2 && Session->Layout.RingCapacity==2);
    PB_QUEUE_VIEW *views[]={q->Rings+PB_QUEUE_TX_READY,Consumers+index,q->Rings+PB_QUEUE_TX_RECYCLE,Recyclers+index};
    for(ULONG i=0;i<4;++i) {views[i]->Cursor=views[i]->PeerCursor=base;views[i]->Shared->Producer=views[i]->Shared->Consumer=base;}
    for(ULONG i=0;i<2;++i) {
        CHECK(ProductionPoolReserve(&q->TxPool,&items[i].Token)==PB_OK &&
            ProductionPoolTransition(&q->TxPool,&items[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);
        items[i].Length=20;items[i].DataOffset=Session->Layout.Queues[index].TxData+items[i].Token.Slot*PB_BLOCK_SIZE;
    }
    CHECK(PbQueuePush(q->Rings+PB_QUEUE_TX_READY,items,2,&n,&notify)==PB_OK && n==2);
}
static VOID FullRingLockCost(ULONG queues,ULONG length)
{
    const ULONG count=4096;Setup(queues,2,2);
    for(ULONG i=0;i<queues;++i) FillReadyRing(i,UINT64_MAX-1);
    NET_BUFFER_LIST *lists=PbTestGuardAlloc(count*sizeof(*lists));NET_BUFFER *buffers=PbTestGuardAlloc(count*sizeof(*buffers));
    memset(lists,0,count*sizeof(*lists));memset(buffers,0,count*sizeof(*buffers));
    for(ULONG i=0;i<count;++i) {lists[i].Nb=buffers+i;lists[i].Next=i+1<count?lists+i+1:NULL;buffers[i].Length=length;buffers[i].Queue=i%queues;}
    CHECK(PbV2Send(Current,lists) && Completed==count && AfterPinRelease==1 && !Reserves && !Describes && !Faults && !Session->TxObjects);
    CHECK(Current->Probe.OffloadRejected==count && Current->Probe.SendDiagnostics.Reasons[PB_SEND_QUEUE_FULL]==count &&
        !Current->Probe.SendDiagnostics.Reasons[PB_SEND_POOL_FULL]);
    CHECK(Reads==count && RecyclePolls==count && ReadySpaceReads==count && MaxQueueReads<=PB_MAX_BATCH && !UserSignals);
    ULONG budget=min(PB_MAX_BATCH,PB_TX_BYTE_BUDGET/min(length,PB_BLOCK_SIZE));
    ULONG boundedLocks=queues==1?(count+budget-1)/budget:count;
#ifdef PB_RING_PRESSURE_BASELINE
    CHECK(TxLockAcquires==count);
#else
    CHECK(TxLockAcquires==boundedLocks);
#endif
    CHECK(Windows==1+(count+budget-1)/budget);
    for(ULONG i=0;i<count;++i) CHECK(lists[i].Status==NDIS_STATUS_RESOURCES);
    for(ULONG i=0;i<queues;++i) {
        PB_KERNEL_QUEUE *q=Session->Queues+i;CHECK(!q->TxPoolExhausted && !q->TxPool.FreeBits[0] && q->Rings[PB_QUEUE_TX_READY].Cursor==0);
        for(ULONG j=0;j<2;++j) CHECK(q->TxSlots[j].State==PB_SLOT_PUBLISHED && q->TxSlots[j].Generation==1);
    }
    printf("PASS: actual full ready ring: attempts=%lu queues=%lu length=%lu TxLock pairs=%lu bounded-window target=%lu recycle polls=%lu ready-space reads=%lu max additional reads/TxLock=%lu; no slot reuse or extra notification.\n",
        count,queues,length,TxLockAcquires,boundedLocks,RecyclePolls,ReadySpaceReads,MaxQueueReads);
    PbTestGuardFree(lists);PbTestGuardFree(buffers);Destroy();
}
static VOID FullRingLatePeer(VOID)
{
    for(ULONG wrap=0;wrap<2;++wrap) for(ULONG action=1;action<=4;++action) {
        Setup(1,2,2);ULONG64 base=wrap?UINT64_MAX-1:0;FillReadyRing(0,base);
        if(action==2) {
            /* The ready consumer and return producer are independent inputs.
             * A previously validated early return leaves a free private slot. */
            PB_DESCRIPTOR d=Session->Queues[0].Rings[PB_QUEUE_TX_READY].Entries[(ULONG)base&1];
            PushRecycles(&d,1);
        }
        ReadyAtRead=2;ReadyAction=action;
        NET_BUFFER buffers[3]={{.Length=64},{.Length=64},{.Length=64}};
        NET_BUFFER_LIST lists[3]={{.Nb=buffers,.Next=lists+1},{.Nb=buffers+1,.Next=lists+2},{.Nb=buffers+2}};
        CHECK(PbV2Send(Current,lists) && !ReadyAtRead && Completed==3 && AfterPinRelease==1 && lists[0].Status==NDIS_STATUS_RESOURCES);
        PB_KERNEL_QUEUE *q=Session->Queues;
        if(action<=2) {
            CHECK(!Faults && lists[1].Status==NDIS_STATUS_SUCCESS && lists[2].Status==NDIS_STATUS_RESOURCES &&
                Session->TxObjects==1 && Reserves==1 && Describes==1 && RecycleAttempts==1 &&
                q->Rings[PB_QUEUE_TX_READY].Cursor==base+3 && q->TxSlots[0].Generation==2 && q->TxSlots[1].Generation==1);
        } else {
            CHECK(Faults==1 && Session->Faulted && !Session->TxObjects && !Reserves && !Describes && !RecycleAttempts &&
                lists[1].Status==(action==3?NDIS_STATUS_PAUSED:NDIS_STATUS_RESOURCES) && lists[2].Status==NDIS_STATUS_PAUSED &&
                q->Rings[PB_QUEUE_TX_READY].Cursor==base+2 && q->TxSlots[0].Generation==1 && q->TxSlots[1].Generation==1);
            CHECK(Current->Probe.SendDiagnostics.Reasons[action==3?PB_SEND_RECYCLE:PB_SEND_QUEUE_RING]==1);
        }
        CHECK(MaxQueueReads<=PB_MAX_BATCH && !q->TxPoolExhausted);
        Destroy();
    }
    puts("PASS: full-ring peer progress before the next attempt: late ready+return, ready-only with prior valid retirement, corrupt return producer and corrupt ready consumer; eight ordinary/wrapped schedules preserve reuse authorization, original failures, fault admission stop and callback/reference barriers.");
}
static VOID FullRingSchedules(VOID)
{
    FullRingLockCost(1,64);FullRingLockCost(8,64);FullRingLockCost(1,PB_BLOCK_SIZE);FullRingLockCost(1,UINT32_MAX);FullRingLatePeer();
    for(ULONG wrap=0;wrap<2;++wrap) {
        Setup(1,2,2);ULONG64 base=wrap?UINT64_MAX-1:0;FillReadyRing(0,base);
        PB_KERNEL_QUEUE *q=Session->Queues;
        for(ULONG i=0;i<2;++i) LateReadItems[i]=q->Rings[PB_QUEUE_TX_READY].Entries[(ULONG)(base+i)&1];
        LateReadDrip=TRUE;LateReadCount=2;
        NET_BUFFER buffers[4]={{.Length=64},{.Length=64},{.Length=64},{.Length=64}};
        NET_BUFFER_LIST lists[4]={{.Nb=buffers,.Next=lists+1},{.Nb=buffers+1,.Next=lists+2},{.Nb=buffers+2,.Next=lists+3},{.Nb=buffers+3}};
        CHECK(PbV2Send(Current,lists) && Completed==4 && AfterPinRelease==1 && !Faults && !Reserves && !Describes &&
            RecycleAttempts==2 && RecyclePolls==6 && ReadySpaceReads==4 && !Session->TxObjects && !q->TxPoolExhausted &&
            q->TxPool.FreeBits[0]==3 && q->Rings[PB_QUEUE_TX_READY].Cursor==base+2);
        for(ULONG i=0;i<4;++i) CHECK(lists[i].Status==NDIS_STATUS_RESOURCES);
        for(ULONG i=0;i<2;++i) CHECK(q->TxSlots[i].State==PB_SLOT_FREE && q->TxSlots[i].Generation==1);
        Destroy();
    }
    puts("PASS: two valid late returns with no ready-space release are still retired on their own attempts; full ready ring authorizes no reservation/publication or pool-full cache, ordinary/wrapped cursors and reference barriers preserved.");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc==2 && !strcmp(argv[1],"--ring-pressure-locks")) {FullRingSchedules();return 0;}
#ifdef PB_RECYCLE_TOKEN_BASELINE
    if(argc==2 && !strcmp(argv[1],"--historical-recycle")) {
        RecycleFaultPrefix();RecycleTokenValidation();SendAfterRecycleFault(FALSE);SendAfterRecycleFault(TRUE);
        puts("PASS: historical recycle A/B scope: copied bytes, bounded token retirement, valid prefix, stale/duplicate tokens, cursor wrap and post-fault send admission; later persistent-pool recovery runs on current code only.");return 0;
    }
#endif
    if(argc==2 && !strcmp(argv[1],"--pressure-observation")){PressureObservation();return 0;}
    if(argc==2 && !strcmp(argv[1],"--pressure-handoff")) {PublishBeforeFurtherPressure();return 0;}
    if(argc==2 && !strcmp(argv[1],"--full-pool-windows")) {FullPoolWindows(1);return 0;}
    if(argc==2 && !strcmp(argv[1],"--full-pool-switches")) {FullPoolWindows(8);return 0;}
    if(argc==2 && !strcmp(argv[1],"--late-recycle-after-full")) {ReturnAfterFullObservation();return 0;}
    if(argc==2 && !strcmp(argv[1],"--late-recycle")) {ReturnDuringSendWindow();return 0;}
    Chain(32,64,0,0,1,0); Chain(4096,64,0,0,1,1); Chain(4096,64,1,0,1,0);
    Chain(4096,64,0,1,1,0); Chain(64,PB_BLOCK_SIZE,1,0,1,0); Chain(64,UINT32_MAX,1,0,1,0);
    Chain(128,64,1,0,2,0);
    for(ULONG action=1;action<=4;++action) Boundary(action);
    MultiBufferAndCallbacks();
    RecycleFaultPrefix();
#ifndef PB_SEND_FAULT_BASELINE
    RecycleTokenValidation();
    RecycleSnapshotReuse();
#endif
    SendAfterRecycleFault(FALSE);SendAfterRecycleFault(TRUE);EthernetClassification();
#if defined(PB_SEND_FAULT_BASELINE) || defined(PB_SEND_PREFIX_BASELINE)
    if(argc==2 && !strcmp(argv[1],"--historical-send")) {
#ifdef PB_SEND_PREFIX_BASELINE
        SendPrefixes();
#endif
        puts("PASS: historical send A/B scope: original budgets, ownership, recycle faults, callback tails and Ethernet/prefix contract; later diagnostics and persistent-pool recovery run on current code only.");return 0;
    }
#endif
    SendFailureHistory();
    ReturnDuringSendWindow();
    ReturnAfterFullObservation();
    FullPoolWindows(1);RingPressureIsNotPoolPressure();
    PublishBeforeFurtherPressure();
    PressureHandoffBoundaries();
    PressureObservation();
    CurrentPoolObservation();
    FullRingSchedules();
#ifndef PB_SEND_FAULT_BASELINE
    SendPrefixes();
#endif
    puts("PASS: actual send/recycle/reference paths, bounded attempts and bytes on every outcome, queue switches, pause/detach/fault/recycle at window boundaries, same-NBL publication failure, unlocked reentrant callback, pin through completion, no NBL access after completion."); return 0;
}
