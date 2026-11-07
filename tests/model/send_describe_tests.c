/* SPDX-License-Identifier: GPL-2.0 */
/* Actual Read -> Describe -> Send; the existing send fixture supplies the
 * bounded lock/queue ledger. These are hosted MDLs, not native NDIS objects. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../common/packet.h"
#include "../../include/packetbridge_v2_abi.h"
#include "../../include/packetbridge_version.h"
#ifdef PB_API_TESTS
#error Send Describe requires shipping kernel paths
#endif
#ifndef NDEBUG
#error Send Describe requires NDEBUG build configuration
#endif
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"Send Describe requires shipping source fingerprint");
#define SD_CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static LONG HeapObjects,RegionObjects,SourceMappings;
static VOID *OwnedRegion;
static ULONG HostedNdisVersion,FutureInfoReads,FailedBytes;
static uint64_t FailedOffset;
static PB_TOKEN FailedToken;
static VOID *TrackedCalloc(SIZE_T count,SIZE_T size)
{
    SD_CHECK(count && size && count<=SIZE_MAX/size);VOID *p;
#ifdef PB_GUARD_TESTS
    p=PbTestGuardAlloc(count*size);memset(p,0,count*size);
#else
    p=calloc(count,size);
#endif
    SD_CHECK(p);++HeapObjects;return p;
}
static VOID TrackedFree(VOID *p)
{
    SD_CHECK(p && HeapObjects>0);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    free(p);
#endif
    --HeapObjects;
}
static VOID *AllocateRegion(SIZE_T size)
{
    SD_CHECK(!RegionObjects && !OwnedRegion);
#ifdef PB_GUARD_TESTS
    OwnedRegion=PbTestGuardAlloc(size);memset(OwnedRegion,0,size);
#else
    OwnedRegion=VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#endif
    SD_CHECK(OwnedRegion);++RegionObjects;return OwnedRegion;
}
static VOID ReleaseRegion(VOID *p)
{
    SD_CHECK(p==OwnedRegion && RegionObjects==1);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(p);
#else
    SD_CHECK(VirtualFree(p,0,MEM_RELEASE));
#endif
    OwnedRegion=NULL;--RegionObjects;
}
#define NDIS_STATUS_NOT_SUPPORTED 6
#define NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE 1u
#define NDIS_SUPPORT_NDIS630 1
#include PB_SEND_DESCRIBE_TYPES
#define calloc TrackedCalloc
#define free TrackedFree
#define main UnusedSendMain
#include PB_SEND_DESCRIBE_HOST
#undef main
#undef free
#undef calloc
#ifdef PB_NBL_HOST_CURRENT
#error Send Describe must bind runtime NDIS metadata gates
#endif
static ULONG Cases,Repairs,PartialCopies,SourceAllocs,SourceFrees,FutureReads,AttributionWitnesses;
static BOOL Negative,OldAttribution;
static VOID SourceCreate(MDL *mdls,const UCHAR *frame,ULONG length)
{
    MakeReadChain(mdls,frame,length);
    for(ULONG i=0;i<4;++i)if(mdls[i].Address){++SourceMappings;++SourceAllocs;}
}
static VOID SourceFree(MDL *mdls)
{
    for(ULONG i=0;i<4;++i)if(mdls[i].Address){CHECK(SourceMappings>0);--SourceMappings;++SourceFrees;}
    FreeReadChain(mdls);
}
static VOID PopAndRetire(ULONG queue,PB_DESCRIPTOR *items,ULONG expected)
{
    uint32_t count,accepted,notify;CHECK(expected && expected<=4);
    CHECK(PbQueuePop(Consumers+queue,items,4,&count)==PB_OK && count==expected);
    CHECK(PbQueuePush(Recyclers+queue,items,count,&accepted,&notify)==PB_OK && accepted==count);
    PB_KERNEL_QUEUE *q=Session->Queues+queue;KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&q->TxLock,&lock);CHECK(PbV2RecycleLocked(q)==PB_OK);KeReleaseInStackQueuedSpinLock(&lock);
}
static VOID Scenario(ULONG queues,ULONG ip,ULONG version,ULONG kind)
{
    /* kinds4/5 are defensive malformed-current-offset/short-chain inputs.
     * Other shapes represent readable chains or a legitimate mapping failure. */
    BOOL failure=(kind>=1 && kind<=5) || kind==8,payload=kind==3 || kind==5;
    CHECK(kind<9 && (!Negative || kind==3) && (!OldAttribution || kind==1));Setup(queues,failure?2:4,8);
    RealEthernet=RealMdl=TRUE;HostedNdisVersion=version;FutureInfoReads=FailedBytes=0;FailedOffset=UINT64_MAX;memset(&FailedToken,0,sizeof(FailedToken));
    UCHAR frames[4][1500];MDL mdls[4][4],boundary,view;NET_BUFFER nbs[4];memset(nbs,0,sizeof(nbs));
    for(ULONG i=0;i<4;++i){OrdinaryFrame(frames[i],1500,ip,1,FALSE);frames[i][100]=(UCHAR)(0x70+i);
        if(kind==8 && i==1)(void)EthernetFrame(frames[i],3);SourceCreate(mdls[i],frames[i],1500);
        nbs[i]=(NET_BUFFER){.Length=1500,.Frame=frames[i],.Mdl=mdls[i],.MdlOffset=7};}
    if(kind==1)mdls[1][0].Failed=TRUE;
    if(kind==2)mdls[1][2].Failed=TRUE;
    if(kind==3 || kind==8)mdls[1][3].Failed=TRUE;
    if(kind==4)nbs[1].MdlOffset=mdls[1][0].Length+1;
    if(kind==5)mdls[1][2].Next=NULL;
    if(kind==6 || kind==7){
        /* A current offset exactly at a descriptor end, or an empty current
         * descriptor, must advance without mapping either exhausted node. */
        view=mdls[1][0];view.Address+=7;view.Length-=7;boundary=mdls[1][0];boundary.Next=&view;
        if(kind==6)nbs[1].MdlOffset=boundary.Length;else{boundary.Length=0;boundary.Address=NULL;nbs[1].MdlOffset=0;}
        nbs[1].Mdl=&boundary;
    }
    nbs[0].Next=nbs+1;nbs[1].Next=nbs+2;
    NET_BUFFER_LIST lists[2]={{.Nb=nbs,.Next=lists+1},{.Nb=nbs+3}};
    /* Full hosted array is retained for fixture ABI. The observer rejects any
     * evaluation of its nonexistent old-kernel USO field before dereference. */
    if(version<0x60053){lists[0].Info[1]=(PVOID)(ULONG_PTR)0xabababab;lists[1].Info[1]=(PVOID)(ULONG_PTR)0xcdcdcdcd;}
    ULONG index=ActualSelectQueue(frames[0]+14,1486,queues);CHECK(PbV2Send(Current,lists));
    CHECK(lists[0].Status==(failure?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(lists[1].Status==(Negative?NDIS_STATUS_RESOURCES:NDIS_STATUS_SUCCESS));
    CHECK(Completed==2 && AfterPinRelease==1 && !Faults && !Session->Active && !Session->Returns.Value && Session->AllReturned.Value);
    CHECK(Reserves==(failure?(payload?3u:2u):4u) && Describes==(failure?(payload?3u:2u):4u)-(Negative?1u:0u));
    if(payload){CHECK(FailedBytes==50 && FailedToken.Slot==1 && FailedToken.Generation==1);++PartialCopies;}
    else CHECK(!FailedBytes && FailedOffset==UINT64_MAX);
    if(failure){ULONG reason=payload?PB_SEND_PAYLOAD_READ:kind==8?PB_SEND_CONTROL_READ:PB_SEND_PREFIX_READ;
        PB_SEND_EVENT *event=Current->Probe.SendDiagnostics.Records;
        CHECK(Current->Probe.SendDiagnostics.Reasons[reason]==1 && Current->Probe.OffloadRejected==(Negative?2:1));
        ULONG expectedOffset=payload?14u:0u,expectedQueue=payload?index:PB_SEND_QUEUE_UNKNOWN;
        if(OldAttribution){CHECK(kind==1 && event->EthernetLength==14 && event->Queue==index);expectedOffset=14;expectedQueue=index;++AttributionWitnesses;}
        CHECK(event->Reason==reason && event->NdisStatus==NDIS_STATUS_RESOURCES && event->PriorNdisStatus==NDIS_STATUS_SUCCESS &&
            event->NetBufferLength==1500 && event->NetBufferScope==2 && event->EthernetLength==expectedOffset &&
            event->Queue==expectedQueue && !event->LsoInfo && !event->UsoInfo && !event->ChecksumInfo);
    }
    CHECK(PrefixBytes==(failure?(kind==2?158u:payload?177u:kind==8?182u:118u):236u));
    PB_KERNEL_QUEUE *q=Session->Queues+index;PB_DESCRIPTOR items[4];ULONG expected=failure?(Negative?1:2):4;
    if(Negative){
        CHECK(payload && q->TxSlots[FailedToken.Slot].State==PB_SLOT_RESERVED && !(q->TxPool.FreeBits[0]&(1u<<FailedToken.Slot)));
        CHECK(Current->Probe.SendDiagnostics.Reasons[PB_SEND_POOL_FULL]==1 && q->TxPoolExhausted);
        /* Only the deliberately omitted owner release is compensated. The
         * original valid prefix is then retired by actual recycle before any
         * subsequent send; no cache/reference/cursor repair is performed. */
        CHECK(ProductionPoolTransition(&q->TxPool,&FailedToken,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_OK);++Repairs;
    }
    CHECK(Session->TxObjects==expected);PopAndRetire(index,items,expected);
    const ULONG good[]={0,3,2,3};
    for(ULONG i=0;i<expected;++i){ULONG source=failure?good[i]:i;
        CHECK(items[i].Length==1486 && items[i].Token.Queue==index && !memcmp(Session->Region+items[i].DataOffset,frames[source]+14,1486));}
    if(payload && !Negative)CHECK(items[1].Token.Slot==FailedToken.Slot && items[1].Token.Generation==2 && items[1].DataOffset==FailedOffset);
    if(Negative){
        CHECK(!q->TxPoolExhausted);nbs[0].Next=nbs+3;nbs[3].Next=NULL;NET_BUFFER_LIST retry={.Nb=nbs};
        if(version<0x60053)retry.Info[1]=(PVOID)(ULONG_PTR)0xefefefef;
        CHECK(PbV2Send(Current,&retry) && retry.Status==NDIS_STATUS_SUCCESS && Completed==3 && AfterPinRelease==2);
        PopAndRetire(index,items,2);CHECK(items[0].Token.Slot==0 && items[0].Token.Generation==2 && items[1].Token.Slot==FailedToken.Slot && items[1].Token.Generation==2);
        CHECK(!memcmp(Session->Region+items[0].DataOffset,frames[0]+14,1486) && !memcmp(Session->Region+items[1].DataOffset,frames[3]+14,1486));
    }
    for(ULONG j=0;j<queues;++j){PB_DESCRIPTOR ignored;uint32_t count;CHECK(PbQueuePop(Consumers+j,&ignored,1,&count)==PB_EMPTY && !count);
        for(ULONG i=0;i<Session->Layout.SlotCount;++i)CHECK(Session->Queues[j].TxSlots[i].State==PB_SLOT_FREE);}
    CHECK(version>=0x60053 || !FutureInfoReads);FutureReads+=FutureInfoReads;
    for(ULONG i=0;i<4;++i)SourceFree(mdls[i]);Destroy();CHECK(!HeapObjects && !RegionObjects && !SourceMappings && !OwnedRegion && !Session);
    ++Cases;printf("CASE: send describe queues%lu ip%lu ndis%x kind%lu partial%u repair%u attribution%u\n",queues,ip,version,kind,(unsigned)payload,(unsigned)Negative,(unsigned)OldAttribution);
}
static VOID Matrix(VOID)
{
    const ULONG queues[]={1,2,8},versions[]={0x60014,0x60059};
    for(ULONG q=0;q<3;++q)for(ULONG ip=4;ip<=6;ip+=2)for(ULONG v=0;v<2;++v)for(ULONG kind=0;kind<9;++kind)Scenario(queues[q],ip,versions[v],kind);
}
int main(int argc,char **argv)
{
    BOOL releaseCase=argc==2 && !strcmp(argv[1],"134"),attributionCase=argc==2 && !strcmp(argv[1],"135");
    BOOL selected=releaseCase || attributionCase;CHECK(argc==1 || selected);DWORD before,after;ULONG kind=attributionCase?1:3;
#ifdef PB_SEND_DESCRIBE_WITHOUT_ROLLBACK
    Negative=TRUE;CHECK(releaseCase);
#endif
#ifdef PB_SEND_DESCRIBE_OLD_ATTRIBUTION
    OldAttribution=TRUE;CHECK(attributionCase);
#endif
    if(selected){Scenario(2,6,0x60014,kind);Scenario(2,6,0x60014,kind);}else Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Scenario(2,6,0x60014,kind);else Matrix();CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    CHECK(SourceAllocs==SourceFrees && !HeapObjects && !RegionObjects && !SourceMappings && !OwnedRegion);
    printf("PASS: %lu actual Read Describe Send cases; partial%lu repair%lu attribution%lu mappings%lu/%lu future_reads%lu handles%lu/%lu; bounded queue ownership and complete payloads\n",Cases,PartialCopies,Repairs,AttributionWitnesses,SourceAllocs,SourceFrees,FutureReads,before,after);
    if(OldAttribution){puts("SEND_DESCRIBE_OLD_NB_ATTRIBUTION: actual frozen Send inherited the preceding NB queue/offset after current prefix mapping failure; complete source/heap/region/reference cleanup before135");return 135;}
    if(Negative){puts("SEND_DESCRIBE_RESERVATION_NOT_RELEASED: omitted rollback retained the failed token; actual single release and prefix recycle preceded subsequent reuse and complete cleanup before134");return 134;}return 0;
}
