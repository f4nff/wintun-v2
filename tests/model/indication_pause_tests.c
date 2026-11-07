/* SPDX-License-Identifier: GPL-2.0 */
/* Ready hosted kernel session: actual Receive/Return/Pause/bank functions,
 * native Windows locks/events and joined OS threads; no native NDIS or IRQL. */
#ifdef PB_API_TESTS
#error Indication/pause must not enable API test shortcuts
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"indication/pause requires shipping source fingerprint");
#include PB_INDICATION_NBL_HOST
typedef NET_BUFFER_LIST *PNET_BUFFER_LIST;
typedef VOID *NDIS_HANDLE;
typedef ULONG NDIS_STATUS;
typedef VOID *PNDIS_MINIPORT_PAUSE_PARAMETERS;
typedef NDIS_STATUS MINIPORT_PAUSE(NDIS_HANDLE,PNDIS_MINIPORT_PAUSE_PARAMETERS);
typedef VOID MINIPORT_RETURN_NET_BUFFER_LISTS(NDIS_HANDLE,NET_BUFFER_LIST *,ULONG);
#define NDIS_STATUS_PAUSED 1u
#define Executive 0
#define KernelMode 0
static TUN_CTX *Ctx;
static PB_KERNEL_SESSION *Session;
static HANDLE Entered[2],Leave[2],BankWait,PauseDone,Rotated;
static NET_BUFFER_LIST *Lists[2];
static SRWLOCK PolicyMutex;
static volatile LONG Handles,Objects,Locked,Threads,Regions,PauseId;
static __declspec(thread) LONG TransitionHeld,Critical;
static ULONG WaitMask,Scenarios,Detected;
static HANDLE Event(BOOLEAN manual,BOOLEAN initial){HANDLE h=CreateEventW(NULL,manual,initial,NULL);CHECK(h);InterlockedIncrement(&Handles);return h;}
static VOID Close(HANDLE h){CHECK(CloseHandle(h)&&InterlockedDecrement(&Handles)>=0);}
static VOID Await(HANDLE h){CHECK(WaitForSingleObject(h,5000)==WAIT_OBJECT_0);}
static KIRQL ExAcquireSpinLockShared(SRWLOCK *l){CHECK(!TransitionHeld);AcquireSRWLockShared(l);TransitionHeld=1;return 0;}
static KIRQL ExAcquireSpinLockExclusive(SRWLOCK *l){CHECK(!TransitionHeld);AcquireSRWLockExclusive(l);TransitionHeld=2;return 0;}
static VOID ExReleaseSpinLockShared(SRWLOCK *l,KIRQL saved){CHECK(!saved&&TransitionHeld==1);TransitionHeld=0;ReleaseSRWLockShared(l);}
static VOID ExReleaseSpinLockExclusive(SRWLOCK *l,KIRQL saved){CHECK(!saved&&TransitionHeld==2);TransitionHeld=0;ReleaseSRWLockExclusive(l);}
static VOID KeEnterCriticalRegion(VOID){CHECK(!Critical++);}
static VOID KeLeaveCriticalRegion(VOID){CHECK(Critical--==1);}
static VOID ExAcquireResourceSharedLite(SRWLOCK *l,BOOLEAN wait){CHECK(Critical==1&&wait);AcquireSRWLockShared(l);}
static VOID ExReleaseResourceLite(SRWLOCK *l){ReleaseSRWLockShared(l);}
static VOID KeWaitForSingleObject(KEVENT *event,int reason,int mode,BOOLEAN alertable,VOID *timeout)
{
    CHECK(!reason&&!mode&&!alertable&&!timeout&&!TransitionHeld);
    if((LONG)GetCurrentThreadId()==PauseId)for(ULONG i=0;i<2;++i)if(event==Ctx->Probe.IndicationIdle+i){
        WaitMask|=1u<<i;if(WaitForSingleObject(event->Native,0)==WAIT_TIMEOUT){CHECK(!Session->Active&&Session->Returns.References==1&&SetEvent(BankWait));}
    }
    Await(event->Native);
}
#include PB_INDICATION_GATE_SOURCE
static ULONG PbEffectiveMask(TUN_CTX *ctx){CHECK(ctx==Ctx);return 0;}
static VOID PbIndicateReceive(VOID *adapter,NET_BUFFER_LIST *lists,ULONG count,ULONG flags)
{
    CHECK(adapter==Ctx&&!TransitionHeld&&count&&count<=2&&!flags);
    PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(lists)&~(ULONG_PTR)1);ULONG q=r->Queue->Index;
    CHECK(q<2&&!Lists[q]);Lists[q]=lists;CHECK(SetEvent(Entered[q]));Await(Leave[q]);
    /* lists and every NBL/MDL can already be freed; do not examine them. */
}
#include "../../driver/v2/session_drain.h"
#include "../../driver/v2/session_coalesce.h"
#include "../../driver/v2/session_rx_stats.h"
#include PB_INDICATION_RECEIVE_SOURCE
#include PB_INDICATION_PAUSE_SOURCE
#pragma warning(push)
#pragma warning(disable:4100 4459)
#include PB_INDICATION_OUTER_SOURCE
#include PB_INDICATION_TUNPAUSE_SOURCE
#pragma warning(pop)
static VOID ZwWaitForSingleObject(HANDLE h,BOOLEAN alertable,VOID *timeout){CHECK(!alertable&&!timeout);Await(h);}
static VOID ZwClose(HANDLE h){Close(h);CHECK(InterlockedDecrement(&Threads)>=0);}
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){AcquireSRWLockExclusive(&r->Lock);CHECK(!r->References);ReleaseSRWLockExclusive(&r->Lock);}
static VOID ObDereferenceObject(KEVENT *e){CHECK(CloseHandle(e->Native)&&InterlockedDecrement(&Handles)>=0&&InterlockedDecrement(&Objects)>=0);free(e);}
static VOID MmUnlockPages(MDL *m){CHECK(m==Session->Mdl&&Locked==1);m->Address=NULL;InterlockedDecrement(&Locked);}
#include "../../driver/v2/session_cleanup.h"
static KEVENT *Object(BOOLEAN manual){KEVENT *e=calloc(1,sizeof(*e));CHECK(e);e->Native=Event(manual,FALSE);InterlockedIncrement(&Objects);return e;}
static VOID PoolFree(VOID *p)
{
    if(p==Session){CHECK(!Threads&&!Objects&&!Locked&&!Mdls&&!Nbls);Close(Session->StartEvent.Native);Close(Session->StopEvent.Native);Close(Session->AllReturned.Native);}
    if(p==Ctx){Close(Ctx->Device.Disconnected.Native);Close(Ctx->Probe.RunningEvent.Native);Close(Ctx->Probe.IndicationIdle[0].Native);Close(Ctx->Probe.IndicationIdle[1].Native);}
}
static DWORD WINAPI Receive(VOID *q){CHECK(PbV2ReceiveBatch(q)&&!TransitionHeld);return 0;}
static DWORD WINAPI Returning(VOID *lists){TunReturnNetBufferLists(Ctx,lists,0);return 0;}
static DWORD WINAPI Pausing(VOID *unused){(void)unused;PauseId=(LONG)GetCurrentThreadId();CHECK(TunPause(Ctx,NULL)==NDIS_STATUS_SUCCESS&&!TransitionHeld&&!Critical&&SetEvent(PauseDone));return 0;}
static DWORD WINAPI Rotation(VOID *unused)
{
    (void)unused;AcquireSRWLockExclusive(&PolicyMutex);KIRQL saved=ExAcquireSpinLockExclusive(&Ctx->TransitionLock);
    ULONG old=PbRotateIndications(Ctx);ExReleaseSpinLockExclusive(&Ctx->TransitionLock,saved);CHECK(old==0&&SetEvent(Rotated));
    PbWaitIndications(Ctx,old);ReleaseSRWLockExclusive(&PolicyMutex);return 0;
}
static VOID Run(ULONG shape,ULONG nbls,BOOLEAN manual,BOOLEAN negative)
{
    CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked&&!Threads&&!Regions);memset(Lists,0,sizeof(Lists));WaitMask=0;PauseId=0;
    Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);ULONG queues=shape==2?2:1;
    Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->MiniportAdapterHandle=Ctx->NblPool=Ctx;
    CHECK(PbCalculateLayout(queues,2,2,&Session->Layout)==PB_OK);UCHAR *region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(region);InterlockedIncrement(&Regions);
    Session->Region=region;Session->Mdl=IoAllocateMdl(region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
    Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);
    Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);
    for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
    for(ULONG i=0;i<queues;++i){PB_KERNEL_QUEUE *q=Session->Queues+i;q->Session=Session;q->Index=i;q->KernelEvent=Object(manual);q->UserEvent=Object(manual);
        q->RxSlots=ExAllocatePool2(0,2*sizeof(*q->RxSlots),0);q->Pending=ExAllocatePool2(0,2*sizeof(*q->Pending),0);CHECK(q->RxSlots&&q->Pending);
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r)CHECK(PbAttachQueue(q->Rings+r,region,&Session->Layout,i,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);
        PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,region,&Session->Layout,i,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK);
        PB_DESCRIPTOR d[2]={0};for(ULONG j=0;j<nbls;++j){d[j]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)j,(USHORT)i},.Length=20,.DataOffset=Session->Layout.Queues[i].RxData+j*PB_BLOCK_SIZE};region[d[j].DataOffset]=0x45;region[d[j].DataOffset+3]=20;}
        uint32_t count,notify;CHECK(PbQueuePush(&producer,d,nbls,&count,&notify)==PB_OK&&count==nbls);Entered[i]=Event(TRUE,FALSE);Leave[i]=Event(TRUE,FALSE);
    }
    BankWait=Event(TRUE,FALSE);PauseDone=Event(TRUE,FALSE);Rotated=Event(TRUE,FALSE);HANDLE rotation=NULL;
    if(shape==1){AcquireSRWLockExclusive(&PolicyMutex);KIRQL saved=ExAcquireSpinLockExclusive(&Ctx->TransitionLock);ULONG old=PbRotateIndications(Ctx);ExReleaseSpinLockExclusive(&Ctx->TransitionLock,saved);PbWaitIndications(Ctx,old);ReleaseSRWLockExclusive(&PolicyMutex);}
    for(ULONG i=0;i<queues;++i){if(i){rotation=CreateThread(NULL,0,Rotation,NULL,0,NULL);CHECK(rotation);InterlockedIncrement(&Handles);Await(Rotated);}
        Session->Queues[i].Thread=CreateThread(NULL,0,Receive,Session->Queues+i,0,NULL);CHECK(Session->Queues[i].Thread);InterlockedIncrement(&Threads);InterlockedIncrement(&Handles);Await(Entered[i]);}
    CHECK(Session->Active==(LONG)(queues*nbls)&&Nbls==(LONG)(queues*nbls)&&Session->Indications==queues);
    NET_BUFFER_LIST *chain=Lists[0];if(queues==2){NET_BUFFER_LIST *tail=chain;while(tail->Next)tail=tail->Next;tail->Next=Lists[1];}
    HANDLE returning=CreateThread(NULL,0,Returning,chain,0,NULL);CHECK(returning);InterlockedIncrement(&Handles);Await(returning);Close(returning);
    CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References&&!Nbls&&Mdls==1&&Session->RxReturned==queues*nbls&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0);
    CHECK(Ctx->Probe.IndicationCount[shape==1?1:0]==1);if(shape==2)CHECK(Ctx->Probe.IndicationCount[1]==1);
    HANDLE pause=CreateThread(NULL,0,Pausing,NULL,0,NULL);CHECK(pause);InterlockedIncrement(&Handles);
    HANDLE stages[]={BankWait,PauseDone};DWORD observed=WaitForMultipleObjects(2,stages,FALSE,5000);CHECK(observed<WAIT_OBJECT_0+2);
    BOOLEAN early=observed==WAIT_OBJECT_0+1;CHECK(early==negative);
    if(early){CHECK(!Session->Returns.References&&Threads==(LONG)queues);++Detected;}
    else CHECK(Session->Returns.References==1&&WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
    /* No Halt/Free while an indication is held, even in the negative. */
    for(ULONG i=0;i<queues;++i)CHECK(SetEvent(Leave[i]));
    for(ULONG i=0;i<queues;++i)Await(Session->Queues[i].Thread);
    Await(pause);Close(pause);if(rotation){Await(rotation);Close(rotation);}
    CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->Probe.ReturnRundown.References&&!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]);
    CHECK(!Ctx->Running&&WaitForSingleObject(Ctx->Probe.RunningEvent.Native,0)==WAIT_TIMEOUT);
    CHECK(negative?!WaitMask:WaitMask==3);Ctx->V2=NULL;BeforePoolFree=PoolFree;PbV2Free(Session);Session=NULL;ExFreePoolWithTag(Ctx,0);Ctx=NULL;BeforePoolFree=NULL;
    CHECK(VirtualFree(region,0,MEM_RELEASE)&&InterlockedDecrement(&Regions)==0);for(ULONG i=0;i<queues;++i){Close(Entered[i]);Close(Leave[i]);}Close(BankWait);Close(PauseDone);Close(Rotated);
    CHECK(!Pools&&!Mdls&&!Nbls&&!Handles&&!Objects&&!Locked&&!Threads&&!Regions&&FixtureRundownAcquires==FixtureRundownReleases);++Scenarios;
}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"114");CHECK(argc==1||selected);
#ifdef PB_INDICATION_WITHOUT_WAIT
    BOOLEAN negative=TRUE;
#else
    BOOLEAN negative=FALSE;
#endif
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Run(2,2,TRUE,negative);
    else for(ULONG shape=0;shape<3;++shape)for(ULONG n=1;n<=2;++n)for(ULONG manual=0;manual<2;++manual)Run(shape,n,(BOOLEAN)manual,negative);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual Receive/outer Return/TunPause/indication-bank schedules, early=%lu, joined before Free, handles %lu/%lu\n",Scenarios,Detected,before,after);
    if(negative){CHECK(Detected==Scenarios);puts("INDICATION_PAUSE_SKIPPED_BANK_WAIT: early Pause observed with all receive objects returned; upcalls and every test thread joined before native session/Ctx free; zero owned resources");return 114;}
    CHECK(!Detected);return 0;
}
