/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Send/ControlReply/RESOURCES/Pause/common FILE unregister/Free.
 * Ready registration, idle-only workers, NDIS and IRQL are hosted. */
#ifdef PB_API_TESTS
#error Control reply tests require shipping SYS paths
#endif
#ifndef PB_CONTROL_REUSED_HOST
#error Build through Build-ControlReplyTests for explicit source binding
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
static uint32_t RuntimeNdisVersion;
#define PB_NBL_VERSION RuntimeNdisVersion
#include PB_CONTROL_REUSED_HOST
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"control reply requires shipping source fingerprint");
#ifdef PB_NBL_HOST_CURRENT
#error Explicit runtime metadata gate must remain enabled
#endif
typedef struct CONTROL_INPUT {NET_BUFFER Nb;UCHAR Bytes[128];} CONTROL_INPUT;
static CONTROL_INPUT *ControlInput;
static NET_BUFFER_LIST *ControlOriginal;
static HANDLE ResourceEntered,ResourceGate;
static uint32_t ReplyLength;
static ULONG ControlFailure,ControlReplies,ControlRejects,InfoCalls[InfoCount];
static UCHAR ExpectedReply[128];
static LONG BasePools,BaseMdls,BaseNbls;
static LONG CaseRundownStart;
static __declspec(thread) KIRQL ControlIrql;
static MDL *ReplyMdl;
static VOID *ReplyData;
static HANDLE RollbackEntered,RollbackRescue;
static ULONG RollbackObserved,RollbackRepairs,RepairCases;
static MDL *ReplyAllocateMdl(VOID *address,ULONG length,BOOLEAN secondary,BOOLEAN quota,VOID *irp)
{CHECK(!ReplyMdl&&!ReplyData);ReplyData=address;ReplyMdl=IoAllocateMdl(address,length,secondary,quota,irp);return ReplyMdl;}
static VOID ReplyReleaseMdl(MDL *mdl){CHECK(mdl&&mdl==ReplyMdl);ReplyMdl=NULL;IoFreeMdl(mdl);}
static VOID ReplyReleaseData(VOID *data,ULONG tag)
{
    CHECK(data&&data==ReplyData);
    if(ReplyMdl){
        CHECK(ControlFailure==3&&Acquisitions==3&&Nbls==BaseNbls&&!ControlReplies&&Session->Active==1&&Session->Returns.References==1);
        CHECK(!RollbackObserved++&&!RollbackRepairs&&SetEvent(RollbackEntered));Await(RollbackRescue);
        CHECK(!ReplyMdl&&RollbackRepairs==1);
    }
    CHECK(!ReplyMdl);ReplyData=NULL;ExFreePoolWithTag(data,tag);
}
static VOID **CheckedInfo(NET_BUFFER_LIST *nbl,ULONG field)
{
    CHECK(nbl&&field<InfoCount);
    ULONG minor=field==TcpRecvSegCoalesceInfo?(nbl==ControlOriginal?83:30):
        field==UdpRecvSegCoalesceOffloadInfo?89:field==RscTcpTimestampDelta?30:20;
    CHECK(RuntimeNdisVersion>=PB_NDIS_VERSION(minor));++InfoCalls[field];return nbl->Info+field;
}
#undef NET_BUFFER_LIST_INFO
#define NET_BUFFER_LIST_INFO(n,field) (*CheckedInfo(n,field))
#ifndef DISPATCH_LEVEL
#define DISPATCH_LEVEL 2
#endif
#define NDIS_DEFAULT_PORT_NUMBER 0u
#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL 1u
#define NDIS_RECEIVE_FLAGS_RESOURCES 2u
#define NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL 1u
static VOID KeRaiseIrql(KIRQL next,KIRQL *saved)
{CHECK(next==DISPATCH_LEVEL&&ControlIrql<=next&&!TransitionHeld);*saved=ControlIrql;ControlIrql=next;}
static VOID KeLowerIrql(KIRQL saved){CHECK(ControlIrql==DISPATCH_LEVEL);ControlIrql=saved;}
static VOID NdisMIndicateReceiveNetBufferLists(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbl,ULONG port,ULONG count,ULONG flags)
{
    CHECK(adapter==Ctx&&nbl&&nbl!=ControlOriginal&&!port&&count==1&&flags==(NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL|NDIS_RECEIVE_FLAGS_RESOURCES));
    CHECK(ControlIrql==DISPATCH_LEVEL&&!TransitionHeld&&Session->Active==1&&Session->Returns.References==1&&!nbl->Next&&!nbl->Reserved[1]);
    CHECK(nbl->SourceHandle==Ctx&&!nbl->Flags&&!nbl->Status&&nbl->FirstMdl&&!nbl->FirstMdl->Next&&!nbl->FirstMdl->Partial);
    CHECK(nbl->Length==ReplyLength&&nbl->FirstMdl->Length==ReplyLength&&!memcmp(nbl->FirstMdl->Address,ExpectedReply,ReplyLength));
    CHECK(nbl->Info[NetBufferListFrameType]==(VOID *)(ULONG_PTR)(((ULONG)ExpectedReply[13]<<8)|ExpectedReply[12]));
    CHECK(!nbl->Info[TcpIpChecksumNetBufferListInfo]&&!nbl->Info[Ieee8021QNetBufferListInfo]);
    CHECK(nbl->Info[TcpRecvSegCoalesceInfo]==(RuntimeNdisVersion>=PB_NDIS_VERSION(30)?NULL:(VOID *)(ULONG_PTR)~0u));
    CHECK(nbl->Info[RscTcpTimestampDelta]==(RuntimeNdisVersion>=PB_NDIS_VERSION(30)?NULL:(VOID *)(ULONG_PTR)~0u));
    CHECK(nbl->Info[UdpRecvSegCoalesceOffloadInfo]==(RuntimeNdisVersion>=PB_NDIS_VERSION(89)?NULL:(VOID *)(ULONG_PTR)~0u));
    CHECK(Pools==BasePools+1&&Mdls==BaseMdls+1&&Nbls==BaseNbls+1&&++ControlReplies==1);
    CHECK(SetEvent(ResourceEntered));Await(ResourceGate);
    /* RESOURCES owner stays this Send. NDIS does not return this NBL through
     * TunReturnNetBufferLists, and no access occurs after the upcall returns. */
}
static VOID NdisMSendNetBufferListsComplete(NDIS_HANDLE adapter,NET_BUFFER_LIST *nbl,ULONG flags)
{
    CHECK(adapter==Ctx&&nbl==ControlOriginal&&flags==NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL&&ControlIrql==DISPATCH_LEVEL&&!TransitionHeld);
    CHECK(Session->Active==1&&Session->Returns.References==(ControlFailure?1:2)&&nbl->Status==(ControlFailure?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(Pools==BasePools&&Mdls==BaseMdls&&Nbls==BaseNbls&&Acquisitions==(ControlFailure?ControlFailure:3));
    CHECK(!Ctx->Probe.IndicationCount[0]&&!Ctx->Probe.IndicationCount[1]);
    CHECK(!Session->TxObjects&&!Session->RxObjects&&!Session->RxReturned&&!Session->Completions&&!Session->Indications&&!Ctx->Probe.ReturnRundown.References);
    CHECK(ControlRejects==(ControlFailure?1u:0u)&&ControlReplies==(ControlFailure?0u:1u));
    CHECK(ControlInput&&nbl->Reserved[0]==ControlInput&&nbl->FirstMdl==ControlInput->Nb.Mdl);
    IoFreeMdl(ControlInput->Nb.Mdl);NdisFreeNetBufferList(nbl);ExFreePoolWithTag(ControlInput,0);ControlInput=NULL;ControlOriginal=NULL;Originals=0;
    CHECK(SetEvent(CompleteEntered));Await(CompleteGate);
}
#define PbIndicateReceive ActualControlIndicate
#define PbCompleteSend ActualControlComplete
#include PB_CONTROL_CALLOUTS_SOURCE
#undef PbIndicateReceive
#undef PbCompleteSend
#define PbV2ControlReply ActualControlReply
#define PbIndicateReceive ActualControlIndicate
#define IoAllocateMdl ReplyAllocateMdl
#define IoFreeMdl ReplyReleaseMdl
#define ExFreePoolWithTag ReplyReleaseData
#include PB_CONTROL_REPLY_SOURCE
#undef PbV2ControlReply
#undef PbIndicateReceive
#undef IoAllocateMdl
#undef IoFreeMdl
#undef ExFreePoolWithTag
#define NET_BUFFER_CURRENT_MDL(n) ((n)->Mdl)
#define NET_BUFFER_CURRENT_MDL_OFFSET(n) 0u
#define MmGetMdlByteCount(m) ((m)->Length)
#define POOL_NX_OPTIN 1
#define POOL_NX_OPTOUT 0
#define POOL_NX_OPTIN_AUTO 0
#define NormalPagePriority 0u
static VOID *MmGetSystemAddressForMdlSafe(MDL *mdl,ULONG priority){CHECK(!priority&&mdl);return mdl->Address;}
#define PbV2Read ActualControlRead
#include "../../driver/v2/session_read.h"
#undef PbV2Read
static VOID ControlReject(TUN_CTX *ctx,uint64_t epoch,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,NDIS_STATUS status,ULONG prior,ULONG queue,ULONG ethernet,const PB_SEND_FAILURE *failure,ULONG accepted,ULONG attempted,ULONG scope,PB_KERNEL_QUEUE *q,ULONG pending)
{
    CHECK(ctx==Ctx&&epoch==42&&nbl==ControlOriginal&&nb==&ControlInput->Nb&&status==NDIS_STATUS_RESOURCES&&prior==NDIS_STATUS_SUCCESS);
    CHECK(queue==PB_SEND_QUEUE_UNKNOWN&&ethernet==(ControlInput->Bytes[12]==0x86?14u:0u)&&failure->Reason==PB_SEND_CONTROL_REPLY&&!accepted&&!attempted&&scope==2&&!q&&!pending&&++ControlRejects==1);
}

static ULONG ControlCases;
static VOID Matrix(VOID);
static VOID Put16(UCHAR *p,USHORT value){p[0]=(UCHAR)(value>>8);p[1]=(UCHAR)value;}
static ULONG Frame(UCHAR *frame,BOOLEAN ipv6)
{
    memset(frame,0,128);const UCHAR mac[]={2,1,2,3,4,5};memcpy(frame+6,mac,6);UCHAR *ip=frame+14;
    Put16(frame+12,ipv6?0x86dd:0x0806);
    if(!ipv6){Put16(ip,1);Put16(ip+2,0x0800);ip[4]=6;ip[5]=4;Put16(ip+6,1);memcpy(ip+8,mac,6);ip[14]=ip[24]=10;ip[17]=1;ip[27]=2;return 60;}
    ip[0]=0x60;Put16(ip+4,32);ip[6]=58;ip[7]=255;ip[8]=ip[24]=0x20;ip[23]=1;ip[39]=2;
    UCHAR *ns=ip+40;ns[0]=135;ns[8]=0x20;ns[23]=2;ns[24]=ns[25]=1;memcpy(ns+26,mac,6);
    Put16(ns+2,PbChecksumFinish(PbChecksumAdd(PbChecksumAdd(32+58,ip+8,32),ns,32)));return 86;
}
static VOID RunControl(ULONG version,ULONG fail,BOOLEAN ipv6,BOOLEAN manual)
{
    CHECK(!Pools&&!Mdls&&!Nbls&&!Objects&&!Threads&&!Regions&&!Handles&&!Locked&&!Originals&&!Ctx&&!Session);
    RuntimeNdisVersion=PB_NDIS_VERSION(version);ControlFailure=fail;ControlReplies=ControlRejects=RollbackObserved=RollbackRepairs=0;CHECK(!ReplyMdl&&!ReplyData);memset(InfoCalls,0,sizeof(InfoCalls));
    SendId=UnregisterId=FreeCalls=Unsafe=PauseId=0;WaitMask=0;CaseRundownStart=FixtureRundownAcquires;
    Factory(2,manual);Ctx->Probe.PeerMac[0]=2;Ctx->Probe.PeerMac[5]=7;
    HANDLE *gates[]={&LookupEntered,&LookupGate,&CompleteEntered,&CompleteGate,&TailEntered,&TailGate,&PauseAll,&FreeAll,&FreeRundown,&UnsafeEntered,&UnsafeGate,&PauseDone,&BankWait,&ResourceEntered,&ResourceGate,&RollbackEntered,&RollbackRescue};
    for(ULONG i=0;i<_countof(gates);++i)*gates[i]=Event(TRUE,FALSE);
    ControlInput=ExAllocatePool2(0,sizeof(*ControlInput),0);CHECK(ControlInput);
    ControlInput->Nb.Length=Frame(ControlInput->Bytes,ipv6);ControlInput->Nb.Data=ControlInput->Bytes;
    CHECK(PbEthernetControl(ControlInput->Bytes,ControlInput->Nb.Length,Ctx->Probe.PeerMac,ExpectedReply,&ReplyLength)==PB_ETH_REPLY);
    ControlInput->Nb.Mdl=IoAllocateMdl(ControlInput->Bytes,ControlInput->Nb.Length,FALSE,FALSE,NULL);CHECK(ControlInput->Nb.Mdl);MmBuildMdlForNonPagedPool(ControlInput->Nb.Mdl);
    ControlOriginal=NdisAllocateNetBufferAndNetBufferList(Ctx,0,0,ControlInput->Nb.Mdl,0,ControlInput->Nb.Length);CHECK(ControlOriginal);
    memset(ControlOriginal->Info,0,sizeof(ControlOriginal->Info));ControlOriginal->Flags=0;ControlOriginal->Reserved[0]=ControlInput;Originals=1;
    BasePools=Pools;BaseMdls=Mdls;BaseNbls=Nbls;Acquisitions=0;Failure=fail;
    AfterRundownAcquire=Retained;BeforeRundownRelease=Releasing;BeforePoolFree=PoolFree;
    HANDLE send=CreateThread(NULL,0,Sending,ControlOriginal,0,NULL);CHECK(send);InterlockedIncrement(&Handles);Await(LookupEntered);CHECK(SetEvent(LookupGate));
    HANDLE pause=NULL,unreg=NULL;
    if(!fail){
        Await(ResourceEntered);CHECK(Session->Active==1&&Session->Returns.References==1&&Ctx->Probe.IndicationCount[0]==1);
        pause=CreateThread(NULL,0,Pausing,NULL,0,NULL);CHECK(pause);InterlockedIncrement(&Handles);Await(BankWait);
        CHECK(Session->Active==1&&Session->Returns.References==2&&WaitForSingleObject(PauseDone,0)==WAIT_TIMEOUT&&WaitForSingleObject(CompleteEntered,0)==WAIT_TIMEOUT);
        unreg=CreateThread(NULL,0,Unregistering,NULL,0,NULL);CHECK(unreg);InterlockedIncrement(&Handles);Await(FreeAll);
        CHECK(!Ctx->V2&&Pools==BasePools+1&&Mdls==BaseMdls+1&&Nbls==BaseNbls+1&&!FreeCalls&&SetEvent(ResourceGate));
    }else{
#ifdef PB_CONTROL_WITHOUT_FAILED_MDL
        CHECK(fail==3);Await(RollbackEntered);CHECK(ReplyMdl&&ReplyData&&ReplyMdl->Address==ReplyData&&Nbls==BaseNbls&&Pools==BasePools+1&&Mdls==BaseMdls+1);
        ReplyReleaseMdl(ReplyMdl);++RollbackRepairs;CHECK(SetEvent(RollbackRescue));
#endif
        /* The completion gate is reached before Pause obtains its extra pin. */
        Await(CompleteEntered);
        pause=CreateThread(NULL,0,Pausing,NULL,0,NULL);CHECK(pause);InterlockedIncrement(&Handles);Await(PauseAll);
        unreg=CreateThread(NULL,0,Unregistering,NULL,0,NULL);CHECK(unreg);InterlockedIncrement(&Handles);Await(FreeAll);
    }
    Await(CompleteEntered);Await(PauseAll);CHECK(!Originals&&Session->Active==1&&Session->Returns.References==2&&Mdls==1&&!Nbls&&!FreeCalls&&!Unsafe);
    CHECK(SetEvent(CompleteGate));Await(TailEntered);Await(PauseDone);Await(pause);Close(pause);Await(FreeRundown);
    CHECK(!Session->Active&&Session->Returns.References==1&&!FreeCalls&&!Unsafe&&SetEvent(TailGate));
    Await(send);Close(send);Await(unreg);Close(unreg);
    CHECK(Pools==1&&!Mdls&&!Nbls&&!Objects&&!Threads&&FreeCalls==5&&IsListEmpty(&TunDispatchDeviceList));
    Session=NULL;AfterRundownAcquire=BeforeRundownRelease=NULL;ExFreePoolWithTag(Ctx,0);Ctx=NULL;BeforePoolFree=NULL;
    CHECK(VirtualFree(UserRegion,0,MEM_RELEASE));UserRegion=NULL;Regions=0;
    for(ULONG i=0;i<_countof(gates);++i)Close(*gates[i]);Failure=0;
    CHECK(!Pools&&!Handles&&!Locked&&!Originals&&!Regions&&!ReplyMdl&&!ReplyData&&WaitMask==3&&FixtureRundownAcquires==CaseRundownStart+2&&FixtureRundownAcquires==FixtureRundownReleases);
    ++ControlCases;RepairCases+=RollbackRepairs;
}
int main(int argc,char **argv)
{
    BOOLEAN selected=argc==2&&!strcmp(argv[1],"127");CHECK(argc==1||selected);
#ifdef PB_CONTROL_WITHOUT_FAILED_MDL
    CHECK(selected);
#endif
    if(selected){RunControl(20,3,FALSE,FALSE);RunControl(20,3,FALSE,FALSE);}else Matrix();
    ULONG warmed=ControlCases,warmRepairs=RepairCases;ControlCases=RepairCases=0;
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)RunControl(20,3,FALSE,FALSE);else Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu actual ControlReply/Send/RESOURCES/Pause/commonFILEUnregister/Free cases plus %lu checked warmup; explicit runtime NBL gate, actual packet/parser/read/callout paths, complete thread joins and zero owned resources; handles %lu/%lu; repaired=%lu/%lu\n",ControlCases,warmed,before,after,RepairCases,warmRepairs);
#ifdef PB_CONTROL_WITHOUT_FAILED_MDL
    CHECK(RepairCases==ControlCases&&warmRepairs==warmed);
    puts("CONTROL_REPLY_FAILED_NBL_MDL_LEAK: payload release parked with owned MDL and no constructed NBL or NDIS owner; actual IoFreeMdl compensation once per negative case, then complete normal joins and reclamation before127");return 127;
#else
    CHECK(!RepairCases&&!warmRepairs);return 0;
#endif
}
static VOID Matrix(VOID)
{const ULONG versions[]={20,30,83,89};for(ULONG v=0;v<4;++v)for(ULONG f=0;f<4;++f)for(ULONG ip=0;ip<2;++ip)for(ULONG manual=0;manual<2;++manual)RunControl(versions[v],f,(BOOLEAN)ip,(BOOLEAN)manual);}
