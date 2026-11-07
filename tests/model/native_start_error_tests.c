/* SPDX-License-Identifier: GPL-2.0 */
/* Actual non-test native OpenSession/Allocate and legacy default Api.Open.
 * Only acquisition/DDI boundaries are hosted; no fabricated Api.Open result.
 * No device, registry, driver or actual kernel I/O is accessed. */
#ifdef PB_API_TESTS
#error Native startup errors must execute the shipping session implementation
#endif
#ifdef PB_COMPAT_OPEN
#error Native startup errors require the default legacy Api.Open path
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include "../safety/guard_memory.h"
#include "../../api/v2/packetbridge.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"startup tests require the shipping source fingerprint layout");
#define CHECK SAFETY_CHECK
enum { K_CALLOC=1,K_EVENT,K_RANDOM,K_REGION,K_PIN,K_POOL,K_MINIMUM,K_WORK,K_FILE,K_PROBE,K_REGISTER };
typedef struct BLOCK {VOID *Memory;SIZE_T Bytes;BOOL Guard;} BLOCK;
static BLOCK Blocks[128],Regions[32];
static HANDLE NativeEvents[256],Device;
static HMODULE NativeModules[64];
static PTP_POOL NativePools[32];
static PTP_WORK NativeWorks[32];
static LONG LiveBlocks,LiveRegions,LiveEvents,LivePins,LivePools,LiveWorks;
static DWORD Step,Kinds[64],FailAt,InjectedError,AmbientError,SuccessError,CleanupError,IoCalls,FileCalls;
static DWORD ObservedResult,ObservedError,Scenarios;
static BOOL Starting,ZeroEpoch,ControlMode;
static DWORD Acquire(DWORD kind)
{if(!Starting)return FALSE;CHECK(Step<_countof(Kinds));Kinds[Step++]=kind;if(Step!=FailAt)return FALSE;if(kind==K_CALLOC)errno=ENOMEM;else SetLastError(InjectedError);return TRUE;}
static VOID AddBlock(BLOCK *blocks,DWORD count,VOID *memory,SIZE_T bytes,BOOL guard)
{DWORD i;for(i=0;i<count&&blocks[i].Memory;++i){}CHECK(i<count);blocks[i]=(BLOCK){memory,bytes,guard};}
static VOID RemoveBlock(BLOCK *blocks,DWORD count,VOID *memory)
{DWORD i;for(i=0;i<count&&blocks[i].Memory!=memory;++i){}CHECK(i<count);if(blocks[i].Guard)PbTestGuardFree(memory);else free(memory);blocks[i]=(BLOCK){0};}
static VOID *NativeCalloc(SIZE_T count,SIZE_T bytes)
{
    CHECK(!bytes||count<=SIZE_MAX/bytes);if(Acquire(K_CALLOC))return NULL;VOID *memory;BOOL guard=FALSE;
#ifdef PB_GUARD_TESTS
    memory=PbTestGuardAlloc(count*bytes);memset(memory,0,count*bytes);guard=TRUE;
#else
    memory=calloc(count,bytes);
#endif
    CHECK(memory);AddBlock(Blocks,_countof(Blocks),memory,count*bytes,guard);++LiveBlocks;return memory;
}
static VOID NativeFree(VOID *memory)
{if(memory){RemoveBlock(Blocks,_countof(Blocks),memory);CHECK(--LiveBlocks>=0);}SetLastError(CleanupError);}
static HANDLE AddEvent(HANDLE event)
{CHECK(event);DWORD i;for(i=0;i<_countof(NativeEvents)&&NativeEvents[i];++i){}CHECK(i<_countof(NativeEvents));NativeEvents[i]=event;++LiveEvents;return event;}
static HANDLE WINAPI NativeEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL initial,LPCWSTR name)
{if(Acquire(K_EVENT))return NULL;HANDLE event=AddEvent(CreateEventW(security,manual,initial,name));SetLastError(SuccessError);return event;}
static BOOL WINAPI NativeClose(HANDLE event)
{
    DWORD i;for(i=0;i<_countof(NativeEvents)&&NativeEvents[i]!=event;++i){}CHECK(i<_countof(NativeEvents)&&CloseHandle(event));NativeEvents[i]=NULL;CHECK(--LiveEvents>=0);
    if(event==Device)Device=NULL;SetLastError(CleanupError);return TRUE;
}
static NTSTATUS WINAPI NativeRandom(BCRYPT_ALG_HANDLE algorithm,PUCHAR output,ULONG bytes,ULONG flags)
{CHECK(!algorithm&&output&&bytes==sizeof(uint64_t)&&flags==BCRYPT_USE_SYSTEM_PREFERRED_RNG);if(Acquire(K_RANDOM))return(NTSTATUS)0xc000009au;if(ZeroEpoch)memset(output,0,bytes);else CHECK(BCryptGenRandom(algorithm,output,bytes,flags)>=0);SetLastError(SuccessError);return 0;}
static VOID *WINAPI NativeRegion(VOID *address,SIZE_T bytes,DWORD type,DWORD protection)
{
    CHECK(!address&&type==(MEM_COMMIT|MEM_RESERVE)&&protection==PAGE_READWRITE);if(Acquire(K_REGION))return NULL;VOID *memory;BOOL guard=FALSE;
#ifdef PB_GUARD_TESTS
    memory=PbTestGuardAlloc(bytes);guard=TRUE;
#else
    memory=VirtualAlloc(NULL,bytes,type,protection);
#endif
    CHECK(memory);AddBlock(Regions,_countof(Regions),memory,bytes,guard);++LiveRegions;SetLastError(SuccessError);return memory;
}
static BOOL WINAPI NativeUnmap(VOID *memory,SIZE_T bytes,DWORD type)
{
    CHECK(memory&&!bytes&&type==MEM_RELEASE);DWORD i;for(i=0;i<_countof(Regions)&&Regions[i].Memory!=memory;++i){}CHECK(i<_countof(Regions));
    if(Regions[i].Guard)PbTestGuardFree(memory);else CHECK(VirtualFree(memory,0,MEM_RELEASE));Regions[i]=(BLOCK){0};CHECK(--LiveRegions>=0);SetLastError(CleanupError);return TRUE;
}
static BOOL WINAPI NativePin(DWORD flags,LPCWSTR address,HMODULE *module)
{if(Acquire(K_PIN))return FALSE;CHECK(GetModuleHandleExW(flags,address,module));DWORD i;for(i=0;i<_countof(NativeModules)&&NativeModules[i];++i){}CHECK(i<_countof(NativeModules));NativeModules[i]=*module;++LivePins;SetLastError(SuccessError);return TRUE;}
static BOOL WINAPI NativeUnpin(HMODULE module)
{DWORD i;for(i=0;i<_countof(NativeModules)&&NativeModules[i]!=module;++i){}CHECK(i<_countof(NativeModules)&&FreeLibrary(module));NativeModules[i]=NULL;CHECK(--LivePins>=0);SetLastError(CleanupError);return TRUE;}
static PTP_POOL WINAPI NativePool(VOID *reserved)
{if(Acquire(K_POOL))return NULL;PTP_POOL pool=CreateThreadpool(reserved);CHECK(pool);DWORD i;for(i=0;i<_countof(NativePools)&&NativePools[i];++i){}CHECK(i<_countof(NativePools));NativePools[i]=pool;++LivePools;SetLastError(SuccessError);return pool;}
static VOID WINAPI NativePoolClose(PTP_POOL pool)
{DWORD i;for(i=0;i<_countof(NativePools)&&NativePools[i]!=pool;++i){}CHECK(i<_countof(NativePools));CloseThreadpool(pool);NativePools[i]=NULL;CHECK(--LivePools>=0);SetLastError(CleanupError);}
static BOOL WINAPI NativeMinimum(PTP_POOL pool,DWORD minimum)
{if(Acquire(K_MINIMUM))return FALSE;BOOL okay=SetThreadpoolThreadMinimum(pool,minimum);CHECK(okay);SetLastError(SuccessError);return okay;}
static PTP_WORK WINAPI NativeWork(PTP_WORK_CALLBACK callback,VOID *input,PTP_CALLBACK_ENVIRON environment)
{if(Acquire(K_WORK))return NULL;PTP_WORK work=CreateThreadpoolWork(callback,input,environment);CHECK(work);DWORD i;for(i=0;i<_countof(NativeWorks)&&NativeWorks[i];++i){}CHECK(i<_countof(NativeWorks));NativeWorks[i]=work;++LiveWorks;SetLastError(SuccessError);return work;}
static VOID WINAPI NativeWorkClose(PTP_WORK work)
{DWORD i;for(i=0;i<_countof(NativeWorks)&&NativeWorks[i]!=work;++i){}CHECK(i<_countof(NativeWorks));CloseThreadpoolWork(work);NativeWorks[i]=NULL;CHECK(--LiveWorks>=0);SetLastError(CleanupError);}
static HANDLE WINAPI NativeFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE template)
{CHECK(!Device&&path&&access==(GENERIC_READ|GENERIC_WRITE)&&share==(FILE_SHARE_READ|FILE_SHARE_WRITE)&&!security&&disposition==OPEN_EXISTING&&!flags&&!template);++FileCalls;if(Acquire(K_FILE))return INVALID_HANDLE_VALUE;Device=AddEvent(CreateEventW(NULL,FALSE,FALSE,NULL));SetLastError(SuccessError);return Device;}
static BOOL WINAPI NativeIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlap)
{
    CHECK(Device&&device==Device&&returned&&!overlap);++IoCalls;*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY){CHECK(!input&&!inputSize&&output&&outputSize==sizeof(PB_PROBE_STATUS));if(Acquire(K_PROBE))return FALSE;
        PB_PROBE_STATUS *status=output;memset(status,0,sizeof(*status));status->Size=sizeof(*status);status->Magic=PB_PROBE_MAGIC;status->Schema=PB_PROBE_SCHEMA;status->FileVersion=PB_VERSION_PACKED;status->DataAbi=PB_ABI_MAJOR;status->NdisVersion=0x00060020;
        memcpy(status->SourceFingerprint,PB_SOURCE_STRING,sizeof(PB_SOURCE_STRING));*returned=sizeof(*status);SetLastError(SuccessError);return TRUE;}
    CHECK(code==PB_V2_IOCTL_REGISTER&&input&&inputSize==sizeof(PB_REGISTER_V2)&&!output&&!outputSize);CHECK(Acquire(K_REGISTER));return FALSE;
}
#define calloc NativeCalloc
#define free NativeFree
#define CreateEventW NativeEvent
#define CloseHandle NativeClose
#define BCryptGenRandom NativeRandom
#define VirtualAlloc NativeRegion
#define VirtualFree NativeUnmap
#define GetModuleHandleExW NativePin
#define FreeLibrary NativeUnpin
#define CreateThreadpool NativePool
#define CloseThreadpool NativePoolClose
#define SetThreadpoolThreadMinimum NativeMinimum
#define CreateThreadpoolWork NativeWork
#define CloseThreadpoolWork NativeWorkClose
#define CreateFileW NativeFile
#define DeviceIoControl NativeIo
#ifndef PB_NATIVE_START_CORE_SOURCE
#error Build through Build-NativeStartErrorTests to bind complete native source
#endif
#include PB_NATIVE_START_CORE_SOURCE
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
#undef CreateFileW
#undef DeviceIoControl
PB_RESULT WINAPI PbAdapterCreate(const WCHAR *name,const WCHAR *instance,PB_ADAPTER **out){(void)name;(void)instance;*out=NULL;return PB_UNSUPPORTED;}
VOID WINAPI PbAdapterRelease(PB_ADAPTER *adapter){CHECK(!adapter);}
VOID PbAdapterRetain(PB_ADAPTER *adapter){CHECK(!adapter);}
const WCHAR *PbAdapterPath(PB_ADAPTER *adapter){CHECK(!adapter);return NULL;}
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter,uint64_t *luid){(void)adapter;(void)luid;return PB_UNSUPPORTED;}

