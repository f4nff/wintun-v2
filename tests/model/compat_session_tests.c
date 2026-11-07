/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../../api/adapter.h"
#include "../../api/logger.h"
#include "../../api/main.h"
#include "../../api/v2/test.h"
#include "../../api/v2/lifecycle.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s error=%lu\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
#define PB_TUN32_DECLARATIONS
#include "compat_tun32_cases.h"
#undef PB_TUN32_DECLARATIONS
HANDLE ModuleHeap;
static LONG RuntimeUsers, Failure, Acquisitions, RingAllocations, Paths, Pins;
static DWORD InjectedError;
static HANDLE TermTarget,TermWitness,TermEntered,TermRetry,TermWorkerEntered,TermWorkerGate;
static volatile LONG TermArmed,TermStartup,TermFailures,TermBackoffs;
static DWORD TermFailureResult=WAIT_FAILED;
static BOOL WINAPI TestDuplicate(HANDLE thread)
{return DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&TermWitness,SYNCHRONIZE,FALSE,0);}
BOOL MainRuntimeAcquire(void) { InterlockedIncrement(&RuntimeUsers); return TRUE; }
void MainRuntimeRelease(void) { CHECK(InterlockedDecrement(&RuntimeUsers) >= 0);SetLastError(ERROR_GEN_FAILURE); }
DWORD LoggerErrorV(DWORD error, LPCWSTR format, va_list args)
{ (void)format; (void)args; SetLastError(error); return error; }
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level, LPCWSTR format, va_list args)
{ (void)level; (void)format; (void)args; return GetLastError(); }
static BOOL Fails(void) { if(InterlockedIncrement(&Acquisitions)!=Failure)return FALSE;InjectedError=0x2100u+Acquisitions;SetLastError(InjectedError);return TRUE; }
static void *TestSessionAlloc(SIZE_T bytes)
{ if(Fails()){InjectedError=ERROR_NOT_ENOUGH_MEMORY;SetLastError(InjectedError);return NULL;}return LoggerAlloc(L"compat test",HEAP_ZERO_MEMORY,bytes); }
static HANDLE WINAPI TestEvent(LPSECURITY_ATTRIBUTES a, BOOL m, BOOL s, LPCWSTR n)
{ if (Fails()) return NULL; return CreateEventW(a,m,s,n); }
static void *WINAPI TestAlloc(void *p, SIZE_T n, DWORD t, DWORD protect)
{ if (Fails()) return NULL; void *r=VirtualAlloc(p,n,t,protect); if(r) InterlockedIncrement(&RingAllocations); return r; }
static BOOL WINAPI TestFree(void *p, SIZE_T n, DWORD t)
{
    if(TermWitness&&WaitForSingleObject(TermWitness,0)==WAIT_TIMEOUT) {
        fputs("LIVE_WORKER_MAPPING_RELEASED\n",stderr);ExitProcess(44);
    }
    CHECK(VirtualFree(p,n,t)); InterlockedDecrement(&RingAllocations); return TRUE;
}
static WCHAR *TestPath(const WCHAR *p)
{ if (Fails()) { InjectedError=ERROR_NOT_ENOUGH_MEMORY;return NULL; } WCHAR *r=_wcsdup(p); if(r) InterlockedIncrement(&Paths); return r; }
static void TestFreePath(void *p) { if(p) InterlockedDecrement(&Paths); free(p); }
static BOOL WINAPI TestPin(DWORD flags, LPCWSTR address, HMODULE *module)
{ if(Fails()) return FALSE; BOOL r=GetModuleHandleExW(flags,address,module); if(r) InterlockedIncrement(&Pins); return r; }
static BOOL WINAPI TestUnpin(HMODULE m) { InterlockedDecrement(&Pins); return FreeLibrary(m); }
static HANDLE WINAPI TestThread(LPSECURITY_ATTRIBUTES a, SIZE_T stack, LPTHREAD_START_ROUTINE f, void *p, DWORD flags, DWORD *id)
{
    if(Fails())return NULL;HANDLE thread=CreateThread(a,stack,f,p,flags,id);
    if(thread&&TermArmed){TermTarget=thread;CHECK(TestDuplicate(thread));}return thread;
}
static DWORD WINAPI TestTerminationWait(HANDLE object,DWORD timeout)
{
    if(TermArmed&&timeout==INFINITE) {
        if(object==TermTarget&&InterlockedCompareExchange(&TermFailures,0,0)>0) {
            InterlockedDecrement(&TermFailures);SetLastError(ERROR_NOT_ENOUGH_MEMORY);return TermFailureResult;
        }
        if(object!=TermTarget&&InterlockedCompareExchange(&TermStartup,2,1)==1) {
            SetLastError(ERROR_OPERATION_ABORTED);return WAIT_FAILED;
        }
    }
    return WaitForSingleObject(object,timeout);
}
static VOID WINAPI TestTerminationSleep(DWORD delay)
{
    if(TermArmed&&delay==1000) {
        InterlockedIncrement(&TermBackoffs);CHECK(SetEvent(TermEntered)&&WaitForSingleObject(TermRetry,10000)==WAIT_OBJECT_0);
        SetLastError(ERROR_GEN_FAILURE);return;
    }
    Sleep(delay);
}
static PB_RESULT TestOpen(void *, const PB_CONFIG *);
static BOOL WINAPI TestSignal(HANDLE);
#define PB_COMPAT_OPEN(s,c) TestOpen(s,c)
#undef Zalloc
#define Zalloc(bytes) TestSessionAlloc(bytes)
#define CreateEventW TestEvent
#define VirtualAlloc TestAlloc
#define VirtualFree TestFree
#define _wcsdup TestPath
#define free TestFreePath
#define GetModuleHandleExW TestPin
#define FreeLibrary TestUnpin
#define CreateThread TestThread
#define SetEvent TestSignal
#define WaitForSingleObject TestTerminationWait
#define Sleep TestTerminationSleep
#ifndef PB_COMPAT_SESSION_SOURCE
#define PB_COMPAT_CURRENT 1
#define PB_COMPAT_SESSION_SOURCE "../../api/session.c"
#endif
/* Only production legacy code sees this wrapper. The wrapper also checks the
 * two payload address ranges; test injection and token/descriptor copies do
 * not contribute to the bridge's payload-copy ledger. */
