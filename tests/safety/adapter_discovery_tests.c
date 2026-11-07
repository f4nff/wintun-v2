/* SPDX-License-Identifier: GPL-2.0 */
/* Actual CM interface discovery with valid size/list growth races. Only CM
 * boundaries, clock and cancellable waits are hosted. Heap operations, error
 * mapping and CRT formatting are native; no real device or registry changes. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include <ndisguid.h>
#include "../../api/adapter.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static LPVOID WINAPI TestAlloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI TestFree(HANDLE,DWORD,LPVOID);
#define HeapAlloc TestAlloc
#define HeapFree TestFree
#include "../../api/logger.h"
static CONFIGRET WINAPI TestSize(PULONG,LPGUID,DEVINSTID_W,ULONG);
static CONFIGRET WINAPI TestList(LPGUID,DEVINSTID_W,PZZWSTR,ULONG,ULONG);
static ULONGLONG WINAPI TestClock(VOID);
#define CM_Get_Device_Interface_List_SizeW TestSize
#define CM_Get_Device_Interface_ListW TestList
#define GetTickCount64 TestClock
#ifndef PB_ADAPTER_DISCOVERY_SOURCE
#define PB_ADAPTER_DISCOVERY_SOURCE "../../api/adapter_discovery.h"
#endif
#include PB_ADAPTER_DISCOVERY_SOURCE
#undef CM_Get_Device_Interface_List_SizeW
#undef CM_Get_Device_Interface_ListW
#undef GetTickCount64
#undef HeapAlloc
#undef HeapFree

HANDLE ModuleHeap;
static LPVOID Allocation;
static SIZE_T AllocationBytes;
static DWORD Allocations,Frees,AllocCalls,FailAllocAt,Sizes,Lists,SmallCount,SizeFailureAt,ListFailureAt;
static DWORD ZeroSleeps,DelaySleeps,CancelZeroAt,CancelDelayAt,Delays[512],ParentBudget,FirstListCost;
static DWORD Shape,Logs,Scenarios;
static CONFIGRET SizeError,ListError;
static ULONGLONG Now,Started;
static LPCWSTR Instance;
static WCHAR InstanceStorage[MAX_DEVICE_ID_LEN],Payload[32770];
static ULONG PayloadChars;
static BOOL Forever;
static WCHAR LastLog[1024];
static VOID Begin(VOID)
{
    CHECK(!Allocation);
    Allocations=Frees=AllocCalls=FailAllocAt=Sizes=Lists=SmallCount=SizeFailureAt=ListFailureAt=0;
    ZeroSleeps=DelaySleeps=CancelZeroAt=CancelDelayAt=FirstListCost=Shape=Logs=0;
    ParentBudget=INFINITE; SizeError=CR_ACCESS_DENIED; ListError=CR_NO_SUCH_DEVNODE;
    Now=Started=0; Instance=L"hosted-instance"; Forever=FALSE; LastLog[0]=0;
    wcscpy_s(Payload,_countof(Payload),L"\\\\?\\hosted-interface"); PayloadChars=(ULONG)wcslen(Payload)+2; Payload[PayloadChars-1]=0;
    memset(Delays,0,sizeof(Delays));
}
static VOID End(VOID)
{
    CHECK(!Allocation&&Allocations==Frees&&Sizes<=306&&Lists<=Sizes&&DelaySleeps<=305);
    for (DWORD i=0;i<DelaySleeps;++i) CHECK(Delays[i]&&Delays[i]<=50);
    ++Scenarios;
}
static LPVOID WINAPI TestAlloc(HANDLE Heap,DWORD Flags,SIZE_T Bytes)
{
    CHECK(Heap==ModuleHeap&&!Flags&&!Allocation); ++AllocCalls;
    if (AllocCalls==FailAllocAt) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    Allocation=HeapAlloc(Heap,Flags,Bytes); CHECK(Allocation); AllocationBytes=Bytes; ++Allocations;
    memset(Allocation,0x41,Bytes); return Allocation;
}
static BOOL WINAPI TestFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    CHECK(Heap==ModuleHeap&&!Flags&&Pointer==Allocation&&Allocation&&HeapFree(Heap,Flags,Pointer));
    Allocation=NULL; AllocationBytes=0; ++Frees; SetLastError(ERROR_INVALID_HANDLE); return TRUE;
}
static ULONGLONG WINAPI TestClock(VOID) { return Now; }
BOOL MainOperationSleep(DWORD Delay)
{
    CHECK(!Allocation);
    if (!Delay)
    {
        ++ZeroSleeps;
        if (ZeroSleeps==CancelZeroAt) { SetLastError(ERROR_CANCELLED); return FALSE; }
    }
    else
    {
        CHECK(DelaySleeps<_countof(Delays)); Delays[DelaySleeps++]=Delay;
        if (DelaySleeps==CancelDelayAt) { Now+=Delay/2; SetLastError(ERROR_CANCELLED); return FALSE; }
    }
    ULONGLONG Elapsed=Now-Started;
    if (ParentBudget!=INFINITE&&(Elapsed>=ParentBudget||Delay>=ParentBudget-Elapsed))
    {
        if (Elapsed<ParentBudget) Now+=ParentBudget-Elapsed;
        SetLastError(ERROR_TIMEOUT); return FALSE;
    }
    Now+=Delay; return TRUE;
}
static VOID CheckCm(LPGUID Class,DEVINSTID_W Id,ULONG Flags)
{
    CHECK(IsEqualGUID(Class,&GUID_DEVINTERFACE_NET)&&Id==Instance&&Flags==CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
}
static ULONG Hint(VOID)
{
    BOOL Small=Forever||Lists<SmallCount;
    if (!Small) return Shape==2?512:PayloadChars;
    if (Shape==1) return 2+Sizes*3;
    if (Shape==2) return 512-Sizes;
    return 2;
}
static CONFIGRET WINAPI TestSize(PULONG Count,LPGUID Class,DEVINSTID_W Id,ULONG Flags)
{
    CheckCm(Class,Id,Flags); CHECK(Count&&!Allocation); ++Sizes;
    if (Sizes==SizeFailureAt) return SizeError;
    *Count=Hint(); CHECK(*Count>=2); return CR_SUCCESS;
}
static CONFIGRET WINAPI TestList(LPGUID Class,DEVINSTID_W Id,PZZWSTR Buffer,ULONG Count,ULONG Flags)
{
    CheckCm(Class,Id,Flags); CHECK(Buffer==Allocation&&AllocationBytes==(SIZE_T)Count*sizeof(WCHAR)); ++Lists;
    if (Lists==1) Now+=FirstListCost;
    if (Lists==ListFailureAt) return ListError;
    if (Forever||Lists<=SmallCount)
    {
        /* List growth is valid: at copy time at least Count+1 characters are
         * required, although the earlier size snapshot fit Count. No failed
         * output is consumed, and the next snapshot may grow or shrink. */
        return CR_BUFFER_SMALL;
    }
    CHECK(Count>=PayloadChars); memcpy(Buffer,Payload,PayloadChars*sizeof(WCHAR)); return CR_SUCCESS;
}
static VOID FormatLog(LPCWSTR Format,va_list Args)
{
    ++Logs; _vsnwprintf_s(LastLog,_countof(LastLog),_TRUNCATE,Format,Args); SetLastError(ERROR_INVALID_DATA);
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{
    (void)Level; DWORD Error=GetLastError(); FormatLog(Format,Args); SetLastError(Error); return Error;
}
DWORD LoggerErrorV(DWORD Error,LPCWSTR Format,va_list Args)
{ FormatLog(Format,Args); return Error; }
static LPWSTR Run(VOID) { return AdapterGetDeviceObjectFileName(Instance); }
static VOID Successful(VOID)
{
    LPWSTR Value=Run(); CHECK(Value==Allocation&&Value&&!memcmp(Value,Payload,PayloadChars*sizeof(WCHAR)));
    CHECK(Sizes==SmallCount+1&&Lists==SmallCount+1&&Allocations==SmallCount+1&&Frees==SmallCount&&!Logs);
    Free(Value); End();
}
static VOID SuccessMatrix(VOID)
{
    const DWORD Retries[]={0,1,2,7,100};
    for (DWORD shape=0;shape<3;++shape) for (DWORD retry=0;retry<_countof(Retries);++retry)
    {
        Begin(); Shape=shape; SmallCount=Retries[retry]; Successful();
    }
    for (DWORD kind=0;kind<5;++kind) for (DWORD retry=0;retry<2;++retry)
    {
        Begin(); SmallCount=retry;
        if (kind==0) Instance=L"";
        if (kind==1)
        {
            for (DWORD i=0;i<MAX_DEVICE_ID_LEN-1;++i) InstanceStorage[i]=i%2?L'\x8bbe':L'x';
            InstanceStorage[MAX_DEVICE_ID_LEN-1]=0; Instance=InstanceStorage;
        }
        if (kind==2) { wcscpy_s(Payload,_countof(Payload),L"\\\\?\\\x4e2d\x6587"); PayloadChars=(ULONG)wcslen(Payload)+2; Payload[PayloadChars-1]=0; }
        if (kind==3)
        {
            for (DWORD i=0;i<32767;++i) Payload[i]=i%2?L'\x7f51':L'p';
            Payload[32767]=Payload[32768]=0; PayloadChars=32769;
        }
        if (kind==4) { memcpy(Payload,L"one\0two\0\0",9*sizeof(WCHAR)); PayloadChars=9; }
        Successful();
    }
    for (DWORD retry=0;retry<3;++retry)
    {
        Begin(); SmallCount=retry; Payload[0]=Payload[1]=0; PayloadChars=2;
        CHECK(!Run()&&GetLastError()==ERROR_DEVICE_NOT_AVAILABLE&&!Logs&&Sizes==retry+1&&Lists==retry+1); End();
    }
    Begin(); SmallCount=2; Now=Started=~(ULONGLONG)0-1; Successful();
}
static VOID FailureMatrix(VOID)
{
    const CONFIGRET Errors[]={CR_ACCESS_DENIED,CR_NO_SUCH_DEVNODE,CR_OUT_OF_MEMORY,CR_FAILURE};
    for (DWORD phase=0;phase<2;++phase) for (DWORD at=1;at<=4;++at) for (DWORD error=0;error<_countof(Errors);++error)
    {
        Begin(); SmallCount=4;
        if (!phase) { SizeFailureAt=at; SizeError=Errors[error]; } else { ListFailureAt=at; ListError=Errors[error]; }
        CHECK(!Run()&&GetLastError()==CM_MapCrToWin32Err(Errors[error],ERROR_GEN_FAILURE)&&Logs==1);
        CHECK(Sizes==at&&Lists==at-(!phase?1u:0u)); End();
    }
    /* Every allocation reachable within the complete retry budget can fail. */
    for (DWORD at=1;at<=305;++at)
    {
        Begin(); Forever=TRUE; FailAllocAt=at;
        CHECK(!Run()&&GetLastError()==ERROR_OUTOFMEMORY&&AllocCalls==at&&Allocations==at-1&&Logs==1); End();
    }
    Begin(); Forever=TRUE; FailAllocAt=306;
    CHECK(!Run()&&GetLastError()==ERROR_TIMEOUT&&AllocCalls==305&&!Logs); End();
}
static VOID DeadlineMatrix(VOID)
{
    Begin(); Forever=TRUE; CHECK(!Run()&&GetLastError()==ERROR_TIMEOUT&&Now-Started==15000&&Sizes==305&&Lists==305&&DelaySleeps==305&&!Logs);
    CHECK(Delays[0]==1&&Delays[1]==2&&Delays[2]==4&&Delays[3]==8&&Delays[4]==16&&Delays[5]==32&&Delays[6]==50&&Delays[304]==37); End();
    const DWORD Costs[]={14998,14999,15000,15001};
    for (DWORD cost=0;cost<_countof(Costs);++cost)
    {
        Begin(); SmallCount=1; FirstListCost=Costs[cost];
        if (cost==0) { CHECK(Now==0); Successful(); CHECK(Now==14999); }
        else
        {
            CHECK(!Run()&&GetLastError()==ERROR_TIMEOUT&&Sizes==1&&Lists==1&&Now>=15000&&Now<=15001); End();
        }
    }
    const DWORD Budgets[]={0,1,9,14999};
    for (DWORD budget=0;budget<_countof(Budgets);++budget)
    {
        Begin(); Forever=TRUE; ParentBudget=Budgets[budget];
        CHECK(!Run()&&GetLastError()==ERROR_TIMEOUT&&Now==ParentBudget&&!Logs); End();
    }
    /* A caller such as Win7 readiness may invoke discovery again after the
     * local retry budget. Reset per-call observations, retaining the same
     * parent's origin and elapsed time: a new local budget cannot renew it. */
    ULONGLONG Continued=0;
    for (DWORD call=0;call<3;++call)
    {
        Begin(); Forever=TRUE; ParentBudget=40000; Now=Continued;
        CHECK(!Run()&&GetLastError()==ERROR_TIMEOUT&&!Logs);
        Continued=Now; CHECK(Continued==(call==0?15000u:call==1?30000u:40000u)); End();
    }
    for (DWORD point=1;point<=8;++point)
    {
        Begin(); Forever=TRUE; CancelZeroAt=point;
        CHECK(!Run()&&GetLastError()==ERROR_CANCELLED&&Sizes==point-1&&Lists==point-1&&!Logs); End();
        Begin(); Forever=TRUE; CancelDelayAt=point;
        CHECK(!Run()&&GetLastError()==ERROR_CANCELLED&&Sizes==point&&Lists==point&&!Logs); End();
    }
}
int main(int argc,char **argv)
{
    CHECK(argc<=2); ModuleHeap=HeapCreate(0,0,0); CHECK(ModuleHeap);
    if (argc==2)
    {
        CHECK(!strcmp(argv[1],"1")); Begin(); SmallCount=1;
        LPWSTR Value=Run(); DWORD Error=GetLastError();
        if (!Value)
        {
            CHECK(Sizes==1&&Lists==1&&Error==CM_MapCrToWin32Err(CR_BUFFER_SMALL,ERROR_GEN_FAILURE));
            End(); CHECK(HeapDestroy(ModuleHeap)); puts("INTERFACE_LIST_GROWTH_NOT_RETRIED"); return 95;
        }
        CHECK(Value==Allocation&&Sizes==2&&Lists==2&&!memcmp(Value,Payload,PayloadChars*sizeof(WCHAR)));
        Free(Value); End();
    }
    else
    {
        DWORD Before,After; CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for (DWORD repeat=0;repeat<3;++repeat) { SuccessMatrix(); FailureMatrix(); DeadlineMatrix(); }
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After);
    }
    CHECK(HeapDestroy(ModuleHeap)); printf("PASS: actual adapter discovery growth/deadline/ownership (%lu scenarios)\n",Scenarios); return 0;
}
