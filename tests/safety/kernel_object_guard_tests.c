/* SPDX-License-Identifier: GPL-2.0 */
/* Hosted MDL/NBL objects only: no native DDI, NDIS, IRQL or driver execution.
 * Ordinary mode never probes invalid memory. Guarded probes run immediately
 * around free, before another test allocation can reuse the virtual address. */
#ifndef PB_KERNEL_NBL_FIXTURE_SOURCE
#define PB_KERNEL_NBL_FIXTURE_SOURCE "../model/kernel_nbl_fixture.h"
#endif
#include PB_KERNEL_NBL_FIXTURE_SOURCE

static ULONG Scenarios,Faults,AllocationHooks,FreeHooks,PoolHooks,AcquireHooks,ReleaseHooks;
static BOOLEAN InspectGuards,RejectOld;
static NET_BUFFER_LIST *Allocated;
static PB_KERNEL_SESSION *Session;

#ifdef PB_GUARD_TESTS
static BOOLEAN GuardLayout(const VOID *object,SIZE_T size)
{
    SYSTEM_INFO system;GetSystemInfo(&system);SIZE_T page=system.dwPageSize;
    MEMORY_BASIC_INFORMATION inside,after;
    CHECK(VirtualQuery(object,&inside,sizeof(inside))==sizeof(inside));
    CHECK(VirtualQuery((const UCHAR *)object+size,&after,sizeof(after))==sizeof(after));
    return inside.State==MEM_COMMIT && inside.Protect==PAGE_READWRITE && inside.RegionSize==page &&
        inside.BaseAddress==(UCHAR *)inside.AllocationBase+page &&
        (const UCHAR *)object+size==(UCHAR *)inside.AllocationBase+2*page &&
        after.AllocationBase==inside.AllocationBase && after.State==MEM_RESERVE && after.BaseAddress==(const UCHAR *)object+size;
}
static LONG ProbeFilter(EXCEPTION_POINTERS *exception,const VOID *address,BOOLEAN write)
{
    EXCEPTION_RECORD *record=exception->ExceptionRecord;
    return record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2 &&
        record->ExceptionInformation[0]==(ULONG_PTR)write && record->ExceptionInformation[1]==(ULONG_PTR)address ?
        EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH;
}
static VOID Probe(const VOID *address,BOOLEAN write)
{
    BOOLEAN caught=FALSE;
    __try {
        if(write)*(volatile UCHAR *)address=0x5a;
        else {volatile UCHAR value=*(const volatile UCHAR *)address;(void)value;}
    }
    __except(ProbeFilter(GetExceptionInformation(),address,write)){caught=TRUE;}
    CHECK(caught);++Faults;
}
#endif
static VOID DirectObjects(BOOLEAN control)
{
    CHECK(!Pools&&!Mdls&&!Nbls);Failure=Acquisitions=0;UCHAR payload=0;MDL parent={0};
    MDL *mdl=IoAllocateMdl(&payload,1,FALSE,FALSE,NULL);CHECK(mdl&&Acquisitions==1&&Mdls==1);
    CHECK(!((ULONG_PTR)mdl%__alignof(MDL))&&!mdl->Next&&!mdl->Partial&&mdl->Address==&payload&&mdl->Length==1);
#ifdef PB_GUARD_TESTS
    BOOLEAN guarded=GuardLayout(mdl,sizeof(*mdl));
    if(control&&!guarded)RejectOld=TRUE;else CHECK(guarded);
    if(guarded){Probe((UCHAR *)mdl+sizeof(*mdl),FALSE);Probe((UCHAR *)mdl+sizeof(*mdl),TRUE);}
#else
    (void)control;
#endif
    IoFreeMdl(mdl);
#ifdef PB_GUARD_TESTS
    if(guarded){Probe(mdl,FALSE);Probe(mdl,TRUE);}
#endif
    CHECK(!Mdls);NET_BUFFER_LIST *nbl=NdisAllocateNetBufferAndNetBufferList(NULL,0,0,&parent,0,1);
    CHECK(nbl&&Acquisitions==2&&Nbls==1&&!((ULONG_PTR)nbl%__alignof(NET_BUFFER_LIST)));
    CHECK(!nbl->Next&&!nbl->Reserved[0]&&!nbl->Reserved[1]&&!nbl->SourceHandle&&!nbl->Status&&nbl->Flags==15&&nbl->FirstMdl==&parent&&nbl->Length==1);
    for(ULONG i=0;i<InfoCount;++i)CHECK(nbl->Info[i]==(VOID *)(ULONG_PTR)~0u);
#ifdef PB_GUARD_TESTS
    guarded=GuardLayout(nbl,sizeof(*nbl));
    if(control&&!guarded)RejectOld=TRUE;else CHECK(guarded);
    if(guarded){Probe((UCHAR *)nbl+sizeof(*nbl),FALSE);Probe((UCHAR *)nbl+sizeof(*nbl),TRUE);}
#endif
    NdisFreeNetBufferList(nbl);
#ifdef PB_GUARD_TESTS
    if(guarded){Probe(nbl,FALSE);Probe(nbl,TRUE);}
#endif
    CHECK(!Pools&&!Mdls&&!Nbls);++Scenarios;
}
static VOID OnAllocate(VOID)
{
    ++AllocationHooks;CHECK(Pools==1&&Mdls==2&&Nbls==1&&!Session->Active);
    /* Production has not yet published the record/tag when this hook runs. */
}
static VOID OnFree(NET_BUFFER_LIST *nbl)
{
    ++FreeHooks;CHECK(nbl==Allocated&&Pools==1&&!Mdls&&Nbls==1&&Session->Active==1&&Session->Returns.References==1);
    CHECK(!((ULONG_PTR)nbl%__alignof(NET_BUFFER_LIST)));
    nbl->Status=0xdeadbeefu; /* Raw status must already have been captured. */
}
static VOID OnPoolFree(VOID *memory)
{CHECK(memory&&!Pools&&!Mdls&&!Nbls);++PoolHooks;}
static VOID OnAcquire(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Session->Returns&&refs->References==1&&Session->Active==1);++AcquireHooks;}
static VOID OnRelease(EX_RUNDOWN_REF *refs)
{CHECK(refs==&Session->Returns&&refs->References==1&&!Session->Active&&!Pools&&!Mdls&&!Nbls);++ReleaseHooks;}
static VOID Case(ULONG format,ULONG failure,BOOLEAN local,ULONG status)
{
    CHECK(!Pools&&!Mdls&&!Nbls);PB_KERNEL_SESSION s={0};TUN_CTX ctx={0};PB_LAYOUT layout;
    CHECK(PbCalculateLayout(1,8,8,&layout)==PB_OK);Session=&s;s.Ctx=&ctx;s.Layout=layout;s.Epoch=42;
    UCHAR *region=_aligned_malloc(layout.RegionSize,64);CHECK(region);memset(region,0,layout.RegionSize);
    MDL parent={.Address=region,.Length=layout.RegionSize};s.Region=region;s.Mdl=&parent;
    s.AllReturned.Native=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(s.AllReturned.Native);
    KEVENT kernel={CreateEventW(NULL,FALSE,FALSE,NULL)},user={CreateEventW(NULL,FALSE,FALSE,NULL)};CHECK(kernel.Native&&user.Native);
    PB_KERNEL_QUEUE *q=s.Queues;q->Session=&s;q->KernelEvent=&kernel;q->UserEvent=&user;
    PB_RX_SLOT slots[8]={0};PB_DESCRIPTOR pending[8]={0},items[8]={0};q->RxSlots=slots;q->Pending=pending;
    PB_QUEUE_VIEW complete;CHECK(PbAttachQueue(q->Rings+PB_QUEUE_RX_COMPLETE,region,&layout,0,PB_QUEUE_RX_COMPLETE,1)==PB_OK);
    CHECK(PbAttachQueue(&complete,region,&layout,0,PB_QUEUE_RX_COMPLETE,0)==PB_OK);
    ULONG ip=format==1||format==3?6:4,count=format==2?4:format==3?8:1,length=ip==4?60:80;
    ULONG protocol=format==3?17:6;PB_COALESCE coal={.Length=length,.Protocol=(UCHAR)protocol,.IpVersion=(UCHAR)ip,.PayloadLength=16,.SegmentSize=2};
    UCHAR *copied=calloc(1,PB_BLOCK_SIZE);CHECK(copied);q->CoalesceBuffers[0]=copied;
    for(ULONG i=0;i<count;++i){
        items[i]=(PB_DESCRIPTOR){.Token={42,1,(USHORT)i,0},.Length=length,.DataOffset=layout.Queues[0].RxData+i*PB_BLOCK_SIZE};
        UCHAR *bytes=region+items[i].DataOffset;bytes[0]=(UCHAR)(ip==4?0x45:0x60);bytes[ip==4?9:6]=(UCHAR)protocol;
        ULONG declared=ip==4?length:length-40,at=ip==4?2:4;bytes[at]=(UCHAR)(declared>>8);bytes[at+1]=(UCHAR)declared;slots[i].Active=TRUE;
    }
    memcpy(copied,region+items[0].DataOffset,length);q->CoalesceBusy[0]=count>1;q->Credits=count;
    Failure=failure;Acquisitions=0;AllocationHooks=FreeHooks=PoolHooks=AcquireHooks=ReleaseHooks=0;
    AfterNblAllocate=OnAllocate;BeforeNblFree=OnFree;BeforePoolFree=OnPoolFree;AfterRundownAcquire=OnAcquire;BeforeRundownRelease=OnRelease;
    NET_BUFFER_LIST *nbl=PbV2Receive(q,items,count,count>1?0:MAXULONG,count>1?&coal:NULL);Allocated=nbl;
    CHECK(Acquisitions==(failure?failure:4)&&AllocationHooks==(failure?0u:1u));
    if(failure){CHECK(!nbl&&!Pools&&!Mdls&&!Nbls&&!FreeHooks&&PoolHooks==(failure==1?0u:1u)&&!AcquireHooks&&!ReleaseHooks);}
    else{
        MDL *header=nbl->FirstMdl,*data=header->Next;CHECK(header&&data&&!data->Next&&!header->Partial&&data->Partial==(count==1));
        CHECK(!((ULONG_PTR)header%__alignof(MDL))&&!((ULONG_PTR)data%__alignof(MDL))&&!((ULONG_PTR)nbl%__alignof(NET_BUFFER_LIST)));
#ifdef PB_GUARD_TESTS
        if(InspectGuards)CHECK(GuardLayout(header,sizeof(*header))&&GuardLayout(data,sizeof(*data))&&GuardLayout(nbl,sizeof(*nbl)));
#endif
        PB_RX_RECORD *record=(PB_RX_RECORD *)((ULONG_PTR)nbl->Reserved[1]&~(ULONG_PTR)1);CHECK(record&&record->Count==count);
        record->LocallyCancelled=local;nbl->Status=status;PbV2RetainReceives(&s,1,count);CHECK(PbV2ReturnOne(&ctx,nbl));
        CHECK(FreeHooks==1&&PoolHooks==1&&AcquireHooks==1&&ReleaseHooks==1&&!Pools&&!Mdls&&!Nbls&&!s.Active&&!s.Returns.References);
        CHECK(FixtureRundownAcquires==FixtureRundownReleases&&WaitForSingleObject(s.AllReturned.Native,0)==WAIT_OBJECT_0);
        CHECK(!q->Credits&&!q->PendingCount&&!q->CoalesceBusy[0]&&s.RxReturned==count&&s.Completions==count);
        PB_DESCRIPTOR output[8];uint32_t received;CHECK(PbQueuePop(&complete,output,8,&received)==PB_OK&&received==count);
        for(ULONG i=0;i<count;++i)CHECK(!slots[i].Active&&output[i].Token.Slot==i&&output[i].Status==(ULONG)(local?PB_STOPPED:status?PB_RX_REJECTED:PB_OK));
    }
    AfterNblAllocate=NULL;BeforeNblFree=NULL;BeforePoolFree=NULL;AfterRundownAcquire=NULL;BeforeRundownRelease=NULL;Allocated=NULL;Session=NULL;Failure=0;
    CHECK(CloseHandle(kernel.Native)&&CloseHandle(user.Native)&&CloseHandle(s.AllReturned.Native));free(copied);_aligned_free(region);++Scenarios;
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    BOOLEAN control=argc==2;if(control)CHECK(!strcmp(argv[1],"1"));InspectGuards=TRUE;DirectObjects(control);
    if(control){
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
        if(RejectOld){puts("KERNEL_MDL_NBL_OBJECTS_LACK_GUARD_COVERAGE");return 95;}
        printf("PASS: hosted object guard control; caught access violations=%lu (ordinary mode performs no invalid probes)\n",Faults);return 0;
    }
    const ULONG statuses[]={0,0x00010003u,0xc0000001u};
    for(ULONG repeat=0;repeat<3;++repeat)for(ULONG format=0;format<4;++format)for(ULONG fail=0;fail<=4;++fail)
        for(ULONG local=0;local<2;++local)for(ULONG status=0;status<_countof(statuses);++status)Case(format,fail,(BOOLEAN)local,statuses[status]);
    CHECK(!Pools&&!Mdls&&!Nbls&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu hosted MDL/NBL scenarios, %lu exact-address read/write access violations; actual allocation/return/free chain, plain4/plain6/RSC4/URO6, failure ordinals 1..4, hook order/counts, alignment, completions and resource/handle balance; no native DDI execution\n",Scenarios,Faults);return 0;
}