#define memcpy Tun32Copy
#include PB_COMPAT_SESSION_SOURCE
#undef memcpy
#undef CreateEventW
#undef VirtualAlloc
#undef VirtualFree
#undef _wcsdup
#undef free
#undef GetModuleHandleExW
#undef FreeLibrary
#undef CreateThread
#undef SetEvent
#undef WaitForSingleObject
#undef Sleep
static TUN_SESSION *RecordedSession;
static PB_INTERFACE_V2 RawApi;
static LONG BatchMode, ReserveCalls, CommitCalls, ReceiveCalls, ReleaseCalls, Cancelled, ReadSignals, WakeSignals;
static volatile LONG ReleasedPackets;
static uint32_t ReserveLimit, CommitLimit, ReceiveLimit, ReleaseLimit;
static HANDLE BatchEntered, BatchGate;
static volatile LONG BatchGateUsed;
static BOOL WINAPI TestSignal(HANDLE event)
{
    TUN_SESSION *s=RecordedSession;
    if(s) {
        if(event==s->Descriptor.Send.TailMoved) InterlockedIncrement(&ReadSignals);
        if(event==s->Wake) InterlockedIncrement(&WakeSignals);
    }
    return SetEvent(event);
}
static PB_RESULT WINAPI RecordedReserve(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    InterlockedIncrement(&ReserveCalls);
    PB_RESULT r=RawApi.ReserveBatch(q,leases,ReserveLimit&&count>ReserveLimit?ReserveLimit:count,done);
    Tun32Record(TUN32_RESERVE,count,*done,r);Tun32AfterReserve(r,*done);return r;
}
static PB_RESULT WINAPI RecordedCommit(PB_USER_QUEUE *q,const PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    InterlockedIncrement(&CommitCalls);
    PB_RESULT r=RawApi.CommitBatch(q,leases,CommitLimit&&count>CommitLimit?CommitLimit:count,done);
    Tun32Record(TUN32_COMMIT,count,*done,r);return r;
}
static PB_RESULT WINAPI RecordedCancel(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count)
{
    InterlockedAdd(&Cancelled,(LONG)count);return RawApi.CancelBatch(q,tokens,count);
}
static PB_RESULT WINAPI RecordedReceive(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    Tun32BeforeReceive();
    if(BatchGate&&!InterlockedExchange(&BatchGateUsed,1)) {
        CHECK(SetEvent(BatchEntered));CHECK(WaitForSingleObject(BatchGate,5000)==WAIT_OBJECT_0);
    }
    InterlockedIncrement(&ReceiveCalls);
    PB_RESULT r=RawApi.ReceiveBatch(q,leases,ReceiveLimit&&count>ReceiveLimit?ReceiveLimit:count,done);
    Tun32Record(TUN32_RECEIVE,count,*done,r);return r;
}
static PB_RESULT WINAPI RecordedRelease(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count,uint32_t *done)
{
    InterlockedIncrement(&ReleaseCalls);
    PB_RESULT r=RawApi.ReleaseBatch(q,tokens,ReleaseLimit&&count>ReleaseLimit?ReleaseLimit:count,done);
    Tun32Record(TUN32_RELEASE,count,*done,r);InterlockedAdd(&ReleasedPackets,(LONG)*done);return r;
}
static PB_RESULT WINAPI RecordedPoll(PB_USER_QUEUE *q,PB_DESCRIPTOR *items,uint32_t count,uint32_t *done)
{
    PB_RESULT r=RawApi.PollCompletions(q,items,count,done);
    Tun32Record(TUN32_POLL,count,*done,r);
    Tun32AfterPoll(*done);
    return r;
}
static PB_SESSION *Native;
static void *Region;
static PB_LAYOUT Layout;
static uint64_t Epoch;
static HANDLE NativeEntered,NativeGate,Observer,NativeThread;
static DWORD WINAPI ObserveNative(void *opaque)
{
    PB_SESSION *native=opaque;
    CHECK(WaitForSingleObject(NativeEntered,10000)==WAIT_OBJECT_0);
    DWORD id=PbTestBoundaryThread(native,PB_TEST_WORKER_START);CHECK(id);
    NativeThread=OpenThread(SYNCHRONIZE,FALSE,id);CHECK(NativeThread);
    CHECK(SetEvent(NativeGate));return 0;
}
static PB_RESULT TestOpen(void *opaque, const PB_CONFIG *config)
{
    TUN_SESSION *s=opaque;
    if(TermWorkerGate)CHECK(SetEvent(TermWorkerEntered)&&WaitForSingleObject(TermWorkerGate,10000)==WAIT_OBJECT_0);
    if(Failure == 100) { SetLastError(ERROR_ACCESS_DENIED); return PB_UNSUPPORTED; }
    PB_RESULT r=PbTestOpen(config,&s->Native,&Region,&Layout,&Epoch); Native=s->Native;
    if(r==PB_OK) {
        if(BatchMode) {
            RawApi=s->Api;RecordedSession=s;
            s->Api.ReserveBatch=RecordedReserve;s->Api.CommitBatch=RecordedCommit;s->Api.CancelBatch=RecordedCancel;
            s->Api.ReceiveBatch=RecordedReceive;s->Api.ReleaseBatch=RecordedRelease;
            if(ReadAcquire(&Tun32.Enabled))s->Api.PollCompletions=RecordedPoll;
        }
        CHECK(!NativeEntered&&!NativeGate&&!Observer&&!NativeThread);
        NativeEntered=CreateEventW(NULL,TRUE,FALSE,NULL);NativeGate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(NativeEntered&&NativeGate);
        PbTestBlock(Native,PB_TEST_WORKER_START,NativeEntered,NativeGate);
        Observer=CreateThread(NULL,0,ObserveNative,Native,0,NULL);CHECK(Observer);
    }
    return r;
}
PB_RESULT PbTestSend(PB_SESSION *,uint32_t,const uint8_t *,uint32_t,uint32_t,uint32_t);
PB_RESULT PbTestTake(PB_SESSION *,uint32_t,PB_DESCRIPTOR *,uint32_t,uint32_t *);
PB_RESULT PbTestComplete(PB_SESSION *,uint32_t,const PB_DESCRIPTOR *,uint32_t);
static TUN_SESSION *Start(void)
{
    WINTUN_ADAPTER adapter={0}; adapter.InterfaceFilename=L"hosted-v2";
    return WintunStartSession(&adapter,WINTUN_MIN_RING_CAPACITY);
}
static void EmptyRuntime(void)
{
    RecordedSession=NULL;
    PB_LIFECYCLE_INTERFACE life; CHECK(PbGetLifecycleInterface(1,sizeof(life),&life)==PB_OK);
    PB_RUNTIME_STATUS status; ULONGLONG end=GetTickCount64()+10000;
    do { CHECK(life.GetRuntimeStatus(&status)==PB_OK); if(!status.Sessions) break; Sleep(1); } while(GetTickCount64()<end);
    CHECK(!status.Sessions && !status.RetainedBytes && !status.Adapters);
    /* Registry removal precedes the callback's final module/pool release.
     * Join the actual private-pool thread instead of sampling that interval. */
    if(Observer) {
        CHECK(WaitForSingleObject(Observer,5000)==WAIT_OBJECT_0&&CloseHandle(Observer));Observer=NULL;
        CHECK(NativeThread&&WaitForSingleObject(NativeThread,5000)==WAIT_OBJECT_0&&CloseHandle(NativeThread));NativeThread=NULL;
        CHECK(CloseHandle(NativeEntered)&&CloseHandle(NativeGate));NativeEntered=NativeGate=NULL;
    }
    CHECK(!RuntimeUsers && !CompatRingBytes && !RingAllocations && !Paths && !Pins);
    PROCESS_HEAP_ENTRY entry={0};
    while(HeapWalk(ModuleHeap,&entry)) CHECK(!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY));
}
static void Take(PB_DESCRIPTOR *d,uint32_t wanted)
{
    uint32_t total=0,n; ULONGLONG end=GetTickCount64()+5000;
    do {
        PB_RESULT r=PbTestTake(Native,0,d+total,wanted-total,&n);
        CHECK(r==PB_OK||r==PB_EMPTY); total+=n; if(total==wanted) return; Sleep(1);
    } while(GetTickCount64()<end);
    CHECK(total==wanted);
}
static BYTE *Receive(TUN_SESSION *s,DWORD *size)
{
    ULONGLONG end=GetTickCount64()+5000; BYTE *p;
    do { p=WintunReceivePacket(s,size); if(p) return p; CHECK(GetLastError()==ERROR_NO_MORE_ITEMS); WaitForSingleObject(WintunGetReadWaitEvent(s),10); } while(GetTickCount64()<end);
    CHECK(FALSE); return NULL;
}
#ifdef PB_COMPAT_CURRENT
#include "compat_pump_cases.h"
#endif
#include "compat_tun32_cases.h"
#include "compat_terminal32_cases.h"
static void Batches(BOOL partial,BOOL historical)
{
    BatchMode=1;ReserveCalls=CommitCalls=ReceiveCalls=ReleaseCalls=Cancelled=ReadSignals=WakeSignals=ReleasedPackets=0;
    ReserveLimit=partial?7:0;CommitLimit=partial?3:0;ReceiveLimit=partial?11:0;ReleaseLimit=partial?2:0;
    BatchEntered=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGate=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGateUsed=0;
    CHECK(BatchEntered&&BatchGate);TUN_SESSION *s=Start();CHECK(s);
    CHECK(WaitForSingleObject(BatchEntered,5000)==WAIT_OBJECT_0);
    BYTE *out[64],*in[64];
    for(uint32_t i=0;i<64;++i) {
        BYTE bytes[32];memset(bytes,(BYTE)i,sizeof(bytes));
        CHECK(PbTestSend(Native,0,bytes,sizeof(bytes),0,0)==PB_OK);
        out[i]=WintunAllocateSendPacket(s,sizeof(bytes));CHECK(out[i]);memcpy(out[i],bytes,sizeof(bytes));
    }
    for(unsigned i=64;i--;)WintunSendPacket(s,out[i]);
    CHECK(WakeSignals==1);CHECK(SetEvent(BatchGate));
    PB_DESCRIPTOR d[64];Take(d,64);
    for(unsigned i=0;i<64;++i) {
        CHECK(d[i].Length==32);for(unsigned j=0;j<32;++j)CHECK(((BYTE *)Region)[d[i].DataOffset+j]==i);
        DWORD size;in[i]=Receive(s,&size);CHECK(size==32);
        for(unsigned j=0;j<32;++j)CHECK(in[i][j]==i);
    }
    CHECK(PbTestComplete(Native,0,d,64)==PB_OK);
    ULONGLONG end=GetTickCount64()+5000;
    while(ReadAcquire(&ReleasedPackets)!=64&&GetTickCount64()<end)Sleep(1);
    CHECK(ReleasedPackets==64);
    LONG wakes=ReadAcquire(&WakeSignals);
    for(unsigned i=64;i--;) {
        WintunReleaseReceivePacket(s,in[i]);
        if(i)CHECK(s->Descriptor.Send.Ring->Head==0);
    }
    CHECK(s->Descriptor.Send.Ring->Head==s->Send.Head);
    CHECK(WakeSignals-wakes==(historical?64:1));
    if(partial)CHECK(Cancelled>0&&ReserveCalls>1&&CommitCalls>1&&ReleaseCalls>1);
    else CHECK(ReserveCalls==(historical?64:1)&&CommitCalls==(historical?64:1)&&ReleaseCalls==(historical?64:1)&&ReadSignals==(historical?64:1));
    printf("PASS: %s exact 64-packet duplex burst; reserve=%ld commit=%ld release=%ld readable=%ld return-wake=%ld cancelled=%ld.\n",
        partial?"partial-prefix":"full-prefix",ReserveCalls,CommitCalls,ReleaseCalls,ReadSignals,WakeSignals-wakes,Cancelled);
    WintunEndSession(s);EmptyRuntime();BatchMode=0;
    CHECK(CloseHandle(BatchEntered)&&CloseHandle(BatchGate));BatchEntered=BatchGate=NULL;
    ReserveLimit=CommitLimit=ReceiveLimit=ReleaseLimit=0;
}
static void BlockedBatch(void)
{
    BatchMode=1;BatchGateUsed=0;ReceiveCalls=ReleaseCalls=ReleasedPackets=0;
    BatchEntered=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(BatchEntered&&BatchGate);
    TUN_SESSION *s=Start();CHECK(s&&WaitForSingleObject(BatchEntered,5000)==WAIT_OBJECT_0);
    BYTE *packet=calloc(1,65535);CHECK(packet);
    for(unsigned i=0;i<64;++i)CHECK(PbTestSend(Native,0,packet,65535,0,0)==PB_OK);
    CHECK(SetEvent(BatchGate));
    ULONGLONG end=GetTickCount64()+5000;
    while(ReadAcquire(&ReleasedPackets)!=1&&GetTickCount64()<end)Sleep(1);
    CHECK(ReleasedPackets==1);
    while(PbTestWaiters(Native)!=1&&GetTickCount64()<end)Sleep(1);
    CHECK(PbTestWaiters(Native)==1);LONG calls=ReadAcquire(&ReceiveCalls);Sleep(100);
    CHECK(PbTestWaiters(Native)==1&&ReceiveCalls==calls&&ReleasedPackets==1);
    /* Observe the actual event wait, rather than inferring sleep from stable API counts. */
    for(unsigned i=0;i<64;++i) {
        DWORD size;BYTE *p=Receive(s,&size);CHECK(size==65535&&!memcmp(p,packet,size));WintunReleaseReceivePacket(s,p);
    }
    while(ReadAcquire(&ReleasedPackets)!=64&&GetTickCount64()<end)Sleep(1);
    CHECK(ReleasedPackets==64);
    WintunEndSession(s);EmptyRuntime();
    /* Stop with 63 still-held native loans. */
    ResetEvent(BatchEntered);ResetEvent(BatchGate);BatchGateUsed=0;ReleasedPackets=0;
    s=Start();CHECK(s&&WaitForSingleObject(BatchEntered,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<64;++i)CHECK(PbTestSend(Native,0,packet,65535,0,0)==PB_OK);
    CHECK(SetEvent(BatchGate));end=GetTickCount64()+5000;
    while(ReadAcquire(&ReleasedPackets)!=1&&GetTickCount64()<end)Sleep(1);
    CHECK(ReleasedPackets==1);ReleaseLimit=1;WintunEndSession(s);EmptyRuntime();ReleaseLimit=0;free(packet);BatchMode=0;
    CHECK(CloseHandle(BatchEntered)&&CloseHandle(BatchGate));BatchEntered=BatchGate=NULL;
    puts("PASS: 64 maximum-size native loans, full legacy ring parks without polling, every prefix release wakes progress, stop returns all 63 pending loans.");
}
static void Ordering(void)
{
    TUN_SESSION *s=Start(); CHECK(s);
    CHECK(!WintunAllocateSendPacket(s,65536)&&GetLastError()==ERROR_INVALID_PARAMETER);
    CHECK(!WintunAllocateSendPacket(s,MAXDWORD)&&GetLastError()==ERROR_INVALID_PARAMETER);
    BYTE *zero=WintunAllocateSendPacket(s,0);CHECK(zero);WintunSendPacket(s,zero);
    BYTE *a=WintunAllocateSendPacket(s,32),*b=WintunAllocateSendPacket(s,64); CHECK(a&&b);
    memset(a,0xa1,32); memset(b,0xb2,64); WintunSendPacket(s,b);
    WintunSendPacket(s,NULL); CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    WintunSendPacket(s,b); CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    PB_DESCRIPTOR d[2]; uint32_t n=77; Sleep(10);
    CHECK(PbTestTake(Native,0,d,2,&n)==PB_EMPTY&&!n);
    WintunSendPacket(s,a); Take(d,2);
    CHECK(d[0].Length==32 && d[1].Length==64);
    CHECK(((BYTE *)Region)[d[0].DataOffset]==0xa1 && ((BYTE *)Region)[d[1].DataOffset]==0xb2);
    CHECK(PbTestComplete(Native,0,d,2)==PB_OK);
    BYTE *large=malloc(65535); CHECK(large); memset(large,0x75,65535);
    CHECK(PbTestSend(Native,0,large,65535,0,0)==PB_OK);
    CHECK(PbTestSend(Native,0,large,1,0,0)==PB_OK);
    DWORD length; a=Receive(s,&length); CHECK(length==65535&&!memcmp(a,large,length));
    b=Receive(s,&length); CHECK(length==1&&*b==0x75);
    WintunReleaseReceivePacket(s,b); CHECK(s->Descriptor.Send.Ring->Head==0);
    WintunReleaseReceivePacket(s,NULL);CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    WintunReleaseReceivePacket(s,b);CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    WintunReleaseReceivePacket(s,a); CHECK(s->Descriptor.Send.Ring->Head==s->Send.Head);
    for(unsigned i=0;i<400;++i) {
        large[0]=(BYTE)i;
        CHECK(PbTestSend(Native,0,large,65535,0,0)==PB_OK);
        a=Receive(s,&length); CHECK(length==65535&&!memcmp(a,large,length)); WintunReleaseReceivePacket(s,a);
    }
    free(large); WintunEndSession(s); EmptyRuntime();
    puts("PASS: actual Wintun/v2 adapter, out-of-order send/release, exact maximum IP packets, ring wraps and clean native close.");
}
static SRWLOCK SendOrder=SRWLOCK_INIT;
static TUN_SESSION *Concurrent;
static volatile LONG Assigned;
static DWORD WINAPI Producer(void *opaque)
{
    (void)opaque;
    for(unsigned i=0;i<1000;++i) {
        BYTE *p;
        do { AcquireSRWLockExclusive(&SendOrder); p=WintunAllocateSendPacket(Concurrent,32);
             if(!p) { CHECK(GetLastError()==ERROR_BUFFER_OVERFLOW); ReleaseSRWLockExclusive(&SendOrder); SwitchToThread(); }
        } while(!p);
        LONG id=Assigned++; memcpy(p,&id,sizeof(id)); ReleaseSRWLockExclusive(&SendOrder);
        if((i&7)==0) SwitchToThread(); WintunSendPacket(Concurrent,p);
    }
    return 0;
}
static void Concurrency(void)
{
    Concurrent=Start(); CHECK(Concurrent); Assigned=0;
    HANDLE threads[4]; for(unsigned i=0;i<4;++i) { threads[i]=CreateThread(NULL,0,Producer,NULL,0,NULL); CHECK(threads[i]); }
    LONG expected=0; ULONGLONG end=GetTickCount64()+15000;
    while(expected<4000 && GetTickCount64()<end) {
        PB_DESCRIPTOR d[64]; uint32_t n=0; PB_RESULT r=PbTestTake(Native,0,d,64,&n); CHECK(r==PB_EMPTY||r==PB_OK);
        for(uint32_t i=0;i<n;++i) { LONG id; memcpy(&id,(BYTE *)Region+d[i].DataOffset,sizeof(id)); CHECK(id==expected++); }
        if(n) CHECK(PbTestComplete(Native,0,d,n)==PB_OK); else SwitchToThread();
    }
    CHECK(expected==4000); CHECK(WaitForMultipleObjects(4,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i) CloseHandle(threads[i]);
    WintunEndSession(Concurrent); EmptyRuntime();
    puts("PASS: four concurrent legacy producers, 4000 exact ordered payloads, native completion recycling and worker join.");
}
static volatile LONG Seen[4000],Received;
static DWORD WINAPI Consumer(void *opaque)
{
    (void)opaque;ULONGLONG end=GetTickCount64()+15000;
    while(ReadAcquire(&Received)<4000&&GetTickCount64()<end) {
        DWORD size;BYTE *p=WintunReceivePacket(Concurrent,&size);
        if(!p) { CHECK(GetLastError()==ERROR_NO_MORE_ITEMS);WaitForSingleObject(WintunGetReadWaitEvent(Concurrent),10);continue; }
        LONG id;memcpy(&id,p,sizeof(id));CHECK(size==80&&id>=0&&id<4000);
        CHECK(p[79]==(BYTE)id&&!InterlockedExchange(Seen+id,1));
        if((id&7)==0)SwitchToThread();WintunReleaseReceivePacket(Concurrent,p);InterlockedIncrement(&Received);
    }
    CHECK(Received==4000);return 0;
}
static void ReceiveConcurrency(void)
{
    Concurrent=Start();CHECK(Concurrent);Received=0;memset((void *)Seen,0,sizeof(Seen));
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,Consumer,NULL,0,NULL);CHECK(threads[i]);}
    ULONGLONG end=GetTickCount64()+15000;
    for(LONG id=0;id<4000;++id) {
        BYTE bytes[80];memset(bytes,(BYTE)id,sizeof(bytes));memcpy(bytes,&id,sizeof(id));PB_RESULT r;
        do{r=PbTestSend(Native,0,bytes,sizeof(bytes),0,0);if(r==PB_FULL)SwitchToThread();CHECK(GetTickCount64()<end);}while(r==PB_FULL);
        CHECK(r==PB_OK);
    }
    CHECK(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i)CloseHandle(threads[i]);
    for(unsigned i=0;i<4000;++i)CHECK(Seen[i]==1);
    WintunEndSession(Concurrent);EmptyRuntime();
    puts("PASS: four concurrent legacy receivers, 4000 unique exact payloads, out-of-order returns and full-ring wakeups.");
}
static void Faults(void)
{
    WINTUN_ADAPTER adapter={0}; adapter.InterfaceFilename=L"hosted-v2";
    DWORD caps[]={0,1,0x1ffff,0x20001,0x80000000,0xffffffff};
    for(unsigned i=0;i<sizeof(caps)/sizeof(caps[0]);++i) CHECK(!WintunStartSession(&adapter,caps[i])&&GetLastError()==ERROR_INVALID_PARAMETER);
    Failure=0; Acquisitions=0; TUN_SESSION *s=Start(); CHECK(s); LONG steps=Acquisitions;
    WintunEndSession(s); EmptyRuntime();
    for(LONG i=1;i<=steps;++i) { Failure=i; Acquisitions=0; CHECK(!Start());CHECK(GetLastError()==InjectedError); EmptyRuntime(); }
    Failure=100; CHECK(!Start()&&GetLastError()==ERROR_ACCESS_DENIED); EmptyRuntime(); Failure=0;
    CHECK(CompatReserveBytes(COMPAT_MAX_RING_BYTES)); CHECK(!Start());
    InterlockedAdd64(&CompatRingBytes,-COMPAT_MAX_RING_BYTES); EmptyRuntime();
    /* Close with a local-full receive ring and an outstanding private loan. */
    s=Start(); CHECK(s); BYTE *packet=calloc(1,65535); CHECK(packet);
    for(unsigned i=0;i<4;++i) CHECK(PbTestSend(Native,0,packet,65535,0,0)==PB_OK);
    Sleep(20); WintunEndSession(s); free(packet); EmptyRuntime();
    s=Start(); CHECK(s); PbTestFault(Native);
    DWORD size; ULONGLONG end=GetTickCount64()+5000;
    while(!WintunReceivePacket(s,&size)&&GetLastError()==ERROR_NO_MORE_ITEMS&&GetTickCount64()<end) Sleep(1);
    CHECK(GetLastError()==ERROR_HANDLE_EOF); WintunEndSession(s); EmptyRuntime();
    s=Start();CHECK(s);PB_SESSION *retained=Native;PbTestActive(retained,1);
    WintunEndSession(s);
    CHECK(PbTestRetained(Epoch)&&!RuntimeUsers&&!CompatRingBytes&&!RingAllocations&&!Paths&&!Pins);
    PbTestActive(retained,0);EmptyRuntime();
    puts("PASS: invalid capacities, every acquisition failure, native-open failure, ring-budget exhaustion, full-ring stop and native fault; zero heap/ring/path/module/runtime residue.");
}
static void Burst(TUN_SESSION *s,uint32_t round)
{
    BYTE payload[1500];memset(payload,(BYTE)round,sizeof(payload));memcpy(payload,&round,sizeof(round));
    BYTE *send[64];ULONGLONG end=GetTickCount64()+10000;
    for(unsigned i=0;i<64;++i) {
        PB_RESULT r;
        do { r=PbTestSend(Native,0,payload,sizeof(payload),0,0);if(r==PB_FULL)SwitchToThread();CHECK(GetTickCount64()<end); } while(r==PB_FULL);
        CHECK(r==PB_OK);
        do { send[i]=WintunAllocateSendPacket(s,sizeof(payload));if(!send[i]) { CHECK(GetLastError()==ERROR_BUFFER_OVERFLOW);SwitchToThread(); } CHECK(GetTickCount64()<end); } while(!send[i]);
        memcpy(send[i],payload,sizeof(payload));
    }
    for(unsigned i=64;i--;)WintunSendPacket(s,send[i]);
    uint32_t received=0,taken=0;
    while(received<64||taken<64) {
        PB_DESCRIPTOR d[64];uint32_t n=0;
        if(taken<64) {
            PB_RESULT r=PbTestTake(Native,0,d,64-taken,&n);CHECK(r==PB_OK||r==PB_EMPTY);
            for(uint32_t i=0;i<n;++i)CHECK(d[i].Length==sizeof(payload)&&!memcmp((BYTE *)Region+d[i].DataOffset,payload,sizeof(payload)));
            if(n)CHECK(PbTestComplete(Native,0,d,n)==PB_OK);taken+=n;
        }
        while(received<64) {
            DWORD size;BYTE *p=WintunReceivePacket(s,&size);
            if(!p) { CHECK(GetLastError()==ERROR_NO_MORE_ITEMS);break; }
            CHECK(size==sizeof(payload)&&!memcmp(p,payload,size));WintunReleaseReceivePacket(s,p);received++;
        }
        CHECK(GetTickCount64()<end);if(!n)SwitchToThread();
    }
}
static uint64_t ThreadTime(HANDLE thread)
{
    FILETIME created,exited,kernel,user;CHECK(GetThreadTimes(thread,&created,&exited,&kernel,&user));
    ULARGE_INTEGER k={.LowPart=kernel.dwLowDateTime,.HighPart=kernel.dwHighDateTime};
    ULARGE_INTEGER u={.LowPart=user.dwLowDateTime,.HighPart=user.dwHighDateTime};return k.QuadPart+u.QuadPart;
}
static void Benchmark(void)
{
    DWORD_PTR allowed,system;CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    DWORD_PTR caller=allowed&(~allowed+1),remaining=allowed&~caller,worker=remaining&(~remaining+1);
    if(!worker)worker=caller;
    CHECK(SetThreadAffinityMask(GetCurrentThread(),caller));
    TUN_SESSION *s=Start();CHECK(s&&SetThreadAffinityMask(s->Thread,worker));
    for(uint32_t i=0;i<32;++i)Burst(s,i);
    LARGE_INTEGER first,last,freq;ULONG64 before,after;uint64_t cpu=ThreadTime(s->Thread);
    CHECK(QueryPerformanceFrequency(&freq)&&QueryThreadCycleTime(s->Thread,&before)&&QueryPerformanceCounter(&first));
    const uint32_t bursts=10000;
    for(uint32_t i=0;i<bursts;++i)Burst(s,i);
    CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(s->Thread,&after));cpu=ThreadTime(s->Thread)-cpu;
    printf("{\"case\":\"compat-duplex\",\"bytes\":1500,\"burst\":64,\"packets_per_direction\":%u,\"worker_cpu_ms\":%.3f,\"worker_cycles_per_packet\":%.3f,\"wall_ms\":%.3f,\"caller_affinity\":%llu,\"worker_affinity\":%llu}\n",
        bursts*64,(double)cpu/10000.,(double)(after-before)/(bursts*128.),(last.QuadPart-first.QuadPart)*1000./freq.QuadPart,
        (unsigned long long)caller,(unsigned long long)worker);
    WintunEndSession(s);EmptyRuntime();
}
typedef struct TERM_JOB {TUN_SESSION *Session;BOOL Startup;DWORD Error;} TERM_JOB;
static DWORD WINAPI TermCaller(void *opaque)
{
    TERM_JOB *j=opaque;
    if(j->Startup)j->Session=Start();else WintunEndSession(j->Session);
    j->Error=GetLastError();return 0;
}
static void TerminationWait(BOOL startup,DWORD result,BOOL exitBeforeRetry)
{
    TermEntered=CreateEventW(NULL,FALSE,FALSE,NULL);TermRetry=CreateEventW(NULL,FALSE,FALSE,NULL);
    TermWorkerEntered=CreateEventW(NULL,TRUE,FALSE,NULL);HANDLE workerGate=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(TermEntered&&TermRetry&&TermWorkerEntered&&workerGate);TermFailures=2;TermBackoffs=0;TermFailureResult=result;
    TERM_JOB job={.Startup=startup};
    if(startup){TermWorkerGate=workerGate;TermStartup=1;TermArmed=1;}
    else {
        BatchMode=1;BatchEntered=TermWorkerEntered;BatchGate=workerGate;BatchGateUsed=0;
        job.Session=Start();CHECK(job.Session);TermTarget=job.Session->Thread;CHECK(TestDuplicate(TermTarget));TermArmed=1;
        /* Ready precedes the pump loop. Hold the intended live-worker gate
         * before End can set Stopping and skip RecordedReceive entirely. */
        CHECK(WaitForSingleObject(TermWorkerEntered,5000)==WAIT_OBJECT_0);
    }
    HANDLE caller=CreateThread(NULL,0,TermCaller,&job,0,NULL);CHECK(caller);
    if(startup)CHECK(WaitForSingleObject(TermWorkerEntered,5000)==WAIT_OBJECT_0);
    for(LONG retry=1;retry<=2;++retry) {
        CHECK(WaitForSingleObject(TermEntered,5000)==WAIT_OBJECT_0&&TermBackoffs==retry);
        CHECK(WaitForSingleObject(TermWitness,0)==WAIT_TIMEOUT&&WaitForSingleObject(caller,0)==WAIT_TIMEOUT);
        CHECK(RuntimeUsers==1&&RingAllocations==1&&Paths==1&&Pins==1&&CompatRingBytes>0);
        if(retry==1)CHECK(SetEvent(TermRetry));
    }
    if(exitBeforeRetry){CHECK(SetEvent(workerGate)&&WaitForSingleObject(TermWitness,5000)==WAIT_OBJECT_0&&SetEvent(TermRetry));}
    else CHECK(SetEvent(TermRetry)&&SetEvent(workerGate));
    CHECK(WaitForSingleObject(caller,5000)==WAIT_OBJECT_0&&WaitForSingleObject(TermWitness,0)==WAIT_OBJECT_0);
    if(startup)CHECK(!job.Session&&job.Error==ERROR_OPERATION_ABORTED);
    EmptyRuntime();TermArmed=TermStartup=0;TermWorkerGate=NULL;BatchMode=0;BatchGate=BatchEntered=NULL;
    CHECK(CloseHandle(caller)&&CloseHandle(TermWitness)&&CloseHandle(workerGate)&&CloseHandle(TermWorkerEntered)&&CloseHandle(TermEntered)&&CloseHandle(TermRetry));
    TermWitness=TermTarget=TermEntered=TermRetry=TermWorkerEntered=NULL;
}
static void TerminationWaits(void)
{
    const DWORD results[]={WAIT_FAILED,WAIT_TIMEOUT,WAIT_ABANDONED_0};
    for(unsigned startup=0;startup<2;++startup)for(unsigned r=0;r<3;++r)for(unsigned order=0;order<2;++order)
        TerminationWait((BOOL)startup,results[r],(BOOL)order);
    puts("PASS: 12 actual legacy worker termination schedules: end/startup rollback, failed/unexpected waits, two bounded retries, both exit/retry orders; live worker keeps rings/path/module/runtime ownership, original startup error preserved and all threads/resources reclaimed after real exit.");
}
int main(int argc,char **argv)
{
    ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if(argc==2&&!strcmp(argv[1],"--tun32")){Tun32Cases();CHECK(HeapDestroy(ModuleHeap));return 0;}
    if(argc==2&&!strcmp(argv[1],"--tun32-partial")){Tun32PartialCases();CHECK(HeapDestroy(ModuleHeap));return 0;}
    if(argc==2&&!strcmp(argv[1],"--tun32-terminal")){Terminal32Cases();CHECK(HeapDestroy(ModuleHeap));return 0;}
    if(argc==2&&!strcmp(argv[1],"--termination-end")){TerminationWait(FALSE,WAIT_FAILED,FALSE);CHECK(HeapDestroy(ModuleHeap));return 0;}
    if(argc==2&&!strcmp(argv[1],"--termination-start")){TerminationWait(TRUE,WAIT_FAILED,FALSE);CHECK(HeapDestroy(ModuleHeap));return 0;}
    if(argc==2&&!strcmp(argv[1],"--bench")) { Benchmark();CHECK(HeapDestroy(ModuleHeap));return 0; }
    if(argc==2&&!strcmp(argv[1],"--expect-baseline")) { Batches(FALSE,TRUE);CHECK(HeapDestroy(ModuleHeap));return 0; }
#ifdef PB_COMPAT_CURRENT
    PumpCases();
#endif
    TerminationWaits();Batches(FALSE,FALSE);Batches(TRUE,FALSE);BlockedBatch();Tun32Cases();Tun32PartialCases();
    Ordering(); Concurrency(); ReceiveConcurrency(); Faults();
    DWORD before,after; CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(unsigned i=0;i<100;++i) { TUN_SESSION *s=Start(); CHECK(s); WintunEndSession(s); EmptyRuntime(); }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)); CHECK(after==before);
    CHECK(HeapDestroy(ModuleHeap));
    puts("PASS: 100 repeated actual legacy/v2 session lifecycles, zero process handle delta."); return 0;
}
