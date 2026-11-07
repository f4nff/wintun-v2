/* SPDX-License-Identifier: GPL-2.0 */
/* Actual worker, Pause/Restart, return and unregister paths with hosted DDIs.
 * Public peers are core queue/pool objects, not the DLL lifecycle. Gates bound
 * legal callback tails; production events are cleared only by actual code. */
#ifdef PB_API_TESTS
#error Shipping worker paths required
#endif
#ifndef NDEBUG
#error Shipping worker pause configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"worker pause requires shipping fingerprint");
static ULONG RuntimeVersion;
#define PB_NBL_VERSION RuntimeVersion
#include PB_WP_HOST
#include "../../include/packetbridge_probe.h"
typedef LONG NTSTATUS;
typedef NET_BUFFER_LIST *PNET_BUFFER_LIST;
typedef VOID *NDIS_HANDLE,*PNDIS_MINIPORT_PAUSE_PARAMETERS,*PNDIS_MINIPORT_RESTART_PARAMETERS;
typedef ULONG NDIS_STATUS;
typedef VOID MINIPORT_RETURN_NET_BUFFER_LISTS(NDIS_HANDLE,NET_BUFFER_LIST *,ULONG);
typedef NDIS_STATUS MINIPORT_PAUSE(NDIS_HANDLE,PNDIS_MINIPORT_PAUSE_PARAMETERS);
typedef NDIS_STATUS MINIPORT_RESTART(NDIS_HANDLE,PNDIS_MINIPORT_RESTART_PARAMETERS);
typedef VOID KSTART_ROUTINE(PVOID);
#define Executive 0
#define KernelMode 0
#define WaitAny 0
#define STATUS_SUCCESS 0
#define NDIS_STATUS_PAUSED 1u
static TUN_CTX *Ctx;static PB_KERNEL_SESSION *Session;
static HANDLE InitialIdle,FirstPaused,FirstGate,QueuedPaused,RestartGate,NewIdle,PauseWaiting,OldTail[2],OldGate[2];
static NET_BUFFER_LIST *Lists[2];
static ULONG Enabled,Indications,SelectedPaused,RestartIndex,FirstIndex,Created,Joined,WorkerJoined;
static volatile LONG Handles,Objects,Locked;
static DWORD PauseId;
static uint64_t Base;
static ULONG Cases,AllJoins,AllWorkerJoins;
static __declspec(thread) LONG Critical,ReturnRole=-1;
static HANDLE Event(BOOLEAN manual,BOOLEAN initial){HANDLE h=CreateEventW(NULL,manual,initial,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static VOID Await(HANDLE h){CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static VOID Close(HANDLE h){CHECK(CloseHandle(h)&&InterlockedDecrement(&Handles)>=0);}
static HANDLE Start(LPTHREAD_START_ROUTINE fn,VOID *arg){HANDLE h=CreateThread(NULL,0,fn,arg,0,NULL);CHECK(h);InterlockedIncrement(&Handles);++Created;return h;}
static VOID Join(HANDLE h){DWORD code;Await(h);CHECK(GetExitCodeThread(h,&code)&&!code);Close(h);++Joined;}
static VOID KeEnterCriticalRegion(VOID){CHECK(!Critical++);}
static VOID KeLeaveCriticalRegion(VOID){CHECK(Critical--==1);}
static VOID ExAcquireResourceSharedLite(SRWLOCK *l,BOOLEAN wait){CHECK(Critical==1&&wait);AcquireSRWLockShared(l);}
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *l,BOOLEAN wait){CHECK(Critical==1&&wait);AcquireSRWLockExclusive(l);}
static VOID ExReleaseResourceLite(SRWLOCK *l){ReleaseSRWLockShared(l);}
/* Exclusive release is selected by the caller in the unregister wrapper. */
static KIRQL ExAcquireSpinLockShared(SRWLOCK *l){AcquireSRWLockShared(l);return 0;}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *l){AcquireSRWLockExclusive(l);return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *l,KIRQL saved){CHECK(!saved);ReleaseSRWLockShared(l);}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *l,KIRQL saved){CHECK(!saved);ReleaseSRWLockExclusive(l);}
static VOID KeWaitForSingleObject(KEVENT *e,int reason,int mode,BOOLEAN alert,VOID *timeout)
{CHECK(!reason&&!mode&&!alert&&!timeout);if(GetCurrentThreadId()==PauseId&&e==&Session->AllReturned)CHECK(SetEvent(PauseWaiting));Await(e->Native);}
static VOID KeWaitForMultipleObjects(ULONG count,VOID **events,int type,int reason,int mode,BOOLEAN alert,VOID *timeout,VOID *blocks)
{
    CHECK((count==2||count==3)&&!type&&!reason&&!mode&&!alert&&!timeout&&!blocks);
    HANDLE native[3];for(ULONG i=0;i<count;++i)native[i]=((KEVENT *)events[i])->Native;
    if(count==2&&Indications==1)CHECK(SetEvent(InitialIdle));
    if(count==2&&Indications==2)CHECK(SetEvent(NewIdle));
    if(count==3){
        if(!SelectedPaused++){CHECK(SetEvent(FirstPaused));Await(FirstGate);}
        else if(Session->Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Shared->Producer==Base+64){
            CHECK(!Ctx->Running&&!Session->Active&&!Session->Queues[0].Credits&&!Session->Queues[0].CoalesceBusy[0]&&!Session->Queues[0].CoalesceBusy[1]);
            CHECK(WaitForSingleObject(Session->Queues[0].KernelEvent->Native,0)==WAIT_TIMEOUT&&WaitForSingleObject(Ctx->Probe.RunningEvent.Native,0)==WAIT_TIMEOUT);
            CHECK(SetEvent(QueuedPaused));Await(RestartGate);DWORD result=WaitForMultipleObjects(count,native,FALSE,5000);CHECK(result==WAIT_OBJECT_0+2);RestartIndex=result-WAIT_OBJECT_0;return;
        }
    }
    DWORD result=WaitForMultipleObjects(count,native,FALSE,5000);CHECK(result<WAIT_OBJECT_0+count);if(count==3&&SelectedPaused==1){CHECK(result==WAIT_OBJECT_0+1);FirstIndex=1;}
}
static VOID PsTerminateSystemThread(LONG status){CHECK(!status);}
#include PB_WP_GATE
static ULONG PbEffectiveMask(TUN_CTX *ctx){CHECK(ctx==Ctx);return Enabled;}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *lists,ULONG count,ULONG flags)
{CHECK(adapter==Ctx&&lists&&count==(Enabled?2u:32u)&&!flags&&Indications<2&&!Lists[Indications]);Lists[Indications++]=lists;}
#include "../../driver/v2/session_drain.h"
#include "../../driver/v2/session_coalesce.h"
#include "../../driver/v2/session_rx_stats.h"
#include PB_WP_RECEIVE
#include PB_WP_PAUSE
#pragma warning(push)
#pragma warning(disable:4100 4459)
#include PB_WP_OUTER
#include PB_WP_TUNPAUSE
#include PB_WP_RESTART
#pragma warning(pop)
#include "../../driver/v2/session_recycle.h"
#include "../../driver/v2/session_worker.h"
static VOID ZwWaitForSingleObject(HANDLE thread,BOOLEAN alert,VOID *timeout){CHECK(!alert&&!timeout);Await(thread);DWORD code;CHECK(GetExitCodeThread(thread,&code)&&!code);++Joined;++WorkerJoined;}
static VOID ZwClose(HANDLE thread){Close(thread);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){AcquireSRWLockExclusive(&r->Lock);CHECK(!r->References);ReleaseSRWLockExclusive(&r->Lock);}
static VOID ObDereferenceObject(KEVENT *e){Close(e->Native);CHECK(InterlockedDecrement(&Objects)>=0);free(e);}
static VOID MmUnlockPages(MDL *m){CHECK(m==Session->Mdl&&Locked==1);m->Address=NULL;InterlockedDecrement(&Locked);}
#include "../../driver/v2/session_cleanup.h"
static VOID PbReportOffload(TUN_CTX *ctx){CHECK(ctx==Ctx&&!ctx->V2&&!ctx->Probe.V2SessionActive&&!ctx->Probe.AllowedMask);}
#include "../../driver/v2/session_unregister.h"
static KEVENT *Object(BOOLEAN manual){KEVENT *e=calloc(1,sizeof(*e));CHECK(e);e->Native=Event(manual,FALSE);InterlockedIncrement(&Objects);return e;}
static VOID Freed(VOID *p){if(p==Session){CHECK(!Locked&&!Objects&&!Mdls&&!Nbls);Close(Session->StartEvent.Native);Close(Session->StopEvent.Native);Close(Session->AllReturned.Native);Session=NULL;}}
static VOID ReturnTail(EX_RUNDOWN_REF *refs){if(Session&&refs==&Session->Returns&&ReturnRole>=0&&ReturnRole<2){CHECK(SetEvent(OldTail[ReturnRole]));Await(OldGate[ReturnRole]);}}
typedef struct RETURN_JOB {NET_BUFFER_LIST *Head;LONG Role;} RETURN_JOB;
static DWORD WINAPI Returning(VOID *p){RETURN_JOB *j=p;ReturnRole=j->Role;TunReturnNetBufferLists(Ctx,j->Head,0);ReturnRole=-1;return 0;}
static DWORD WINAPI Working(VOID *p){PbV2Worker(p);return 0;}
static DWORD WINAPI Pausing(VOID *unused){(void)unused;PauseId=GetCurrentThreadId();CHECK(TunPause(Ctx,NULL)==NDIS_STATUS_SUCCESS);return 0;}
static DWORD WINAPI Unregistering(VOID *unused){(void)unused;KeEnterCriticalRegion();ExAcquireResourceExclusiveLite(&Ctx->Device.RegistrationLock,TRUE);PbV2Unregister(Ctx);ReleaseSRWLockExclusive(&Ctx->Device.RegistrationLock);KeLeaveCriticalRegion();return 0;}
static VOID W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID W32(UCHAR *p,ULONG n){W16(p,n>>16);W16(p+2,n);}
static VOID Packet(UCHAR *p,ULONG ip,ULONG protocol,ULONG slot,ULONG generation)
{
    ULONG offset=ip==4?20:40,header=offset+(protocol==6?20:8);memset(p,0,1500);
    if(ip==4){p[0]=0x45;W16(p+2,1500);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;}else{p[0]=0x60;W16(p+4,1460);p[6]=(UCHAR)protocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
    W16(p+offset,4000+slot/16);W16(p+offset+2,5000);if(protocol==6){W32(p+offset+4,1000+generation*50000+(slot%16)*(1500-header));p[offset+12]=0x50;p[offset+13]=0x10;W16(p+offset+14,8192);}else W16(p+offset+4,1500-offset);
    memset(p+header,(int)(generation*64+slot),1500-header);CHECK(PbFinalizeChecksums(p,1500,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);
}
static VOID Submit(PB_POOL *pool,PB_QUEUE_VIEW *producer,ULONG ip,ULONG protocol,ULONG generation)
{
    PB_DESCRIPTOR d[32];for(ULONG i=0;i<32;++i){memset(d+i,0,sizeof(*d));CHECK(PbPoolReserve(pool,&d[i].Token)==PB_OK&&d[i].Token.Slot==i&&d[i].Token.Generation==generation);d[i].Length=1500;d[i].DataOffset=Session->Layout.Queues[0].RxData+i*PB_BLOCK_SIZE;Packet(Session->Region+d[i].DataOffset,ip,protocol,i,generation);CHECK(PbPoolTransition(pool,&d[i].Token,PB_SLOT_RESERVED,PB_SLOT_PUBLISHED)==PB_OK);}
    uint32_t count,notify;CHECK(PbQueuePush(producer,d,32,&count,&notify)==PB_OK&&count==32);if(notify)KeSetEvent(Session->Queues[0].KernelEvent,0,FALSE);
}
static VOID Retire(PB_POOL *pool,PB_QUEUE_VIEW *consumer,ULONG generation)
{/* Polling frees a genuinely full completion ring, the public consumer's wake condition. */
 PB_DESCRIPTOR d[32];uint32_t count;CHECK(consumer->Shared->Producer-consumer->Shared->Consumer==32);
 CHECK(PbQueuePop(consumer,d,32,&count)==PB_OK&&count==32);ULONG64 seen=0;for(ULONG i=0;i<32;++i){CHECK(d[i].Status==PB_OK&&d[i].Token.Generation==generation&&d[i].Token.Slot<32);ULONG64 bit=1ull<<d[i].Token.Slot;CHECK(!(seen&bit));seen|=bit;CHECK(PbPoolTransition(pool,&d[i].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);}CHECK(seen==0xffffffffull);KeSetEvent(Session->Queues[0].KernelEvent,0,FALSE);}
static VOID VerifyChain(NET_BUFFER_LIST *head,ULONG ip,ULONG protocol,ULONG generation)
{
    ULONG seen=0,objects=0,originals=0;ULONG header=(ip==4?20u:40u)+(protocol==6?20u:8u);
    for(NET_BUFFER_LIST *n=head;n;n=n->Next){
        PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);
        CHECK(r->Queue==Session->Queues && r->Count==(Enabled?16u:1u) && r->IpVersion==ip && r->Protocol==protocol);
        CHECK(n->FirstMdl==r->HeaderMdl && r->HeaderMdl->Length==14 && r->HeaderMdl->Next==r->Mdl && !r->Mdl->Next);
        CHECK(n->Length==14+r->IndicationLength && r->Mdl->Length==r->IndicationLength);
        CHECK((ULONG)(((BYTE *)r->Mdl->Address)[0]>>4)==ip);
        if(Enabled){CHECK(r->CoalesceIndex==objects && !r->Mdl->Partial && r->Mdl->Address==Session->Queues[0].CoalesceBuffers[objects]);CHECK(r->IndicationLength==header+16*(1500-header));}
        else CHECK(r->CoalesceIndex==MAXULONG && r->Mdl->Partial && r->IndicationLength==1500);
        for(ULONG i=0;i<r->Count;++i){
            const PB_DESCRIPTOR *d=r->Descriptors+i;CHECK(d->Token.Slot==originals && d->Token.Generation==generation && d->Length==1500 && originals<32);
            ULONG bit=1u<<originals;CHECK(!(seen&bit));seen|=bit;
            BYTE expected[1500];Packet(expected,ip,protocol,originals,generation);
            CHECK(!memcmp(Session->Region+d->DataOffset,expected,1500));
            if(Enabled)CHECK(!memcmp((BYTE *)r->Mdl->Address+header+i*(1500-header),expected+header,1500-header));
            else CHECK(r->Mdl->Address==Session->Region+d->DataOffset && !memcmp(r->Mdl->Address,expected,1500));
            ++originals;
        }
        if(RuntimeVersion<PB_NDIS_VERSION(30))CHECK(n->Info[TcpRecvSegCoalesceInfo]==(VOID *)(ULONG_PTR)~0u && n->Info[RscTcpTimestampDelta]==(VOID *)(ULONG_PTR)~0u);
        if(RuntimeVersion<PB_NDIS_VERSION(89))CHECK(n->Info[UdpRecvSegCoalesceOffloadInfo]==(VOID *)(ULONG_PTR)~0u);
        if(Enabled && protocol==6){NDIS_RSC_NBL_INFO info={.Value=n->Info[TcpRecvSegCoalesceInfo]};CHECK(info.Info.CoalescedSegCount==16 && r->ReceiveType==PB_DESC_TCP_GSO);}
        if(Enabled && protocol==17){NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info={.Value=n->Info[UdpRecvSegCoalesceOffloadInfo]};CHECK(info.Receive.SegCount==16 && info.Receive.SegSize==1500-header && r->ReceiveType==PB_DESC_UDP_GSO);}
        ++objects;
    }
    CHECK(seen==0xffffffffu && originals==32 && objects==(Enabled?2u:32u));
}
static VOID Case(ULONG ip,ULONG protocol,ULONG minor,BOOLEAN manual,BOOLEAN wrap,BOOLEAN measured)
{
    CHECK(ip==PB_TEST_IP_VERSION && !Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);RuntimeVersion=PB_NDIS_VERSION(minor);Enabled=minor==20?0:protocol==6?(ip==4?PB_RX_RSC4:PB_RX_RSC6):PB_RX_URO;
    Indications=SelectedPaused=RestartIndex=FirstIndex=Created=Joined=WorkerJoined=0;PauseId=0;memset(Lists,0,sizeof(Lists));
    Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->Probe.V2SessionActive=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;Ctx->Probe.LocalMac[0]=Ctx->Probe.PeerMac[0]=2;
    CHECK(PbCalculateLayout(1,32,32,&Session->Layout)==PB_OK);
    UCHAR *region;
#ifdef PB_GUARD_TESTS
    region=PbTestGuardAlloc(Session->Layout.RegionSize);
#else
    region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#endif
    CHECK(region);Session->Region=region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(region+Session->Layout.LifecycleOffset);Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
    Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);
    Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
    PB_KERNEL_QUEUE *q=Session->Queues;q->Session=Session;q->KernelEvent=Object(manual);q->UserEvent=Object(FALSE);q->RxSlots=ExAllocatePool2(0,32*sizeof(*q->RxSlots),0);q->TxSlots=ExAllocatePool2(0,32*sizeof(*q->TxSlots),0);q->Pending=ExAllocatePool2(0,32*sizeof(*q->Pending),0);q->Scratch=ExAllocatePool2(0,PB_BLOCK_SIZE,0);
    for(ULONG i=0;i<2;++i)q->CoalesceBuffers[i]=ExAllocatePool2(0,PB_BLOCK_SIZE,0);CHECK(PbPoolInitialize(&q->TxPool,q->TxSlots,32,42,0)==PB_OK);
    Base=wrap?UINT64_MAX-15:0;
    for(ULONG i=0;i<PB_QUEUE_COUNT;++i){CHECK(PbAttachQueue(q->Rings+i,region,&Session->Layout,0,i,i==PB_QUEUE_TX_READY||i==PB_QUEUE_RX_COMPLETE)==PB_OK);PB_QUEUE_VIEW *v=q->Rings+i;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=Base;}
    PB_QUEUE_VIEW producer,consumer;CHECK(PbAttachQueue(&producer,region,&Session->Layout,0,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK&&PbAttachQueue(&consumer,region,&Session->Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);producer.Cursor=producer.PeerCursor=consumer.Cursor=consumer.PeerCursor=Base;
    PB_SLOT slots[32];PB_POOL pool;CHECK(PbPoolInitialize(&pool,slots,32,42,0)==PB_OK);
    InitialIdle=Event(TRUE,FALSE);FirstPaused=Event(TRUE,FALSE);FirstGate=Event(TRUE,FALSE);QueuedPaused=Event(TRUE,FALSE);RestartGate=Event(TRUE,FALSE);NewIdle=Event(TRUE,FALSE);PauseWaiting=Event(TRUE,FALSE);for(ULONG i=0;i<2;++i){OldTail[i]=Event(TRUE,FALSE);OldGate[i]=Event(TRUE,FALSE);}
    Submit(&pool,&producer,ip,protocol,1);q->Thread=Start(Working,q);Await(InitialIdle);
    CHECK(Indications==1&&Session->Active==(Enabled?2:32)&&q->Credits==32&&!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]);
    VerifyChain(Lists[0],ip,protocol,1);
    HANDLE pause=Start(Pausing,NULL);Await(FirstPaused);Await(PauseWaiting);CHECK(!Ctx->Running&&WaitForSingleObject(pause,0)==WAIT_TIMEOUT);
    RETURN_JOB jobs[2]={{.Role=0},{.Role=1}};NET_BUFFER_LIST *head=Lists[0];Lists[0]=NULL;
    for(ULONG i=0;i<2;++i){jobs[i].Head=head;ULONG take=Enabled?1:16;for(ULONG j=1;j<take;++j){CHECK(head);head=head->Next;}CHECK(head);NET_BUFFER_LIST *next=head->Next;head->Next=NULL;head=next;}CHECK(!head);
    BeforeRundownRelease=ReturnTail;HANDLE oldThreads[2]={Start(Returning,jobs),Start(Returning,jobs+1)};Await(OldTail[0]);Await(OldTail[1]);Join(pause);
    CHECK(!Session->Active&&Session->Returns.References==2&&!Nbls&&Mdls==1&&!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0);
    Retire(&pool,&consumer,1);Submit(&pool,&producer,ip,protocol,2);CHECK(q->Rings[PB_QUEUE_RX_SUBMIT].Cursor==Base+32&&producer.Cursor==Base+64&&!Ctx->Running);
    CHECK(SetEvent(FirstGate));Await(QueuedPaused);CHECK(FirstIndex==1&&Indications==1&&Session->Returns.References==2);
    CHECK(TunRestart(Ctx,NULL)==NDIS_STATUS_SUCCESS&&SetEvent(RestartGate));Await(NewIdle);
    CHECK(RestartIndex==2&&Indications==2&&Session->Active==(Enabled?2:32)&&q->Credits==32&&Session->Returns.References==2&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_TIMEOUT);
    VerifyChain(Lists[1],ip,protocol,2);
    CHECK(q->CoalesceBusy[0]==!!Enabled&&q->CoalesceBusy[1]==!!Enabled);UCHAR preserved[2][65536];if(Enabled)for(ULONG i=0;i<2;++i)memcpy(preserved[i],q->CoalesceBuffers[i],PB_BLOCK_SIZE);
    for(ULONG i=0;i<2;++i){CHECK(SetEvent(OldGate[i]));Join(oldThreads[i]);}
    CHECK(!Session->Returns.References&&Session->Active==(Enabled?2:32)&&q->CoalesceBusy[0]==!!Enabled&&q->CoalesceBusy[1]==!!Enabled&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_TIMEOUT);
    if(Enabled)for(ULONG i=0;i<2;++i)CHECK(!memcmp(preserved[i],q->CoalesceBuffers[i],PB_BLOCK_SIZE));
    RETURN_JOB fresh={.Head=Lists[1],.Role=2};Lists[1]=NULL;Join(Start(Returning,&fresh));Retire(&pool,&consumer,2);
    CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References&&Session->RxObjects==64&&Session->RxReturned==64&&Session->Completions==64&&!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&!q->CoalesceBusy[1]&&!Session->Faulted&&!Session->Draining);
    for(ULONG i=0;i<32;++i)CHECK(slots[i].Generation==2&&slots[i].State==PB_SLOT_FREE&&q->RxSlots[i].Generation==2&&!q->RxSlots[i].Active);
    BeforePoolFree=Freed;Join(Start(Unregistering,NULL));CHECK(!Session&&!Ctx->V2&&!Ctx->Probe.ReturnRundown.References&&WorkerJoined==1&&Created==6&&Joined==6&&!Objects&&!Locked&&!Mdls&&!Nbls);
    BeforePoolFree=NULL;BeforeRundownRelease=NULL;Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);for(ULONG i=0;i<2;++i)Close(Ctx->Probe.IndicationIdle[i].Native);ExFreePoolWithTag(Ctx,0);Ctx=NULL;
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(region);
#else
    CHECK(VirtualFree(region,0,MEM_RELEASE));
#endif

    Close(InitialIdle);Close(FirstPaused);Close(FirstGate);Close(QueuedPaused);Close(RestartGate);Close(NewIdle);Close(PauseWaiting);for(ULONG i=0;i<2;++i){Close(OldTail[i]);Close(OldGate[i]);}CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked);
    ++Cases;AllJoins+=Joined;AllWorkerJoins+=WorkerJoined;
    printf("WORKER_PAUSE32 ip%lu protocol%lu ndis%lu manual%u wrap%u measured%u old32 new32 pausedAdmission0 kernelWake1 restartWake2 oldPins2 joins6 owners0 repairs0\n",ip,protocol,minor,manual,wrap,measured);
}
int main(VOID)
{
    DWORD before=0,after;
    for(ULONG pass=0;pass<2;++pass){
        if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(ULONG shape=0;shape<4;++shape)for(ULONG manual=0;manual<2;++manual)for(ULONG wrap=0;wrap<2;++wrap)
            Case(PB_TEST_IP_VERSION,shape==1||shape==3?17:6,shape<2?20:shape==2?30:89,(BOOLEAN)manual,(BOOLEAN)wrap,(BOOLEAN)pass);
    }
    CHECK(Cases==32 && AllJoins==192 && AllWorkerJoins==32 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("PASS: family%u actual worker Pause32/Restart32 cases%lu warm16 measured16 joins%lu workerjoins%lu handles%lu/%lu\n",PB_TEST_IP_VERSION,Cases,AllJoins,AllWorkerJoins,before,after);return 0;
}
