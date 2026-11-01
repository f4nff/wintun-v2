/* SPDX-License-Identifier: GPL-2.0 */
/* Actual NBL/completion/reference paths. DDI operation counts are not kernel timings. */
#include "kernel_nbl_fixture.h"
typedef struct FIXTURE {
    TUN_CTX Ctx; PB_KERNEL_SESSION S; MDL Parent;
    PB_RX_SLOT Slots[2][64]; PB_DESCRIPTOR Pending[2][128];
    PB_QUEUE_VIEW Consumers[2]; KEVENT Kernel[2],User[2];
    volatile LONG KernelWakes,UserWakes,ReturnedSignals;
} FIXTURE;
static FIXTURE *Current;
static VOID CountEvent(KEVENT *event)
{
    FIXTURE *f=Current;
    if(event==&f->S.AllReturned) InterlockedIncrement(&f->ReturnedSignals);
    for(ULONG i=0;i<f->S.Layout.QueueCount;++i) {
        if(event==f->Kernel+i) InterlockedIncrement(&f->KernelWakes);
        if(event==f->User+i) InterlockedIncrement(&f->UserWakes);
    }
}
static FIXTURE *CreateSized(ULONG queues,ULONG capacity)
{
    FIXTURE *f=calloc(1,sizeof(*f)); CHECK(f && !Pools && !Mdls && !Nbls); Current=f;
    f->S.Ctx=&f->Ctx; f->S.Epoch=42; f->Ctx.MiniportAdapterHandle=f; f->Ctx.NblPool=f;
    CHECK(PbCalculateLayout(queues,64,capacity,&f->S.Layout)==PB_OK);
    f->S.Region=VirtualAlloc(NULL,f->S.Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(f->S.Region);
    f->Parent.Address=f->S.Region; f->Parent.Length=f->S.Layout.RegionSize; f->S.Mdl=&f->Parent;
    f->S.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(f->S.AllReturned.Native);
    for(ULONG i=0;i<queues;++i) {
        PB_KERNEL_QUEUE *q=f->S.Queues+i; q->Session=&f->S; q->Index=i;
        q->RxSlots=f->Slots[i]; q->Pending=f->Pending[i]; q->KernelEvent=f->Kernel+i; q->UserEvent=f->User+i;
        f->Kernel[i].Native=CreateEventW(NULL,FALSE,FALSE,NULL); f->User[i].Native=CreateEventW(NULL,FALSE,FALSE,NULL);
        CHECK(f->Kernel[i].Native && f->User[i].Native);
        CHECK(PbAttachQueue(q->Rings+PB_QUEUE_RX_COMPLETE,f->S.Region,&f->S.Layout,i,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
        CHECK(PbAttachQueue(f->Consumers+i,f->S.Region,&f->S.Layout,i,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
        uint32_t ready; CHECK(PbQueueArm(f->Consumers+i,&ready)==PB_OK && !ready);
        for(ULONG b=0;b<2;++b) {q->CoalesceBuffers[b]=calloc(1,PB_BLOCK_SIZE); CHECK(q->CoalesceBuffers[b]);}
    }
    return f;
}
static FIXTURE *Create(ULONG queues) {return CreateSized(queues,128);}
static VOID Destroy(FIXTURE *f)
{
    CHECK(!Pools && !Mdls && !Nbls && !f->S.Active && !f->S.Returns.References && f->ReturnedSignals==1);
    CHECK(WaitForSingleObject(f->S.AllReturned.Native,0)==WAIT_OBJECT_0 && CloseHandle(f->S.AllReturned.Native));
    for(ULONG i=0;i<f->S.Layout.QueueCount;++i) {
        PB_KERNEL_QUEUE *q=f->S.Queues+i; CHECK(!q->PendingCount && !q->Credits);
        for(ULONG j=0;j<64;++j) CHECK(!q->RxSlots[j].Active);
        for(ULONG b=0;b<2;++b) { CHECK(!q->CoalesceBusy[b]); free(q->CoalesceBuffers[b]); }
        CHECK(CloseHandle(f->Kernel[i].Native) && CloseHandle(f->User[i].Native));
    }
    CHECK(VirtualFree(f->S.Region,0,MEM_RELEASE)); OnSetEvent=NULL; free(f); Current=NULL;
}
static NET_BUFFER_LIST *Receive(FIXTURE *f,ULONG index,ULONG slot,ULONG count,ULONG copied,ULONG status)
{
    PB_KERNEL_QUEUE *q=f->S.Queues+index; PB_DESCRIPTOR d[32]={0};
    CHECK(slot+count<=64);
    for(ULONG i=0;i<count;++i) {
        d[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)(slot+i),(USHORT)index},.Length=count>1?48:20,
            .DataOffset=f->S.Layout.Queues[index].RxData+(slot+i)*PB_BLOCK_SIZE};
        f->S.Region[d[i].DataOffset]=0x45; uint32_t complete;
        CHECK(PbAdmitReceive(&f->S.Layout,42,index,q->RxSlots,d+i,0,&complete)==PB_OK); q->Credits++;
    }
    PB_COALESCE coal={.Length=40+count*8,.Protocol=6,.IpVersion=4,.SegmentSize=8,.PayloadLength=count*8};
    if(count>1) {CHECK(copied<2 && !q->CoalesceBusy[copied]); q->CoalesceBuffers[copied][0]=0x45; q->CoalesceBusy[copied]=1;}
    NET_BUFFER_LIST *nbl=PbV2Receive(q,d,count,copied,count>1?&coal:NULL); CHECK(nbl);
    PbV2RetainReceives(&f->S,1,count); NET_BUFFER_LIST_STATUS(nbl)=status; return nbl;
}
static BOOLEAN ReturnWindow(TUN_CTX *ctx,NET_BUFFER_LIST **head)
{
#ifdef PB_RETURN_BASELINE
    NET_BUFFER_LIST *nbl=*head,*next=nbl->Next;
    if(!PbV2ReturnOne(ctx,nbl)) return FALSE;
    *head=next; return TRUE;
#else
    return PbV2ReturnNbls(ctx,head,PB_MAX_BATCH);
#endif
}
static VOID FillCompletion(FIXTURE *f,ULONG index)
{
    PB_DESCRIPTOR junk[32]={0}; uint32_t n,notify;
    for(ULONG batch=0;batch<4;++batch) CHECK(PbQueuePush(f->S.Queues[index].Rings+PB_QUEUE_RX_COMPLETE,junk,32,&n,&notify)==PB_OK && n==32);
}
static VOID Run(ULONG nblCount,ULONG queues,ULONG descriptors,BOOLEAN mixed,BOOLEAN full,BOOLEAN fault,BOOLEAN foreign,ULONG expectedWindows)
{
    FIXTURE *f=Create(queues); NET_BUFFER_LIST *head=NULL,*tail=NULL,alien={0};
    if(foreign==2) head=tail=&alien;
    ULONG slots[2]={0},statuses[2][64]={0},success=0,errors=0,foreigns=0,windows=0; ULONG64 bytes=0;
    for(ULONG i=0;i<nblCount;++i) {
        ULONG index=i%queues,status=mixed && i%3==1?1u:0u;
        NET_BUFFER_LIST *nbl=Receive(f,index,slots[index],descriptors,descriptors>1?i/queues:MAXULONG,status);
        for(ULONG j=0;j<descriptors;++j) statuses[index][slots[index]+j]=status?PB_CORRUPT:PB_OK;
        slots[index]+=descriptors;
        if(status) errors++; else {success+=descriptors; bytes+=(ULONG64)descriptors*(descriptors>1?48:20);}
        if(tail) tail->Next=nbl; else head=nbl; tail=nbl;
        if(foreign==1 && !i) {tail->Next=&alien; tail=&alien;}
    }
    if(full) for(ULONG i=0;i<queues;++i) FillCompletion(f,i);
    if(fault) PbV2Fault(&f->S);
    OnSetEvent=CountEvent; FixtureLocks=FixtureRundownAcquires=FixtureRundownReleases=0;
    while(head) {
        if(ReturnWindow(&f->Ctx,&head)) windows++;
        else {CHECK(head==&alien); head=head->Next; foreigns++;}
    }
#ifdef PB_RETURN_BASELINE
    expectedWindows=nblCount;
#endif
    CHECK(windows==expectedWindows && foreigns==(ULONG)!!foreign);
    CHECK(FixtureLocks==(LONG)(2*windows) && FixtureRundownAcquires==(LONG)windows && FixtureRundownReleases==(LONG)windows);
#if defined(PB_RETURN_BASELINE) || defined(PB_RETURN_WAKE_BASELINE)
    CHECK(f->KernelWakes==(LONG)windows);
#else
    CHECK(!f->KernelWakes); /* At most 64 credits in a capacity-128 queue. */
#endif
    CHECK(f->S.RxObjects==nblCount*descriptors && f->S.RxReturned==nblCount*descriptors);
    CHECK(f->Ctx.Statistics.ifHCInOctets==(LONG64)bytes && f->Ctx.Statistics.ifHCInUcastOctets==(LONG64)bytes && f->Ctx.Statistics.ifHCInUcastPkts==success && f->Ctx.Statistics.ifInErrors==errors);
    printf("PASS: returned NBLs=%lu queues=%lu descriptors/NBL=%lu mixed=%u full=%u fault=%u foreign=%u, windows=%lu locks=%lu rundown pairs=%lu worker wakes=%lu.\n",
        nblCount,queues,descriptors,mixed,full,fault,foreign,windows,(ULONG)FixtureLocks,(ULONG)FixtureRundownAcquires,(ULONG)f->KernelWakes);
    CHECK(f->UserWakes==(LONG)((full || fault)?0:queues));
    for(ULONG index=0;index<queues;++index) {
        PB_KERNEL_QUEUE *q=f->S.Queues+index; PB_DESCRIPTOR returned[32]; uint32_t n;
        if(fault) {CHECK(!q->PendingCount && !q->Credits && !f->S.Completions); continue;}
        if(full) {
            CHECK(q->PendingCount==slots[index] && q->Credits==slots[index]);
            for(ULONG batch=0;batch<4;++batch) CHECK(PbQueuePop(f->Consumers+index,returned,32,&n)==PB_OK && n==32);
            PbV2FlushLocked(q);
        }
        for(ULONG at=0;at<slots[index];) {
            CHECK(PbQueuePop(f->Consumers+index,returned,32,&n)==PB_OK && n);
            for(ULONG j=0;j<n;++j,++at) CHECK(returned[j].Token.Slot==at && returned[j].Token.Queue==index && returned[j].Status==statuses[index][at]);
        }
        CHECK(PbQueuePop(f->Consumers+index,returned,32,&n)==PB_EMPTY);
    }
    if(!fault) CHECK(f->S.Completions==nblCount*descriptors);
    Destroy(f);
}
static HANDLE BothPinned;
static volatile LONG Pins;
static VOID PinTogether(EX_RUNDOWN_REF *refs)
{
    CHECK(refs==&Current->S.Returns);
    if(InterlockedIncrement(&Pins)>2) return;
    if(Pins==2) CHECK(SetEvent(BothPinned));
    CHECK(WaitForSingleObject(BothPinned,5000)==WAIT_OBJECT_0);
}
typedef struct RETURN_JOB { FIXTURE *F; NET_BUFFER_LIST *Head; ULONG Windows; } RETURN_JOB;
static DWORD WINAPI ReturnConcurrently(VOID *opaque)
{
    RETURN_JOB *job=opaque;
    while(job->Head) {CHECK(ReturnWindow(&job->F->Ctx,&job->Head)); job->Windows++;}
    return 0;
}
static VOID ConcurrentReturns(ULONG capacity)
{
    FIXTURE *f=CreateSized(1,capacity); RETURN_JOB jobs[2]={{.F=f},{.F=f}};
    for(ULONG t=0;t<2;++t) {
        NET_BUFFER_LIST *tail=NULL;
        for(ULONG n=0;n<32;++n) {
            NET_BUFFER_LIST *nbl=Receive(f,0,t*32+n,1,MAXULONG,0);
            if(tail) tail->Next=nbl; else jobs[t].Head=nbl; tail=nbl;
        }
    }
    OnSetEvent=CountEvent; FixtureLocks=FixtureRundownAcquires=FixtureRundownReleases=Pins=0;
    BothPinned=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(BothPinned); AfterRundownAcquire=PinTogether;
    HANDLE threads[2]={CreateThread(NULL,0,ReturnConcurrently,jobs,0,NULL),CreateThread(NULL,0,ReturnConcurrently,jobs+1,0,NULL)};
    CHECK(threads[0] && threads[1] && WaitForMultipleObjects(2,threads,TRUE,5000)==WAIT_OBJECT_0);
    AfterRundownAcquire=NULL;
#ifdef PB_RETURN_BASELINE
    LONG windows=64;
#else
    LONG windows=2;
#endif
    CHECK(jobs[0].Windows+jobs[1].Windows==(ULONG)windows && FixtureLocks==2*windows && FixtureRundownAcquires==windows && FixtureRundownReleases==windows);
#if defined(PB_RETURN_BASELINE) || defined(PB_RETURN_WAKE_BASELINE)
    CHECK(f->KernelWakes==windows);
#else
    CHECK(f->KernelWakes==(capacity==64?1:0)); /* Exactly one serialized full -> usable transition. */
#endif
    CHECK(f->S.RxObjects==64 && f->S.RxReturned==64 && f->S.Completions==64 && f->Ctx.Statistics.ifHCInUcastPkts==64 && f->Ctx.Statistics.ifHCInOctets==1280 && !f->Ctx.Statistics.ifInErrors);
    ULONG64 seen=0; PB_DESCRIPTOR returned[32]; uint32_t n;
    for(ULONG batch=0;batch<2;++batch) {
        CHECK(PbQueuePop(f->Consumers,returned,32,&n)==PB_OK && n==32);
        for(ULONG i=0;i<n;++i) {
            CHECK(returned[i].Token.Slot<64 && !returned[i].Token.Queue && returned[i].Status==PB_OK);
            ULONG64 bit=1ull<<returned[i].Token.Slot; CHECK(!(seen&bit)); seen|=bit;
        }
    }
    CHECK(seen==UINT64_MAX && f->UserWakes==1);
    CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]) && CloseHandle(BothPinned)); BothPinned=NULL;
    printf("PASS: two concurrent return chains capacity=%lu: worker wakes=%ld, all 64 unique tokens, one last-reference signal and balanced rundown.\n",capacity,f->KernelWakes);Destroy(f);
}
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0); DWORD before,after; CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Run(1,1,1,FALSE,FALSE,FALSE,FALSE,1);
    Run(32,1,1,FALSE,FALSE,FALSE,FALSE,1); Run(64,1,1,TRUE,FALSE,FALSE,FALSE,(64+PB_MAX_BATCH-1)/PB_MAX_BATCH);
    Run(16,2,1,TRUE,FALSE,FALSE,FALSE,16);
    Run(2,1,16,TRUE,FALSE,FALSE,FALSE,1); Run(2,1,32,FALSE,FALSE,FALSE,FALSE,(64+PB_MAX_BATCH-1)/PB_MAX_BATCH);
    Run(32,1,1,TRUE,TRUE,FALSE,FALSE,1); Run(32,1,1,TRUE,FALSE,TRUE,FALSE,1);
    Run(3,1,1,TRUE,FALSE,FALSE,TRUE,2);
    Run(3,1,1,TRUE,FALSE,FALSE,2,1); ConcurrentReturns(128);ConcurrentReturns(64);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("PASS: actual receive returns, plain/coalesced descriptor budget, mixed queues/status, foreign boundary, full completion ring and fault retirement; exact completion order/statistics, no active slots, references, resources or handle delta."); return 0;
}
