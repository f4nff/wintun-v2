/* SPDX-License-Identifier: GPL-2.0 */
/* Actual rename/alias/collision functions. NCI, IP-helper, SetupAPI and registry
 * boundaries are hosted. GUID parsing, CRT formatting, native heap strings and
 * event-backed set/key handles remain real. No network/device/registry changes. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include <devguid.h>
#include "../../api/adapter.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static LPVOID WINAPI TestAlloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI TestFree(HANDLE,DWORD,LPVOID);
#define HeapAlloc TestAlloc
#define HeapFree TestFree
#include "../../api/logger.h"
static HDEVINFO WINAPI TestSet(const GUID *,PCWSTR,HWND,DWORD,HDEVINFO,PCWSTR,PVOID);
static BOOL WINAPI TestEnum(HDEVINFO,DWORD,PSP_DEVINFO_DATA);
static HKEY WINAPI TestKey(HDEVINFO,PSP_DEVINFO_DATA,DWORD,DWORD,DWORD,REGSAM);
static LSTATUS WINAPI TestRegClose(HKEY);
static BOOL WINAPI TestDestroy(HDEVINFO);
static BOOL WINAPI TestProperty(HDEVINFO,PSP_DEVINFO_DATA,const DEVPROPKEY *,DEVPROPTYPE,const BYTE *,DWORD,DWORD);
static DWORD WINAPI TestNci(const GUID *,LPCWSTR);
static NETIO_STATUS NETIOAPI_API_ TestAlias(PCWSTR,PNET_LUID);
static NETIO_STATUS NETIOAPI_API_ TestGuid(const NET_LUID *,GUID *);
#define SetupDiGetClassDevsExW TestSet
#define SetupDiEnumDeviceInfo TestEnum
#define SetupDiOpenDevRegKey TestKey
#define RegCloseKey TestRegClose
#define SetupDiDestroyDeviceInfoList TestDestroy
#define SetupDiSetDevicePropertyW TestProperty
#define NciSetConnectionName TestNci
#define ConvertInterfaceAliasToLuid TestAlias
#define ConvertInterfaceLuidToGuid TestGuid
#ifndef PB_ADAPTER_RENAME_SOURCE
#define PB_ADAPTER_RENAME_SOURCE "../../api/adapter_rename.h"
#endif
#include PB_ADAPTER_RENAME_SOURCE
#undef HeapAlloc
#undef HeapFree

HANDLE ModuleHeap;
BOOL IsWindows7=TRUE;
const DEVPROPKEY DEVPKEY_Wintun_Name={{0x3361c968,0x2f2e,0x4660,{0xb4,0x7e,0x69,0x9c,0xdc,0x4c,0x32,0xb9}},DEVPROPID_FIRST_USABLE+1};
static const GUID Own={1,2,3,{4}},Foreign={5,6,7,{8}},Unrelated={9,10,11,{12}};
static const NET_LUID ForeignLuid={.Info={.Reserved=0,.NetLuidIndex=17,.IfType=53}};
static HANDLE Set,Key;
static LPVOID Value;
static DWORD Handles,SetAttempts,SetsOpened,SetsClosed,KeysOpened,KeysClosed,Allocs,Frees,AllocCalls,FailAllocAt;
static DWORD OwnCalls,ForeignCalls,NativeChanges,AliasCalls,GuidCalls,SetterAttempts,MetadataWrites,Enums,Rows,CurrentIndex;
static DWORD InitialResult,RetryResult,SuffixResult,ForeignResult,ForeignDups,AliasError,GuidError,Phase,MetaError;
static DWORD Sleeps,CancelAt,Logs,Scenarios;
static BOOL ForeignMoved,OwnRetried,AliasFailFirst;
static LPCWSTR InputName;
static WCHAR MetadataName[PB_MAX_ADAPTER_NAME],OwnName[PB_MAX_ADAPTER_NAME],FirstProposal[PB_MAX_ADAPTER_NAME];
enum {MetaOk,MetaSetFailure,MetaEnumFailure,MetaKeyFailure,MetaReadFailure,MetaOtherGuid,MetaBadGuid,MetaWriteFailure};
static VOID Begin(VOID)
{
    CHECK(!Set&&!Key&&!Value&&!Handles&&Allocs==Frees);
    SetAttempts=SetsOpened=SetsClosed=KeysOpened=KeysClosed=Allocs=Frees=AllocCalls=FailAllocAt=0;
    OwnCalls=ForeignCalls=NativeChanges=AliasCalls=GuidCalls=SetterAttempts=MetadataWrites=Enums=CurrentIndex=0;
    InitialResult=ERROR_DUP_NAME;RetryResult=SuffixResult=ForeignResult=ERROR_SUCCESS;ForeignDups=0;
    AliasError=GuidError=0;Phase=MetaOk;MetaError=ERROR_ACCESS_DENIED;Rows=1;Sleeps=CancelAt=Logs=0;
    ForeignMoved=OwnRetried=AliasFailFirst=FALSE;InputName=L"collision";
    MetadataName[0]=OwnName[0]=FirstProposal[0]=0;
}
static VOID End(VOID)
{
    CHECK(!Set&&!Key&&!Value&&!Handles&&SetsOpened==SetsClosed&&KeysOpened==KeysClosed&&Allocs==Frees);++Scenarios;
}
static LPVOID WINAPI TestAlloc(HANDLE Heap,DWORD Flags,SIZE_T Bytes)
{
    CHECK(Heap==ModuleHeap&&!Value);++AllocCalls;
    if(AllocCalls==FailAllocAt){SetLastError(ERROR_INVALID_HANDLE);return NULL;}
    Value=HeapAlloc(Heap,Flags,Bytes);CHECK(Value);++Allocs;return Value;
}
static BOOL WINAPI TestFree(HANDLE Heap,DWORD Flags,LPVOID Memory)
{CHECK(Heap==ModuleHeap&&Memory==Value&&Value&&HeapFree(Heap,Flags,Memory));Value=NULL;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
BOOL MainOperationSleep(DWORD Delay)
{CHECK(!Delay);if(++Sleeps!=CancelAt)return TRUE;SetLastError(ERROR_CANCELLED);return FALSE;}
static HDEVINFO WINAPI TestSet(const GUID *Class,PCWSTR Enumerator,HWND Window,DWORD Flags,HDEVINFO Existing,PCWSTR Machine,PVOID Reserved)
{
    CHECK(IsEqualGUID(Class,&GUID_DEVCLASS_NET)&&!wcscmp(Enumerator,WINTUN_ENUMERATOR)&&!Window&&!Flags&&!Existing&&!Machine&&!Reserved&&!Set);
    ++SetAttempts;
    if(Phase==MetaSetFailure){SetLastError(MetaError);return INVALID_HANDLE_VALUE;}
    Set=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Set);++Handles;++SetsOpened;return(HDEVINFO)Set;
}
static BOOL WINAPI TestEnum(HDEVINFO List,DWORD Index,PSP_DEVINFO_DATA Info)
{
    CHECK(List==(HDEVINFO)Set&&Set&&Info->cbSize==sizeof(*Info));++Enums;
    if(Phase==MetaEnumFailure&&!Index){SetLastError(MetaError);return FALSE;}
    if(Index>=Rows){SetLastError(ERROR_NO_MORE_ITEMS);return FALSE;}Info->DevInst=Index+1;return TRUE;
}
static HKEY WINAPI TestKey(HDEVINFO List,PSP_DEVINFO_DATA Info,DWORD Scope,DWORD Profile,DWORD Kind,REGSAM Access)
{
    CHECK(List==(HDEVINFO)Set&&Set&&!Key&&Scope==DICS_FLAG_GLOBAL&&!Profile&&Kind==DIREG_DRV&&Access==KEY_QUERY_VALUE);
    if(Phase==MetaKeyFailure){SetLastError(MetaError);return INVALID_HANDLE_VALUE;}
    Key=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Key);CurrentIndex=Info->DevInst-1;++Handles;++KeysOpened;return(HKEY)Key;
}
static LSTATUS WINAPI TestRegClose(HKEY Open)
{CHECK(Open==(HKEY)Key&&Key&&CloseHandle(Key));Key=NULL;--Handles;++KeysClosed;SetLastError(ERROR_INVALID_HANDLE);return ERROR_SUCCESS;}
static BOOL WINAPI TestDestroy(HDEVINFO List)
{CHECK(List==(HDEVINFO)Set&&Set&&!Key&&CloseHandle(Set));Set=NULL;--Handles;++SetsClosed;SetLastError(ERROR_INVALID_HANDLE);return TRUE;}
LPWSTR RegistryQueryString(HKEY Open,LPCWSTR Name,BOOL Log)
{
    CHECK(Open==(HKEY)Key&&Key&&!wcscmp(Name,L"NetCfgInstanceId")&&Log);
    if(Phase==MetaReadFailure){SetLastError(MetaError);return NULL;}
    LPWSTR Text=Alloc(40*sizeof(WCHAR));if(!Text)return NULL;
    if(Phase==MetaBadGuid)wcscpy_s(Text,40,L"{not-a-guid}");
    else CHECK(StringFromGUID2(Phase==MetaOtherGuid||(Rows>1&&!CurrentIndex)?&Unrelated:&Foreign,Text,40));
    return Text;
}
static BOOL WINAPI TestProperty(HDEVINFO List,PSP_DEVINFO_DATA Info,const DEVPROPKEY *Property,DEVPROPTYPE Type,const BYTE *Data,DWORD Bytes,DWORD Flags)
{
    CHECK(List==(HDEVINFO)Set&&Set&&Info->DevInst==CurrentIndex+1&&Property==&DEVPKEY_Wintun_Name&&Type==DEVPROP_TYPE_STRING&&!Flags&&
        Bytes==(wcslen((LPCWSTR)Data)+1)*sizeof(WCHAR));++SetterAttempts;
    if(Phase==MetaWriteFailure){SetLastError(MetaError);return FALSE;}
    wcscpy_s(MetadataName,PB_MAX_ADAPTER_NAME,(LPCWSTR)Data);++MetadataWrites;return TRUE;
}
static DWORD WINAPI TestNci(const GUID *Guid,LPCWSTR Name)
{
    CHECK(Name&&wcslen(Name)<PB_MAX_ADAPTER_NAME);DWORD Result;
    if(!memcmp(Guid,&Own,sizeof(Own)))
    {
        ++OwnCalls;
        if(OwnCalls==1){CHECK(!wcscmp(Name,InputName));Result=InitialResult;}
        else if(ForeignMoved&&!OwnRetried){OwnRetried=TRUE;Result=RetryResult;}
        else Result=SuffixResult;
        if(!Result)wcscpy_s(OwnName,PB_MAX_ADAPTER_NAME,Name);
    }
    else
    {
        CHECK(!memcmp(Guid,&Foreign,sizeof(Foreign)));++ForeignCalls;
        if(ForeignCalls==1)wcscpy_s(FirstProposal,PB_MAX_ADAPTER_NAME,Name);
        Result=ForeignCalls<=ForeignDups?ERROR_DUP_NAME:ForeignResult;
        if(!Result){ForeignMoved=TRUE;++NativeChanges;}
    }
    SetLastError(ERROR_INVALID_DATA);return Result;
}
static NETIO_STATUS NETIOAPI_API_ TestAlias(PCWSTR Name,PNET_LUID Luid)
{
    CHECK(Name&&wcslen(Name)<PB_MAX_ADAPTER_NAME);++AliasCalls;
    DWORD Error=AliasFailFirst&&AliasCalls==1?ERROR_NOT_FOUND:AliasError;
    if(!Error)*Luid=ForeignLuid;return Error;
}
static NETIO_STATUS NETIOAPI_API_ TestGuid(const NET_LUID *Luid,GUID *Guid)
{CHECK(Luid->Value==ForeignLuid.Value);++GuidCalls;if(!GuidError)*Guid=Foreign;return GuidError;}
static VOID FormatLog(LPCWSTR Format,va_list Args)
{WCHAR Text[1024];++Logs;_vsnwprintf_s(Text,_countof(Text),_TRUNCATE,Format,Args);SetLastError(ERROR_INVALID_HANDLE);}
DWORD LoggerLogV(WINTUN_LOGGER_LEVEL Level,LPCWSTR Format,va_list Args)
{(void)Level;DWORD Error=GetLastError();FormatLog(Format,Args);SetLastError(Error);return Error;}
DWORD LoggerErrorV(DWORD Error,LPCWSTR Format,va_list Args)
{FormatLog(Format,Args);return Error;}
static BOOL Run(VOID){GUID Target=Own;return NciSetAdapterName(&Target,InputName);}
static VOID MakeName(WCHAR Name[PB_MAX_ADAPTER_NAME+1],DWORD Length)
{CHECK(Length<=PB_MAX_ADAPTER_NAME);for(DWORD i=0;i<Length;++i)Name[i]=L'x';Name[Length]=0;}
static VOID NativeMatrix(VOID)
{
    const DWORD Errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INVALID_DATA,ERROR_GEN_FAILURE};
    for(DWORD error=0;error<_countof(Errors);++error)
    {
        Begin();InitialResult=Errors[error];CHECK(!Run()&&GetLastError()==InitialResult&&OwnCalls==1&&!AliasCalls&&!ForeignCalls&&!SetterAttempts);End();
        Begin();ForeignResult=Errors[error];CHECK(Run()&&OwnCalls==2&&ForeignCalls==1&&!NativeChanges&&!SetAttempts&&!SetterAttempts&&!wcscmp(OwnName,L"collision 1"));End();
        Begin();RetryResult=Errors[error];CHECK(!Run()&&GetLastError()==RetryResult&&NativeChanges==1&&MetadataWrites==1&&OwnRetried);End();
        Begin();AliasError=Errors[error];CHECK(Run()&&!ForeignCalls&&!SetsOpened&&OwnCalls==2&&!wcscmp(OwnName,L"collision 1"));End();
        Begin();GuidError=Errors[error];CHECK(Run()&&!ForeignCalls&&!SetsOpened&&OwnCalls==2);End();
    }
    Begin();InitialResult=ERROR_SUCCESS;CHECK(Run()&&OwnCalls==1&&!ForeignCalls&&!SetsOpened&&!wcscmp(OwnName,InputName));End();
    Begin();CHECK(Run()&&NativeChanges==1&&MetadataWrites==1&&OwnRetried&&!wcscmp(OwnName,InputName)&&!wcscmp(MetadataName,L"collision 1"));End();
    Begin();InputName=L"\x7f51\x5361";CHECK(Run()&&MetadataWrites==1&&!wcscmp(MetadataName,L"\x7f51\x5361 1")&&!wcscmp(OwnName,InputName));End();
    for(DWORD duplicates=1;duplicates<=9;++duplicates)
    {Begin();ForeignDups=duplicates;CHECK(Run()&&ForeignCalls==duplicates+1&&MetadataWrites==1&&OwnRetried);End();}
    Begin();AliasFailFirst=TRUE;SuffixResult=ERROR_DUP_NAME;
    CHECK(Run()&&OwnCalls==3&&AliasCalls==2&&ForeignCalls==1&&!wcscmp(FirstProposal,L"collision 2")&&!wcscmp(OwnName,L"collision 1"));End();
}
static VOID MetadataMatrix(VOID)
{
    const DWORD Errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_INVALID_DATA};
    for(DWORD phase=MetaSetFailure;phase<=MetaWriteFailure;++phase)for(DWORD error=0;error<_countof(Errors);++error)
    {
        Begin();Phase=phase;MetaError=Errors[error];
        CHECK(Run()&&NativeChanges==1&&OwnRetried&&!wcscmp(OwnName,InputName)&&!MetadataWrites&&Logs>=1);End();
        Begin();Phase=phase;MetaError=Errors[error];ForeignResult=Errors[error];
        CHECK(Run()&&!NativeChanges&&!SetAttempts&&!SetterAttempts&&!Logs);End();
        Begin();Phase=phase;MetaError=ERROR_WRITE_FAULT;RetryResult=Errors[error];
        CHECK(!Run()&&GetLastError()==RetryResult&&NativeChanges==1&&OwnRetried&&!MetadataWrites);End();
    }
    Begin();FailAllocAt=1;CHECK(Run()&&NativeChanges==1&&!Allocs&&!MetadataWrites&&OwnRetried&&Logs>=1);End();
    Begin();Rows=2;CHECK(Run()&&MetadataWrites==1&&KeysOpened==2&&KeysClosed==2&&Allocs==2&&Frees==2);End();
    Begin();Rows=2;FailAllocAt=2;CHECK(Run()&&!MetadataWrites&&Allocs==1&&Frees==1&&KeysOpened==2&&OwnRetried);End();
    Begin();Rows=0;CHECK(Run()&&NativeChanges==1&&!MetadataWrites&&!Allocs&&!KeysOpened&&OwnRetried);End();
    /* Direct metadata updates keep their original best-effort search policy. */
    for(DWORD phase=MetaOk;phase<=MetaWriteFailure;++phase)
    {
        Begin();Phase=phase;GUID Guid=Foreign;BOOL Result=RenameByNetGUID(&Guid,L"new-name");DWORD Error=GetLastError();
        CHECK(Result==(phase==MetaOk));
        if(phase==MetaOk)CHECK(MetadataWrites==1&&!wcscmp(MetadataName,L"new-name"));
        else CHECK(Error==(phase==MetaSetFailure||phase==MetaWriteFailure?MetaError:ERROR_NOT_FOUND));End();
    }
}
static VOID BudgetAndNames(VOID)
{
    WCHAR Name[PB_MAX_ADAPTER_NAME+1];
    const DWORD Lengths[]={126,127,128,129};
    for(DWORD i=0;i<_countof(Lengths);++i)
    {
        MakeName(Name,Lengths[i]);Begin();InputName=Name;InitialResult=ERROR_SUCCESS;
        CHECK(Run()==(Lengths[i]<PB_MAX_ADAPTER_NAME));if(Lengths[i]>=PB_MAX_ADAPTER_NAME)CHECK(GetLastError()==ERROR_BUFFER_OVERFLOW&&!OwnCalls);End();
        Begin();InputName=Name;BOOL Result=Run();CHECK(Result==(Lengths[i]==126));
        if(!Result)CHECK(GetLastError()==ERROR_BUFFER_OVERFLOW&&!ForeignCalls&&!SetterAttempts);End();
    }
    Begin();MakeName(Name,125);InputName=Name;ForeignDups=99;
    CHECK(!Run()&&GetLastError()==ERROR_BUFFER_OVERFLOW&&ForeignCalls==99&&!SetsOpened);End();
    Begin();MakeName(Name,124);InputName=Name;ForeignDups=999;
    CHECK(!Run()&&GetLastError()==ERROR_BUFFER_OVERFLOW&&ForeignCalls==999&&!SetsOpened);End();
    Begin();ForeignDups=MAXDWORD;SuffixResult=ERROR_ACCESS_DENIED;
    CHECK(!Run()&&GetLastError()==SuffixResult&&ForeignCalls==1000&&OwnCalls==2&&!SetsOpened);End();
    Begin();InitialResult=SuffixResult=ERROR_DUP_NAME;AliasError=ERROR_NOT_FOUND;
    CHECK(!Run()&&GetLastError()==ERROR_DUP_NAME&&OwnCalls==1001&&AliasCalls==1001&&!ForeignCalls&&!SetsOpened);End();
    const DWORD Stops[]={1,2,1001};
    for(DWORD i=0;i<_countof(Stops);++i)
    {
        Begin();InitialResult=SuffixResult=ERROR_DUP_NAME;AliasError=ERROR_NOT_FOUND;CancelAt=Stops[i];
        CHECK(!Run()&&GetLastError()==ERROR_CANCELLED&&OwnCalls==CancelAt-1&&!SetsOpened);End();
    }
    /* Cancellation remains at the existing outer checkpoints. A proposal
     * loop finishes its bounded 1000 attempts before reaching checkpoint 2. */
    Begin();ForeignDups=MAXDWORD;CancelAt=2;
    CHECK(!Run()&&GetLastError()==ERROR_CANCELLED&&ForeignCalls==1000&&OwnCalls==1&&!SetsOpened);End();
    Begin();CancelAt=1;GUID Target=Foreign;
    CHECK(!RenameByNetGUID(&Target,L"new-name")&&GetLastError()==ERROR_CANCELLED&&SetsOpened==1&&!KeysOpened);End();
    /* Metadata remains best effort, including its existing cancellation exit.
     * This change does not turn that exit into a new native retry barrier. */
    Begin();CancelAt=2;
    CHECK(Run()&&NativeChanges==1&&OwnRetried&&!MetadataWrites&&SetsOpened==1&&!KeysOpened);End();
}
int main(int argc,char **argv)
{
    CHECK(argc<=2);ModuleHeap=HeapCreate(0,0,0);CHECK(ModuleHeap);
    if(argc==2)
    {
        CHECK(!strcmp(argv[1],"1"));Begin();ForeignResult=ERROR_ACCESS_DENIED;
        CHECK(Run()&&!NativeChanges&&OwnCalls==2&&ForeignCalls==1);BOOL Changed=MetadataWrites!=0;End();CHECK(HeapDestroy(ModuleHeap));
        if(Changed){puts("FOREIGN_METADATA_CHANGED_AFTER_RENAME_FAILURE");return 103;}
    }
    else
    {
        DWORD Before,After;CHECK(GetProcessHandleCount(GetCurrentProcess(),&Before));
        for(DWORD repeat=0;repeat<3;++repeat){NativeMatrix();MetadataMatrix();BudgetAndNames();}
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&After)&&Before==After&&HeapDestroy(ModuleHeap));
    }
    printf("PASS: actual adapter rename/native-metadata/suffix/ownership (%lu scenarios)\n",Scenarios);return 0;
}
