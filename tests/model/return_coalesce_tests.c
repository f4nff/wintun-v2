/* SPDX-License-Identifier: GPL-2.0 */
/* Actual return/coalesce/NBL functions: hold a returned callback at RxLock exit
 * while a sole receive worker borrows its buffer for a new live indication. */
#include "kernel_nbl_fixture.h"
#ifndef PB_GUARD_TESTS
#include "../safety/guard_memory.h"
#endif
#include "../../include/packetbridge_probe.h"
#include "../../driver/v2/session_coalesce.h"

typedef struct FIXTURE {
    TUN_CTX Ctx;PB_KERNEL_SESSION S;MDL Parent;
    PB_RX_SLOT Slots[8];PB_DESCRIPTOR Pending[8];PB_QUEUE_VIEW Consumer;
    KEVENT Kernel,User;HANDLE Paused,Continue;
    NET_BUFFER_LIST *Old;DWORD ReturnThread;
    BOOLEAN DiagnosticPhase;
    ULONG Ip,Protocol,PacketLength,NewNbls,BeforeFreeChecks;
    volatile LONG KernelWakes,UserWakes;
} FIXTURE;
static FIXTURE *Current;
static BOOLEAN RequireReuse=TRUE;
static VOID Put16(UCHAR *p,ULONG n) {p[0]=(UCHAR)(n>>8);p[1]=(UCHAR)n;}
static VOID Put32(UCHAR *p,ULONG n) {Put16(p,n>>16);Put16(p+2,n);}
static VOID Packet(FIXTURE *f,UCHAR *p,ULONG sequence,UCHAR payload)
{
    ULONG offset=f->Ip==4?20:40,header=offset+(f->Protocol==6?20:8);
    memset(p,0,f->PacketLength);
    if(f->Ip==4) {p[0]=0x45;Put16(p+2,f->PacketLength);p[8]=64;p[9]=(UCHAR)f->Protocol;p[12]=10;p[16]=11;}
    else {p[0]=0x60;Put16(p+4,f->PacketLength-40);p[6]=(UCHAR)f->Protocol;p[7]=64;p[8]=0x20;p[24]=0x30;}
    Put16(p+offset,4000);Put16(p+offset+2,5000);
    if(f->Protocol==6) {Put32(p+offset+4,sequence);p[offset+12]=0x50;p[offset+13]=0x10;Put16(p+offset+14,8192);}
    else Put16(p+offset+4,f->PacketLength-offset);
    memset(p+header,payload,16);
    CHECK(PbFinalizeChecksums(p,f->PacketLength,f->Ip==4,f->Protocol==6,f->Protocol==17)==PB_PACKET_OK);
}
static NET_BUFFER_LIST *Pair(FIXTURE *f,ULONG first,ULONG *nblCount)
{
    PB_KERNEL_QUEUE *q=f->S.Queues;PB_DESCRIPTOR d[2]={0};BOOLEAN valid[2]={TRUE,TRUE};
    for(ULONG i=0;i<2;++i) {
        d[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)(first+i),0},.Length=f->PacketLength,
            .DataOffset=f->S.Layout.Queues[0].RxData+(first+i)*PB_BLOCK_SIZE};
        Packet(f,f->S.Region+d[i].DataOffset,1000+i*16,(UCHAR)(0x40+first+i));
    }
    KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->RxLock,&lock);
    for(ULONG i=0;i<2;++i) {uint32_t complete;CHECK(PbAdmitReceive(&f->S.Layout,42,0,q->RxSlots,d+i,0,&complete)==PB_OK);q->Credits++;}
    KeReleaseInStackQueuedSpinLock(&lock);
    ULONG copied;PB_COALESCE coal={0};ULONG group=PbV2TryCoalesce(q,d,valid,2,
        f->Protocol==17?PB_RX_URO:f->Ip==4?PB_RX_RSC4:PB_RX_RSC6,&copied,&coal);
    NET_BUFFER_LIST *head=PbV2Receive(q,d,group,copied,group==2?&coal:NULL);CHECK(head);
    if(group==1) {head->Next=PbV2Receive(q,d+1,1,MAXULONG,NULL);CHECK(head->Next);}
    *nblCount=group==2?1:2;PbV2RetainReceives(&f->S,*nblCount,2);return head;
}
static VOID CountEvent(KEVENT *event)
{
    if(event==&Current->Kernel) InterlockedIncrement(&Current->KernelWakes);
    if(event==&Current->User) InterlockedIncrement(&Current->UserWakes);
}
static VOID BeforeFree(NET_BUFFER_LIST *nbl)
{
    FIXTURE *f=Current;if(nbl!=f->Old) return;
    /* Both MDLs are already gone, but the NBL itself has not been freed yet. */
    CHECK(Mdls==2 && Nbls==2 && f->S.Queues[0].CoalesceBusy[0]==1);
    f->BeforeFreeChecks++;
}
static VOID PauseReturn(KSPIN_LOCK *lock)
{
    FIXTURE *f=Current;
    if(f->DiagnosticPhase || GetCurrentThreadId()!=f->ReturnThread || lock!=&f->S.Queues[0].RxLock) return;
    CHECK(SetEvent(f->Paused) && WaitForSingleObject(f->Continue,5000)==WAIT_OBJECT_0);
}
static VOID PauseDiagnostic(KSPIN_LOCK *lock)
{
    FIXTURE *f=Current;
    if(!f->DiagnosticPhase || GetCurrentThreadId()!=f->ReturnThread || lock!=&f->Ctx.Probe.ReturnDiagnosticsLock) return;
    CHECK(SetEvent(f->Paused) && WaitForSingleObject(f->Continue,5000)==WAIT_OBJECT_0);
}
static DWORD WINAPI ReturnOld(VOID *opaque)
{
    FIXTURE *f=opaque;f->ReturnThread=GetCurrentThreadId();CHECK(PbV2ReturnOne(&f->Ctx,f->Old));return 0;
}
static VOID Consume(FIXTURE *f,ULONG first,ULONG count,ULONG status)
{
    PB_DESCRIPTOR items[8];uint32_t n;CHECK(PbQueuePop(&f->Consumer,items,8,&n)==PB_OK && n==count);
    for(ULONG i=0;i<n;++i) CHECK(items[i].Token.Epoch==42 && items[i].Token.Slot==first+i && items[i].Status==status);
}
static VOID Run(ULONG ip,ULONG protocol,ULONG result,BOOLEAN full,BOOLEAN fault,BOOLEAN diagnosticPhase)
{
    FIXTURE *f=calloc(1,sizeof(*f));CHECK(f && !Pools && !Mdls && !Nbls);Current=f;
    f->Ip=ip;f->Protocol=protocol;f->PacketLength=(ip==4?20:40)+(protocol==6?20:8)+16;
    f->DiagnosticPhase=diagnosticPhase;ULONG oldLength=f->PacketLength;
    f->S.Ctx=&f->Ctx;f->S.Epoch=42;f->Ctx.Running=1;
    CHECK(PbCalculateLayout(1,8,8,&f->S.Layout)==PB_OK);
    f->S.Region=VirtualAlloc(NULL,f->S.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(f->S.Region);
    f->Parent.Address=f->S.Region;f->Parent.Length=f->S.Layout.RegionSize;f->S.Mdl=&f->Parent;
    PB_KERNEL_QUEUE *q=f->S.Queues;q->Session=&f->S;q->RxSlots=f->Slots;q->Pending=f->Pending;q->KernelEvent=&f->Kernel;q->UserEvent=&f->User;
    q->Scratch=PbTestGuardAlloc(PB_BLOCK_SIZE);
    for(ULONG b=0;b<2;++b) q->CoalesceBuffers[b]=PbTestGuardAlloc(PB_BLOCK_SIZE);
    f->S.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);f->Kernel.Native=CreateEventW(NULL,FALSE,FALSE,NULL);f->User.Native=CreateEventW(NULL,FALSE,FALSE,NULL);
    f->Paused=CreateEventW(NULL,TRUE,FALSE,NULL);f->Continue=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(f->S.AllReturned.Native && f->Kernel.Native && f->User.Native && f->Paused && f->Continue);
    CHECK(PbAttachQueue(q->Rings+PB_QUEUE_RX_COMPLETE,f->S.Region,&f->S.Layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
    CHECK(PbAttachQueue(&f->Consumer,f->S.Region,&f->S.Layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    ULONG nblCount;f->Old=Pair(f,0,&nblCount);CHECK(nblCount==1);
    NET_BUFFER_LIST *held=Pair(f,2,&nblCount);CHECK(nblCount==1 && q->CoalesceBusy[0] && q->CoalesceBusy[1]);
    UCHAR heldBytes[128];ULONG heldLength=held->FirstMdl->Next->Length;CHECK(heldLength<=sizeof(heldBytes));memcpy(heldBytes,q->CoalesceBuffers[1],heldLength);
    NET_BUFFER_LIST_STATUS(f->Old)=result==1?0xc0000001u:0;
    PB_RX_RECORD *oldRecord=(PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(f->Old)&~(ULONG_PTR)1);
    oldRecord->LocallyCancelled=result==2;
    if(full) {PB_DESCRIPTOR junk[8]={0};uint32_t n,notify;CHECK(PbQueuePush(q->Rings+PB_QUEUE_RX_COMPLETE,junk,8,&n,&notify)==PB_OK && n==8);}
    if(fault) PbV2Fault(&f->S);
    uint32_t ready;if(!full) CHECK(PbQueueArm(&f->Consumer,&ready)==PB_OK && !ready);
    BeforeNblFree=BeforeFree;AfterQueueUnlock=PauseReturn;BeforeDiagnosticLock=PauseDiagnostic;OnSetEvent=CountEvent;
    FixtureLocks=FixtureRundownAcquires=FixtureRundownReleases=0;
    HANDLE thread=CreateThread(NULL,0,ReturnOld,f,0,NULL);CHECK(thread && WaitForSingleObject(f->Paused,5000)==WAIT_OBJECT_0);
    CHECK(f->BeforeFreeChecks==1 && Nbls==1 && Mdls==2 && Pools==2 && f->S.Active==2 && f->S.Returns.References==1);
    CHECK(q->RxSlots[0].Active==(ULONG)diagnosticPhase && q->RxSlots[1].Active==(ULONG)diagnosticPhase && q->RxSlots[2].Active && q->RxSlots[3].Active);
    CHECK(q->CoalesceBusy[0]==(RequireReuse?0:1) && q->CoalesceBusy[1]==1);
    NET_BUFFER_LIST *fresh=NULL;UCHAR freshBytes[128];ULONG freshLength=0;
    if(!fault) {
        /* Before diagnostic capture, overwrite the released copied buffer with
         * a different IP/protocol/length. The old record must remain a snapshot. */
        if(diagnosticPhase) {f->Ip=ip==4?6:4;f->Protocol=protocol==6?17:6;f->PacketLength=(f->Ip==4?20:40)+(f->Protocol==6?20:8)+16;}
        ULONG before=Acquisitions;fresh=Pair(f,4,&f->NewNbls);
        CHECK(f->NewNbls==(RequireReuse?1u:2u) && Acquisitions-before==4*f->NewNbls);
        if(RequireReuse) {
            CHECK(fresh->FirstMdl->Next->Address==q->CoalesceBuffers[0] && q->CoalesceBusy[0]==1);
            freshLength=fresh->FirstMdl->Next->Length;CHECK(freshLength<=sizeof(freshBytes));memcpy(freshBytes,q->CoalesceBuffers[0],freshLength);
        }
    }
    CHECK(!memcmp(heldBytes,q->CoalesceBuffers[1],heldLength));
    CHECK(SetEvent(f->Continue) && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && CloseHandle(thread));
    AfterQueueUnlock=NULL;BeforeNblFree=NULL;BeforeDiagnosticLock=NULL;
    /* The old callback tail must not clear a newly acquired buffer lease. */
    CHECK(q->CoalesceBusy[0]==(LONG)(!fault && RequireReuse) && q->CoalesceBusy[1]==1);
    if(freshLength) CHECK(!memcmp(freshBytes,q->CoalesceBuffers[0],freshLength));
    CHECK(!memcmp(heldBytes,q->CoalesceBuffers[1],heldLength) && f->S.Active==1+(LONG)f->NewNbls && !f->S.Returns.References);
    CHECK(WaitForSingleObject(f->S.AllReturned.Native,0)==WAIT_TIMEOUT && !f->KernelWakes);
    PB_RETURN_SNAPSHOT diagnostic={0};PbSnapshotReturnEvents(&f->Ctx.Probe.ReturnDiagnostics,&diagnostic);
    CHECK(diagnostic.RecordCount==(ULONG)(result!=0));
    if(result) CHECK(diagnostic.Records[0].FirstToken.Slot==0 && diagnostic.Records[0].OriginalCount==2 &&
        diagnostic.Records[0].OriginalBytes==2*oldLength && diagnostic.Records[0].IndicationLength==oldLength+16 &&
        diagnostic.Records[0].Protocol==protocol && diagnostic.Records[0].IpVersion==ip);
    if(!fault) {
        if(full) {PB_DESCRIPTOR junk[8];uint32_t n;CHECK(q->PendingCount==2 && PbQueuePop(&f->Consumer,junk,8,&n)==PB_OK && n==8);
            KLOCK_QUEUE_HANDLE lock;KeAcquireInStackQueuedSpinLock(&q->RxLock,&lock);PbV2FlushLocked(q);KeReleaseInStackQueuedSpinLock(&lock);}
        Consume(f,0,2,result==2?PB_STOPPED:result==1?PB_RX_REJECTED:PB_OK);
        CHECK(PbV2ReturnNbls(&f->Ctx,&fresh,PB_MAX_BATCH) && !fresh);Consume(f,4,2,PB_OK);
    }
    CHECK(PbV2ReturnOne(&f->Ctx,held));if(!fault) Consume(f,2,2,PB_OK);
    CHECK(!Pools && !Mdls && !Nbls && !f->S.Active && !f->S.Returns.References && !q->Credits && !q->PendingCount);
    CHECK(!q->CoalesceBusy[0] && !q->CoalesceBusy[1] && f->S.RxReturned==(fault?4:6) && f->S.Completions==(fault?0:6));
    CHECK(FixtureLocks==(fault?4:full?9:8) && FixtureRundownAcquires==(fault?2:3) && FixtureRundownReleases==FixtureRundownAcquires);
    CHECK(f->Ctx.Statistics.ifHCInUcastPkts==(fault?2:4)+(result?0:2) && f->Ctx.Statistics.ifInErrors==(result==1?1:0) && f->Ctx.Statistics.ifInDiscards==(result==2?2:0));
    CHECK(f->Ctx.Statistics.ifHCInOctets==(LONG64)(2+(result?0:2))*oldLength+(fault?0:2*f->PacketLength));
    for(ULONG i=0;i<8;++i) CHECK(!q->RxSlots[i].Active);
    CHECK(WaitForSingleObject(f->S.AllReturned.Native,0)==WAIT_OBJECT_0 && f->UserWakes==(!fault && !full?1:0));
    OnSetEvent=NULL;
    CHECK(CloseHandle(f->S.AllReturned.Native) && CloseHandle(f->Kernel.Native) && CloseHandle(f->User.Native) && CloseHandle(f->Paused) && CloseHandle(f->Continue));
    for(ULONG b=0;b<2;++b) PbTestGuardFree(q->CoalesceBuffers[b]);PbTestGuardFree(q->Scratch);CHECK(VirtualFree(f->S.Region,0,MEM_RELEASE));
    printf("PASS: return/coalesce reuse ip=%lu protocol=%lu result=%lu full=%u fault=%u diagnostic_phase=%u: replacement_nbls=%lu replacement_ddi_acquisitions=%lu, exact completion/status/stats, held buffer and replacement ownership across callback tail.\n",
        ip,protocol,result,full,fault,diagnosticPhase,f->NewNbls,4*f->NewNbls);free(f);Current=NULL;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
#ifdef PB_COALESCE_REUSE_BASELINE
    RequireReuse=argc==2 && !strcmp(argv[1],"--require-reuse");
#else
    (void)argc;(void)argv;
#endif
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(ULONG ip=4;ip<=6;ip+=2) for(ULONG protocol=6;protocol<=17;protocol+=11) for(ULONG result=0;result<3;++result) {
        Run(ip,protocol,result,FALSE,FALSE,FALSE);Run(ip,protocol,result,TRUE,FALSE,FALSE);Run(ip,protocol,result,FALSE,TRUE,FALSE);
        if(result) Run(ip,protocol,result,FALSE,FALSE,TRUE);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
