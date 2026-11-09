/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Receive/Return/Flush/Fault/Query with a complete hosted DDI fixture.
 * A host gate selects another CPU's interleaving; it does not authorize native
 * spinlock/IRQL code to sleep. Both real worker callers start already Faulted,
 * publish their own private witnesses and exit without packet/wait-loop work.
 * Final cleanup is the original ready-factory Destroy, not native Free/Halt. */
#ifdef PB_API_TESTS
#error Shipping cross queue fault paths required
#endif
#ifndef NDEBUG
#error Shipping cross queue fault configuration required
#endif
#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6
#error Select one IP family
#endif
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"cross queue fault requires shipping fingerprint");
static unsigned long CqVersion;
#define PB_NBL_VERSION CqVersion
#define main UnusedCrossQueueDrainMain
#include PB_CQ_HOST
#undef main
enum { CQ_RETURN0=1,CQ_RETURN1,CQ_QUERY,CQ_WORKER };
static __declspec(thread) ULONG CqRole;
static FIXTURE *CqFixture;
static HANDLE CqPublished,CqRelease,CqQueryBlocked,CqQueryDone;
static KEVENT CqProgress;
static BOOLEAN CqPublicationFirst;
static uint64_t CqBase;
static ULONG CqCases,CqJoins,CqWorkerJoins,CqHandles,CqProtocol;
static volatile LONG CqPushes,CqBadPushes,CqQueryChecks,CqKernel[2],CqUser[2],CqProgressSignals;
static PB_V2_STATUS CqStatus;
typedef struct CQ_THREAD {HANDLE Handle;DWORD Id;ULONG Role;} CQ_THREAD;
static VOID CqAwait(HANDLE h){CHECK(WaitForSingleObject(h,10000)==WAIT_OBJECT_0);}
static HANDLE CqEvent(VOID){HANDLE h=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(h);++CqHandles;return h;}
static VOID CqClose(HANDLE h){CHECK(CqHandles&&CloseHandle(h));--CqHandles;}
static PB_RESULT CqPush(PB_QUEUE_VIEW *v,const PB_DESCRIPTOR *items,uint32_t capacity,uint32_t head,uint32_t requested,uint32_t *accepted,uint32_t *notify)
{
 PB_RESULT r=PbQueuePushRing(v,items,capacity,head,requested,accepted,notify);
 if(!CqFixture)return r;
 PB_KERNEL_QUEUE *q=CqFixture->S.Queues;
 if(v==q[0].Rings+PB_QUEUE_RX_COMPLETE){
  CHECK(CqRole==CQ_RETURN0&&CqPublicationFirst&&r==PB_OK&&requested==16&&*accepted==16&&!*notify);
  CHECK(q[0].PendingCount==16&&q[0].Credits==16&&v->Cursor==CqBase+16&&v->Shared->Producer==CqBase+16);
  CHECK(!TryAcquireSRWLockExclusive((SRWLOCK *)&q[0].RxLock));
  InterlockedIncrement(&CqPushes);CHECK(SetEvent(CqPublished));CqAwait(CqRelease);
 }else if(v==q[1].Rings+PB_QUEUE_RX_COMPLETE){CHECK(CqRole==CQ_RETURN1&&r==PB_CORRUPT&&!*accepted);InterlockedIncrement(&CqBadPushes);}
 else CHECK(FALSE);
 return r;
}
static VOID CqQueryAcquire(KSPIN_LOCK *lock,KLOCK_QUEUE_HANDLE *handle)
{
 if(CqRole==CQ_QUERY&&CqPublicationFirst&&lock==&CqFixture->S.Queues[0].RxLock){
  BOOLEAN got=TryAcquireSRWLockExclusive((SRWLOCK *)lock);if(got)ReleaseSRWLockExclusive((SRWLOCK *)lock);
  CHECK(!got);InterlockedIncrement(&CqQueryChecks);CHECK(SetEvent(CqQueryBlocked));
 }
 KeAcquireInStackQueuedSpinLock(lock,handle);
}
static VOID CqSignals(KEVENT *e)
{if(e==&CqProgress)InterlockedIncrement(&CqProgressSignals);for(ULONG i=0;i<2;++i){if(e==CqFixture->S.Queues[i].KernelEvent)InterlockedIncrement(CqKernel+i);if(e==CqFixture->S.Queues[i].UserEvent)InterlockedIncrement(CqUser+i);}}
typedef struct CQ_RETURN_JOB {NET_BUFFER_LIST *Head;ULONG Queue;} CQ_RETURN_JOB;
static DWORD WINAPI CqReturn(VOID *p){CQ_RETURN_JOB *job=p;CqRole=job->Queue?CQ_RETURN1:CQ_RETURN0;NET_BUFFER_LIST *head=job->Head;CHECK(PbV2ReturnNbls(&CqFixture->Ctx,&head,PB_MAX_BATCH)&&!head);return 0;}
static DWORD WINAPI CqQuery(VOID *p){(void)p;CqRole=CQ_QUERY;CHECK(PbV2Query(&CqFixture->Ctx,&CqStatus)==STATUS_SUCCESS&&CqStatus.State!=PB_SESSION_STOPPED&&SetEvent(CqQueryDone));return 0;}
static DWORD WINAPI CqWorker(VOID *q){CqRole=CQ_WORKER;PbV2Worker(q);return 0;}
static CQ_THREAD CqStart(LPTHREAD_START_ROUTINE fn,VOID *arg,ULONG role)
{CQ_THREAD t={0};t.Role=role;t.Handle=CreateThread(NULL,0,fn,arg,0,&t.Id);CHECK(t.Handle&&t.Id);++CqHandles;return t;}
static VOID CqJoin(CQ_THREAD t){DWORD code;CqAwait(t.Handle);CHECK(GetExitCodeThread(t.Handle,&code)&&!code&&GetThreadId(t.Handle)==t.Id);CqClose(t.Handle);++CqJoins;if(t.Role==CQ_WORKER)++CqWorkerJoins;}
static VOID CqW16(UCHAR *p,ULONG n){p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID CqPacket(UCHAR *p,ULONG index)
{
 ULONG family=PB_TEST_IP_VERSION,ip=family==4?20:40,header=ip+(CqProtocol==6?20:8);memset(p,0,1500);
 if(family==4){p[0]=0x45;CqW16(p+2,1500);p[8]=64;p[9]=(UCHAR)CqProtocol;p[12]=10;p[16]=11;}
 else{p[0]=0x60;CqW16(p+4,1460);p[6]=(UCHAR)CqProtocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
 CqW16(p+ip,4000+index);CqW16(p+ip+2,5000);if(CqProtocol==6){p[ip+12]=0x50;p[ip+13]=0x10;CqW16(p+ip+14,8192);}else CqW16(p+ip+4,1500-ip);
 memset(p+header,(int)(index+1),1500-header);CHECK(PbFinalizeChecksums(p,1500,PB_TEST_IP_VERSION==4,CqProtocol==6,CqProtocol==17)==PB_PACKET_OK);
}
static NET_BUFFER_LIST *CqFill(ULONG queue)
{
 FIXTURE *f=CqFixture;PB_KERNEL_QUEUE *q=f->S.Queues+queue;PB_QUEUE_VIEW producer;
 CHECK(PbAttachQueue(&producer,f->S.Region,&f->S.Layout,queue,PB_QUEUE_RX_SUBMIT,TRUE)==PB_OK);producer.Cursor=producer.PeerCursor=CqBase;
 PB_DESCRIPTOR items[16];for(ULONG i=0;i<16;++i){items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,(USHORT)queue},.Length=1500,.DataOffset=f->S.Layout.Queues[queue].RxData+i*PB_BLOCK_SIZE};CqPacket(f->S.Region+items[i].DataOffset,queue*16+i);}
 uint32_t count,notify;CHECK(PbQueuePush(&producer,items,16,&count,&notify)==PB_OK&&count==16&&!notify);
 CHECK(ActualReceiveBatch(q)&&Indicated);NET_BUFFER_LIST *head=Indicated;Indicated=NULL;ULONG seen=0;
 for(NET_BUFFER_LIST *n=head;n;n=n->Next){PB_RX_RECORD *r=(PB_RX_RECORD *)((ULONG_PTR)n->Reserved[1]&~(ULONG_PTR)1);CHECK(seen<16&&r->Queue==q&&r->Count==1&&r->CoalesceIndex==MAXULONG&&r->Descriptors[0].Token.Slot==seen&&r->Descriptors[0].Token.Queue==queue&&r->Descriptors[0].Token.Generation==1&&r->Mdl->Partial&&r->IndicationLength==1500&&r->IpVersion==PB_TEST_IP_VERSION&&r->Protocol==CqProtocol);UCHAR expected[1500];CqPacket(expected,queue*16+seen);CHECK(!memcmp(r->Mdl->Address,expected,1500));VOID *future=(VOID *)(ULONG_PTR)~0u;CHECK(n->Info[TcpRecvSegCoalesceInfo]==(CqVersion==PB_NDIS_VERSION(20)?future:NULL)&&n->Info[RscTcpTimestampDelta]==(CqVersion==PB_NDIS_VERSION(20)?future:NULL)&&n->Info[UdpRecvSegCoalesceOffloadInfo]==(CqVersion==PB_NDIS_VERSION(20)?future:NULL));++seen;}
 CHECK(seen==16);return head;
}
static VOID CqRun(ULONG protocol,ULONG minor,BOOLEAN wrap,BOOLEAN publicationFirst,BOOLEAN measured)
{
 CHECK(!CqFixture&&!CqHandles&&!Pools&&!Mdls&&!Nbls);ULONG joins=CqJoins,workers=CqWorkerJoins;CqProtocol=protocol;CqVersion=PB_NDIS_VERSION(minor);CqBase=wrap?UINT64_MAX-7:0;CqPublicationFirst=publicationFirst;
 CqFixture=Create(2,16,32,0);FIXTURE *f=CqFixture;AutoConsume=FALSE;UnexpectedIndication=0;f->Ctx.Probe.V2SessionActive=1;
 for(ULONG qi=0;qi<2;++qi)for(ULONG r=0;r<PB_QUEUE_COUNT;++r){PB_QUEUE_VIEW *v=f->S.Queues[qi].Rings+r;v->Cursor=v->PeerCursor=v->Shared->Producer=v->Shared->Consumer=CqBase;}
 NET_BUFFER_LIST *heads[2]={CqFill(0),CqFill(1)};CHECK(f->S.Active==32&&f->S.RxObjects==32&&Nbls==32&&Mdls==64&&Pools==32);
 CqPublished=CqEvent();CqRelease=CqEvent();CqQueryBlocked=CqEvent();CqQueryDone=CqEvent();CqProgress.Native=CqEvent();f->S.ProgressEvent=&CqProgress;f->S.Lifecycle=(PB_SHARED_LIFECYCLE *)(f->S.Region+f->S.Layout.LifecycleOffset);
 CqPushes=CqBadPushes=CqQueryChecks=CqProgressSignals=0;for(ULONG i=0;i<2;++i)CqKernel[i]=CqUser[i]=0;OnSetEvent=CqSignals;
 CQ_RETURN_JOB jobs[2]={{heads[0],0},{heads[1],1}};CQ_THREAD first={0},query;
 if(publicationFirst){first=CqStart(CqReturn,jobs,CQ_RETURN0);CqAwait(CqPublished);CHECK(f->S.Active==32&&f->S.Returns.References==1&&!f->S.Completions&&Nbls==16);}
 /* Defensive shared ABI input only. Public DLL calls do not manufacture a
  * Consumer ahead of Producer. No private ledger/reference is changed. */
 PB_QUEUE_VIEW *bad=f->S.Queues[1].Rings+PB_QUEUE_RX_COMPLETE;bad->Shared->Consumer=CqBase+1;
 CqJoin(CqStart(CqReturn,jobs+1,CQ_RETURN1));CHECK(f->S.Faulted&&f->S.Draining&&f->S.Active==16&&f->S.Returns.References==(publicationFirst?1:0)&&CqBadPushes==1&&!f->S.Completions&&f->S.Lifecycle->Terminal);
 CHECK(CqKernel[0]==1&&CqKernel[1]==1&&CqUser[0]==1&&CqUser[1]==1);PbV2Fault(&f->S);CHECK(CqKernel[0]==1&&CqKernel[1]==1&&CqUser[0]==1&&CqUser[1]==1);
 query=CqStart(CqQuery,NULL,CQ_QUERY);
 if(publicationFirst){CqAwait(CqQueryBlocked);CHECK(WaitForSingleObject(CqQueryDone,0)==WAIT_TIMEOUT&&CqQueryChecks==1&&f->S.Queues[0].PendingCount==16&&f->S.Queues[0].Credits==16);CHECK(SetEvent(CqRelease));CqJoin(first);CqJoin(query);}
 else{CqJoin(query);CqJoin(CqStart(CqReturn,jobs,CQ_RETURN0));CHECK(!CqQueryChecks&&!CqPushes);}
 CHECK(!Pools&&!Mdls&&!Nbls&&!f->S.Active&&!f->S.Returns.References&&f->S.RxReturned==32&&f->S.Completions==(publicationFirst?16:0)&&CqPushes==(LONG)publicationFirst);
 CHECK(State(f,0)==PB_SESSION_QUARANTINED);for(ULONG qi=0;qi<2;++qi){PB_KERNEL_QUEUE *q=f->S.Queues+qi;CHECK(!q->FaultQuiesced&&!q->Credits&&!q->PendingCount&&q->PendingHead==(qi==0&&publicationFirst?16u:0u));for(ULONG i=0;i<16;++i)CHECK(!q->RxSlots[i].Active&&q->RxSlots[i].Generation==1);}
 for(ULONG qi=0;qi<2;++qi){CqJoin(CqStart(CqWorker,f->S.Queues+qi,CQ_WORKER));CHECK(f->S.Queues[qi].FaultQuiesced&&State(f,0)==(qi?PB_SESSION_STOPPED:PB_SESSION_QUARANTINED));}
 PB_QUEUE_VIEW consumer;CHECK(PbAttachQueue(&consumer,f->S.Region,&f->S.Layout,0,PB_QUEUE_RX_COMPLETE,FALSE)==PB_OK);consumer.Cursor=consumer.PeerCursor=CqBase;PB_DESCRIPTOR completed[32];uint32_t n;PB_RESULT r=PbQueuePop(&consumer,completed,32,&n);
 CHECK(r==(publicationFirst?PB_OK:PB_EMPTY)&&n==(publicationFirst?16u:0u));for(ULONG i=0;i<n;++i)CHECK(completed[i].Token.Epoch==42&&completed[i].Token.Generation==1&&completed[i].Token.Queue==0&&completed[i].Token.Slot==i&&completed[i].Length==1500&&completed[i].Status==PB_OK);
 CHECK(PbQueuePop(&consumer,completed,32,&n)==PB_EMPTY&&!n&&bad->Shared->Producer==CqBase&&bad->Shared->Consumer==CqBase+1&&Exits==2&&CqKernel[0]==1&&CqKernel[1]==1&&CqUser[0]==1&&CqUser[1]==1&&CqProgressSignals>0&&CqJoins-joins==5&&CqWorkerJoins-workers==2);
 OnSetEvent=NULL;f->S.ProgressEvent=NULL;CqClose(CqProgress.Native);CqClose(CqPublished);CqClose(CqRelease);CqClose(CqQueryBlocked);CqClose(CqQueryDone);Destroy(f);CqFixture=NULL;CHECK(!Current&&!CqHandles&&!Pools&&!Mdls&&!Nbls);++CqCases;
 printf("CROSSFAULT32 ip%u proto%lu ndis%lu wrap%u publicationFirst%u measured%u packets32 publish%u privateRetired%u badPush1 queryBlocked%u joins5 workerJoins2 owners0 repairs0\n",PB_TEST_IP_VERSION,protocol,minor,wrap,publicationFirst,measured,publicationFirst?16:0,publicationFirst?16:32,publicationFirst);
}
static VOID CqMatrix(BOOLEAN measured)
{for(ULONG protocol=6;protocol<=17;protocol+=11)for(ULONG minor=20;minor<=89;minor+=69)for(ULONG wrap=0;wrap<2;++wrap)for(ULONG order=0;order<2;++order)CqRun(protocol,minor,(BOOLEAN)wrap,(BOOLEAN)order,measured);}
int main(VOID)
{
 setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CqMatrix(FALSE);CHECK(CqCases==16&&GetProcessHandleCount(GetCurrentProcess(),&before));CqMatrix(TRUE);
 CHECK(CqCases==32&&CqJoins==160&&CqWorkerJoins==64&&!CqHandles&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
 printf("PASS: CROSSFAULT32 family%u cases32 warm16 measured16 joins160 workerJoins64 handles%lu/%lu owners0 repairs0\n",PB_TEST_IP_VERSION,before,after);return 0;
}
