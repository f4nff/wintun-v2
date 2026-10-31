/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/test.h"
#include "../../api/v2/lifecycle.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
PB_RESULT PbTestOpen(const PB_CONFIG *, PB_SESSION **, void **, PB_LAYOUT *, uint64_t *);
void PbTestActive(PB_SESSION *, uint32_t);
void PbTestFault(PB_SESSION *);
uint32_t PbTestWaiters(PB_SESSION *);
_Static_assert(sizeof(PB_INTERFACE_V2) == 280, "C function table ABI");
_Static_assert(sizeof(PB_PACKET_INTERFACE) == 32, "C checked packet extension ABI");
_Static_assert(sizeof(PB_LEASE) == 80, "C lease ABI");
static PB_INTERFACE_V2 Api;

typedef struct FIXTURE { PB_SESSION *Session; PB_USER_QUEUE *Queue; void *Region; PB_LAYOUT Layout; uint64_t Epoch; PB_QUEUE_VIEW Driver[4]; } FIXTURE;
static FIXTURE Create(void)
{
    FIXTURE f = { 0 };
    PB_CONFIG config = { sizeof(config), 1, 4, 4, 0, 0 };
    CHECK(PbTestOpen(&config, &f.Session, &f.Region, &f.Layout, &f.Epoch) == PB_OK);
    CHECK(Api.OpenQueue(f.Session, 0, &f.Queue) == PB_OK);
    for (unsigned r = 0; r < 4; ++r) CHECK(PbAttachQueue(f.Driver + r, f.Region, &f.Layout, 0, r, r == 0 || r == 3) == PB_OK);
    return f;
}
static void TxTests(void)
{
    FIXTURE f = Create();
    PB_DESCRIPTOR items[4] = { 0 };
    for (unsigned i = 0; i < 4; ++i) {
        items[i].Token = (PB_TOKEN){ f.Epoch, 1, (uint16_t)i, 0 }; items[i].Length = 20;
        items[i].DataOffset = f.Layout.Queues[0].TxData + i * PB_BLOCK_SIZE;
        memset((BYTE *)f.Region + items[i].DataOffset, (int)i + 1, 20);
    }
    uint32_t n, notify;
    CHECK(PbQueuePush(&f.Driver[0], items, 4, &n, &notify) == PB_OK && n == 4);
    PB_LEASE leases[4]; CHECK(Api.ReceiveBatch(f.Queue, leases, 4, &n) == PB_OK && n == 4);
    for (unsigned i = 0; i < 4; ++i) CHECK(leases[i].Data[0] == i + 1);
    PB_TOKEN bad[2] = { leases[0].Descriptor.Token, leases[0].Descriptor.Token };
    CHECK(Api.ReleaseBatch(f.Queue, bad, 2, &n) == PB_INVALID && !n);
    PB_TOKEN tokens[4]; for (unsigned i = 0; i < 4; ++i) tokens[i] = leases[3 - i].Descriptor.Token;
    CHECK(Api.PrepareClose(f.Session, 0) == PB_BUSY); /* Held packet views prevent freeing their mapping. */
    CHECK(Api.ReleaseBatch(f.Queue, tokens, 1, &n) == PB_OK && n == 1);
    CHECK(Api.PrepareClose(f.Session, 0) == PB_BUSY); /* A returned prefix preserves the other views. */
    CHECK(Api.ReleaseBatch(f.Queue, tokens + 1, 3, &n) == PB_OK && n == 3);
    CHECK(Api.ReleaseBatch(f.Queue, tokens, 1, &n) == PB_BUSY && !n);
    CHECK(PbQueuePop(&f.Driver[1], items, 4, &n) == PB_OK && n == 4);
    CHECK(items[0].Token.Slot == 3 && items[3].Token.Slot == 0);
    for (unsigned i = 0; i < 4; ++i) {
        PB_DESCRIPTOR expected = { .Token = tokens[i] };
        CHECK(!memcmp(items + i, &expected, sizeof(expected))); /* Preserve zero recycle metadata. */
    }
    CHECK(Api.PrepareClose(f.Session, 1000) == PB_OK);
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
}
static void RxTests(void)
{
    FIXTURE f = Create(); uint32_t n, notify;
    PB_LEASE leases[8];
    CHECK(Api.ReserveBatch(f.Queue, leases, 8, &n) == PB_OK && n == 4);
    PB_LEASE extra; CHECK(Api.ReserveBatch(f.Queue, &extra, 1, &n) == PB_FULL && n == 0);
    for (unsigned i = 0; i < 4; ++i) { leases[i].Descriptor.Length = 20; memset(leases[i].Data, (int)i, 20); }
    leases[3].Descriptor.Length = 65536;
    CHECK(Api.CommitBatch(f.Queue, leases, 4, &n) == PB_INVALID && n == 0);
    PB_DESCRIPTOR items[4]; CHECK(PbQueuePop(&f.Driver[2], items, 4, &n) == PB_EMPTY);
    leases[3].Descriptor.Length = 20;
    CHECK(Api.CommitBatch(f.Queue, leases, 4, &n) == PB_OK && n == 4);
    PB_TOKEN token = leases[0].Descriptor.Token;
    CHECK(Api.CancelBatch(f.Queue, &token, 1) == PB_BUSY); /* A submitted buffer cannot be cancelled. */
    CHECK(PbQueuePop(&f.Driver[2], items, 4, &n) == PB_OK && n == 4);
    PB_DESCRIPTOR complete[4]; for (unsigned i = 0; i < 4; ++i) complete[i] = items[3 - i];
    complete[1].Status = PB_INVALID;
    CHECK(PbQueuePush(&f.Driver[3], complete, 4, &n, &notify) == PB_OK);
    CHECK(Api.PollCompletions(f.Queue, items, 4, &n) == PB_OK && n == 4 && items[1].Status == PB_INVALID);
    CHECK(Api.ReserveBatch(f.Queue, leases, 4, &n) == PB_OK && n == 4);
    CHECK(leases[0].Descriptor.Token.Generation == 2);
    PB_TOKEN tokens[4]; for (unsigned i = 0; i < 4; ++i) tokens[i] = leases[i].Descriptor.Token;
    CHECK(Api.CancelBatch(f.Queue, &token, 1) == PB_STALE);
    CHECK(Api.CancelBatch(f.Queue, tokens, 4) == PB_OK);
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
}
static void QuarantineTests(void)
{
    FIXTURE f = Create();
    PbTestActive(f.Session, 1);
    CHECK(Api.Close(f.Session, 0) == PB_QUARANTINED);
    PB_V2_STATUS status; CHECK(Api.GetStatus(f.Session, &status) == PB_OK && status.ActiveReferences == 1);
    CHECK(Api.Wait(f.Queue, 0) == PB_STOPPED);
    PB_LEASE lease; uint32_t n;
    CHECK(Api.ReserveBatch(f.Queue, &lease, 1, &n) == PB_STOPPED && n == 0);
    PbTestActive(f.Session, 0);
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
}
typedef struct WAIT_CONTEXT { PB_USER_QUEUE *Queue; HANDLE Entered; } WAIT_CONTEXT;
static DWORD WINAPI Waiter(void *opaque)
{
    WAIT_CONTEXT *context = opaque;
    SetEvent(context->Entered);
    CHECK(Api.Wait(context->Queue, INFINITE) == PB_STOPPED);
    return 0;
}
static void WaitCloseTests(void)
{
    FIXTURE f = Create();
    WAIT_CONTEXT context = { f.Queue, CreateEventW(NULL, TRUE, FALSE, NULL) };
    HANDLE thread = CreateThread(NULL, 0, Waiter, &context, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(context.Entered, 1000) == WAIT_OBJECT_0);
    /* Force the waiter to own the queue, then close from another thread. */
    ULONGLONG started = GetTickCount64();
    while (!PbTestWaiters(f.Session)) { CHECK(GetTickCount64() - started < 1000); SwitchToThread(); }
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
    CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0);
    CloseHandle(thread); CloseHandle(context.Entered);
}
static DWORD WINAPI FaultWaiter(void *opaque)
{
    WAIT_CONTEXT *context=opaque; SetEvent(context->Entered);
    for (ULONG i=0;i<8;++i) CHECK(Api.Wait(context->Queue,INFINITE)==PB_STOPPED);
    PB_LEASE lease; uint32_t count;
    CHECK(Api.ReserveBatch(context->Queue,&lease,1,&count)==PB_STOPPED && !count);
    return 0;
}
static void KernelFaultWakeTests(void)
{
    FIXTURE f = Create();
    WAIT_CONTEXT context = { f.Queue, CreateEventW(NULL, TRUE, FALSE, NULL) };
    HANDLE thread = CreateThread(NULL, 0, FaultWaiter, &context, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(context.Entered, 1000) == WAIT_OBJECT_0);
    ULONGLONG started = GetTickCount64();
    while (!PbTestWaiters(f.Session)) { CHECK(GetTickCount64() - started < 1000); SwitchToThread(); }
    PbTestFault(f.Session); /* No user CloseRequested flag: kernel state + wake must interrupt the wait. */
    CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0);
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
    CloseHandle(thread); CloseHandle(context.Entered);
}
static void CrossQueueFaultWakeTests(void)
{
    PB_CONFIG config={sizeof(config),2,4,4,0,0}; PB_SESSION *session; void *region; PB_LAYOUT layout; uint64_t epoch;
    CHECK(PbTestOpen(&config,&session,&region,&layout,&epoch)==PB_OK);
    PB_USER_QUEUE *waiting,*broken; CHECK(Api.OpenQueue(session,0,&waiting)==PB_OK && Api.OpenQueue(session,1,&broken)==PB_OK);
    WAIT_CONTEXT context={waiting,CreateEventW(NULL,TRUE,FALSE,NULL)}; CHECK(context.Entered);
    HANDLE thread=CreateThread(NULL,0,Waiter,&context,0,NULL); CHECK(thread && WaitForSingleObject(context.Entered,1000)==WAIT_OBJECT_0);
    ULONGLONG started=GetTickCount64();
    while(!PbTestWaiters(session)) { CHECK(GetTickCount64()-started<1000); SwitchToThread(); }
    PB_QUEUE_VIEW driver; CHECK(PbAttachQueue(&driver,region,&layout,1,PB_QUEUE_TX_READY,1)==PB_OK);
    PB_DESCRIPTOR bad={.Token={epoch+1,1,0,1},.Length=20,.DataOffset=layout.Queues[1].TxData}; uint32_t n,notify;
    CHECK(PbQueuePush(&driver,&bad,1,&n,&notify)==PB_OK);
    PB_LEASE lease; CHECK(Api.ReceiveBatch(broken,&lease,1,&n)==PB_CORRUPT && !n);
    CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0); /* No kernel fault/event is needed to wake the other queue. */
    CHECK(Api.Wait(broken,0)==PB_CORRUPT && Api.Close(session,1000)==PB_OK);
    CloseHandle(thread); CloseHandle(context.Entered);
}

