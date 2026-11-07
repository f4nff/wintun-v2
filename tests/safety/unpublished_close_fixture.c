/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the native-policy resource ledger. The unregistered close boundary
 * adds real protected-event failure without inventing native registrations. */
#ifdef PB_API_TESTS
#error Unpublished close fixture requires shipping session paths
#endif
#include "guard_memory.h"
#include "../model/unpublished_close_fixture.h"
static UC_CONFIG *Config;
static HANDLE WINAPI UnpublishedFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL WINAPI UnpublishedClose(HANDLE);
static BOOL WINAPI UnpublishedIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
static PTP_WORK WINAPI UnpublishedWork(PTP_WORK_CALLBACK,VOID *,PTP_CALLBACK_ENVIRON);
#ifndef PB_UNPUBLISHED_SESSION_SOURCE
#error Build through Build-UnpublishedCloseTests for explicit source binding
#endif
#include "native_policy_fixture.c"
static PTP_WORK WINAPI UnpublishedWork(PTP_WORK_CALLBACK callback,VOID *opaque,PTP_CALLBACK_ENVIRON environment)
{
    /* Observe construction without acquiring an owner. Failed Open never
     * returns a session for the normal fixture Configure export. */
    CHECK(Config && !Current);Current=opaque;Progress=Current->ProgressEvent;Config->Progress=Progress;
    return TrackedWork(callback,opaque,environment);
}
static HANDLE WINAPI UnpublishedFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template)
{
    HANDLE handle=HostedFile(path,access,share,security,creation,flags,template);
    CHECK(SetHandleInformation(handle,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE));Config->Device=handle;return handle;
}
static BOOL WINAPI UnpublishedClose(HANDLE handle)
{
    if(handle!=Device)return TrackedClose(handle);
    CHECK(Config && Current && !NativeRegistered && !NativeActive && !LoansNow());InterlockedIncrement(&Config->CloseAttempts);
    BOOL result=CloseHandle(handle);DWORD error=GetLastError();
    if(result){Device=NULL;InterlockedIncrement(&DeviceCloses);CHECK(InterlockedDecrement(&Events)>=0);InterlockedIncrement(&Config->CloseSuccesses);}
    else{CHECK(error==ERROR_INVALID_HANDLE);InterlockedIncrement(&Config->CloseFailures);}
    SetLastError(error);return result;
}
static BOOL WINAPI UnpublishedIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlap)
{
    CHECK(Config && device==Device && !NativeRegistered);*returned=0;
    if(code==PB_PROBE_IOCTL_QUERY) {
        if(Config->Failure==0){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        BOOL result=HostedIo(device,code,input,inputSize,output,outputSize,returned,overlap);
        if(Config->Failure==1)((PB_PROBE_STATUS *)output)->Magic^=1;return result;
    }
    /* Failed registration publishes nothing. An unavailable/unregistered
     * driver cannot accept DRAIN or QUERY; those codes fail this assertion. */
    CHECK(code==PB_V2_IOCTL_REGISTER && Config->Failure==2 && input && inputSize==sizeof(PB_REGISTER_V2) && !output && !outputSize && !overlap);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;
}
__declspec(dllexport) VOID WINAPI PrepareUnpublished(UC_CONFIG *config)
{CHECK(config && !Config);Config=config;Initialize(&config->Native);NativeActive=0;}
__declspec(dllexport) VOID WINAPI ReadUnpublished(UC_SNAPSHOT *out)
{
    CHECK(out && Current);PB_SESSION *session=Current;
    /* The protected handle prevents physical close; construction/registry or
     * the admitted worker owns the shell. No observer Retain is added. */
    AcquireSRWLockShared(&session->State);
    *out=(UC_SNAPSHOT){.References=ReadAcquire(&session->References),.PublicReleased=session->PublicReleased,.Registered=session->Registered,
        .Scheduled=session->Scheduled,.Callbacks=CallbackCount,.Active=ActiveCallbacks,.Pins=Pins,.Regions=Regions,.Works=Works,.Pools=Pools,
        .NativeRegistered=NativeRegistered,.NativeActive=NativeActive,.Drains=Drains,.Queries=Queries,
        .Phase=session->Progress.Phase,.Block=session->Progress.BlockStage,.Error=session->Progress.LastError};
    ReleaseSRWLockShared(&session->State);
    AcquireSRWLockShared(&RuntimeLock);out->Sessions=RuntimeCount;out->Bytes=RuntimeBytes;ReleaseSRWLockShared(&RuntimeLock);
}
__declspec(dllexport) VOID WINAPI RepairUnpublishedHandoff(VOID)
{
    CHECK(Current && !Current->PublicReleased && !Current->Scheduled && Current->Registered && Current->References==2 && !CallbackCount);
    CHECK(InterlockedIncrement(&Config->Repairs)==1);
    /* Negative teardown only, while construction+registry keep it alive and
     * the ordinary loader reference is still owned. This is the ACTUAL
     * transfer that the generated Destroy mutation omitted. No manual Retain. */
    ReleaseSession(Current);
}
