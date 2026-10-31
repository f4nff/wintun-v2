/* SPDX-License-Identifier: GPL-2.0 */
/* Production Allocate/Destroy with guarded allocations and acquisition-by-acquisition failures. */
#include "guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
static ULONG FailAt,Step;
static LONG Pools,Events,Regions;
static LONG Modules,WorkPools,Works;
static BOOL Fail(void) { return ++Step==FailAt; }
static void *FaultCalloc(size_t count,size_t size)
{ if(Fail()) return NULL; SAFETY_CHECK(!size || count<=SIZE_MAX/size); void *p=PbTestGuardAlloc(count*size); memset(p,0,count*size); Pools++; return p; }
static void FaultFree(void *p) { if(p) { SAFETY_CHECK(Pools>0); Pools--; PbTestGuardFree(p); } }
static HANDLE WINAPI FaultEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name)
{ if(Fail()) return NULL; HANDLE event=CreateEventW(attributes,manual,initial,name); SAFETY_CHECK(event); Events++; return event; }
static BOOL WINAPI FaultClose(HANDLE event) { SAFETY_CHECK(Events>0); Events--; return CloseHandle(event); }
static LONG WINAPI FaultRandom(BCRYPT_ALG_HANDLE algorithm,PUCHAR bytes,ULONG size,ULONG flags)
{ (void)algorithm; (void)flags; if(Fail()) return (LONG)0xc0000001u; memset(bytes,0x42,size); return 0; }
static LPVOID WINAPI FaultRegion(LPVOID address,SIZE_T size,DWORD type,DWORD protection)
{ SAFETY_CHECK(!address && type==(MEM_COMMIT|MEM_RESERVE) && protection==PAGE_READWRITE); if(Fail()) return NULL; Regions++; return PbTestGuardAlloc(size); }
static BOOL WINAPI FaultUnmap(LPVOID address,SIZE_T size,DWORD type)
{ SAFETY_CHECK(!size && type==MEM_RELEASE && Regions>0); Regions--; PbTestGuardFree(address); return TRUE; }
static BOOL WINAPI FaultModule(DWORD flags,LPCWSTR address,HMODULE *module)
{ if(Fail()) return FALSE; BOOL okay=GetModuleHandleExW(flags,address,module); SAFETY_CHECK(okay); Modules++; return okay; }
static BOOL WINAPI FaultLibrary(HMODULE module) { SAFETY_CHECK(Modules>0); Modules--; return FreeLibrary(module); }
static PTP_POOL WINAPI FaultPool(PVOID reserved) { if(Fail()) return NULL; PTP_POOL p=CreateThreadpool(reserved); SAFETY_CHECK(p); WorkPools++; return p; }
static VOID WINAPI FaultPoolClose(PTP_POOL p) { SAFETY_CHECK(WorkPools>0); WorkPools--; CloseThreadpool(p); }
static BOOL WINAPI FaultMinimum(PTP_POOL p,DWORD count) { if(Fail()) return FALSE; return SetThreadpoolThreadMinimum(p,count); }
static PTP_WORK WINAPI FaultWork(PTP_WORK_CALLBACK callback,PVOID context,PTP_CALLBACK_ENVIRON environment)
{ if(Fail()) return NULL; PTP_WORK w=CreateThreadpoolWork(callback,context,environment); SAFETY_CHECK(w); Works++; return w; }
static VOID WINAPI FaultWorkClose(PTP_WORK w) { SAFETY_CHECK(Works>0); Works--; CloseThreadpoolWork(w); }
#define calloc FaultCalloc
#define free FaultFree
#define CreateEventW FaultEvent
#define CloseHandle FaultClose
#define BCryptGenRandom FaultRandom
#define VirtualAlloc FaultRegion
#define VirtualFree FaultUnmap
#define GetModuleHandleExW FaultModule
#define FreeLibrary FaultLibrary
#define CreateThreadpool FaultPool
#define CloseThreadpool FaultPoolClose
#define SetThreadpoolThreadMinimum FaultMinimum
#define CreateThreadpoolWork FaultWork
#define CloseThreadpoolWork FaultWorkClose
#include "../../api/v2/session.c"
#undef calloc
#undef free
#undef CreateEventW
#undef CloseHandle
#undef BCryptGenRandom
#undef VirtualAlloc
#undef VirtualFree
#undef GetModuleHandleExW
#undef FreeLibrary
#undef CreateThreadpool
#undef CloseThreadpool
#undef SetThreadpoolThreadMinimum
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
static void RecycleNotificationTests(void)
{
    /* Actual DLL queue calls and OS event, without registering a driver or
     * submitting a lifecycle worker. Only an armed consumer needs a wake. */
    PB_CONFIG config={sizeof(config),1,4,4,0,0}; PB_SESSION *s; FailAt=0;
    SAFETY_CHECK(Allocate(&config,&s)==PB_OK);
    PB_USER_QUEUE *q; SAFETY_CHECK(OpenQueue(s,0,&q)==PB_OK);
    PB_QUEUE_VIEW ready,recycle;
    SAFETY_CHECK(PbAttachQueue(&ready,s->Region,&s->Layout,0,PB_QUEUE_TX_READY,1)==PB_OK);
    SAFETY_CHECK(PbAttachQueue(&recycle,s->Region,&s->Layout,0,PB_QUEUE_TX_RECYCLE,0)==PB_OK);
    PB_DESCRIPTOR descriptor={.Token={s->Epoch,1,0,0},.Length=20,.DataOffset=s->Layout.Queues[0].TxData};
    uint32_t count,notify; PB_LEASE lease;
    for(uint32_t armed=0;armed<2;++armed) {
        descriptor.Token.Generation=armed+1;
        if(armed) { uint32_t available; SAFETY_CHECK(PbQueueArm(&recycle,&available)==PB_OK && !available); }
        SAFETY_CHECK(PbQueuePush(&ready,&descriptor,1,&count,&notify)==PB_OK && count==1);
        SAFETY_CHECK(ReceiveBatch(q,&lease,1,&count)==PB_OK && count==1);
        SAFETY_CHECK(ReleaseBatch(q,&lease.Descriptor.Token,1,&count)==PB_OK && count==1);
        SAFETY_CHECK(WaitForSingleObject(q->KernelEvent,0)==(armed?WAIT_OBJECT_0:WAIT_TIMEOUT));
        SAFETY_CHECK(WaitForSingleObject(q->KernelEvent,0)==WAIT_TIMEOUT);
        PB_DESCRIPTOR returned; SAFETY_CHECK(PbQueuePop(&recycle,&returned,1,&count)==PB_OK && count==1);
    }
    Destroy(s);
}
int main(void)
{
    PB_CONFIG config={sizeof(config),8,4,4,0,0};
    PB_SESSION *warm; SAFETY_CHECK(Allocate(&config,&warm)==PB_OK); ULONG acquisitions=Step; Destroy(warm);
    DWORD before,after; SAFETY_CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(ULONG fail=1;fail<=acquisitions+1;++fail) {
        FailAt=fail; Step=0; PB_SESSION *session=(PB_SESSION *)(ULONG_PTR)1;
        PB_RESULT result=Allocate(&config,&session);
        if(fail<=acquisitions) SAFETY_CHECK(result==PB_FULL && !session && Step>=fail);
        else { SAFETY_CHECK(result==PB_OK && session && Step==acquisitions); Destroy(session); }
        SAFETY_CHECK(!Pools && !Events && !Regions && !Modules && !WorkPools && !Works && !RuntimeCount && !RuntimeBytes);
    }
    RecycleNotificationTests();
    SAFETY_CHECK(!Pools && !Events && !Regions && !Modules && !WorkPools && !Works && !RuntimeCount && !RuntimeBytes);
    SAFETY_CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    printf("PASS: actual DLL Allocate/Destroy, acquisitions=%lu failure prefixes plus success, guarded memory, zero pools/events/regions/modules/work objects and handle delta.\n",acquisitions);
    puts("PASS: actual ReleaseBatch suppresses unarmed recycle wakes and signals an armed consumer exactly once.");
    return 0;
}
