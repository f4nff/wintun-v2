/* SPDX-License-Identifier: GPL-2.0 */
/* Actual reader/toggle loops. Custom BINARY/integer/GUID/list properties are
 * legal successes; malformed STRING outputs below are separate fault models.
 * SDK boundaries are hosted; heap and safe-path CRT formatting are native.
 * No real device, registry, driver or external file is changed. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <cfgmgr32.h>
#include <SetupAPI.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../api/driver.h"
#include "../../api/adapter.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError()); ExitProcess(1); } } while (0)
static LPVOID WINAPI TestAlloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI TestFree(HANDLE,DWORD,LPVOID);
#define HeapAlloc TestAlloc
#define HeapFree TestFree
#include "../../api/logger.h"
static BOOL WINAPI TestEnum(HDEVINFO,DWORD,PSP_DEVINFO_DATA);
static BOOL WINAPI TestProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE *,PBYTE,DWORD,PDWORD,DWORD);
static CONFIGRET WINAPI TestStatus(PULONG,PULONG,DEVINST,ULONG);
#define SetupDiEnumDeviceInfo TestEnum
#define SetupDiGetDevicePropertyW TestProperty
#define CM_Get_DevNode_Status TestStatus
#include "../../api/adapter_name_log.h"
struct _SP_DEVINFO_DATA_LIST { SP_DEVINFO_DATA Data; struct _SP_DEVINFO_DATA_LIST *Next; };
#ifndef PB_DRIVER_TOGGLE_SOURCE
#define PB_DRIVER_TOGGLE_SOURCE "../../api/driver_toggle.h"
#endif
#include PB_DRIVER_TOGGLE_SOURCE
#undef SetupDiEnumDeviceInfo
#undef SetupDiGetDevicePropertyW
#undef CM_Get_DevNode_Status
#undef HeapAlloc
#undef HeapFree

HANDLE ModuleHeap;
const DEVPROPKEY DEVPKEY_Wintun_Name={{0x3361c968,0x2f2e,0x4660,{0xb4,0x7e,0x69,0x9c,0xdc,0x4c,0x32,0xb9}},DEVPROPID_FIRST_USABLE+1};
static VOID *Blocks[8];
static DWORD Allocated,Freed,AllocCalls,FailAllocAt,Mode,GetterError,GetterCalls,UnsafeNames,Formats,SafeNames,Logs;
static DWORD Records,EnumSkipAt,CancelAt,Sleeps,DisableFails,EnableFails,AlreadyDisabled,UnknownStatus,Disables,Enables,Scenarios;
static WCHAR Maximum[PB_MAX_ADAPTER_NAME],LastLine[1024];
static LPCWSTR ExpectedName;
static const DWORD OperationErrors[]={ERROR_ACCESS_DENIED,ERROR_SHARING_VIOLATION,ERROR_NOT_ENOUGH_MEMORY};
enum {
    NameString,NameBinary,NameBinaryOdd,NameUint32,NameUint16,NameGuid,NameList,NameBoolean,NameEmpty,NameMaximum,NameUnicode,
    NameFailure,NameFailurePoison,
    /* Explicit corruption boundaries, not natural successful SDK STRINGs. */
    FaultNoTerminator,FaultZeroSize,FaultOddSize,FaultOversize,FaultMaxSize,FaultShortNoTerminator
};
static VOID Begin(VOID)
{
    CHECK(Allocated==Freed);for(DWORD i=0;i<_countof(Blocks);++i)CHECK(!Blocks[i]);
    Allocated=Freed=AllocCalls=FailAllocAt=GetterCalls=UnsafeNames=Formats=SafeNames=Logs=0;
    Mode=NameString;GetterError=ERROR_NOT_FOUND;Records=1;EnumSkipAt=MAXDWORD;
    CancelAt=Sleeps=DisableFails=EnableFails=AlreadyDisabled=UnknownStatus=Disables=Enables=0;
    ExpectedName=L"shown-name";LastLine[0]=0;
    for(DWORD i=0;i<PB_MAX_ADAPTER_NAME-1;++i)Maximum[i]=L'\x7f51';Maximum[PB_MAX_ADAPTER_NAME-1]=0;
}
static VOID End(VOID)
{CHECK(Allocated==Freed);for(DWORD i=0;i<_countof(Blocks);++i)CHECK(!Blocks[i]);++Scenarios;}
static VOID Select(DWORD Selected)
{
    Mode=Selected;ExpectedName=L"<unknown>";
    if(Mode==NameString)ExpectedName=L"shown-name";
    if(Mode==NameEmpty)ExpectedName=L"";
    if(Mode==NameMaximum)ExpectedName=Maximum;
    if(Mode==NameUnicode)ExpectedName=L"\x7f51\x5361\xd83d\xde00";
}
static LPVOID WINAPI TestAlloc(HANDLE Heap,DWORD Flags,SIZE_T Bytes)
{
    CHECK(Heap==ModuleHeap);++AllocCalls;
    if(AllocCalls==FailAllocAt){SetLastError(ERROR_INVALID_HANDLE);return NULL;}
    VOID *Value=HeapAlloc(Heap,Flags,Bytes);CHECK(Value);
    for(DWORD i=0;i<_countof(Blocks);++i)if(!Blocks[i]){Blocks[i]=Value;++Allocated;return Value;}
    CHECK(FALSE);return NULL;
}
static BOOL WINAPI TestFree(HANDLE Heap,DWORD Flags,LPVOID Pointer)
{
    CHECK(Heap==ModuleHeap&&!Flags&&Pointer);
    for(DWORD i=0;i<_countof(Blocks);++i)if(Blocks[i]==Pointer)
    {CHECK(HeapFree(Heap,Flags,Pointer));Blocks[i]=NULL;++Freed;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
    CHECK(FALSE);return FALSE;
}
BOOL MainOperationSleep(DWORD Delay)
{CHECK(!Delay);++Sleeps;if(Sleeps!=CancelAt)return TRUE;SetLastError(ERROR_CANCELLED);return FALSE;}
static BOOL WINAPI TestEnum(HDEVINFO Set,DWORD Index,PSP_DEVINFO_DATA Info)
{
    CHECK(Set==(HDEVINFO)(ULONG_PTR)1&&Info->cbSize==sizeof(*Info));
    if(Index==EnumSkipAt){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}
    if(Index>=Records){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}
    Info->DevInst=Index+1;return TRUE;
}
static BOOL WINAPI TestProperty(HDEVINFO Set,PSP_DEVINFO_DATA Info,const DEVPROPKEY *Key,DEVPROPTYPE *Type,PBYTE Buffer,DWORD Bytes,PDWORD Required,DWORD Flags)
{
    CHECK(Set==(HDEVINFO)(ULONG_PTR)1&&Info->cbSize==sizeof(*Info)&&Key==&DEVPKEY_Wintun_Name&&Type&&Buffer&&
        Bytes==PB_MAX_ADAPTER_NAME*sizeof(WCHAR)&&!Flags);++GetterCalls;DWORD Size=Bytes;
    if(Mode==NameFailure){SetLastError(GetterError);return FALSE;}
    if(Mode==NameFailurePoison){memset(Buffer,0x41,Bytes);if(Required)*Required=MAXDWORD;SetLastError(GetterError);return FALSE;}
    *Type=DEVPROP_TYPE_STRING;
    if(Mode==NameString||Mode==NameEmpty||Mode==NameMaximum||Mode==NameUnicode)
    {Size=(DWORD)(wcslen(ExpectedName)+1)*sizeof(WCHAR);memcpy(Buffer,ExpectedName,Size);}
    else if(Mode==NameList){*Type=DEVPROP_TYPE_STRING_LIST;Size=sizeof(L"one\0two\0");memcpy(Buffer,L"one\0two\0",Size);}
    else if(Mode==NameUint32){*Type=DEVPROP_TYPE_UINT32;Size=sizeof(UINT32);memset(Buffer,0x41,Size);}
    else if(Mode==NameUint16){*Type=DEVPROP_TYPE_UINT16;Size=sizeof(UINT16);memset(Buffer,0x41,Size);}
    else if(Mode==NameGuid){*Type=DEVPROP_TYPE_GUID;Size=sizeof(GUID);memset(Buffer,0x41,Size);}
    else if(Mode==NameBoolean){*Type=DEVPROP_TYPE_BOOLEAN;Size=1;Buffer[0]=(BYTE)DEVPROP_TRUE;}
    else if(Mode==NameBinary||Mode==NameBinaryOdd){*Type=DEVPROP_TYPE_BINARY;Size=Bytes-(Mode==NameBinaryOdd?1u:0u);memset(Buffer,0x41,Size);}
    else
    {
        memset(Buffer,0x41,Bytes);
        if(Mode==FaultZeroSize)Size=0;
        if(Mode==FaultOddSize)Size=3;
        if(Mode==FaultOversize)Size=Bytes+2;
        if(Mode==FaultMaxSize)Size=MAXDWORD;
        if(Mode==FaultShortNoTerminator)Size=6;
    }
    if(Required)*Required=Size;SetLastError(GetterError);return TRUE;
}
static CONFIGRET WINAPI TestStatus(PULONG Status,PULONG Number,DEVINST Id,ULONG Flags)
{
    CHECK(Id>=1&&Id<=Records&&!Flags);
    if(UnknownStatus&(1u<<(Id-1)))return CR_FAILURE;
    *Status=AlreadyDisabled&(1u<<(Id-1))?DN_HAS_PROBLEM:0;*Number=*Status?CM_PROB_DISABLED:0;return CR_SUCCESS;
}
BOOL AdapterDisableInstance(HDEVINFO Set,SP_DEVINFO_DATA *Info)
{
    CHECK(Set==(HDEVINFO)(ULONG_PTR)1&&Info->DevInst>=1&&Info->DevInst<=Records);++Disables;
    if(DisableFails&(1u<<(Info->DevInst-1))){SetLastError(OperationErrors[Info->DevInst-1]);return FALSE;}return TRUE;
}
BOOL AdapterEnableInstance(HDEVINFO Set,SP_DEVINFO_DATA *Info)
{
    CHECK(Set==(HDEVINFO)(ULONG_PTR)1&&Info->DevInst>=1&&Info->DevInst<=Records);++Enables;
    if(EnableFails&(1u<<(Info->DevInst-1))){SetLastError(OperationErrors[Info->DevInst-1]);return FALSE;}return TRUE;
}
static VOID FormatLog(LPCWSTR Format,va_list Args)
{
    ++Logs;
    if(wcsstr(Format,L"adapter \""))
    {
        va_list Copy;va_copy(Copy,Args);LPCWSTR Name=va_arg(Copy,LPCWSTR);va_end(Copy);
        /* Observe the historical unsafe input before reading beyond it. The
         * actual loop then finishes and releases its owned allocation list. */
        if(wcsnlen(Name,PB_MAX_ADAPTER_NAME)>=PB_MAX_ADAPTER_NAME){++UnsafeNames;return;}
        CHECK(!wcscmp(Name,ExpectedName));++SafeNames;
    }
    _vsnwprintf_s(LastLine,_countof(LastLine),_TRUNCATE,Format,Args);++Formats;SetLastError(ERROR_INVALID_DATA);
}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{(void)Level;DWORD Saved=GetLastError();FormatLog(Format,Args);SetLastError(Saved);return Saved;}
DWORD LoggerErrorV(DWORD Code,LPCWSTR Format,va_list Args)
{FormatLog(Format,Args);return Code;}
static VOID ReleaseList(SP_DEVINFO_DATA_LIST *Head)
{while(Head){SP_DEVINFO_DATA_LIST *Next=Head->Next;Free(Head);Head=Next;}}
static VOID DirectMatrix(VOID)
{
    const DWORD Errors[]={ERROR_SUCCESS,ERROR_ACCESS_DENIED,MAXDWORD};
    const DWORD GetterErrors[]={ERROR_NOT_FOUND,ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INSUFFICIENT_BUFFER,ERROR_INVALID_HANDLE};
    for(DWORD mode=NameString;mode<=FaultShortNoTerminator;++mode)for(DWORD error=0;error<_countof(Errors);++error)
        for(DWORD getter=0;getter<_countof(GetterErrors);++getter)
        {
            Begin();Select(mode);GetterError=GetterErrors[getter];struct {WCHAR Before,Name[PB_MAX_ADAPTER_NAME],After;} Guard;
            Guard.Before=0x1234;Guard.After=0x5678;memset(Guard.Name,0xcc,sizeof(Guard.Name));
            SP_DEVINFO_DATA Info={.cbSize=sizeof(Info),.DevInst=1};SetLastError(Errors[error]);
            AdapterNameForLog((HDEVINFO)(ULONG_PTR)1,&Info,Guard.Name);
            CHECK(GetLastError()==Errors[error]&&Guard.Before==0x1234&&Guard.After==0x5678&&!wcscmp(Guard.Name,ExpectedName)&&GetterCalls==1&&!Allocated&&!Logs);End();
        }
}
static VOID ToggleMatrix(VOID)
{
    for(DWORD mode=NameString;mode<=FaultShortNoTerminator;++mode)
    {
        Begin();Select(mode);Records=3;SP_DEVINFO_DATA_LIST *Head=NULL;
        CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&Head&&Head->Next&&Head->Next->Next&&!Head->Next->Next->Next);
        CHECK(EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&Disables==3&&Enables==3&&SafeNames==6&&Formats==6&&!UnsafeNames);ReleaseList(Head);End();
    }
    for(DWORD mode=NameString;mode<=NameFailurePoison;++mode)for(DWORD at=0;at<3;++at)
    {
        Begin();Select(mode);Records=3;DisableFails=1u<<at;SP_DEVINFO_DATA_LIST *Head=NULL;
        CHECK(!DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&GetLastError()==OperationErrors[at]&&Disables==3);
        CHECK(EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&Enables==2&&!UnsafeNames);ReleaseList(Head);End();
        Begin();Select(mode);Records=3;Head=NULL;CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head));EnableFails=1u<<at;
        CHECK(!EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&GetLastError()==OperationErrors[at]&&Enables==3&&!UnsafeNames);ReleaseList(Head);End();
    }
    Begin();Select(NameBinary);Records=3;DisableFails=5;SP_DEVINFO_DATA_LIST *Head=NULL;
    CHECK(!DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&GetLastError()==OperationErrors[0]);ReleaseList(Head);End();
    Begin();Select(NameBinary);Records=3;Head=NULL;CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head));EnableFails=5;
    CHECK(!EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&GetLastError()==OperationErrors[2]);ReleaseList(Head);End();
    for(DWORD at=1;at<=4;++at)
    {
        Begin();Select(NameBinary);Records=3;FailAllocAt=at;Head=NULL;
        CHECK(!DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&GetLastError()==ERROR_OUTOFMEMORY&&Allocated==at-1&&!Freed);
        if(Head)CHECK(EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head));ReleaseList(Head);End();
        Begin();Select(NameBinary);Records=3;CancelAt=at;Head=NULL;
        CHECK(!DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&GetLastError()==ERROR_CANCELLED&&Allocated==at-1);
        if(Head)CHECK(EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head));ReleaseList(Head);End();
    }
    Begin();Select(NameBinary);Records=3;AlreadyDisabled=1;UnknownStatus=2;Head=NULL;
    CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&Disables==1&&GetterCalls==3&&SafeNames==1);
    CHECK(EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&Enables==1);ReleaseList(Head);End();
    Begin();Select(NameBinary);Records=2;EnumSkipAt=0;Head=NULL;
    CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&Disables==1&&EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head)&&Enables==1);ReleaseList(Head);End();
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);
    if(argc==2)
    {
        CHECK(!strcmp(argv[1],"1"));Begin();Select(NameBinary);SP_DEVINFO_DATA_LIST *Head=NULL;
        CHECK(DisableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,&Head)&&Head&&EnableAllOurAdapters((HDEVINFO)(ULONG_PTR)1,Head));
        ReleaseList(Head);BOOL Unsafe=UnsafeNames!=0;End();CHECK(HeapDestroy(ModuleHeap));
        if(Unsafe){puts("DRIVER_BINARY_NAME_FORMATTED_AS_STRING");return 99;}
    }
    else
    {
        DWORD Before,After;CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for(DWORD repeat=0;repeat<3;++repeat){DirectMatrix();ToggleMatrix();}
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After&&HeapDestroy(ModuleHeap));
    }
    printf("PASS: actual adapter log-name/toggle/error/ownership (%lu scenarios)\n",Scenarios);return 0;
}
