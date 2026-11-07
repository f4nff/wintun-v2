/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Send/Pause/common FILE unregister/Free; ready session and idle worker
 * DDI boundaries are hosted. No native NDIS/IRQL or device operation occurs. */
#ifdef PB_API_TESTS
#error Send completion must not enable API test shortcuts
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"send completion requires shipping source fingerprint");
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static VOID ProbeWait(VOID *),ProbeResource(VOID *),ProbeRundownWait(VOID *);
#define main ReusedIndicationMain
#define ExReleaseResourceLite ReusedReleaseShared
#include PB_SEND_COMPLETION_HOST
#undef ExReleaseResourceLite
#undef main
#include "../../include/packetbridge_send_diagnostics.h"
typedef struct NET_BUFFER {struct NET_BUFFER *Next;ULONG Length;MDL *Mdl;UCHAR *Data;} NET_BUFFER;
typedef struct PB_SEND_INPUT {NET_BUFFER Nb;UCHAR Bytes[64];} PB_SEND_INPUT;
#define NET_BUFFER_LIST_FIRST_NB(n) ((NET_BUFFER *)((n)->Reserved[0]))
#define NET_BUFFER_NEXT_NB(n) ((n)->Next)
#define NET_BUFFER_DATA_LENGTH(n) ((n)->Length)
#define TcpLargeSendNetBufferListInfo NetBufferListFrameType
#define UdpSegmentationOffloadInfo TcpRecvSegCoalesceInfo
#define NDIS_STATUS_RESOURCES 2u
#define NDIS_STATUS_MEDIA_DISCONNECTED 3u
#define NDIS_STATUS_INVALID_LENGTH 4u
static HANDLE LookupEntered,LookupGate,CompleteEntered,CompleteGate,TailEntered,TailGate,PauseAll,FreeAll,FreeRundown,UnsafeEntered,UnsafeGate;
static volatile LONG SendId,UnregisterId,FreeCalls,Unsafe,Originals;
static UCHAR *UserRegion;
static volatile LONG UnexpectedWork=1;
static ULONG CompletedScenarios,DetectedScenarios;
static VOID ProbeWait(VOID *event)
{
    if(!Session || event!=&Session->AllReturned)return;
    if((LONG)GetCurrentThreadId()==PauseId)CHECK(SetEvent(PauseAll));
    if((LONG)GetCurrentThreadId()==UnregisterId)CHECK(SetEvent(FreeAll));
}
static VOID ProbeResource(VOID *object)
{
    (void)object;if((LONG)GetCurrentThreadId()!=UnregisterId || InterlockedIncrement(&FreeCalls)!=1)return;
    if(Session->Returns.References){InterlockedIncrement(&Unsafe);CHECK(SetEvent(UnsafeEntered));Await(UnsafeGate);}
    CHECK(!Session->Active&&!Session->Returns.References&&!Ctx->V2&&!Threads&&Regions==1);
}
static VOID ProbeRundownWait(VOID *refs)
{CHECK(refs==&Session->Returns&&!Session->Active);CHECK(SetEvent(FreeRundown));}
static VOID Retained(EX_RUNDOWN_REF *refs)
{
    if((LONG)GetCurrentThreadId()!=SendId)return;
    CHECK(refs==&Session->Returns&&!Session->Active&&TransitionHeld==1);CHECK(SetEvent(LookupEntered));Await(LookupGate);
}
static VOID Releasing(EX_RUNDOWN_REF *refs)
{
    if((LONG)GetCurrentThreadId()!=SendId)return;
    CHECK(refs==&Session->Returns&&!Session->Active&&!TransitionHeld&&WaitForSingleObject(Session->AllReturned.Native,0)==WAIT_OBJECT_0);
    CHECK(SetEvent(TailEntered));Await(TailGate);
}
static BOOLEAN PbV2Read(NET_BUFFER *nb,ULONG skip,UCHAR *bytes,ULONG length)
{(void)nb;(void)skip;(void)bytes;(void)length;CHECK(!UnexpectedWork);return FALSE;}
static NDIS_STATUS PbV2ControlReply(TUN_CTX *ctx,const UCHAR *bytes,ULONG length)
{(void)ctx;(void)bytes;(void)length;CHECK(!UnexpectedWork);return NDIS_STATUS_RESOURCES;}
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG skip,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)
{(void)ctx;(void)nbl;(void)nb;(void)skip;(void)d;(void)bytes;(void)failure;CHECK(!UnexpectedWork);return NDIS_STATUS_RESOURCES;}
static VOID PbV2RejectSend(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,ULONG prior,ULONG queue,ULONG ethernet,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{(void)prior;CHECK(ctx==Ctx&&epoch==42&&nbl&&nb&&status==NDIS_STATUS_PAUSED&&queue==PB_SEND_QUEUE_UNKNOWN&&!ethernet&&failure->Reason==PB_SEND_ADMISSION&&!accepted&&!attempted&&scope==2&&!q&&!pending);}
static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls){CHECK(nbls&&!TransitionHeld);}
static VOID PbCompleteSend(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbl)
{
    CHECK(adapter==Ctx&&!TransitionHeld&&Session->Active==1&&Session->Returns.References==1&&nbl->Status==NDIS_STATUS_PAUSED);
    PB_SEND_INPUT *input=(PB_SEND_INPUT *)NET_BUFFER_LIST_FIRST_NB(nbl);CHECK(!nbl->Next&&input&&input->Nb.Length==64&&input->Nb.Mdl==nbl->FirstMdl);
    IoFreeMdl(input->Nb.Mdl);NdisFreeNetBufferList(nbl);ExFreePoolWithTag(input,0);InterlockedDecrement(&Originals);
    CHECK(SetEvent(CompleteEntered));Await(CompleteGate);
    /* The complete NBL/NB/MDL/payload allocation has been released. */
}
#include "../../driver/v2/session_recycle.h"
#include PB_SEND_COMPLETION_SEND_SOURCE
#include "../../driver/v2/session_fault_drain.h"
typedef struct FILE_OBJECT {ULONG Identity;} FILE_OBJECT;
static SRWLOCK TunDispatchDeviceListLock;
static LIST_ENTRY TunDispatchDeviceList;
static FILE_OBJECT Owner={1};
static VOID InitializeListHead(LIST_ENTRY *head){head->Flink=head->Blink=head;}
static VOID InsertTailList(LIST_ENTRY *head,LIST_ENTRY *entry){entry->Flink=head;entry->Blink=head->Blink;head->Blink->Flink=entry;head->Blink=entry;}
static VOID RemoveEntryList(LIST_ENTRY *entry){entry->Blink->Flink=entry->Flink;entry->Flink->Blink=entry->Blink;}
static BOOLEAN IsListEmpty(LIST_ENTRY *head){return head->Flink==head;}
#define MediaConnectStateDisconnected 0u
static VOID ExAcquireResourceExclusiveLite(SRWLOCK *lock,BOOLEAN wait){CHECK(wait&&Critical==1);AcquireSRWLockExclusive(lock);}
static VOID ExReleaseResourceLite(SRWLOCK *lock){ReleaseSRWLockExclusive(lock);}
static VOID PbReportOffload(TUN_CTX *ctx){CHECK(ctx==Ctx&&!TransitionHeld);}
static VOID TunIndicateStatus(VOID *adapter,ULONG status){CHECK(adapter==Ctx&&status==MediaConnectStateDisconnected&&!TransitionHeld);}
#include "../../driver/v2/session_unregister.h"
#pragma warning(push)
#pragma warning(disable:4459)
#include PB_SEND_COMPLETION_COMMON_SOURCE
#pragma warning(pop)
static DWORD WINAPI Idle(VOID *opaque)
{
    PB_KERNEL_QUEUE *q=opaque;Await(Session->StartEvent.Native);
    HANDLE events[]={Session->StopEvent.Native,q->KernelEvent->Native};CHECK(WaitForMultipleObjects(2,events,FALSE,5000)<WAIT_OBJECT_0+2);
    if(Session->Faulted)PbV2QuiesceFaultedQueue(q);return 0;
}
static DWORD WINAPI Sending(VOID *nbl){SendId=(LONG)GetCurrentThreadId();CHECK(PbV2Send(Ctx,nbl));return 0;}
static DWORD WINAPI Unregistering(VOID *unused)
{(void)unused;UnregisterId=(LONG)GetCurrentThreadId();TunUnregisterBuffers(Ctx,&Owner);CHECK(!Critical&&!TransitionHeld);return 0;}
static VOID Factory(ULONG queues,BOOLEAN manual)
{
    Ctx=ExAllocatePool2(0,sizeof(*Ctx),0);Session=ExAllocatePool2(0,sizeof(*Session),0);CHECK(Ctx&&Session);
    Session->Ctx=Ctx;Session->Epoch=42;Ctx->V2=Session;Ctx->Running=1;Ctx->NblPool=Ctx->MiniportAdapterHandle=Ctx;
    CHECK(PbCalculateLayout(queues,2,2,&Session->Layout)==PB_OK);Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(Session->Region);Regions=1;
    UserRegion=Session->Region;Session->Lifecycle=(PB_SHARED_LIFECYCLE *)(Session->Region+Session->Layout.LifecycleOffset);
    Session->Mdl=IoAllocateMdl(Session->Region,Session->Layout.RegionSize,FALSE,FALSE,NULL);CHECK(Session->Mdl);Session->PagesLocked=TRUE;Locked=1;
    Session->StartEvent.Native=Event(TRUE,TRUE);Session->StopEvent.Native=Event(TRUE,FALSE);Session->AllReturned.Native=Event(TRUE,TRUE);Session->ProgressEvent=Object(FALSE);
    Ctx->Device.Disconnected.Native=Event(TRUE,FALSE);Ctx->Probe.RunningEvent.Native=Event(TRUE,TRUE);for(ULONG i=0;i<2;++i)Ctx->Probe.IndicationIdle[i].Native=Event(TRUE,TRUE);
    Ctx->Device.OwningFileObject=&Owner;Ctx->Device.OwningProcessId=(HANDLE)1;InitializeListHead(&TunDispatchDeviceList);InsertTailList(&TunDispatchDeviceList,&Ctx->Device.Entry);
    for(ULONG i=0;i<queues;++i){PB_KERNEL_QUEUE *q=Session->Queues+i;q->Session=Session;q->Index=i;q->KernelEvent=Object(manual);q->UserEvent=Object(manual);
        q->RxSlots=ExAllocatePool2(0,2*sizeof(*q->RxSlots),0);q->Pending=ExAllocatePool2(0,2*sizeof(*q->Pending),0);CHECK(q->RxSlots&&q->Pending);
        for(ULONG r=0;r<PB_QUEUE_COUNT;++r)CHECK(PbAttachQueue(q->Rings+r,Session->Region,&Session->Layout,i,r,r==PB_QUEUE_TX_READY||r==PB_QUEUE_RX_COMPLETE)==PB_OK);
        q->Thread=CreateThread(NULL,0,Idle,q,0,NULL);CHECK(q->Thread);InterlockedIncrement(&Threads);InterlockedIncrement(&Handles);}
}
static VOID RunSendCompletion(ULONG queues,BOOLEAN manual,BOOLEAN negative)
{
    CHECK(!Pools&&!Mdls&&!Nbls&&!Objects&&!Threads&&!Regions&&!Handles&&!Locked&&!Originals&&!Ctx&&!Session);
    SendId=UnregisterId=FreeCalls=Unsafe=PauseId=0;Factory(queues,manual);
    HANDLE *gates[]={&LookupEntered,&LookupGate,&CompleteEntered,&CompleteGate,&TailEntered,&TailGate,&PauseAll,&FreeAll,&FreeRundown,&UnsafeEntered,&UnsafeGate,&PauseDone};
    for(ULONG i=0;i<_countof(gates);++i)*gates[i]=Event(TRUE,FALSE);
    PB_SEND_INPUT *input=ExAllocatePool2(0,sizeof(*input),0);CHECK(input);input->Nb.Length=64;input->Nb.Data=input->Bytes;input->Bytes[12]=8;input->Bytes[14]=0x45;input->Bytes[17]=50;
    input->Nb.Mdl=IoAllocateMdl(input->Bytes,64,FALSE,FALSE,NULL);CHECK(input->Nb.Mdl);MmBuildMdlForNonPagedPool(input->Nb.Mdl);
    NET_BUFFER_LIST *nbl=NdisAllocateNetBufferAndNetBufferList(Ctx,0,0,input->Nb.Mdl,0,64);CHECK(nbl);memset(nbl->Info,0,sizeof(nbl->Info));nbl->Flags=NDIS_NBL_FLAGS_IS_IPV4;nbl->Reserved[0]=input;Originals=1;
    AfterRundownAcquire=Retained;BeforeRundownRelease=Releasing;BeforePoolFree=PoolFree;
    HANDLE send=CreateThread(NULL,0,Sending,nbl,0,NULL);CHECK(send);InterlockedIncrement(&Handles);Await(LookupEntered);
    PbV2Fault(Session);CHECK(SetEvent(LookupGate));Await(CompleteEntered);CHECK(!Originals&&Session->Active==1&&Session->Returns.References==1&&Mdls==1&&!Nbls);
    HANDLE pause=CreateThread(NULL,0,Pausing,NULL,0,NULL);CHECK(pause);InterlockedIncrement(&Handles);Await(PauseAll);CHECK(Session->Active==1&&Session->Returns.References==2&&WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT);
    HANDLE unreg=CreateThread(NULL,0,Unregistering,NULL,0,NULL);CHECK(unreg);InterlockedIncrement(&Handles);Await(FreeAll);CHECK(!Ctx->V2&&Session->Active==1&&Session->Returns.References==2&&!FreeCalls);
    CHECK(SetEvent(CompleteGate));Await(TailEntered);Await(PauseDone);Await(pause);Close(pause);CHECK(!Session->Active&&Session->Returns.References==1);
    if(negative){Await(UnsafeEntered);CHECK(Unsafe==1&&FreeCalls==1);}else{Await(FreeRundown);CHECK(!FreeCalls&&!Unsafe);}
    CHECK(Mdls==1&&Objects==(LONG)(2*queues+1)&&Regions==1&&WaitForSingleObject(send,0)==WAIT_TIMEOUT&&WaitForSingleObject(unreg,0)==WAIT_TIMEOUT);
    CHECK(SetEvent(TailGate));Await(send);Close(send);if(negative)CHECK(SetEvent(UnsafeGate));Await(unreg);Close(unreg);
    /* Region is separately owned, just as in the shared DLL mapping contract. */
    CHECK(Pools==1&&!Mdls&&!Nbls&&!Objects&&!Threads&&FreeCalls==(LONG)(2*queues+1)&&IsListEmpty(&TunDispatchDeviceList));
    Session=NULL;AfterRundownAcquire=BeforeRundownRelease=NULL;
    ExFreePoolWithTag(Ctx,0);Ctx=NULL;BeforePoolFree=NULL;
    CHECK(VirtualFree(UserRegion,0,MEM_RELEASE));UserRegion=NULL;Regions=0;
    for(ULONG i=0;i<_countof(gates);++i)Close(*gates[i]);
    CHECK(!Pools&&!Handles&&!Locked&&!Originals&&!Regions&&FixtureRundownAcquires==FixtureRundownReleases);
    CHECK(Unsafe==(LONG)negative);++CompletedScenarios;DetectedScenarios+=(ULONG)Unsafe;
}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"117");CHECK(argc==1||selected);
#ifdef PB_SEND_COMPLETION_WITHOUT_WAIT
    BOOLEAN negative=TRUE;
#else
    BOOLEAN negative=FALSE;
#endif
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)RunSendCompletion(2,TRUE,negative);else {const ULONG queues[]={1,2,8};for(ULONG i=0;i<_countof(queues);++i)for(ULONG manual=0;manual<2;++manual)RunSendCompletion(queues[i],(BOOLEAN)manual,negative);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual Send completion/Pause/FILE unregister/Free schedules: original NBL/NB/MDL freed before callback return; Active and Returns tails independently waited; all threads joined; handles %lu/%lu; unsafe=%lu\n",CompletedScenarios,before,after,DetectedScenarios);
    if(negative){CHECK(DetectedScenarios==CompletedScenarios);puts("SEND_COMPLETION_SKIPPED_RETURNS_WAIT: first resource release parked with send tail pinned; actual Send and Pause joined before native Free resumes; zero owned resources");return 117;}
    CHECK(!DetectedScenarios);return 0;
}
