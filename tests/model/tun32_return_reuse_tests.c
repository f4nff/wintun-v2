/* Actual32 NBL return/reuse with hosted DDIs and real Windows locks/events.
 * The frozen host's NT_ASSERT=CHECK remains stronger than native NDEBUG. */
#ifdef PB_API_TESTS
#error T32 return reuse requires actual production paths
#endif
#ifndef NDEBUG
#error T32 return reuse requires NDEBUG
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error T32 return reuse requires independent IPv4 or IPv6
#endif
#define T32_STRING_INNER(n) #n
#define T32_STRING(n) T32_STRING_INNER(n)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"T32 return reuse requires shipping fingerprint");
static ULONG T32Version=0x60014;
#define PB_NBL_VERSION T32Version
#define main UnusedReturnReclaimMain
#include PB_T32_RETURN_HOST
#undef main
typedef VOID *NDIS_HANDLE;
typedef NET_BUFFER_LIST *PNET_BUFFER_LIST;
typedef VOID MINIPORT_RETURN_NET_BUFFER_LISTS(NDIS_HANDLE,PNET_BUFFER_LIST,ULONG);
#pragma warning(push)
#pragma warning(disable:4100)
#include PB_T32_OUTER_RETURN
#pragma warning(pop)
typedef struct T32_JOB {ULONG Index;NET_BUFFER_LIST *Head;HANDLE RxEntered,RxGate,TailEntered,TailGate;BOOLEAN Gated;} T32_JOB;
static T32_JOB Jobs[4];
static __declspec(thread) LONG Role=-1;
static ULONG Cases,Joins;
static volatile LONG WakeCount,ReferenceSignals;
static ULONG AllWakes,AllSignals,MetadataChecks;
static VOID T32Unlock(KSPIN_LOCK *lock)
{
    if(Role<0||Role>3||lock!=&Current->Queues[0].RxLock||Jobs[Role].Gated)return;
    Jobs[Role].Gated=TRUE;CHECK(SetEvent(Jobs[Role].RxEntered)&&WaitForSingleObject(Jobs[Role].RxGate,5000)==WAIT_OBJECT_0);
}
static VOID T32Tail(EX_RUNDOWN_REF *refs)
{if(Current&&refs==&Current->Returns&&Role==3)CHECK(SetEvent(Jobs[3].TailEntered)&&WaitForSingleObject(Jobs[3].TailGate,5000)==WAIT_OBJECT_0);}
static VOID T32Event(KEVENT *event)
{if(Current&&event==Current->Queues[0].KernelEvent){InterlockedIncrement(&WakeCount);InterlockedIncrement(&KernelNotifies);}if(Current&&event==&Current->AllReturned)InterlockedIncrement(&ReferenceSignals);}
static DWORD WINAPI T32Return(VOID *opaque)
{T32_JOB *job=opaque;Role=(LONG)job->Index;TunReturnNetBufferLists(&Context,job->Head,0);Role=-1;return 0;}
static VOID Join32(HANDLE thread)
{DWORD code;CHECK(thread&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&GetExitCodeThread(thread,&code)&&!code&&CloseHandle(thread));++Joins;}
static VOID W32(UCHAR *p,ULONG value){Put16(p,value>>16);Put16(p+2,value);}
static VOID Packet32(PB_LEASE *lease,ULONG ip,ULONG protocol,ULONG generation)
{
    UCHAR *p=lease->Data;ULONG ipBytes=ip==4?20:40,transport=protocol==6?20:8;
    memset(p,0,1500);if(ip==4){p[0]=0x45;Put16(p+2,1500);p[8]=64;p[9]=(UCHAR)protocol;p[12]=10;p[16]=11;}
    else{p[0]=0x60;Put16(p+4,1460);p[6]=(UCHAR)protocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
    Put16(p+ipBytes,4000);Put16(p+ipBytes+2,5000);
    if(protocol==6){W32(p+ipBytes+4,generation*10000+lease->Descriptor.Token.Slot*1460);p[ipBytes+12]=0x50;p[ipBytes+13]=0x10;Put16(p+ipBytes+14,8192);}else Put16(p+ipBytes+4,1500-ipBytes);
    memset(p+ipBytes+transport,(int)(generation*64+lease->Descriptor.Token.Slot),1500-ipBytes-transport);
    CHECK(PbFinalizeChecksums(p,1500,ip==4,protocol==6,protocol==17)==PB_PACKET_OK);lease->Descriptor.Length=1500;
}
static VOID Validate32(NET_BUFFER_LIST *nbl,ULONG ip,ULONG protocol,ULONG generation)
{
    PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl)&~(ULONG_PTR)1);
    CHECK(record->Count==1&&record->CoalesceIndex==MAXULONG&&record->IpVersion==ip&&record->Protocol==protocol&&record->ReceiveType==PB_DESC_PLAIN&&record->IndicationLength==1500&&record->IpDeclaredLength==1500);
    CHECK(record->Descriptors[0].Token.Generation==generation&&record->Mdl->Partial&&record->Mdl->Length==1500&&record->Mdl->Address==Current->Region+record->Descriptors[0].DataOffset&&nbl->FirstMdl==record->HeaderMdl&&record->HeaderMdl->Next==record->Mdl);
    UCHAR bytes[1500];PB_LEASE expected={.Descriptor=record->Descriptors[0],.Data=bytes};Packet32(&expected,ip,protocol,generation);CHECK(!memcmp(bytes,record->Mdl->Address,sizeof(bytes)));
    CHECK(!nbl->Info[TcpIpChecksumNetBufferListInfo]);
    if(T32Version<PB_NDIS_VERSION(30))CHECK(nbl->Info[TcpRecvSegCoalesceInfo]==(VOID *)(ULONG_PTR)~0u&&nbl->Info[RscTcpTimestampDelta]==(VOID *)(ULONG_PTR)~0u);
    else CHECK(!nbl->Info[TcpRecvSegCoalesceInfo]&&!nbl->Info[RscTcpTimestampDelta]);
    CHECK(nbl->Info[UdpRecvSegCoalesceOffloadInfo]==(T32Version<PB_NDIS_VERSION(89)?(VOID *)(ULONG_PTR)~0u:NULL));++MetadataChecks;
}
static VOID Gather32(NET_BUFFER_LIST *head,NET_BUFFER_LIST **items,ULONG expected)
{
    ULONG n=0;while(head){CHECK(n<expected);NET_BUFFER_LIST *next=head->Next;head->Next=NULL;items[n++]=head;head=next;}
    CHECK(n==expected);
}
static VOID Link32(NET_BUFFER_LIST **head,NET_BUFFER_LIST **tail,NET_BUFFER_LIST *nbl)
{CHECK(nbl&&!nbl->Next);if(*tail)(*tail)->Next=nbl;else *head=nbl;*tail=nbl;}
static VOID CheckSlots(PB_USER_QUEUE *q,ULONG64 reused,BOOLEAN active)
{for(ULONG i=0;i<32;++i){ULONG generation=(reused&(1ull<<i))?2:1;CHECK(Current->Queues[0].RxSlots[i].Generation==generation&&Current->Queues[0].RxSlots[i].Active==active);CHECK(q->RxSlots[i].Generation==generation);}}
static VOID Run32(ULONG ip,ULONG protocol,BOOLEAN manual,BOOLEAN wrap,ULONG minor,ULONG terminal)
{
    CHECK(ip==PB_TEST_IP_VERSION&&(protocol==6||protocol==17)&&terminal<3&&(minor==20||minor==89));
    CHECK(!Current&&!Pools&&!Mdls&&!Nbls&&!Objects&&!LockedRegions);memset(&Context,0,sizeof(Context));
    RundownStarted=RundownBlocked=KernelNotifies=0;FixtureRundownAcquires=FixtureRundownReleases=0;WakeCount=ReferenceSignals=0;
    T32Version=PB_NDIS_VERSION(minor);PB_KERNEL_QUEUE *native=CreateSession(32,manual,wrap);PB_SESSION *user=CreateUser(32,wrap?UINT64_MAX-15:0);PB_USER_QUEUE *q=user->Queues;
    Context.Probe.V2SessionActive=1;Context.Probe.LocalMac[0]=Context.Probe.PeerMac[0]=2;
    ReceiveMask=0;PB_LEASE leases[32];uint32_t count,accepted;
    CHECK(ReserveBatch(q,leases,32,&count)==PB_OK&&count==32);
    for(ULONG i=0;i<32;++i){CHECK(leases[i].Descriptor.Token.Generation==1);Packet32(leases+i,ip,protocol,1);}
    CHECK(CommitBatch(q,leases,32,&accepted)==PB_OK&&accepted==32&&PbV2ReceiveBatch(native));
    NET_BUFFER_LIST *old[32];Gather32(Indicated,old,32);Indicated=NULL;
    for(ULONG i=0;i<32;++i)Validate32(old[i],ip,protocol,1);
    CHECK(Current->Active==32&&native->Credits==32&&q->Published==32&&!q->Loans);
    NET_BUFFER_LIST *tails[4]={0};ULONG64 returnedSet=0;
    for(ULONG j=0;j<4;++j){Jobs[j]=(T32_JOB){.Index=j,.RxEntered=Event(TRUE,FALSE),.RxGate=Event(TRUE,FALSE),.TailEntered=Event(TRUE,FALSE),.TailGate=Event(TRUE,FALSE)};}
    for(ULONG i=0;i<32;++i){ULONG at=(i*7)&31,j=i/8;Link32(&Jobs[j].Head,tails+j,old[at]);if(j<3)returnedSet|=1ull<<at;}
    AfterQueueUnlock=T32Unlock;BeforeRundownRelease=T32Tail;OnSetEvent=T32Event;
    HANDLE callbacks[4];for(ULONG j=0;j<3;++j){callbacks[j]=CreateThread(NULL,0,T32Return,Jobs+j,0,NULL);CHECK(callbacks[j]);}
    for(ULONG j=0;j<3;++j)CHECK(WaitForSingleObject(Jobs[j].RxEntered,5000)==WAIT_OBJECT_0);
    CHECK(Current->Active==32&&Current->Returns.References==3&&Context.Probe.ReturnRundown.References==3&&native->Credits==8&&native->PendingCount==0&&Nbls==8);
    PB_DESCRIPTOR completed[32];CHECK(PollCompletions(q,completed,32,&count)==PB_OK&&count==24&&q->Published==8);ULONG64 observed=0;
    for(ULONG i=0;i<count;++i){CHECK(completed[i].Status==PB_OK&&completed[i].Token.Generation==1);ULONG64 bit=1ull<<completed[i].Token.Slot;CHECK(returnedSet&bit&&!(observed&bit));observed|=bit;}CHECK(observed==returnedSet);
    CHECK(ReserveBatch(q,leases,32,&count)==PB_OK&&count==24&&q->Loans==24&&q->Published==8);
    ULONG64 reused=0;for(ULONG i=0;i<count;++i){CHECK(leases[i].Descriptor.Token.Generation==2);reused|=1ull<<leases[i].Descriptor.Token.Slot;Packet32(leases+i,ip,protocol,2);}CHECK(reused==returnedSet);
    PB_LEASE one;CHECK(ReserveBatch(q,&one,1,&accepted)==PB_FULL&&!accepted&&q->Loans==24&&q->Published==8);
    CHECK(CommitBatch(q,leases,24,&accepted)==PB_OK&&accepted==24&&!q->Loans&&q->Published==32&&PbV2ReceiveBatch(native));
    NET_BUFFER_LIST *fresh[24];Gather32(Indicated,fresh,24);Indicated=NULL;CheckSlots(q,reused,TRUE);
    for(ULONG i=0;i<24;++i)Validate32(fresh[i],ip,protocol,2);
    CHECK(Current->Active==56&&native->Credits==32&&Nbls==32&&Current->RxObjects==56&&ReferenceSignals==0);
    KIRQL irql=ExAcquireSpinLockExclusive(&Context.TransitionLock);PbV2BeginDrain(Current,GetTickCount64()+30000);ExReleaseSpinLockExclusive(&Context.TransitionLock,irql);user->Draining=1;
    PB_V2_STATUS status;CHECK(StatusInternal(user,&status)==PB_OK&&status.State==PB_SESSION_DRAINING&&status.ActiveReferences==56);
    for(ULONG j=0;j<3;++j){CHECK(SetEvent(Jobs[j].RxGate));Join32(callbacks[j]);}
    CHECK(Current->Active==32&&!Current->Returns.References&&Current->RxReturned==24&&ReferenceSignals==0);
    NET_BUFFER_LIST *held[8];Gather32(Jobs[3].Head,held,8);Jobs[3].Head=NULL;NET_BUFFER_LIST *tail=NULL;PB_TOKEN finalTokens[32];ULONG at=0;
    for(ULONG i=0;i<8;++i)Validate32(held[i],ip,protocol,1);
    for(ULONG i=0;i<8;++i){NET_BUFFER_LIST *parts[4]={fresh[23-i*3],held[7-i],fresh[22-i*3],fresh[21-i*3]};for(ULONG j=0;j<4;++j){PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(parts[j])&~(ULONG_PTR)1);finalTokens[at++]=record->Descriptors[0].Token;Link32(&Jobs[3].Head,&tail,parts[j]);}}
    CHECK(at==32);callbacks[3]=CreateThread(NULL,0,T32Return,Jobs+3,0,NULL);CHECK(callbacks[3]&&WaitForSingleObject(Jobs[3].RxEntered,5000)==WAIT_OBJECT_0);
    CHECK(Current->Active==32&&Current->Returns.References==1&&native->Credits==0&&native->PendingCount==0&&!Nbls&&Mdls==1&&ReferenceSignals==0);
    CHECK(PollCompletions(q,completed,32,&count)==PB_OK&&count==32&&!q->Published&&!q->Loans);
    for(ULONG i=0;i<32;++i)CHECK(!memcmp(&completed[i].Token,finalTokens+i,sizeof(PB_TOKEN))&&completed[i].Status==PB_OK);
    CheckSlots(q,reused,FALSE);for(ULONG i=0;i<32;++i)CHECK(q->RxSlots[i].State==PB_SLOT_FREE);
    if(terminal){PbV2Fault(Current);if(terminal==1)PbV2QuiesceFaultedQueue(native);CHECK(StatusInternal(user,&status)==PB_OK&&status.State!=PB_SESSION_STOPPED&&status.ActiveReferences==32);}
    CHECK(SetEvent(Jobs[3].RxGate)&&WaitForSingleObject(Jobs[3].TailEntered,5000)==WAIT_OBJECT_0);
    CHECK(!Current->Active&&Current->Returns.References==1&&Current->RxReturned==56&&Current->Completions==56&&ReferenceSignals==1&&!Nbls&&Mdls==1);
    CHECK(Context.Statistics.ifHCInUcastPkts==56&&Context.Statistics.ifHCInOctets==84000&&Context.Statistics.ifHCInUcastOctets==84000&&!Context.Statistics.ifInErrors&&!Context.Statistics.ifInDiscards);
    if(terminal==2){CHECK(StatusInternal(user,&status)==PB_OK&&status.State!=PB_SESSION_STOPPED&&!status.ActiveReferences);PbV2QuiesceFaultedQueue(native);}
    CHECK(StatusInternal(user,&status)==PB_OK&&status.State==PB_SESSION_STOPPED&&!status.ActiveReferences&&WaitForSingleObject(Current->AllReturned.Native,0)==WAIT_OBJECT_0);
    CHECK(Current->Queues[0].Rings[PB_QUEUE_RX_SUBMIT].Cursor==(wrap?UINT64_MAX-15:0)+56&&Current->Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Cursor==(wrap?UINT64_MAX-15:0)+56);
    FreeUser(user);AfterQueueUnlock=NULL;AllWaitEntered=Event(TRUE,FALSE);RundownWaitEntered=Event(TRUE,FALSE);CleanupDone=Event(TRUE,FALSE);ExpectedKernelNotifies=KernelNotifies;BeforePoolFree=Freed;
    HANDLE cleanup=DetachAndCleanup();CHECK(WaitForSingleObject(RundownWaitEntered,5000)==WAIT_OBJECT_0&&RundownBlocked&&Current&&LockedRegions==1&&WaitForSingleObject(CleanupDone,0)==WAIT_TIMEOUT);
    CHECK(SetEvent(Jobs[3].TailGate));Join32(callbacks[3]);Join32(cleanup);CHECK(!Current&&!Pools&&!Mdls&&!Nbls&&!Objects&&!LockedRegions&&!Context.Probe.ReturnRundown.References&&FixtureRundownAcquires==8&&FixtureRundownReleases==8);
    for(ULONG j=0;j<4;++j)CHECK(CloseHandle(Jobs[j].RxEntered)&&CloseHandle(Jobs[j].RxGate)&&CloseHandle(Jobs[j].TailEntered)&&CloseHandle(Jobs[j].TailGate));
    CHECK(CloseHandle(AllWaitEntered)&&CloseHandle(RundownWaitEntered)&&CloseHandle(CleanupDone));BeforePoolFree=NULL;BeforeRundownRelease=NULL;OnSetEvent=NULL;
    CHECK(WakeCount==(terminal?3:2));AllWakes+=(ULONG)WakeCount;AllSignals+=(ULONG)ReferenceSignals;
    ++Cases;printf("RETURN32 ip%lu proto%lu ndis%lu manual%u wrap%u terminal%lu old32 prefix24 held8 reused24 total56 callbacks4 joins5 naturalFull1 repairs0 wakes%ld owners0\n",ip,protocol,minor,manual,wrap,terminal,WakeCount);
}
int main(VOID)
{
    DWORD before=0,after=0;puts("FAMILY ip" T32_STRING(PB_TEST_IP_VERSION));
    for(ULONG round=0;round<2;++round){
        if(round)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)
            for(ULONG manual=0;manual<2;++manual)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG terminal=0;terminal<3;++terminal)
                Run32(PB_TEST_IP_VERSION,protocol,(BOOLEAN)manual,(BOOLEAN)wrap,minor,terminal);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&Cases==96&&Joins==480&&AllSignals==96&&AllWakes==256&&MetadataChecks==6144);
    printf("RETURN32_SUMMARY ip%u cases%lu warm48 measured48 joins%lu callbacks384 naturalFull96 tokens5376 distinctNbls5376 metadataChecks%lu signals%lu wakes%lu handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,Cases,Joins,MetadataChecks,AllSignals,AllWakes,before,after);
    puts("PASS: actual32 disordered returns naturally reuse24 generation2 slots while8 original owners remain; drain/fault witnesses and callback tails preserve resources; all56 tokens retire through actual DLL polling and every OS thread joins");return 0;
}