static HANDLE LegacyEvents[3],LegacyThread,ThreadWitness;
static VOID *LegacyMemory,*LegacyMapping;
static WCHAR *LegacyPath;
static LONG LegacyPins,RuntimeUsers;
static LPTHREAD_START_ROUTINE LegacyWorker;
static BOOL WINAPI LegacyHeapFree(HANDLE,DWORD,LPVOID);
#define HeapFree LegacyHeapFree
#include "../../api/logger.h"
#undef HeapFree
HANDLE ModuleHeap;
static VOID *LegacyZalloc(SIZE_T bytes){CHECK(!LegacyMemory);LegacyMemory=HeapAlloc(ModuleHeap,HEAP_ZERO_MEMORY,bytes);CHECK(LegacyMemory);return LegacyMemory;}
static BOOL WINAPI LegacyHeapFree(HANDLE heap,DWORD flags,LPVOID memory)
{CHECK(memory==LegacyMemory&&heap==ModuleHeap&&!flags&&HeapFree(heap,flags,memory));LegacyMemory=NULL;SetLastError(CleanupError);return TRUE;}
static WCHAR *LegacyDuplicate(const WCHAR *text){CHECK(!LegacyPath);LegacyPath=_wcsdup(text);CHECK(LegacyPath);return LegacyPath;}
static VOID LegacyFreePath(VOID *memory){if(memory){CHECK(memory==LegacyPath);free(memory);LegacyPath=NULL;}SetLastError(CleanupError);}
static VOID *WINAPI LegacyRegion(VOID *address,SIZE_T bytes,DWORD type,DWORD protection)
{CHECK(!LegacyMapping);LegacyMapping=VirtualAlloc(address,bytes,type,protection);CHECK(LegacyMapping);return LegacyMapping;}
static BOOL WINAPI LegacyUnmap(VOID *memory,SIZE_T bytes,DWORD type)
{CHECK(memory==LegacyMapping&&ThreadWitness&&WaitForSingleObject(ThreadWitness,0)==WAIT_OBJECT_0&&VirtualFree(memory,bytes,type));LegacyMapping=NULL;SetLastError(CleanupError);return TRUE;}
static HANDLE WINAPI LegacyEvent(LPSECURITY_ATTRIBUTES security,BOOL manual,BOOL initial,LPCWSTR name)
{DWORD i;for(i=0;i<_countof(LegacyEvents)&&LegacyEvents[i];++i){}CHECK(i<_countof(LegacyEvents));LegacyEvents[i]=CreateEventW(security,manual,initial,name);CHECK(LegacyEvents[i]);return LegacyEvents[i];}
static BOOL WINAPI LegacyClose(HANDLE handle)
{if(handle==LegacyThread){CHECK(WaitForSingleObject(handle,0)==WAIT_OBJECT_0);LegacyThread=NULL;}else{DWORD i;for(i=0;i<_countof(LegacyEvents)&&LegacyEvents[i]!=handle;++i){}CHECK(i<_countof(LegacyEvents));LegacyEvents[i]=NULL;}CHECK(CloseHandle(handle));SetLastError(CleanupError);return TRUE;}
static BOOL WINAPI LegacyPin(DWORD flags,LPCWSTR address,HMODULE *module){CHECK(GetModuleHandleExW(flags,address,module));++LegacyPins;return TRUE;}
static BOOL WINAPI LegacyUnpin(HMODULE module){CHECK(--LegacyPins==0&&FreeLibrary(module));SetLastError(CleanupError);return TRUE;}
static DWORD WINAPI WorkerEntry(VOID *input){SetLastError(AmbientError);return LegacyWorker(input);}
static HANDLE WINAPI LegacyCreateThread(LPSECURITY_ATTRIBUTES security,SIZE_T stack,LPTHREAD_START_ROUTINE entry,VOID *input,DWORD flags,LPDWORD id)
{CHECK(!LegacyThread&&!ThreadWitness);LegacyWorker=entry;LegacyThread=CreateThread(security,stack,WorkerEntry,input,flags,id);CHECK(LegacyThread&&DuplicateHandle(GetCurrentProcess(),LegacyThread,GetCurrentProcess(),&ThreadWitness,SYNCHRONIZE,FALSE,0));return LegacyThread;}
static PB_RESULT (WINAPI *ActualOpen)(const wchar_t *,const PB_CONFIG *,PB_SESSION **);
static PB_RESULT WINAPI ObservedOpen(const wchar_t *path,const PB_CONFIG *config,PB_SESSION **out)
{PB_RESULT result=ActualOpen(path,config,out);ObservedResult=result;ObservedError=GetLastError();return result;}
static PB_RESULT WINAPI ObservedFactory(uint32_t major,uint32_t size,PB_INTERFACE_V2 *out)
{PB_RESULT result=PbGetInterfaceV2(major,size,out);if(result==PB_OK){ActualOpen=out->Open;out->Open=ObservedOpen;}return result;}
BOOL MainRuntimeAcquire(VOID){CHECK(InterlockedIncrement(&RuntimeUsers)==1);return TRUE;}
VOID MainRuntimeRelease(VOID){CHECK(InterlockedDecrement(&RuntimeUsers)==0);SetLastError(CleanupError);}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD error=GetLastError();WCHAR line[512];_vsnwprintf_s(line,_countof(line),_TRUNCATE,format,args);SetLastError(error);return error;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{WCHAR line[512];_vsnwprintf_s(line,_countof(line),_TRUNCATE,format,args);return error;}
#undef Zalloc
#define Zalloc LegacyZalloc
#define free LegacyFreePath
#define _wcsdup LegacyDuplicate
#define VirtualAlloc LegacyRegion
#define VirtualFree LegacyUnmap
#define CreateEventW LegacyEvent
#define CloseHandle LegacyClose
#define CreateThread LegacyCreateThread
#define GetModuleHandleExW LegacyPin
#define FreeLibrary LegacyUnpin
#define PbGetInterfaceV2 ObservedFactory
/* Deliberately no PB_COMPAT_OPEN override: actual default calls table Api.Open. */
#include "../../api/session.c"
#undef free
#undef _wcsdup
#undef VirtualAlloc
#undef VirtualFree
#undef CreateEventW
#undef CloseHandle
#undef CreateThread
#undef GetModuleHandleExW
#undef FreeLibrary
#undef PbGetInterfaceV2
static VOID Balance(VOID)
{
    CHECK(!LiveBlocks&&!LiveRegions&&!LiveEvents&&!LivePins&&!LivePools&&!LiveWorks&&!Device);
    for(DWORD i=0;i<_countof(Blocks);++i)CHECK(!Blocks[i].Memory);for(DWORD i=0;i<_countof(Regions);++i)CHECK(!Regions[i].Memory);
    for(DWORD i=0;i<_countof(NativeEvents);++i)CHECK(!NativeEvents[i]);for(DWORD i=0;i<_countof(NativeModules);++i)CHECK(!NativeModules[i]);
    for(DWORD i=0;i<_countof(NativePools);++i)CHECK(!NativePools[i]);for(DWORD i=0;i<_countof(NativeWorks);++i)CHECK(!NativeWorks[i]);
    CHECK(!RuntimeUsers&&!LegacyPins&&!LegacyMemory&&!LegacyMapping&&!LegacyPath&&!LegacyThread&&!ThreadWitness&&!CompatRingBytes);
    for(DWORD i=0;i<_countof(LegacyEvents);++i)CHECK(!LegacyEvents[i]);CHECK(!RuntimeCount&&!RuntimeBytes&&!RuntimeSessions&&!RuntimeAdapterCount);
    PROCESS_HEAP_ENTRY entry={0};while(HeapWalk(ModuleHeap,&entry))CHECK(!(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY));
}
static VOID Begin(VOID)
{Balance();Step=FailAt=IoCalls=FileCalls=0;memset(Kinds,0,sizeof(Kinds));InjectedError=ERROR_ACCESS_DENIED;AmbientError=ERROR_SUCCESS;SuccessError=ERROR_SUCCESS;CleanupError=ERROR_GEN_FAILURE;ZeroEpoch=FALSE;ObservedResult=MAXDWORD;ObservedError=MAXDWORD;}
static VOID StartCase(DWORD expected)
{
    WINTUN_ADAPTER adapter={0};adapter.InterfaceFilename=L"hosted-no-device";Starting=TRUE;
    TUN_SESSION *session=WintunStartSession(&adapter,WINTUN_MIN_RING_CAPACITY);DWORD actual=GetLastError();Starting=FALSE;
    CHECK(!session&&ThreadWitness&&WaitForSingleObject(ThreadWitness,0)==WAIT_OBJECT_0&&CloseHandle(ThreadWitness));ThreadWitness=NULL;
    CHECK(!LegacyThread&&!LegacyMemory&&!LegacyMapping&&!LegacyPath&&!LegacyPins&&!RuntimeUsers&&!CompatRingBytes);
    if(ControlMode&&expected==ERROR_NOT_ENOUGH_MEMORY&&actual!=expected){
        CHECK(ObservedResult==PB_FULL&&ObservedError==ERROR_SUCCESS&&actual==ERROR_NOT_SUPPORTED&&Step==1&&Kinds[0]==K_CALLOC&&!IoCalls&&!FileCalls);
        Balance();CHECK(HeapDestroy(ModuleHeap));fputs("NATIVE_START_RESOURCE_ERROR_LOST: actual non-test Api.Open returned PB_FULL after calloc failure; legacy reported unrelated/fallback error; all owners reclaimed\n",stderr);ExitProcess(115);
    }
    CHECK(actual==expected&&ObservedError==expected);++Scenarios;
}
static DWORD Expected(DWORD kind)
{return kind==K_CALLOC?ERROR_NOT_ENOUGH_MEMORY:kind==K_RANDOM?ERROR_GEN_FAILURE:InjectedError?InjectedError:ERROR_GEN_FAILURE;}
static VOID Prefixes(VOID)
{
    static const DWORD prefix[]={K_CALLOC,K_EVENT,K_EVENT,K_RANDOM,K_REGION,K_EVENT,K_EVENT,K_CALLOC,K_CALLOC,K_PIN,K_PIN,K_POOL,K_MINIMUM,K_WORK,K_FILE,K_PROBE,K_REGISTER};
    const DWORD ambient[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,MAXDWORD};const DWORD cleanup[]={ERROR_SUCCESS,ERROR_GEN_FAILURE,MAXDWORD};
    for(DWORD at=1;at<=_countof(prefix);++at)for(DWORD a=0;a<_countof(ambient);++a)for(DWORD c=0;c<_countof(cleanup);++c){
        Begin();FailAt=at;AmbientError=ambient[a];CleanupError=cleanup[c];SuccessError=ERROR_INVALID_HANDLE;StartCase(Expected(prefix[at-1]));
        CHECK(Step==at);for(DWORD n=0;n<at;++n)CHECK(Kinds[n]==prefix[n]);CHECK(ObservedResult==(DWORD)(at>=15?PB_UNSUPPORTED:PB_FULL));Balance();
    }
    const DWORD errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_SHARING_VIOLATION,ERROR_CANCELLED,ERROR_TIMEOUT,ERROR_REVISION_MISMATCH,MAXDWORD};
    for(DWORD at=1;at<=_countof(prefix);++at)if(prefix[at-1]!=K_CALLOC&&prefix[at-1]!=K_RANDOM)for(DWORD e=0;e<_countof(errors);++e){Begin();FailAt=at;InjectedError=errors[e];StartCase(InjectedError);CHECK(Step==at);Balance();}
    /* Failed Win32 acquisition with zero GLE is defensive boundary injection;
     * it does not establish a natural additional SDK failure scenario. */
    for(DWORD at=1;at<=14;++at)if(prefix[at-1]!=K_CALLOC&&prefix[at-1]!=K_RANDOM){Begin();FailAt=at;InjectedError=0;CleanupError=MAXDWORD;StartCase(ERROR_GEN_FAILURE);CHECK(Step==at);Balance();}
    Begin();ZeroEpoch=TRUE;StartCase(ERROR_INVALID_DATA);CHECK(Step==4&&ObservedResult==PB_FULL);Balance();
}
static VOID MultipleQueues(VOID)
{
    const DWORD counts[]={2,8},ambient[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,MAXDWORD};
    for(DWORD q=0;q<_countof(counts);++q){DWORD prefix[64],total=0;prefix[total++]=K_CALLOC;prefix[total++]=K_EVENT;prefix[total++]=K_EVENT;prefix[total++]=K_RANDOM;prefix[total++]=K_REGION;
        for(DWORD n=0;n<counts[q];++n){prefix[total++]=K_EVENT;prefix[total++]=K_EVENT;prefix[total++]=K_CALLOC;prefix[total++]=K_CALLOC;}
        prefix[total++]=K_PIN;prefix[total++]=K_PIN;prefix[total++]=K_POOL;prefix[total++]=K_MINIMUM;prefix[total++]=K_WORK;prefix[total++]=K_FILE;prefix[total++]=K_PROBE;prefix[total++]=K_REGISTER;
        for(DWORD at=1;at<=total;++at)for(DWORD a=0;a<_countof(ambient);++a){Begin();FailAt=at;CleanupError=MAXDWORD;
            PB_CONFIG config={sizeof(config),counts[q],2,2,0,0};PB_SESSION *session=(PB_SESSION *)(ULONG_PTR)1;SetLastError(ambient[a]);Starting=TRUE;
            PB_RESULT result=Open(L"hosted-multiqueue",&config,&session);DWORD error=GetLastError();Starting=FALSE;
            CHECK(!session&&result==(at>total-3?PB_UNSUPPORTED:PB_FULL)&&error==Expected(prefix[at-1])&&Step==at);
            for(DWORD n=0;n<at;++n)CHECK(Kinds[n]==prefix[n]);Balance();++Scenarios;
        }
    }
}
static VOID Quota(BOOL bytes,DWORD ambient)
{
    Begin();AmbientError=ambient;PB_SESSION *held[PB_RUNTIME_MAX_SESSIONS]={0};DWORD count=bytes?3:PB_RUNTIME_MAX_SESSIONS;
    for(DWORD i=0;i<count;++i){PB_CONFIG config={sizeof(config),bytes&&i==0?2u:1u,bytes?(i==2?448u:512u):2u,bytes?512u:2u,0,0};CHECK(Allocate(&config,held+i)==PB_OK);}
    uint32_t retained=RuntimeCount;uint64_t retainedBytes=RuntimeBytes;CHECK(retained==count&&retainedBytes);
    StartCase(ERROR_NOT_ENOUGH_QUOTA);CHECK(Step==1&&ObservedResult==PB_FULL&&RuntimeCount==retained&&RuntimeBytes==retainedBytes&&!IoCalls&&!FileCalls);
    for(DWORD i=0;i<count;++i)Destroy(held[i]);Balance();
}
static VOID Invalid(VOID)
{
    PB_CONFIG valid={sizeof(valid),1,2,2,0,0};
    for(DWORD kind=0;kind<10;++kind){Begin();PB_CONFIG config=valid;PB_SESSION *session=(PB_SESSION *)(ULONG_PTR)1;
        switch(kind){case 0:config.Size--;break;case 1:config.Flags=4;break;case 2:config.Offloads=MAXDWORD;break;case 3:config.Offloads=PB_RX_IP4_CHECKSUM;break;
        case 4:config.QueueCount=0;break;case 5:config.SlotCount=0;break;case 6:config.RingCapacity=3;break;case 7:config.Flags=PB_OPEN_EXPERIMENTAL_OFFLOADS;break;case 8:break;default:break;}
        SetLastError(ERROR_ACCESS_DENIED);PB_RESULT result=Allocate(kind==8?NULL:&config,kind==9?NULL:&session);DWORD error=GetLastError();
        CHECK(result==(kind==7?PB_UNSUPPORTED:PB_INVALID)&&error==(DWORD)(kind==7?ERROR_NOT_SUPPORTED:ERROR_INVALID_PARAMETER)&&(!session||kind==9));Balance();++Scenarios;
    }
}
static VOID Matrix(VOID)
{Prefixes();MultipleQueues();Invalid();const DWORD ambient[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,MAXDWORD};for(DWORD b=0;b<2;++b)for(DWORD a=0;a<_countof(ambient);++a)Quota(b!=0,ambient[a]);}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);
    if(argc==2){CHECK(!strcmp(argv[1],"1"));ControlMode=TRUE;Begin();FailAt=1;StartCase(ERROR_NOT_ENOUGH_MEMORY);CHECK(ObservedResult==PB_FULL&&Step==1&&Kinds[0]==K_CALLOC);Balance();}
    else {
        /* Warm exactly the measured workload, including the16-construction
         * quota and native pool/crypto bookkeeping, before comparing handles. */
        Matrix();DWORD warmed=Scenarios,before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Matrix();
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);printf("PASS: native-start %lu warmup/%lu measured schedules, process handles %lu/%lu\n",warmed,Scenarios-warmed,before,after);
    }
    Balance();CHECK(HeapDestroy(ModuleHeap));printf("PASS: %lu actual non-test native/default-legacy startup error scenarios; all core/facade allocations, regions, events, pins, pools, work and joined worker owners balanced\n",Scenarios);return 0;
}
