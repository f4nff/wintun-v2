/* SPDX-License-Identifier: GPL-2.0 */
/* Shipping DLL queue/lifecycle and actual SYS receive/return share the mapping.
 * Registration/NDIS/IRQL are hosted; a controlled NBL status is not evidence
 * that a Windows protocol stack naturally writes that receive-return status. */
#ifdef PB_API_TESTS
#error Rejected reuse requires real shipping API paths
#endif
#ifndef NDEBUG
#error Rejected reuse requires NDEBUG
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select independent IPv4 or IPv6
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static ULONG ReuseVersion;
#define PB_NBL_VERSION ReuseVersion
static DWORD WINAPI ReuseWait(DWORD,const HANDLE *,BOOL,DWORD);
#define main ReusedNativeMain
#include PB_REUSE_HOST
#undef main
#include PB_REUSE_RESOURCE_STATUS
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Shipping fingerprint required");
static uint64_t BaseCursor;
static ULONG CallbackJoins;
static VOID CallbackJoin(HANDLE thread){Join(thread);++CallbackJoins;}
#define Join CallbackJoin
#include PB_REUSE_OPEN_CLOSE
#undef Join
static HANDLE WaitEntered[2],Recovered[2],OwnerGo;
static ULONG Ip,Protocol,FailedSlot,WaitCalls[2],Gen2[2],Cases,PublicJoins,ReturnJoins;
static volatile LONG PartialFailures;
static PB_RESULT PartialResult;
static VOID W16(UCHAR *p,ULONG v){p[0]=(UCHAR)(v>>8);p[1]=(UCHAR)v;}
static VOID W32(UCHAR *p,ULONG v){p[0]=(UCHAR)(v>>24);p[1]=(UCHAR)(v>>16);p[2]=(UCHAR)(v>>8);p[3]=(UCHAR)v;}
static VOID Packet(UCHAR *p,ULONG queue,ULONG sequence)
{
    ULONG offset=Ip==4?20:40,header=offset+(Protocol==6?20:8);memset(p,0,1500);
    if(Ip==4){p[0]=0x45;W16(p+2,1500);p[8]=64;p[9]=(UCHAR)Protocol;p[12]=192;p[14]=2;p[15]=(UCHAR)(1+queue);p[16]=198;p[17]=51;p[18]=100;p[19]=2;}
    else{p[0]=0x60;W16(p+4,1460);p[6]=(UCHAR)Protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[10]=p[26]=0x0d;p[11]=p[27]=0xb8;p[23]=(UCHAR)(1+queue);p[39]=2;}
    W16(p+offset,10000+queue);W16(p+offset+2,20000+queue);
    if(Protocol==6){W32(p+offset+4,1000+sequence*1440);p[offset+12]=0x50;p[offset+13]=0x10;W16(p+offset+14,32768);}else W16(p+offset+4,1500-offset);
    memset(p+header,(int)(0x30+sequence),1500-header);p[header]=(UCHAR)queue;W32(p+header+4,sequence);
    const uint64_t markers[]={UINT64_C(0x1122334455667788),UINT64_C(0xfedcba9876543210)};memcpy(p+header+8,markers,sizeof(markers));
    CHECK(PbFinalizeChecksums(p,1500,Ip==4,Protocol==6,Protocol==17)==PB_PACKET_OK);
}
static DWORD WINAPI ReuseWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(User&&count==2&&!all&&timeout==INFINITE&&events[0]==User->ClosingEvent);
    ULONG queue;for(queue=0;queue<2&&events[1]!=User->Queues[queue].UserEvent;++queue){}
    CHECK(queue<2&&User->Queues[queue].Owner==(LONG)GetCurrentThreadId()&&!WaitCalls[queue]);++WaitCalls[queue];
    CHECK(SetEvent(WaitEntered[queue]));return WaitForMultipleObjects(count,events,all,timeout);
}
static DWORD WINAPI Owner(VOID *opaque)
{
    ULONG index=(ULONG)(ULONG_PTR)opaque;PB_USER_QUEUE *q;PB_LEASE leases[32];PB_DESCRIPTOR completed[32];PB_TOKEN tokens[32];uint32_t count;
    CHECK(Api.OpenQueue(User,index,&q)==PB_OK&&Api.ReserveBatch(q,leases,32,&count)==PB_OK&&count==32);
    for(ULONG i=0;i<32;++i){leases[i].Descriptor.Length=1500;Packet(leases[i].Data,index,i);}
    CHECK(Api.CommitBatch(q,leases,32,&count)==PB_OK&&count==32);
    CHECK(Api.ReserveBatch(q,leases,32,&count)==PB_FULL&&!count);
    CHECK(Api.Wait(q,INFINITE)==PB_OK);
    ULONG expected=index?32:1;CHECK(Api.PollCompletions(q,completed,32,&count)==PB_OK&&count==expected);
    for(ULONG i=0;i<count;++i)CHECK(completed[i].Status==(ULONG)(index?(i?PB_OK:PB_RX_REJECTED):PB_FULL)&&completed[i].Token.Generation==1&&completed[i].Token.Queue==index&&completed[i].Token.Slot==(index?i:FailedSlot));
    PB_RESULT reserved=Api.ReserveBatch(q,leases,32,&count);CHECK(count==expected);Gen2[index]=count;
    if(!index){
        PartialResult=reserved;
#ifdef PB_REJECTED_WITHOUT_PARTIAL_SUCCESS
        CHECK(reserved==PB_FULL);InterlockedIncrement(&PartialFailures);
#else
        CHECK(reserved==PB_OK);
#endif
    }else CHECK(reserved==PB_OK);
    for(ULONG i=0;i<count;++i){CHECK(leases[i].Descriptor.Token.Generation==2&&leases[i].Descriptor.Token.Slot==(index?i:FailedSlot));tokens[i]=leases[i].Descriptor.Token;leases[i].Descriptor.Length=1500;}
    CHECK(SetEvent(Recovered[index]));Await(OwnerGo);
    CHECK(Api.CommitBatch(q,leases,count,&count)==PB_STOPPED&&!count);
    CHECK(Api.CancelBatch(q,tokens,expected)==PB_OK&&Api.Wait(q,0)==PB_STOPPED);return 0;
}
static VOID Inspect(ULONG index)
{
    UCHAR seen[32]={0},expected[1500];ULONG count=0;
    for(NET_BUFFER_LIST *n=Indicated[index];n;n=n->Next){
        PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);
        CHECK(r->Queue==Native->Queues+index&&r->Count==1&&r->Descriptors[0].Token.Queue==index&&r->Descriptors[0].Token.Generation==1);
        ULONG slot=r->Descriptors[0].Token.Slot;CHECK(slot<32&&!seen[slot]++);Packet(expected,index,slot);
        CHECK(n->FirstMdl&&n->FirstMdl->Length==14&&n->FirstMdl->Next&&n->FirstMdl->Next->Partial&&n->FirstMdl->Next->Length==1500&&!n->FirstMdl->Next->Next);
        CHECK(n->FirstMdl->Next->Address==Native->Region+r->Descriptors[0].DataOffset&&!memcmp(expected,n->FirstMdl->Next->Address,1500));
        VOID *future=(VOID *)(ULONG_PTR)~0u;
        CHECK(n->Info[TcpRecvSegCoalesceInfo]==(ReuseVersion>=PB_NDIS_VERSION(30)?NULL:future)&&n->Info[UdpRecvSegCoalesceOffloadInfo]==(ReuseVersion>=PB_NDIS_VERSION(89)?NULL:future)&&n->Info[RscTcpTimestampDelta]==(ReuseVersion>=PB_NDIS_VERSION(30)?NULL:future));
        ++count;
    }
    CHECK(count==(index?32u:31u));for(ULONG i=0;i<32;++i)CHECK(seen[i]==(index||i!=FailedSlot));
}
static VOID Case(ULONG protocol,ULONG failed,BOOL wrap,ULONG ndis,const char *phase)
{
    Ip=PB_TEST_IP_VERSION;Protocol=protocol;FailedSlot=failed;ReuseVersion=ndis;memset(WaitCalls,0,sizeof(WaitCalls));memset(Gen2,0,sizeof(Gen2));ULONG callbacks=CallbackJoins;
    OpenPeer(2,FALSE,wrap);OwnerGo=Event(TRUE,FALSE);HANDLE owners[2];
    for(ULONG i=0;i<2;++i){WaitEntered[i]=Event(TRUE,FALSE);Recovered[i]=Event(TRUE,FALSE);owners[i]=Start(Owner,(VOID *)(ULONG_PTR)i);}
    CHECK(WaitForMultipleObjects(2,WaitEntered,TRUE,5000)==WAIT_OBJECT_0);
    Failure=4*(failed+1);Acquisitions=0;CHECK(PbV2ReceiveBatch(Native->Queues));Failure=0;
    CHECK(PbV2ReceiveBatch(Native->Queues+1));Inspect(0);Inspect(1);
    NET_BUFFER_LIST *head=Indicated[1];Indicated[1]=NULL;
    /* Defensive hosted input on an owned NBL, not a claim about natural
     * protocol-stack receive-return statuses. Actual Return maps it to4. */
    NET_BUFFER_LIST_STATUS(head)=NDIS_STATUS_RESOURCES;Join(Start(Returning,head));++ReturnJoins;
    CHECK(WaitForMultipleObjects(2,Recovered,TRUE,5000)==WAIT_OBJECT_0&&Gen2[0]==1&&Gen2[1]==32&&Native->Active==31&&!User->Fatal&&!User->CloseRequested);
    CHECK(User->Queues[0].Loans==1&&User->Queues[0].Published==31&&User->Queues[1].Loans==32&&!User->Queues[1].Published);
    PB_V2_STATUS status;CHECK(Api.GetStatus(User,&status)==PB_OK&&status.State==PB_SESSION_RUNNING&&status.ActiveReferences==31);
    CHECK(Life.BeginStop(User)==PB_OK);Await(ParkEntered);CHECK(Api.PrepareClose(User,0)==PB_BUSY);
    CHECK(SetEvent(OwnerGo));Join(owners[0]);Join(owners[1]);PublicJoins+=2;CHECK(!Loans(User)&&Native->Active==31);
    for(ULONG i=0;i<2;++i){CloseOther(WaitEntered[i]);CloseOther(Recovered[i]);}CloseOther(OwnerGo);
    CHECK(SetEvent(ParkGate));Await(ParkEntered);CHECK(LastQueryState!=PB_SESSION_STOPPED&&LastQueryReferences==31&&!CloseCalls&&Regions==1);
    head=Indicated[0];Indicated[0]=NULL;Join(Start(Returning,head));++ReturnJoins;CHECK(!Native->Active&&!Native->Returns.References);
    CHECK(SetEvent(ParkGate)&&Api.PrepareClose(User,5000)==PB_OK);FinishPeer();
    CHECK(!OtherHandles&&!Pools&&!Mdls&&!Nbls&&!Regions&&!DllAllocations);
    printf("REUSE32 phase=%s ip%u protocol%lu ndis%lx failed%lu wrap%u poll=1/32 reserve=1/32 partialResult=%u gen=2 wait=1/1 statuses=2/4 nonfatal cancel=1/32 publicJoins=2 returnJoins=2 finalReturn=31 callbackJoins=%lu owners=0\n",phase,PB_TEST_IP_VERSION,protocol,ndis,failed,wrap,(unsigned)PartialResult,CallbackJoins-callbacks);++Cases;
}
static VOID Matrix(const char *phase)
{const ULONG failed[]={0,6,30},versions[]={PB_NDIS_VERSION(20),PB_NDIS_VERSION(89)};for(ULONG version=0;version<2;++version)for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG i=0;i<3;++i)for(ULONG wrap=0;wrap<2;++wrap)Case(protocol,failed[i],(BOOL)wrap,versions[version],phase);}
static VOID Selected(const char *phase)
{Case(6,6,FALSE,PB_NDIS_VERSION(20),phase);Case(17,30,TRUE,PB_NDIS_VERSION(89),phase);}
int main(int argc,char **argv)
{
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"142")));BOOL control=argc==2;
    printf("FAMILY: IPv%u only\n",PB_TEST_IP_VERSION);
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK&&PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    if(control)Selected("warm");else Matrix("warm");ULONG warm=Cases;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(control)Selected("measure");else Matrix("measure");CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&Cases==2*warm&&PublicJoins==2*Cases&&ReturnJoins==2*Cases);
#ifdef PB_REJECTED_WITHOUT_PARTIAL_SUCCESS
    CHECK(control&&PartialFailures==(LONG)Cases);
#else
    CHECK(!PartialFailures);
#endif
    printf("PASS: REUSE32 ip%u cases%lu warm%lu measured%lu partialFailures%ld publicJoins%lu returnJoins%lu callbackJoins%lu handles%lu/%lu owners=0\n",PB_TEST_IP_VERSION,Cases,warm,warm,PartialFailures,PublicJoins,ReturnJoins,CallbackJoins,before,after);
    if(PartialFailures){puts("PARTIAL_RESERVE_FAILURE_WITH_OWNED_PREFIX: actual PB_FULL/count1 preserved, same owner cancelled real loan after stop; all owners callbacks native workers and resources joined/reclaimed");return 142;}return 0;
}