typedef struct RETURN_CONTEXT { PB_USER_QUEUE *Queue; PB_TOKEN Token; PB_RESULT Result; } RETURN_CONTEXT;
static DWORD WINAPI WrongOwnerReturn(void *opaque)
{
    RETURN_CONTEXT *ctx = opaque; ctx->Result = Api.CancelBatch(ctx->Queue, &ctx->Token, 1); return 0;
}
static void FaultRetirementTests(void)
{
    PB_LIFECYCLE_INTERFACE life; CHECK(PbGetLifecycleInterface(1,sizeof(life),&life)==PB_OK);
    for (uint32_t mode=0;mode<3;++mode) {
        PB_CONFIG config={sizeof(config),2,4,4,0,0}; PB_SESSION *s; void *region; PB_LAYOUT layout; uint64_t epoch;
        CHECK(PbTestOpen(&config,&s,&region,&layout,&epoch)==PB_OK);
        PB_USER_QUEUE *q,*broken; CHECK(Api.OpenQueue(s,0,&q)==PB_OK && Api.OpenQueue(s,mode==0?0:1,&broken)==PB_OK);
        PB_QUEUE_VIEW ready,recycle;
        CHECK(PbAttachQueue(&ready,region,&layout,0,PB_QUEUE_TX_READY,1)==PB_OK);
        CHECK(PbAttachQueue(&recycle,region,&layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
        PB_DESCRIPTOR d={.Token={epoch,1,0,0},.Length=20,.DataOffset=layout.Queues[0].TxData}; uint32_t n,notify;
        CHECK(PbQueuePush(&ready,&d,1,&n,&notify)==PB_OK);
        PB_LEASE received,reserved,ignored;
        CHECK(Api.ReceiveBatch(q,&received,1,&n)==PB_OK && n==1 && Api.ReserveBatch(q,&reserved,1,&n)==PB_OK && n==1);
        PbTestActive(s,1);
        if(mode<2) {
            uint32_t index=mode==0?0:1; PB_QUEUE_VIEW badReady;
            CHECK(PbAttachQueue(&badReady,region,&layout,index,PB_QUEUE_TX_READY,1)==PB_OK);
            /* Preserve the producer cursor when corrupting the already used queue. */
            if(!index) { badReady.Cursor=ready.Cursor; badReady.PeerCursor=ready.PeerCursor; }
            d.Token=(PB_TOKEN){epoch+1,1,1,(uint16_t)index}; d.DataOffset=layout.Queues[index].TxData+PB_BLOCK_SIZE;
            CHECK(PbQueuePush(&badReady,&d,1,&n,&notify)==PB_OK && Api.ReceiveBatch(broken,&ignored,1,&n)==PB_CORRUPT);
        } else InterlockedExchange64((volatile LONG64 *)&recycle.Shared->Consumer,5);
        CHECK(Api.ReleaseBatch(q,&received.Descriptor.Token,1,&n)==PB_OK && n==1);
        PB_TOKEN invalid=reserved.Descriptor.Token; invalid.Generation++;
        CHECK(Api.CancelBatch(q,&invalid,1)==PB_STALE);
        PB_TOKEN duplicates[]={reserved.Descriptor.Token,reserved.Descriptor.Token};
        CHECK(Api.CancelBatch(q,duplicates,2)==PB_INVALID);
        RETURN_CONTEXT ctx={q,reserved.Descriptor.Token,PB_OK}; HANDLE thread=CreateThread(NULL,0,WrongOwnerReturn,&ctx,0,NULL);
        CHECK(thread && WaitForSingleObject(thread,2000)==WAIT_OBJECT_0 && ctx.Result==PB_BUSY && CloseHandle(thread));
        CHECK(Api.CancelBatch(q,&reserved.Descriptor.Token,1)==PB_OK);
        CHECK(Api.CancelBatch(q,&reserved.Descriptor.Token,1)==PB_BUSY);
        CHECK(Api.ReserveBatch(q,&ignored,1,&n)==PB_CORRUPT && Api.ReceiveBatch(q,&ignored,1,&n)==PB_CORRUPT);
        PB_CLOSE_PROGRESS progress; CHECK(life.GetCloseProgress(s,&progress)==PB_OK && !progress.UserLeases);
        CHECK(Api.PrepareClose(s,20)==PB_QUARANTINED); /* Local retirement is not a native barrier. */
        PbTestActive(s,0); CHECK(Api.PrepareClose(s,2000)==PB_OK && Api.Close(s,2000)==PB_OK);
    }
    FIXTURE f=Create(); PB_LEASE lease; uint32_t n,notify;
    CHECK(Api.ReserveBatch(f.Queue,&lease,1,&n)==PB_OK); lease.Descriptor.Length=20;
    CHECK(Api.CommitBatch(f.Queue,&lease,1,&n)==PB_OK);
    PB_DESCRIPTOR completed=lease.Descriptor; completed.Token.Epoch++;
    CHECK(PbQueuePush(&f.Driver[PB_QUEUE_RX_COMPLETE],&completed,1,&n,&notify)==PB_OK);
    PbTestActive(f.Session,1);
    CHECK(Api.PollCompletions(f.Queue,&completed,1,&n)==PB_CORRUPT && !n);
    CHECK(Api.PrepareClose(f.Session,20)==PB_QUARANTINED);
    PbTestActive(f.Session,0);
    CHECK(Api.PrepareClose(f.Session,2000)==PB_OK && Api.Close(f.Session,2000)==PB_OK);
    puts("PASS: same/cross-queue faults and corrupt recycle/completion rings preserve validated retirement, reject stale/duplicate/wrong-owner returns, stop admission and retain the native barrier.");
}
typedef struct { PB_SESSION *Session; PB_USER_QUEUE *Queue; HANDLE Woken; } POLICY_CONTEXT;
static DWORD WINAPI PolicyWorker(void *opaque)
{
    POLICY_CONTEXT *ctx = opaque; CHECK(Api.SetPolicy(ctx->Session, 0) == PB_OK); return 0;
}
static DWORD WINAPI PolicyWaiter(void *opaque)
{
    POLICY_CONTEXT *ctx = opaque;
    CHECK(Api.Wait(ctx->Queue, INFINITE) == PB_OK); CHECK(SetEvent(ctx->Woken));
    CHECK(Api.Wait(ctx->Queue, INFINITE) == PB_STOPPED); return 0;
}
static void PolicyWaitProgressTests(void)
{
    FIXTURE f = Create();
    HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL), gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    POLICY_CONTEXT ctx = { f.Session, f.Queue, CreateEventW(NULL, TRUE, FALSE, NULL) }; CHECK(entered && gate && ctx.Woken);
    PbTestBlock(f.Session, PB_TEST_POLICY, entered, gate);
    HANDLE waiter = CreateThread(NULL, 0, PolicyWaiter, &ctx, 0, NULL); CHECK(waiter);
    ULONGLONG start = GetTickCount64();
    while (!PbTestWaiters(f.Session)) { CHECK(GetTickCount64() - start < 2000); SwitchToThread(); }
    HANDLE policy = CreateThread(NULL, 0, PolicyWorker, &ctx, 0, NULL); CHECK(policy);
    /* Policy must reach the I/O boundary while a queue remains idle in Wait(INFINITE). */
    CHECK(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0);
    CHECK(Api.Wake(f.Session, 0) == PB_OK);
    CHECK(WaitForSingleObject(ctx.Woken, 2000) == WAIT_OBJECT_0); /* Policy is still deliberately blocked. */
    CHECK(SetEvent(gate) && WaitForSingleObject(policy, 2000) == WAIT_OBJECT_0);
    CHECK(Api.Drain(f.Session) == PB_OK && WaitForSingleObject(waiter, 2000) == WAIT_OBJECT_0);
    CHECK(Api.Close(f.Session, 1000) == PB_OK);
    CloseHandle(policy); CloseHandle(waiter); CloseHandle(entered); CloseHandle(gate); CloseHandle(ctx.Woken);
}
static void PacketInterfaceTests(void)
{
    PB_PACKET_INTERFACE packet;
    uint8_t guarded[48];memset(guarded,0xa5,sizeof(guarded));
    CHECK(PbGetPacketInterface(1,16,(void *)guarded)==PB_OK);
    for(uint32_t i=16;i<sizeof(guarded);++i) CHECK(guarded[i]==0xa5);
    CHECK(PbGetPacketInterface(1,sizeof(guarded),(void *)guarded)==PB_OK);
    for(uint32_t i=sizeof(packet);i<sizeof(guarded);++i) CHECK(guarded[i]==0xa5);
    CHECK(PbGetPacketInterface(2,sizeof(packet),&packet)==PB_UNSUPPORTED);
    CHECK(PbGetPacketInterface(1,15,&packet)==PB_UNSUPPORTED);
    CHECK(PbGetPacketInterface(1,sizeof(packet),NULL)==PB_UNSUPPORTED);
    CHECK(PbGetPacketInterface(1,sizeof(packet),&packet)==PB_OK);
    CHECK(packet.Size==sizeof(packet) && packet.Major==1 && packet.Minor==PB_PACKET_ABI_MINOR &&
        (packet.Capabilities&PB_PACKET_REQUIRED_CAPS)==PB_PACKET_REQUIRED_CAPS);
    PB_CONFIG config={sizeof(config),1,4,4,0,PB_OPEN_EXPERIMENTAL_OFFLOADS};
    PB_SESSION *session=(void *)1;void *region;PB_LAYOUT layout;uint64_t epoch;
    CHECK(PbTestOpen(&config,&session,&region,&layout,&epoch)==PB_UNSUPPORTED && !session);
    config.Flags|=PB_OPEN_CHECKED_SEGMENTS;
    CHECK(PbTestOpen(&config,&session,&region,&layout,&epoch)==PB_OK);
    CHECK(Api.Close(session,1000)==PB_OK);
    uint8_t source[64]={0},output[64];source[0]=0x45;source[9]=6;source[32]=0x50;
    PB_SEGMENT_PLAN plan;uint32_t written;
    CHECK(Api.PlanSegments(source,sizeof(source),PB_SEGMENT_TCP,12,&plan)==PB_PACKET_OK);
    CHECK(Api.WriteSegment(source,&plan,0,output,sizeof(output),&written)==PB_PACKET_UNSUPPORTED && !written);
    PB_SEGMENT_OUTPUT item={output,sizeof(output),0};
    CHECK(Api.WriteSegmentBatch(source,&plan,0,&item,1,&written)==PB_PACKET_UNSUPPORTED && !written && !item.Length);
    CHECK(packet.WriteSegment(source,sizeof(source),&plan,0,output,sizeof(output),&written)==PB_PACKET_OK && written==52);
    plan.PseudoDestinationOffset=0;memset(output,0xa5,sizeof(output));item.Length=UINT32_MAX;
    CHECK(Api.WriteSegment(source,&plan,0,output,sizeof(output),&written)==PB_PACKET_UNSUPPORTED && !written);
    CHECK(Api.WriteSegmentBatch(source,&plan,0,&item,1,&written)==PB_PACKET_UNSUPPORTED && !written && item.Length==UINT32_MAX);
    plan.PseudoDestinationOffset=16;plan.SourceLength=76;plan.PayloadLength=36;plan.SegmentCount=3;
    CHECK(packet.WriteSegment(source,sizeof(source),&plan,2,output,sizeof(output),&written)==PB_PACKET_INVALID && !written);
    CHECK(packet.WriteSegmentBatch(source,sizeof(source),&plan,2,&item,1,&written)==PB_PACKET_INVALID && !written && item.Length==UINT32_MAX);
    for(uint32_t i=0;i<sizeof(output);++i) CHECK(output[i]==0xa5);
    puts("PASS: packet extension prefix/capability guards, retired V2 writer slots, source/plan mismatch rejection and zero output mutation.");
}
int main(void)
{
    CHECK(PbGetInterfaceV2(1, sizeof(Api), &Api) == PB_UNSUPPORTED);
    CHECK(PbGetInterfaceV2(2, sizeof(Api) - 1, &Api) == PB_UNSUPPORTED);
    CHECK(PbGetInterfaceV2(2, sizeof(Api), &Api) == PB_OK);
    CHECK(Api.Major == 2 && Api.Minor == PB_INTERFACE_V2_MINOR && Api.Size == sizeof(Api));
    PacketInterfaceTests(); TxTests(); RxTests(); QuarantineTests(); WaitCloseTests(); KernelFaultWakeTests(); CrossQueueFaultWakeTests(); FaultRetirementTests(); PolicyWaitProgressTests();
    puts("PASS: policy reaches I/O during idle Wait(INFINITE), Wake and waiter progress while policy I/O is blocked.");
    puts("PASS: API batch receive/release, reserve/cancel/commit, out-of-order completion, quarantine retry, Wait(INFINITE) cancellation, eight repeated kernel-stop waits and cross-queue fatal wake.");
    return 0;
}
