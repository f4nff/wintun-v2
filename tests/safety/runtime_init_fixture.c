/* SPDX-License-Identifier: GPL-2.0 */
/* Actual main.c/InitOnce/runtime/security objects. Successful SDK acquisitions
 * use the real token, SID, SDDL and private heap APIs, without registry/devices.
 * Namespace hooks are counted no-ops: no project namespace is created. */
#ifdef PB_API_TESTS
#error Runtime initialization cannot use PB_API_TESTS
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <sddl.h>
#include <delayimp.h>
#include <stdio.h>
#include "../model/runtime_init_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (GLE %lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
static RI_CONFIG Config;
static volatile LONG Calls,Heaps,Tokens,Locals,NamespaceReady,Failures,FirstThread,UnloadCalls,ClientLeases,InFlight,Repairs;
static HLOCAL LocalOwners[3];
static BOOL Fail(VOID)
{
    if(InterlockedIncrement(&Calls)!=(LONG)Config.Point)return FALSE;
    CHECK(InterlockedIncrement(&Failures)==1);
    if(Config.Events[RI_FAILURE_ENTERED])CHECK(SetEvent(Config.Events[RI_FAILURE_ENTERED])&&WaitForSingleObject(Config.Events[RI_FAILURE_GATE],10000)==WAIT_OBJECT_0);
    SetLastError(Config.Error);return TRUE;
}
static HANDLE WINAPI TrackedHeap(DWORD options,SIZE_T initial,SIZE_T maximum)
{if(Fail())return NULL;HANDLE heap=HeapCreate(options,initial,maximum);CHECK(heap&&InterlockedIncrement(&Heaps)==1);return heap;}
static BOOL WINAPI TrackedHeapDestroy(HANDLE heap)
{CHECK(Heaps==1&&HeapDestroy(heap));CHECK(InterlockedDecrement(&Heaps)==0);return TRUE;}
static BOOL WINAPI TrackedSid(WELL_KNOWN_SID_TYPE type,PSID domain,PSID sid,DWORD *size)
{CHECK(type==WinLocalSystemSid&&!domain&&sid&&size);return Fail()?FALSE:CreateWellKnownSid(type,domain,sid,size);}
static BOOL WINAPI TrackedToken(HANDLE process,DWORD access,HANDLE *token)
{CHECK(process==GetCurrentProcess()&&access==TOKEN_QUERY&&token);if(Fail())return FALSE;BOOL okay=OpenProcessToken(process,access,token);CHECK(okay&&InterlockedIncrement(&Tokens)==1);return okay;}
static BOOL WINAPI TrackedInformation(HANDLE token,TOKEN_INFORMATION_CLASS kind,VOID *buffer,DWORD bytes,DWORD *returned)
{CHECK(Tokens==1&&kind==TokenUser&&buffer&&bytes&&returned);return Fail()?FALSE:GetTokenInformation(token,kind,buffer,bytes,returned);}
static VOID Own(HLOCAL memory)
{CHECK(memory);for(DWORD i=0;i<ARRAYSIZE(LocalOwners);++i)if(!LocalOwners[i]){LocalOwners[i]=memory;InterlockedIncrement(&Locals);return;}CHECK(FALSE);}
static BOOL WINAPI TrackedDescriptor(LPCWSTR text,DWORD revision,PSECURITY_DESCRIPTOR *out,ULONG *bytes)
{CHECK(text&&revision==SDDL_REVISION_1&&out&&!bytes);if(Fail())return FALSE;BOOL okay=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,revision,out,bytes);CHECK(okay);Own(*out);return okay;}
static BOOL WINAPI TrackedSidString(PSID sid,LPWSTR *out)
{CHECK(IsValidSid(sid)&&out);if(Fail())return FALSE;BOOL okay=ConvertSidToStringSidW(sid,out);CHECK(okay);Own(*out);return okay;}
static HLOCAL WINAPI TrackedLocalFree(HLOCAL memory)
{
    if(!memory)return LocalFree(memory);
    for(DWORD i=0;i<ARRAYSIZE(LocalOwners);++i)if(LocalOwners[i]==memory){HLOCAL result=LocalFree(memory);CHECK(!result);LocalOwners[i]=NULL;CHECK(InterlockedDecrement(&Locals)>=0);return result;}
    CHECK(FALSE);return memory;
}
static BOOL WINAPI TrackedClose(HANDLE token)
{CHECK(Tokens==1&&CloseHandle(token));CHECK(InterlockedDecrement(&Tokens)==0);return TRUE;}
static BOOL WINAPI ObservedInitOnce(PINIT_ONCE once,PINIT_ONCE_FN callback,VOID *parameter,VOID **context)
{
    DWORD thread=GetCurrentThreadId();LONG first=InterlockedCompareExchange(&FirstThread,(LONG)thread,0);
    /* This marks entry to the opaque SDK call, not a kernel waiting point.
     * Its caller already acquired a real MainRuntime lease under the real lock. */
    if(first&&first!=(LONG)thread&&Config.Events[RI_SECOND_ENTERED])CHECK(SetEvent(Config.Events[RI_SECOND_ENTERED]));
    BOOL okay=InitOnceExecuteOnce(once,callback,parameter,context);DWORD error=GetLastError();
    if(!okay&&Config.Events[RI_FAILED_RETURNED])CHECK(SetEvent(Config.Events[RI_FAILED_RETURNED])&&WaitForSingleObject(Config.Events[RI_FAILED_GATE],10000)==WAIT_OBJECT_0);
    SetLastError(error);return okay;
}
static BOOL WINAPI ObservedUnload(LPCSTR);
#define HeapCreate TrackedHeap
#define HeapDestroy TrackedHeapDestroy
#define CreateWellKnownSid TrackedSid
#define OpenProcessToken TrackedToken
#define GetTokenInformation TrackedInformation
#define ConvertStringSecurityDescriptorToSecurityDescriptorW TrackedDescriptor
#define ConvertSidToStringSidW TrackedSidString
#define LocalFree TrackedLocalFree
#define CloseHandle TrackedClose
#define InitOnceExecuteOnce ObservedInitOnce
#define __FUnloadDelayLoadedDLL2 ObservedUnload
#define DllMain ActualMainDllMain
#ifndef PB_RUNTIME_INIT_SOURCE
#define PB_RUNTIME_INIT_SOURCE "../../api/main.c"
#endif
#include PB_RUNTIME_INIT_SOURCE
#undef DllMain
#undef HeapCreate
#undef HeapDestroy
#undef CreateWellKnownSid
#undef OpenProcessToken
#undef GetTokenInformation
#undef ConvertStringSecurityDescriptorToSecurityDescriptorW
#undef ConvertSidToStringSidW
#undef LocalFree
#undef CloseHandle
#undef InitOnceExecuteOnce
#undef __FUnloadDelayLoadedDLL2
static BOOL WINAPI ObservedUnload(LPCSTR name)
{InterlockedIncrement(&UnloadCalls);return __FUnloadDelayLoadedDLL2(name);}
static DWORD DependenciesLoaded(VOID)
{
    const BYTE *base=(const BYTE *)ResourceModule;const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt=(const IMAGE_NT_HEADERS *)(base+dos->e_lfanew);DWORD offset=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress,count=0;
    if(offset)for(const ImgDelayDescr *d=(const ImgDelayDescr *)(base+offset);d->rvaDLLName;++d)if(*(HMODULE *)(base+d->rvaHmod))++count;return count;
}
VOID NamespaceInit(VOID){CHECK(InterlockedIncrement(&NamespaceReady)==1);}
VOID NamespaceDone(VOID){CHECK(InterlockedDecrement(&NamespaceReady)==0);}
VOID AdapterCleanupDetach(VOID){}
VOID Rundll32Detach(VOID){}
VOID Rundll32RequestShutdown(VOID){}
__declspec(dllexport) VOID WINAPI ConfigureRuntimeInit(const RI_CONFIG *config)
{CHECK(config&&config->Point>=1&&config->Point<=7&&config->Error&&!Config.Point&&!DependencyUsers&&!RuntimeReady&&!Heaps&&!Locals&&!Tokens);Config=*config;}
__declspec(dllexport) BOOL WINAPI AcquireRuntimeInit(VOID)
{
    InterlockedIncrement(&InFlight);BOOL okay=MainRuntimeAcquire();DWORD error=GetLastError();if(okay)InterlockedIncrement(&ClientLeases);
    CHECK(InterlockedDecrement(&InFlight)>=0);SetLastError(error);return okay;
}
__declspec(dllexport) VOID WINAPI ReleaseRuntimeInit(VOID)
{CHECK(InterlockedDecrement(&ClientLeases)>=0);MainRuntimeRelease();}
__declspec(dllexport) VOID WINAPI RepairRuntimeInit(VOID)
{
    CHECK(!InFlight&&!ClientLeases&&DependencyUsers==1&&!DependencyUnloading&&InterlockedIncrement(&Repairs)==1);
    /* Observer control only. Repay one actual orphaned lease while the host's
     * ordinary LoadLibrary reference still owns the DLL; never edit Users. */
    MainRuntimeRelease();
}
__declspec(dllexport) VOID WINAPI ReadRuntimeInit(RI_STATE *state)
{
    CHECK(state);memset(state,0,sizeof(*state));state->Heaps=Heaps;state->Tokens=Tokens;state->Locals=Locals;state->NamespaceReady=NamespaceReady;state->Calls=Calls;state->Failures=Failures;
    state->UnloadCalls=UnloadCalls;state->ClientLeases=ClientLeases;state->InFlight=InFlight;state->Repairs=Repairs;
    AcquireSRWLockShared(&DependencyLock);state->Users=DependencyUsers;state->Unloading=DependencyUnloading;ReleaseSRWLockShared(&DependencyLock);
    state->RuntimeReady=RuntimeReady;state->FileDescriptor=SecurityAttributes.lpSecurityDescriptor!=NULL;state->PipeDescriptor=PipeSecurityAttributes.lpSecurityDescriptor!=NULL;
    state->ModuleHeapPresent=ModuleHeap!=NULL;state->LoadedDependencies=DependenciesLoaded();
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,VOID *reserved)
{
    BOOL okay=ActualMainDllMain(module,reason,reserved);
    if(reason==DLL_PROCESS_DETACH&&!reserved){CHECK(!Heaps&&!Tokens&&!Locals&&!NamespaceReady&&!DependencyUsers&&!DependencyUnloading&&!ClientLeases&&!InFlight);for(DWORD i=0;i<ARRAYSIZE(LocalOwners);++i)CHECK(!LocalOwners[i]);}
    return okay;
}
