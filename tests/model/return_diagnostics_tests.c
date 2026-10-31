/* SPDX-License-Identifier: GPL-2.0 */
/* Actual query and formatter, bounded history and concurrent writers/readers. */
#ifndef PB_SOURCE_FINGERPRINT
#define PB_SOURCE_FINGERPRINT 0000000000000000000000000000000000000000000000000000000000000000
#endif
#include "kernel_nbl_fixture.h"
#include "../../include/packetbridge_version.h"
typedef LONG NTSTATUS;
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023L)
#endif
#include "../../driver/v2/return_diagnostic_query.h"
#include "../../tools/probe/return_diagnostics.h"

static PB_RETURN_EVENT Event(ULONG ordinal,ULONG origin,ULONG status)
{
    PB_RETURN_EVENT e={.FirstToken={42,ordinal,(USHORT)(ordinal%32),0},.NblStatus=status,
        .CompletionStatus=origin==PB_RETURN_ORIGIN_LOCAL_CANCEL?PB_STOPPED:PB_CORRUPT,
        .Origin=origin,.OriginalCount=ordinal%3+1,.IndicationLength=64,.IpDeclaredLength=64,.IpVersion=4,.Protocol=17,.ReceiveType=PB_DESC_PLAIN};
    e.OriginalBytes=e.OriginalCount*64; return e;
}
static VOID CheckSnapshot(const PB_RETURN_SNAPSHOT *s)
{
    CHECK(s->Size==sizeof(*s) && s->Magic==PB_RETURN_DIAG_MAGIC && s->Schema==PB_RETURN_DIAG_SCHEMA && s->FileVersion==PB_VERSION_PACKED);
    CHECK(s->RecordCount==(s->LatestSequence<64?s->LatestSequence:64) && s->RecordCapacity==64);
    CHECK(!strcmp(s->SourceFingerprint,PB_SOURCE_STRING));
    for(ULONG i=0;i<s->RecordCount;++i) {
        const PB_RETURN_EVENT *e=s->Records+i;
        CHECK(e->Sequence==s->LatestSequence-s->RecordCount+1+i && e->IpDeclaredLength==e->IndicationLength && e->FirstToken.Epoch==42 && e->FirstToken.Generation);
    }
    for(ULONG i=s->RecordCount;i<64;++i) {
        const UCHAR *bytes=(const UCHAR *)(s->Records+i);
        for(SIZE_T j=0;j<sizeof(*s->Records);++j) CHECK(!bytes[j]);
    }
    for(ULONG i=0;i<7;++i) CHECK(!s->Padding[i]);
}
static VOID Query(TUN_CTX *ctx,PB_RETURN_SNAPSHOT *out)
{
    memset(out,0xcc,sizeof(*out)); ULONG_PTR written=99;
    CHECK(PbQueryReturnDiagnostics(ctx,out,sizeof(*out),0,&written)==STATUS_SUCCESS && written==sizeof(*out));
    CheckSnapshot(out);
}
static VOID BuffersAndWrap(VOID)
{
    TUN_CTX *ctx=calloc(1,sizeof(*ctx)); CHECK(ctx);
#ifdef PB_GUARD_TESTS
    PB_RETURN_SNAPSHOT *out=PbTestGuardAlloc(sizeof(*out));
#else
    PB_RETURN_SNAPSHOT *out=malloc(sizeof(*out)); CHECK(out);
#endif
    memset(out,0xcc,sizeof(*out)); ULONG_PTR written=99; FixtureDiagnosticLocks=0;
    CHECK(PbQueryReturnDiagnostics(ctx,out,sizeof(*out)-1,0,&written)==STATUS_BUFFER_TOO_SMALL && !written);
    CHECK(((UCHAR *)out)[0]==0xcc && !FixtureDiagnosticLocks);
    CHECK(PbQueryReturnDiagnostics(ctx,out,sizeof(*out),1,&written)==STATUS_INVALID_PARAMETER && !written && !FixtureDiagnosticLocks);
    CHECK(PbQueryReturnDiagnostics(ctx,NULL,sizeof(*out),0,&written)==STATUS_BUFFER_TOO_SMALL && !written && !FixtureDiagnosticLocks);
    Query(ctx,out); CHECK(!out->RecordCount && FixtureDiagnosticLocks==1);
    ULONG64 ndis=0,packets=0,cancelled=0;
    for(ULONG i=1;i<=130;++i) {
        ULONG origin=i%3?PB_RETURN_ORIGIN_NDIS:PB_RETURN_ORIGIN_LOCAL_CANCEL;
        PB_RETURN_EVENT e=Event(i,origin,i%2?0x00010003u:0xc0000001u);
        PbRecordReturnEvent(&ctx->Probe.ReturnDiagnostics,&e);
        if(origin==PB_RETURN_ORIGIN_NDIS) {ndis++;packets+=e.OriginalCount;} else cancelled+=e.OriginalCount;
    }
    Query(ctx,out); CHECK(out->RecordCount==64 && out->LatestSequence==130 && out->Records[0].Sequence==67);
    CHECK(out->NdisNonzeroNbls==ndis && out->NdisNonzeroPackets==packets && out->LocalCancelledPackets==cancelled);
    for(ULONG i=0;i<64;++i) CHECK(out->Records[i].NblStatus==(out->Records[i].Sequence%2?0x00010003u:0xc0000001u));
    ctx->Probe.ReturnDiagnostics.NdisNonzeroPackets=UINT64_MAX;
    ctx->Probe.ReturnDiagnostics.Sequence=UINT64_MAX;
    PB_RETURN_EVENT e=Event(7,PB_RETURN_ORIGIN_NDIS,0xffffffffu);
    PbRecordReturnEvent(&ctx->Probe.ReturnDiagnostics,&e);
    CHECK(ctx->Probe.ReturnDiagnostics.NdisNonzeroPackets==UINT64_MAX && ctx->Probe.ReturnDiagnostics.Sequence==UINT64_MAX);
#ifdef PB_GUARD_TESTS
    PbTestGuardFree(out);
#else
    free(out);
#endif
    free(ctx);
    puts("PASS: actual diagnostic query rejects short/null/input buffers before locking, zeros unused output, preserves raw statuses, returns the latest 64 records after overwrite, and saturates counters.");
}
static TUN_CTX Concurrent;
static HANDLE Start;
static volatile LONG Done,Reads;
static DWORD WINAPI Writer(VOID *opaque)
{
    ULONG queue=(ULONG)(ULONG_PTR)opaque; CHECK(WaitForSingleObject(Start,5000)==WAIT_OBJECT_0);
    for(ULONG i=1;i<=1024;++i) {
        PB_RETURN_EVENT e=Event(i,PB_RETURN_ORIGIN_NDIS,queue?0xc0000001u:0x00010003u);e.FirstToken.Queue=(USHORT)queue;
        KIRQL irql;KeAcquireSpinLock(&Concurrent.Probe.ReturnDiagnosticsLock,&irql);
        PbRecordReturnEvent(&Concurrent.Probe.ReturnDiagnostics,&e);
        KeReleaseSpinLock(&Concurrent.Probe.ReturnDiagnosticsLock,irql);
    }
    InterlockedIncrement(&Done);return 0;
}
static DWORD WINAPI Reader(VOID *opaque)
{
    (void)opaque;PB_RETURN_SNAPSHOT out;
    CHECK(WaitForSingleObject(Start,5000)==WAIT_OBJECT_0);
    do {Query(&Concurrent,&out);InterlockedIncrement(&Reads);SwitchToThread();} while(InterlockedCompareExchange(&Done,0,0)<2);
    return 0;
}
static VOID ConcurrentSnapshots(VOID)
{
    Start=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Start);
    HANDLE threads[3]={CreateThread(NULL,0,Writer,NULL,0,NULL),CreateThread(NULL,0,Writer,(VOID *)1,0,NULL),CreateThread(NULL,0,Reader,NULL,0,NULL)};
    CHECK(threads[0] && threads[1] && threads[2] && SetEvent(Start));
    CHECK(WaitForMultipleObjects(3,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(ULONG i=0;i<3;++i) CHECK(CloseHandle(threads[i]));CHECK(CloseHandle(Start));
    PB_RETURN_SNAPSHOT out;Query(&Concurrent,&out);
    CHECK(Reads && Done==2 && out.LatestSequence==2048 && out.RecordCount==64 && out.NdisNonzeroNbls==2048 && !out.LocalCancelledPackets);
    puts("PASS: two writers and a concurrent actual query produce consistent copied snapshots through 2048 exceptional returns with balanced handles.");
}
int main(int argc,char **argv)
{
    if(argc==2) {
        TUN_CTX *ctx=calloc(1,sizeof(*ctx));CHECK(ctx);
        BOOLEAN protocolCase=!strcmp(argv[1],"--protocol-rejected-json");
        for(ULONG i=1;i<=(protocolCase?2u:3u);++i) {
            PB_RETURN_EVENT e=Event(i,i==3?PB_RETURN_ORIGIN_LOCAL_CANCEL:PB_RETURN_ORIGIN_NDIS,
                protocolCase?0xc000023eu:i==1?0x00010003u:i==2?0xc0000001u:0xc023002au);
            if(protocolCase) {
                e.OriginalCount=1;e.OriginalBytes=e.IndicationLength=e.IpDeclaredLength=i==1?576u:1280u;
                e.IpVersion=i==1?4u:6u;e.Protocol=i==1?1u:58u;
            }
            PbRecordReturnEvent(&ctx->Probe.ReturnDiagnostics,&e);
        }
        PB_RETURN_SNAPSHOT out;Query(ctx,&out);free(ctx);
        if(!strcmp(argv[1],"--bad-count")) out.RecordCount=65;
        else if(!strcmp(argv[1],"--bad-source")) out.SourceFingerprint[0]='"';
        else if(!strcmp(argv[1],"--bad-sequence")) out.Records[0].Sequence=7;
        else if(!strcmp(argv[1],"--unknown-origin")) out.Records[0].Origin=99;
        else if(!strcmp(argv[1],"--bad-record")) out.Records[0].OriginalCount=0;
        else if(!strcmp(argv[1],"--different-build")) out.FileVersion=1;
        else CHECK(!strcmp(argv[1],"--json") || protocolCase);
        return PbPrintReturnSnapshot(&out);
    }
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    BuffersAndWrap();ConcurrentSnapshots();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    return 0;
}
