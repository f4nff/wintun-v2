/* SPDX-License-Identifier: GPL-2.0 */
/* Exact complete OpenAdapterInternal from its hash-locked production aggregate.
 * SetupAPI, metadata/wait, runtime and cleanup admission are hosted. Native
 * heap/events/mutexes, CRT formatting/comparison and optional allocation guard
 * pages verify every acquired owner. No real devices or registry are accessed. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../api/adapter.h"
#include "guard_memory.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static LPVOID WINAPI TestHeapAlloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI TestHeapFree(HANDLE,DWORD,LPVOID);
#define HeapAlloc TestHeapAlloc
#define HeapFree TestHeapFree
#include "../../api/logger.h"
#undef HeapAlloc
#undef HeapFree
static HDEVINFO WINAPI TestSet(const GUID *,PCWSTR,HWND,DWORD,HDEVINFO,PCWSTR,PVOID);
static BOOL WINAPI TestEnum(HDEVINFO,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE *,PBYTE,DWORD,PDWORD,DWORD);
static BOOL WINAPI TestId(HDEVINFO,PSP_DEVINFO_DATA,PWSTR,DWORD,PDWORD);
static BOOL WINAPI TestDestroy(HDEVINFO);
static BOOL WaitForInterface(WCHAR *);
static BOOL PopulateAdapterData(WINTUN_ADAPTER *);
static VOID QueueUpOrphanedDeviceCleanupRoutine(VOID);
HANDLE NamespaceTakeDeviceInstallationMutex(VOID);
VOID NamespaceReleaseMutex(HANDLE);
#define SetupDiGetClassDevsExW TestSet
#define SetupDiEnumDeviceInfo TestEnum
#define SetupDiGetDevicePropertyW TestProperty
#define SetupDiGetDeviceInstanceIdW TestId
#define SetupDiDestroyDeviceInfoList TestDestroy
#ifndef PB_ADAPTER_OPEN_SOURCE
#error Build through Build-AdapterOpenTests to extract the complete actual function
#endif
#include PB_ADAPTER_OPEN_SOURCE
#undef SetupDiGetClassDevsExW
#undef SetupDiEnumDeviceInfo
#undef SetupDiGetDevicePropertyW
#undef SetupDiGetDeviceInstanceIdW
#undef SetupDiDestroyDeviceInfoList

HANDLE ModuleHeap;
BOOL IsWindows7;
const DEVPROPKEY DEVPKEY_Wintun_Name={{0x3361c968,0x2f2e,0x4660,{0xb4,0x7e,0x69,0x9c,0xdc,0x4c,0x32,0xb9}},DEVPROPID_FIRST_USABLE+1};
typedef struct RECORD {LPCWSTR Name;DWORD Error;DEVPROPTYPE Type;} RECORD;
typedef struct BLOCK {VOID *Memory;SIZE_T Size;BOOL Guard;} BLOCK;
static BLOCK Blocks[8];
static RECORD Records[8];
static HANDLE DeviceSet,InstallMutex;
static WINTUN_ADAPTER *LiveAdapter;
static DWORD Count,EnumErrorAt,EnumError,CancelAt,FailureStage,FailureError,FailAllocation;
static DWORD Enums,Properties,IdCalls,WaitCalls,Populates,Sleeps,Queues,Closes,Leases;
static DWORD SetsCreated,SetsClosed,MutexesCreated,MutexesClosed,AllocationAttempts,Allocations,Frees,Live;
static DWORD Logs,LoggerErrorState,Scenarios,ExpectedHandles,LastIndex;
static BOOL Persistent,PopulatePartial,BaselineControl;
static LPCWSTR Wanted,Instance;
static const WCHAR DefaultInstance[]=L"ROOT\\PacketBridgeV2\\0000";
static BOOL Fail(DWORD stage)
{if(FailureStage!=stage)return FALSE;SetLastError(FailureError);return TRUE;}
static LPVOID WINAPI TestHeapAlloc(HANDLE heap,DWORD flags,SIZE_T bytes)
{
    CHECK(heap==ModuleHeap&&!(flags&~HEAP_ZERO_MEMORY));++AllocationAttempts;
    if(AllocationAttempts==FailAllocation){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    VOID *memory;BOOL guard=FALSE;
#ifdef PB_GUARD_TESTS
    memory=PbTestGuardAlloc(bytes);guard=TRUE;if(!(flags&HEAP_ZERO_MEMORY))memset(memory,0xa5,bytes);
#else
    memory=HeapAlloc(heap,flags,bytes);
#endif
    CHECK(memory);DWORD i;for(i=0;i<_countof(Blocks)&&Blocks[i].Memory;++i){}CHECK(i<_countof(Blocks));
    Blocks[i]=(BLOCK){memory,bytes,guard};++Live;++Allocations;return memory;
}
static BOOL WINAPI TestHeapFree(HANDLE heap,DWORD flags,LPVOID memory)
{
    CHECK(heap==ModuleHeap&&!flags&&memory);DWORD i;for(i=0;i<_countof(Blocks)&&Blocks[i].Memory!=memory;++i){}CHECK(i<_countof(Blocks)&&Live);
    if(Blocks[i].Guard)PbTestGuardFree(memory);else CHECK(HeapFree(heap,flags,memory));
    Blocks[i]=(BLOCK){0};--Live;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;
}
BOOL MainRuntimeAcquire(VOID)
{CHECK(!Leases);if(Fail(1))return FALSE;++Leases;return TRUE;}
VOID MainRuntimeRetain(VOID){CHECK(Leases==1);++Leases;}
VOID MainRuntimeRelease(VOID){CHECK(Leases);--Leases;SetLastError(ERROR_INVALID_HANDLE);}
HANDLE NamespaceTakeDeviceInstallationMutex(VOID)
{CHECK(Leases==1&&!InstallMutex);if(Fail(2))return NULL;InstallMutex=CreateMutexW(NULL,TRUE,NULL);CHECK(InstallMutex);++MutexesCreated;return InstallMutex;}
VOID NamespaceReleaseMutex(HANDLE mutex)
{CHECK(mutex==InstallMutex&&InstallMutex&&ReleaseMutex(mutex)&&CloseHandle(mutex));InstallMutex=NULL;++MutexesClosed;SetLastError(ERROR_INVALID_HANDLE);}
BOOL MainOperationSleep(DWORD delay)
{CHECK(!delay);++Sleeps;if(Sleeps==CancelAt){SetLastError(ERROR_CANCELLED);return FALSE;}return TRUE;}
static HDEVINFO WINAPI TestSet(const GUID *guid,PCWSTR enumerator,HWND window,DWORD flags,HDEVINFO reuse,PCWSTR machine,PVOID reserved)
{
    CHECK(guid==&GUID_DEVCLASS_NET&&!wcscmp(enumerator,IsWindows7?L"ROOT\\" WINTUN_HWID:L"SWD\\" WINTUN_HWID)&&!window&&flags==DIGCF_PRESENT&&!reuse&&!machine&&!reserved&&Leases==2&&!DeviceSet);
    if(Fail(3))return INVALID_HANDLE_VALUE;DeviceSet=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(DeviceSet);++SetsCreated;return(HDEVINFO)DeviceSet;
}
static BOOL WINAPI TestEnum(HDEVINFO set,DWORD index,PSP_DEVINFO_DATA data)
{
    CHECK(set==(HDEVINFO)DeviceSet&&DeviceSet&&data&&data->cbSize==sizeof(*data));++Enums;
    if(Persistent||index==EnumErrorAt){SetLastError(EnumError);return FALSE;}
    if(index>=Count){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}
    data->DevInst=index+1;LastIndex=index;return TRUE;
}
static BOOL WINAPI TestProperty(HDEVINFO set,PSP_DEVINFO_DATA data,const DEVPROPKEY *key,DEVPROPTYPE *type,PBYTE bytes,DWORD count,PDWORD required,DWORD flags)
{
    CHECK(set==(HDEVINFO)DeviceSet&&data->DevInst==LastIndex+1&&key==&DEVPKEY_Wintun_Name&&type&&bytes&&count==PB_MAX_ADAPTER_NAME*sizeof(WCHAR)&&!required&&!flags);++Properties;
    RECORD *record=Records+LastIndex;if(record->Error){SetLastError(record->Error);return FALSE;}
    *type=record->Type;
    if(record->Type==DEVPROP_TYPE_STRING)CHECK(!wcscpy_s((LPWSTR)bytes,PB_MAX_ADAPTER_NAME,record->Name));
    else if(record->Type==DEVPROP_TYPE_BINARY)memset(bytes,0x41,count);
    else if(record->Type==DEVPROP_TYPE_UINT32)*(DWORD *)bytes=42;
    else{static const WCHAR list[]=L"other\0value\0";CHECK(record->Type==DEVPROP_TYPE_STRING_LIST);memcpy(bytes,list,sizeof(list));}
    SetLastError(ERROR_INVALID_HANDLE);return TRUE;
}
static BOOL WINAPI TestId(HDEVINFO set,PSP_DEVINFO_DATA data,PWSTR output,DWORD capacity,PDWORD required)
{CHECK(set==(HDEVINFO)DeviceSet&&data->DevInst==LastIndex+1&&output&&capacity==MAX_DEVICE_ID_LEN&&required&&*required==capacity);++IdCalls;if(Fail(4))return FALSE;*required=(DWORD)wcslen(Instance)+1;CHECK(*required<=capacity&&!wcscpy_s(output,capacity,Instance));return TRUE;}
static BOOL WaitForInterface(WCHAR *instance)
{CHECK(DeviceSet&&instance&&!wcscmp(instance,Instance));++WaitCalls;return!Fail(5);}
static BOOL PopulateAdapterData(WINTUN_ADAPTER *adapter)
{
    CHECK(DeviceSet&&adapter&&adapter->RuntimeOwned&&adapter->DevInfo==(HDEVINFO)DeviceSet&&adapter->DevInfoData.DevInst==LastIndex+1&&!wcscmp(adapter->DevInstanceID,Instance));++Populates;LiveAdapter=adapter;
    if(FailureStage==6&&!PopulatePartial){SetLastError(FailureError);return FALSE;}
    static const WCHAR path[]=L"\\\\?\\hosted-interface";adapter->InterfaceFilename=AllocArray(_countof(path),sizeof(WCHAR));if(!adapter->InterfaceFilename)return FALSE;
    memcpy(adapter->InterfaceFilename,path,sizeof(path));adapter->LuidIndex=17;adapter->IfType=53;
    if(Fail(6))return FALSE;return TRUE;
}
static BOOL WINAPI TestDestroy(HDEVINFO set)
{CHECK(set==(HDEVINFO)DeviceSet&&DeviceSet&&CloseHandle(DeviceSet));DeviceSet=NULL;++SetsClosed;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
static VOID QueueUpOrphanedDeviceCleanupRoutine(VOID)
{++Queues;SetLastError(ERROR_INVALID_HANDLE);}
VOID WINAPI WintunCloseAdapter(WINTUN_ADAPTER *adapter)
{
    CHECK(adapter&&adapter->RuntimeOwned&&!adapter->SwDevice&&!adapter->DevInfo);++Closes;
    Free(adapter->InterfaceFilename);Free(adapter);LiveAdapter=NULL;MainRuntimeRelease();QueueUpOrphanedDeviceCleanupRoutine();
}
static VOID RenderLog(LPCWSTR format,va_list args)
{WCHAR text[1024];CHECK(_vsnwprintf_s(text,_countof(text),_TRUNCATE,format,args)>=0);++Logs;SetLastError(LoggerErrorState);}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL level,LPCWSTR format,va_list args)
{(void)level;DWORD error=GetLastError();RenderLog(format,args);SetLastError(error);return error;}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{RenderLog(format,args);return error;}
static VOID Begin(VOID)
{
    CHECK(!DeviceSet&&!InstallMutex&&!Live&&!Leases&&!LiveAdapter);Count=1;Records[0]=(RECORD){L"wanted",0,DEVPROP_TYPE_STRING};
    EnumErrorAt=CancelAt=MAXDWORD;EnumError=ERROR_NOT_ENOUGH_MEMORY;FailureStage=0;FailureError=ERROR_ACCESS_DENIED;FailAllocation=0;
    Enums=Properties=IdCalls=WaitCalls=Populates=Sleeps=Queues=Closes=Logs=LastIndex=0;
    SetsCreated=SetsClosed=MutexesCreated=MutexesClosed=AllocationAttempts=Allocations=Frees=0;
    Persistent=PopulatePartial=FALSE;LoggerErrorState=ERROR_INVALID_HANDLE;Wanted=L"wanted";Instance=DefaultInstance;IsWindows7=TRUE;
}
static VOID End(VOID)
{
    CHECK(!DeviceSet&&!InstallMutex&&!LiveAdapter&&!Live&&!Leases&&SetsCreated==SetsClosed&&MutexesCreated==MutexesClosed&&Allocations==Frees);
    DWORD handles;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==ExpectedHandles);++Scenarios;
}
static VOID Run(BOOL success,DWORD error)
{
    SetLastError(ERROR_SUCCESS);WINTUN_ADAPTER *adapter=OpenAdapterInternal(Wanted);DWORD actualError=GetLastError();
    BOOL baseline=BaselineControl&&!adapter&&actualError==ERROR_NOT_FOUND&&EnumErrorAt==0&&Enums==2;
    if(!baseline){CHECK((adapter!=NULL)==success);if(!adapter)CHECK(actualError==error);}
    if(adapter){CHECK(adapter==LiveAdapter&&adapter->InterfaceFilename&&adapter->LuidIndex==17&&adapter->IfType==53&&Leases==1&&!adapter->DevInfo&&!wcscmp(adapter->DevInstanceID,Instance));WintunCloseAdapter(adapter);}
    End();
    if(baseline){CHECK(!Closes||Closes==1);CHECK(HeapDestroy(ModuleHeap));fputs("ADAPTER_OPEN_ENUMERATION_ERROR_LOST_AND_SPUN\n",stderr);ExitProcess(110);}
}
static VOID EnumMatrix(VOID)
{
    /* false+zero is defensive injection, not a normal documented SDK error. */
    const DWORD errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_INVALID_DATA,ERROR_INSUFFICIENT_BUFFER,ERROR_OPERATION_ABORTED,MAXDWORD,ERROR_SUCCESS};
    const DWORD indices[]={0,1,3,7};const DWORD loggerErrors[]={ERROR_SUCCESS,ERROR_INVALID_HANDLE,MAXDWORD};
    for(DWORD system=0;system<2;++system)for(DWORD i=0;i<_countof(indices);++i)for(DWORD e=0;e<_countof(errors);++e)for(DWORD l=0;l<_countof(loggerErrors);++l){
        Begin();IsWindows7=system==0;Count=indices[i];for(DWORD n=0;n<Count;++n)Records[n]=(RECORD){L"other",0,DEVPROP_TYPE_STRING};
        EnumErrorAt=Count;EnumError=errors[e];LoggerErrorState=loggerErrors[l];Run(FALSE,EnumError?EnumError:ERROR_GEN_FAILURE);CHECK(Enums==Count+1&&Properties==Count&&!IdCalls&&!WaitCalls&&!Populates&&Closes==1);
    }
    /* Persistent failures are a bounded fault model, not a claim about SDK
     * error ordering at every invalid index. Cancellation bounds the old loop. */
    Begin();Persistent=TRUE;CancelAt=17;Run(FALSE,EnumError);CHECK(Enums==1&&Sleeps==1);
    Begin();EnumErrorAt=0;Records[1]=(RECORD){L"wanted",0,DEVPROP_TYPE_STRING};Count=2;Run(FALSE,EnumError);CHECK(Enums==1&&!Properties);
}
static VOID SearchMatrix(VOID)
{
    for(DWORD system=0;system<2;++system)for(DWORD count=0;count<=_countof(Records);++count){Begin();IsWindows7=system==0;Count=count;for(DWORD n=0;n<count;++n)Records[n]=(RECORD){L"other",0,DEVPROP_TYPE_STRING};Run(FALSE,ERROR_NOT_FOUND);CHECK(Enums==count+1&&Properties==count);}
    for(DWORD at=0;at<_countof(Records);++at){Begin();Count=_countof(Records);for(DWORD n=0;n<Count;++n)Records[n]=(RECORD){n==at?L"WaNtEd":L"other",0,DEVPROP_TYPE_STRING};EnumErrorAt=at+1;Run(TRUE,0);CHECK(Enums==at+1&&Properties==at+1&&IdCalls==1&&WaitCalls==1&&Populates==1);}
    const DWORD propertyErrors[]={ERROR_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_INSUFFICIENT_BUFFER,ERROR_INVALID_DATA};
    for(DWORD e=0;e<_countof(propertyErrors);++e)for(DWORD match=0;match<2;++match){Begin();Count=2;Records[0]=(RECORD){NULL,propertyErrors[e],DEVPROP_TYPE_STRING};Records[1]=(RECORD){match?L"wanted":L"other",0,DEVPROP_TYPE_STRING};Run(match,ERROR_NOT_FOUND);CHECK(Properties==2&&Enums==(match?2u:3u));}
    const DEVPROPTYPE types[]={DEVPROP_TYPE_BINARY,DEVPROP_TYPE_UINT32,DEVPROP_TYPE_STRING_LIST};
    for(DWORD t=0;t<_countof(types);++t){Begin();Count=2;Records[0]=(RECORD){NULL,0,types[t]};Records[1]=(RECORD){L"wanted",0,DEVPROP_TYPE_STRING};Run(TRUE,0);CHECK(Properties==2&&Enums==2);}
    WCHAR maximum[PB_MAX_ADAPTER_NAME],instance[MAX_DEVICE_ID_LEN];for(DWORD i=0;i<_countof(maximum)-1;++i)maximum[i]=L'\x6d4b';maximum[_countof(maximum)-1]=0;
    for(DWORD i=0;i<_countof(instance)-1;++i)instance[i]=L'A';instance[_countof(instance)-1]=0;
    static const WCHAR prefix[]=L"ROOT\\" WINTUN_HWID L"\\";memcpy(instance,prefix,sizeof(prefix)-sizeof(WCHAR));
    const WCHAR *names[]={L"wanted",L"\x6d4b\x8bd5",maximum};
    for(DWORD n=0;n<_countof(names);++n)for(DWORD system=0;system<2;++system){Begin();IsWindows7=system==0;Wanted=names[n];Records[0].Name=Wanted;Instance=instance;Run(TRUE,0);}
}
static VOID FailureMatrix(VOID)
{
    const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_CANCELLED,ERROR_TIMEOUT,ERROR_INVALID_DATA,MAXDWORD};
    const DWORD loggerErrors[]={ERROR_SUCCESS,ERROR_INVALID_HANDLE,MAXDWORD};
    for(DWORD stage=1;stage<=6;++stage)for(DWORD e=0;e<_countof(errors);++e)for(DWORD l=0;l<_countof(loggerErrors);++l){Begin();FailureStage=stage;FailureError=errors[e];LoggerErrorState=loggerErrors[l];Run(FALSE,FailureError);if(stage<=2)CHECK(!AllocationAttempts&&!Closes);if(stage>=4)CHECK(IdCalls==1);}
    for(DWORD e=0;e<_countof(errors);++e){Begin();FailureStage=6;FailureError=errors[e];PopulatePartial=TRUE;Run(FALSE,FailureError);CHECK(Allocations==2&&Frees==2&&Closes==1);}
    for(DWORD failure=1;failure<=3;++failure){Begin();FailAllocation=failure;Run(failure==3,ERROR_OUTOFMEMORY);CHECK(AllocationAttempts==(failure==1?1u:2u));}
    for(DWORD cancel=1;cancel<=8;++cancel){Begin();Count=8;for(DWORD n=0;n<Count;++n)Records[n]=(RECORD){L"other",0,DEVPROP_TYPE_STRING};CancelAt=cancel;Run(FALSE,ERROR_CANCELLED);CHECK(Sleeps==cancel&&Enums==cancel-1);}
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);CHECK(GetProcessHandleCount(GetCurrentProcess(),&ExpectedHandles));
    if(argc==2){CHECK(!strcmp(argv[1],"1"));BaselineControl=TRUE;Begin();Records[0].Name=L"other";EnumErrorAt=0;Run(FALSE,EnumError);CHECK(Enums==1);}
    else{EnumMatrix();SearchMatrix();FailureMatrix();}
    CHECK(HeapDestroy(ModuleHeap));printf("PASS: %lu actual adapter-open enumeration/search/failure owner scenarios; native resources/CRT with hosted SDK, no devices or registry.\n",Scenarios);return 0;
}
