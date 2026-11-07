/* SPDX-License-Identifier: GPL-2.0 */
/* Ordinary completion data wakes through actual DLL queue code. Registration,
 * idle-only native workers, NDIS and IRQL remain hosted in the reused ledger. */
#ifdef PB_API_TESTS
#error Shipping completion arm paths required
#endif
#ifndef NDEBUG
#error Shipping completion arm configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../common/v2_core.h"
static ULONG ArmVersion;
#define PB_NBL_VERSION ArmVersion
static PB_RESULT ArmObserved(PB_QUEUE_VIEW *,uint32_t *);
static DWORD WINAPI WaitObserved(DWORD,const HANDLE *,BOOL,DWORD);
#define main UnusedCompletionArmHostMain
#include PB_ARM_HOST
#undef main
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"completion arm requires shipping fingerprint");
static uint64_t BaseCursor;
static ULONG CallbackJoins;
static VOID CallbackJoin(HANDLE h){Join(h);++CallbackJoins;}
#define Join CallbackJoin
#include PB_ARM_OPEN_CLOSE
#undef Join
static HANDLE Submitted,FirstWait,SecondArm,SecondGo;
static volatile LONG OwnerId;
static ULONG Arms,WaitCalls,Cases,PublicJoins,ReturnJoins,FirstReady,SecondReady;
static BOOL BeforeArm;
static PB_TOKEN Tokens[32];
static PB_RESULT ArmObserved(PB_QUEUE_VIEW *view,uint32_t *ready)
{
    CHECK(User&&OwnerId==(LONG)GetCurrentThreadId());
    if(view!=User->Queues[0].Rings+PB_QUEUE_RX_COMPLETE)return PbQueueArm(view,ready);
    CHECK(++Arms<=2);
    if(Arms==2&&BeforeArm){CHECK(SetEvent(SecondArm));Await(SecondGo);}
    PB_RESULT result=PbQueueArm(view,ready);CHECK(result==PB_OK);
    if(Arms==1){FirstReady=*ready;CHECK(!*ready);}
    else {SecondReady=*ready;CHECK(*ready==(ULONG)BeforeArm);if(!BeforeArm){CHECK(SetEvent(SecondArm));Await(SecondGo);}}
    return result;
}
static DWORD WINAPI WaitObserved(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(User&&OwnerId==(LONG)GetCurrentThreadId()&&count==2&&!all&&timeout==INFINITE&&events[0]==User->ClosingEvent&&events[1]==User->Queues[0].UserEvent);
    CHECK(++WaitCalls<3);if(WaitCalls==1)CHECK(SetEvent(FirstWait));
    /* Selected SDK entry is not proof that the OS parked the thread. */
    DWORD result=WaitForMultipleObjects(count,events,all,timeout);CHECK(result==WAIT_OBJECT_0+1);return result;
}
static VOID W16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID W32(UCHAR *p,ULONG n){W16(p,n>>16);W16(p+2,n);}
static ULONG Protocol;
static VOID Packet(UCHAR *p,ULONG sequence)
{
    ULONG ip=PB_TEST_IP_VERSION==4?20u:40u,header=ip+(Protocol==6?20u:8u);memset(p,0,1500);
#if PB_TEST_IP_VERSION == 4
    p[0]=0x45;W16(p+2,1500);W16(p+4,sequence);p[8]=64;p[9]=(UCHAR)Protocol;p[12]=10;p[15]=1;p[16]=10;p[19]=2;
#else
    p[0]=0x60;W16(p+4,1460);p[6]=(UCHAR)Protocol;p[7]=64;p[8]=p[24]=0x20;p[9]=p[25]=1;p[23]=1;p[39]=2;
#endif
    W16(p+ip,1234);W16(p+ip+2,4321);if(Protocol==6){W32(p+ip+4,0xfffffff0u+sequence*(1500-header));p[ip+12]=0x50;p[ip+13]=0x10;W16(p+ip+14,65535);}else W16(p+ip+4,1500-ip);
    memset(p+header,(int)(sequence+0x30),1500-header);W32(p+header,sequence);
    const uint64_t markers[]={UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210)};memcpy(p+header+8,markers,sizeof(markers));
    CHECK(PbFinalizeChecksums(p,1500,PB_TEST_IP_VERSION==4,Protocol==6,Protocol==17)==PB_PACKET_OK);
}
static VOID PollSixteen(PB_USER_QUEUE *q,ULONG first)
{
    PB_DESCRIPTOR items[32];uint32_t count;CHECK(Api.PollCompletions(q,items,32,&count)==PB_OK&&count==16);
    for(ULONG i=0;i<16;++i){ULONG slot=first+i;CHECK(items[i].Status==PB_OK&&!memcmp(&items[i].Token,Tokens+slot,sizeof(PB_TOKEN))&&items[i].Length==1500&&items[i].DataOffset==User->Layout.Queues[0].RxData+slot*PB_BLOCK_SIZE);}
}
static DWORD WINAPI Owner(VOID *unused)
{
    (void)unused;OwnerId=(LONG)GetCurrentThreadId();PB_USER_QUEUE *q;PB_LEASE leases[32];uint32_t count;
    CHECK(Api.OpenQueue(User,0,&q)==PB_OK&&Api.ReserveBatch(q,leases,32,&count)==PB_OK&&count==32);
    for(ULONG i=0;i<32;++i){CHECK(leases[i].Descriptor.Token.Slot==i&&leases[i].Descriptor.Token.Generation==1);Tokens[i]=leases[i].Descriptor.Token;leases[i].Descriptor.Length=1500;Packet(leases[i].Data,i);}
    CHECK(Api.CommitBatch(q,leases,32,&count)==PB_OK&&count==32&&!q->Loans&&q->Published==32&&SetEvent(Submitted));
    CHECK(Api.Wait(q,INFINITE)==PB_OK);PollSixteen(q,0);CHECK(q->Published==16&&!q->Loans);
    CHECK(Api.Wait(q,INFINITE)==PB_OK);PollSixteen(q,16);CHECK(!q->Published&&!q->Loans&&!User->Fatal&&!User->CloseRequested);return 0;
}
static NET_BUFFER_LIST *SplitAndInspect(VOID)
{
    NET_BUFFER_LIST *head=Indicated[0],*tail=NULL,*previous=NULL;ULONG count=0;UCHAR expected[1500];CHECK(head);
    for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(n)&~(ULONG_PTR)1);
        CHECK(count<32&&r->Queue==Native->Queues&&r->Count==1&&!memcmp(&r->Descriptors[0].Token,Tokens+count,sizeof(PB_TOKEN))&&r->IpVersion==PB_TEST_IP_VERSION&&r->Protocol==Protocol);
        CHECK(n->FirstMdl&&n->FirstMdl->Length==14&&n->FirstMdl->Next&&n->FirstMdl->Next->Partial&&n->FirstMdl->Next->Length==1500&&!n->FirstMdl->Next->Next);
        Packet(expected,count);CHECK(n->FirstMdl->Next->Address==Native->Region+r->Descriptors[0].DataOffset&&!memcmp(n->FirstMdl->Next->Address,expected,1500));
        VOID *future=(VOID *)(ULONG_PTR)~0u;CHECK(n->Info[TcpRecvSegCoalesceInfo]==(ArmVersion>=PB_NDIS_VERSION(30)?NULL:future)&&n->Info[RscTcpTimestampDelta]==(ArmVersion>=PB_NDIS_VERSION(30)?NULL:future)&&n->Info[UdpRecvSegCoalesceOffloadInfo]==(ArmVersion>=PB_NDIS_VERSION(89)?NULL:future));
        if(count==15)previous=n;if(count==16)tail=n;++count;
    }
    CHECK(count==32&&previous&&tail);previous->Next=NULL;return tail;
}
static VOID Case(ULONG protocol,ULONG minor,BOOL before,BOOL wrap,BOOL measured)
{
    Protocol=protocol;BeforeArm=before;ArmVersion=PB_NDIS_VERSION(minor);Arms=WaitCalls=FirstReady=SecondReady=0;OwnerId=0;ULONG callbacks=CallbackJoins;
    /* Use the real API's auto-reset UserEvent; no test resets this event. */
    OpenPeer(1,FALSE,wrap);Submitted=Event(TRUE,FALSE);FirstWait=Event(TRUE,FALSE);SecondArm=Event(TRUE,FALSE);SecondGo=Event(TRUE,FALSE);
    HANDLE owner=Start(Owner,NULL);Await(Submitted);Await(FirstWait);CHECK(Arms==1&&WaitCalls==1&&NativeWorkers==1);
    CHECK(PbV2ReceiveBatch(Native->Queues)&&Indicated[0]&&Native->Active==32&&Native->RxObjects==32);NET_BUFFER_LIST *last=SplitAndInspect(),*first=Indicated[0];Indicated[0]=NULL;
    Join(Start(Returning,first));++ReturnJoins;Await(SecondArm);
    CHECK(Native->Active==16&&Native->RxReturned==16&&Native->Completions==16&&User->Queues[0].Published==16&&Arms==2&&WaitCalls==1&&!Native->Queues[0].PendingCount);
    Join(Start(Returning,last));++ReturnJoins;CHECK(!Native->Active&&!Native->Returns.References&&Native->Completions==32);
    CHECK(SetEvent(SecondGo));Join(owner);++PublicJoins;
    CHECK(Arms==2&&!FirstReady&&SecondReady==(ULONG)BeforeArm&&WaitCalls==(BeforeArm?1u:2u)&&!Native->Faulted&&!Native->Draining&&Native->RxReturned==32&&!Native->Queues[0].Credits&&!Native->Queues[0].PendingCount&&!Nbls&&Mdls==1);
    CHECK(!User->Queues[0].Loans&&!User->Queues[0].Published&&User->Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Cursor==BaseCursor+32&&Native->Queues[0].Rings[PB_QUEUE_RX_COMPLETE].Cursor==BaseCursor+32);
    for(ULONG i=0;i<32;++i)CHECK(User->Queues[0].RxSlots[i].State==PB_SLOT_FREE&&User->Queues[0].RxSlots[i].Generation==1&&!Native->Queues[0].RxSlots[i].Active);
    CloseOther(Submitted);CloseOther(FirstWait);CloseOther(SecondArm);CloseOther(SecondGo);FinishPeer();++Cases;
    printf("COMPLETION_ARM32 ip%u protocol%lu ndis%lu beforeArm%u wrap%u measured%u originals32 returns16/16 polls16/16 armReady0/%u sdkWaits%lu publicJoins1 returnJoins2 callbackJoins%lu owners0 repairs0\n",PB_TEST_IP_VERSION,protocol,minor,before,wrap,measured,(unsigned)BeforeArm,WaitCalls,CallbackJoins-callbacks);
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK&&PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    DWORD before=0,after;for(ULONG pass=0;pass<2;++pass){if(pass)CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(ULONG udp=0;udp<2;++udp)for(ULONG latest=0;latest<2;++latest)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG arm=0;arm<2;++arm)Case(udp?17:6,latest?89:20,arm,wrap,pass);
    }
    CHECK(Cases==32&&PublicJoins==32&&ReturnJoins==64&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: COMPLETION_ARM32 family%u cases%lu warm16 measured16 publicJoins%lu returnJoins%lu callbackJoins%lu handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,Cases,PublicJoins,ReturnJoins,CallbackJoins,before,after);return 0;
}
