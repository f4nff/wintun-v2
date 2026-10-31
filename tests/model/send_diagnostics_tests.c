/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../safety/guard_memory.h"
#include "../../common/send_diagnostics.h"
#include "../../include/packetbridge_version.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
typedef UCHAR KIRQL;
typedef LONG NTSTATUS;
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023L)
typedef struct { struct { KSPIN_LOCK SendDiagnosticsLock; PB_SEND_HISTORY SendDiagnostics; } Probe; } TUN_CTX;
static VOID KeAcquireSpinLock(KSPIN_LOCK *lock,KIRQL *irql) { C_ASSERT(sizeof(*lock)==sizeof(SRWLOCK));AcquireSRWLockExclusive((PSRWLOCK)lock);*irql=0; }
static VOID KeReleaseSpinLock(KSPIN_LOCK *lock,KIRQL irql) { (void)irql;ReleaseSRWLockExclusive((PSRWLOCK)lock); }
#define PB_POOL_PRESSURE_HISTORY_ONLY 1
#include "../../driver/v2/send_diagnostic_query.h"
#include "../../tools/probe/send_diagnostics.h"
#include "../../tools/probe/pool_pressure.h"
static PB_SEND_EVENT Event(ULONG i)
{
    PB_SEND_EVENT event={.Epoch=42,.LsoInfo=0xfedcba9876543210ull,.NdisStatus=i%2?0xc0230015:0xc000009a,
        .PriorNdisStatus=0,.Reason=i%2?PB_SEND_GSO_PLAN:PB_SEND_POOL_FULL,.Queue=0,.NetBufferLength=2800,
        .EthernetLength=14,.PacketStatus=3,.CoreStatus=2,.EffectiveMask=0x1febf,.NetBufferScope=2};
    return event;
}
static VOID Record(TUN_CTX *ctx,const PB_SEND_EVENT *event)
{
    PB_SEND_HISTORY *h=&ctx->Probe.SendDiagnostics;PbRecordSendEvent(h,event);
    if(event->Reason==PB_SEND_POOL_FULL && (h->Reasons[PB_SEND_POOL_FULL]&63u)==1u) {
        PB_POOL_PRESSURE_EVENT p={.Send=*event,.ReadyProducer=10,.ReadyConsumer=8,.ReturnProducer=7,.ReturnConsumer=6,
            .Slots=4,.Outstanding=4,.ReadyUnread=2,.UserHeld=1,.ReturnQueued=1,.Flags=15};
        p.Send.Sequence=h->Sequence;PbRecordPoolPressure(h,&p);
    }
}
static VOID QueryPressure(TUN_CTX *ctx,PB_POOL_PRESSURE_SNAPSHOT *out)
{
    ULONG_PTR written=99;memset(out,0xcc,sizeof(*out));
    CHECK(PbQueryPoolPressure(ctx,out,sizeof(*out),0,&written)==STATUS_SUCCESS && written==sizeof(*out));
    CHECK(PbValidPoolPressure(out) && !strcmp(out->SourceFingerprint,PB_SOURCE_STRING));
    for(ULONG i=out->RecordCount;i<64;++i) for(SIZE_T j=0;j<sizeof(PB_POOL_PRESSURE_EVENT);++j) CHECK(!((UCHAR *)(out->Records+i))[j]);
    for(ULONG i=0;i<sizeof(out->Padding);++i) CHECK(!out->Padding[i]);
    CHECK(!out->CurrentQueueCount && !out->CurrentStatus);
    for(SIZE_T i=0;i<sizeof(out->Current);++i) CHECK(!((UCHAR *)out->Current)[i]);
}
static VOID Query(TUN_CTX *ctx,PB_SEND_SNAPSHOT *out)
{
    ULONG_PTR written=99;memset(out,0xcc,sizeof(*out));
    CHECK(PbQuerySendDiagnostics(ctx,out,sizeof(*out),0,&written)==STATUS_SUCCESS && written==sizeof(*out));
    CHECK(out->RecordCount==(out->LatestSequence<64?out->LatestSequence:64) && out->RecordCapacity==64);
    CHECK(!strcmp(out->SourceFingerprint,PB_SOURCE_STRING));
    for(ULONG i=0;i<out->RecordCount;++i) CHECK(out->Records[i].Sequence==out->LatestSequence-out->RecordCount+i+1);
    for(ULONG i=out->RecordCount;i<64;++i) for(SIZE_T j=0;j<sizeof(PB_SEND_EVENT);++j) CHECK(!((UCHAR *)(out->Records+i))[j]);
    for(ULONG i=0;i<sizeof(out->Padding);++i) CHECK(!out->Padding[i]);
}
static DWORD WINAPI Writer(VOID *opaque)
{
    TUN_CTX *ctx=opaque;
    for(ULONG i=0;i<10000;++i) {PB_SEND_EVENT e=Event(i);KIRQL irql;KeAcquireSpinLock(&ctx->Probe.SendDiagnosticsLock,&irql);
        Record(ctx,&e);KeReleaseSpinLock(&ctx->Probe.SendDiagnosticsLock,irql);}
    return 0;
}
int main(int argc,char **argv)
{
    TUN_CTX *ctx=calloc(1,sizeof(*ctx));CHECK(ctx);
    PB_SEND_SNAPSHOT *out=PbTestGuardAlloc(sizeof(*out));
    PB_POOL_PRESSURE_SNAPSHOT *pressure=PbTestGuardAlloc(sizeof(*pressure));
    if(argc>1 && !strncmp(argv[1],"--pressure-",11)) {
        PB_SEND_EVENT e=Event(0);Record(ctx,&e);QueryPressure(ctx,pressure);
        pressure->CurrentQueueCount=1;pressure->Current[0]=(PB_TX_POOL_STATE){.Epoch=42,.Queue=0,.Slots=4,.PoolSlots=4,
            .Free=1,.Published=3,.CountsValid=1,.CursorFlags=6,.ReadyProducer=12,.ReadyConsumer=10,.ReturnProducer=10,.ReturnConsumer=9,.Running=1};
        if(!strcmp(argv[1],"--pressure-bad-count"))pressure->RecordCount=65;
        else if(!strcmp(argv[1],"--pressure-bad-source"))pressure->SourceFingerprint[0]='"';
        else if(!strcmp(argv[1],"--pressure-bad-sequence"))pressure->Records[0].Sequence=0;
        else if(!strcmp(argv[1],"--pressure-bad-ledger"))pressure->Records[0].UserHeld++;
        else if(!strcmp(argv[1],"--pressure-bad-cursor"))pressure->Records[0].ReadyProducer++;
        else if(!strcmp(argv[1],"--pressure-bad-flags"))pressure->Records[0].Flags|=16;
        else if(!strcmp(argv[1],"--pressure-bad-current-count"))pressure->CurrentQueueCount=9;
        else if(!strcmp(argv[1],"--pressure-bad-current-slots"))pressure->Current[0].Free++;
        else if(!strcmp(argv[1],"--pressure-different-build"))pressure->FileVersion=1;
        else if(!strcmp(argv[1],"--pressure-invalid-observation")) {
            PB_POOL_PRESSURE_EVENT *p=pressure->Records;p->Flags=5;p->ReturnStatus=PB_CORRUPT;
            p->Outstanding=p->ReadyUnread=p->UserHeld=p->ReturnQueued=p->Unaccounted=0;
            memset(pressure->Classes,0,sizeof(pressure->Classes));pressure->Classes[5]=1;
        } else CHECK(!strcmp(argv[1],"--pressure-json"));
        return PbPrintPoolPressure(pressure);
    }
    if(argc>1) {
        PB_SEND_EVENT e=Event(1);PbRecordSendEvent(&ctx->Probe.SendDiagnostics,&e);Query(ctx,out);
        if(!strcmp(argv[1],"--bad-count")) out->RecordCount=65;
        else if(!strcmp(argv[1],"--bad-source")) out->SourceFingerprint[0]='"';
        else if(!strcmp(argv[1],"--bad-sequence")) out->Records[0].Sequence=0;
        else if(!strcmp(argv[1],"--bad-reason")) out->Records[0].Reason=32;
        else if(!strcmp(argv[1],"--different-build")) out->FileVersion=1;
        else CHECK(!strcmp(argv[1],"--json"));
        return PbPrintSendSnapshot(out);
    }
    ULONG_PTR written=1;memset(out,0xcc,sizeof(*out));
    memset(pressure,0xcc,sizeof(*pressure));
    CHECK(PbQueryPoolPressure(ctx,pressure,sizeof(*pressure)-1,0,&written)==STATUS_BUFFER_TOO_SMALL && !written && ((UCHAR *)pressure)[0]==0xcc);
    CHECK(PbQueryPoolPressure(ctx,pressure,sizeof(*pressure),1,&written)==STATUS_INVALID_PARAMETER && !written);
    CHECK(PbQueryPoolPressure(ctx,NULL,sizeof(*pressure),0,&written)==STATUS_BUFFER_TOO_SMALL && !written);
    QueryPressure(ctx,pressure);CHECK(!pressure->RecordCount && !pressure->PoolFullEvents);
    CHECK(PbQuerySendDiagnostics(ctx,out,sizeof(*out)-1,0,&written)==STATUS_BUFFER_TOO_SMALL && !written && ((UCHAR *)out)[0]==0xcc);
    CHECK(PbQuerySendDiagnostics(ctx,out,sizeof(*out),1,&written)==STATUS_INVALID_PARAMETER && !written);
    CHECK(PbQuerySendDiagnostics(ctx,NULL,sizeof(*out),0,&written)==STATUS_BUFFER_TOO_SMALL && !written);
    Query(ctx,out);CHECK(!out->RecordCount);
    for(ULONG i=1;i<=130;++i) {PB_SEND_EVENT e=Event(i);Record(ctx,&e);}
    Query(ctx,out);CHECK(out->LatestSequence==130 && out->RejectionEvents==130 && out->Records[0].Sequence==67);
    CHECK(out->Reasons[PB_SEND_GSO_PLAN]==65 && out->Reasons[PB_SEND_POOL_FULL]==65);
    CHECK(out->Records[0].NdisStatus==0xc0230015 && out->Records[0].LsoInfo==0xfedcba9876543210ull);
    QueryPressure(ctx,pressure);CHECK(pressure->PoolFullEvents==65 && pressure->SampledEvents==2 && pressure->RecordCount==2);
    CHECK(pressure->Records[0].Send.Sequence==2 && pressure->Records[1].Send.Sequence==130 && pressure->Classes[1]==2);
    memset(ctx,0,sizeof(*ctx));HANDLE thread=CreateThread(NULL,0,Writer,ctx,0,NULL);CHECK(thread);
    for(ULONG i=0;i<1000;++i) {Query(ctx,out);QueryPressure(ctx,pressure);}
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));Query(ctx,out);
    CHECK(out->LatestSequence==10000 && out->RejectionEvents==10000);
    QueryPressure(ctx,pressure);CHECK(pressure->LatestSequence==79 && pressure->RecordCount==64 && pressure->Records[0].Sequence==16);
    CHECK(pressure->Records[63].Send.Sequence==9985 && pressure->PoolFullEvents==5000);
    ctx->Probe.SendDiagnostics.Sequence=ctx->Probe.SendDiagnostics.RejectionEvents=UINT64_MAX;
    ctx->Probe.SendDiagnostics.Reasons[PB_SEND_GSO_PLAN]=UINT64_MAX;
    PB_SEND_EVENT e=Event(1);PbRecordSendEvent(&ctx->Probe.SendDiagnostics,&e);
    CHECK(ctx->Probe.SendDiagnostics.Sequence==UINT64_MAX && ctx->Probe.SendDiagnostics.Reasons[PB_SEND_GSO_PLAN]==UINT64_MAX);
    ctx->Probe.SendDiagnostics.PressureSequence=ctx->Probe.SendDiagnostics.PressureSamples=UINT64_MAX;
    PB_POOL_PRESSURE_EVENT p={0};PbRecordPoolPressure(&ctx->Probe.SendDiagnostics,&p);
    CHECK(ctx->Probe.SendDiagnostics.PressureSequence==UINT64_MAX && ctx->Probe.SendDiagnostics.PressureSamples==UINT64_MAX);
    PbTestGuardFree(pressure);PbTestGuardFree(out);free(ctx);
    puts("PASS: independent pressure query bounds/zeroing, raw source status, 1/64 sampling, history overwrite, concurrent coherent copies and saturation; original send ABI unchanged.");
    puts("PASS: send query bounds, zeroed output, raw status/OOB, bounded wrap, reason totals, saturation and concurrent snapshots.");return 0;
}
