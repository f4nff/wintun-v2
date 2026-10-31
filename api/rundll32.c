/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "rundll32.h"
#include "adapter.h"
#include "main.h"
#include "logger.h"
#include "resource.h"
#include <Windows.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <assert.h>

#ifdef MAYBE_WOW64

_Return_type_success_(return != FALSE)
static BOOL
AppendToBuffer(_Inout_ LPWSTR *Buffer, _In_ CONST WCHAR Addition, _Inout_ SIZE_T *BufferPos, _Inout_ SIZE_T *BufferLen)
{
    SIZE_T NewPos;
    if (FAILED(SIZETAdd(*BufferPos, sizeof(Addition), &NewPos)))
        return FALSE;
    if (NewPos >= *BufferLen)
    {
        SIZE_T NewLen;
        if (FAILED(SIZETMult(NewPos, 3, &NewLen)))
            return FALSE;
        LPWSTR NewBuffer = ReZalloc(*Buffer, NewLen);
        if (!NewBuffer)
            return FALSE;
        *Buffer = NewBuffer;
        *BufferLen = NewLen;
    }
    SIZE_T NewIndex = *BufferPos / sizeof(**Buffer);
    if (*Buffer + NewIndex < *Buffer)
        return FALSE;
    (*Buffer)[NewIndex] = Addition;
    *BufferPos = NewPos;
    return TRUE;
}

_Must_inspect_result_
static _Return_type_success_(return != NULL)
_Post_maybenull_
LPWSTR
ArgvToCommandLineW(_In_ SIZE_T ArgCount, ...)
{
    LPWSTR Output = NULL;
    SIZE_T BufferPos = 0, BufferLen = 0;
#    define Append(Char) \
        do \
        { \
            if (!AppendToBuffer(&Output, Char, &BufferPos, &BufferLen)) \
                goto cleanupBuffer; \
        } while (0)

    va_list Args;
    va_start(Args, ArgCount);
    for (SIZE_T i = 0; i < ArgCount; ++i)
    {
        LPCWSTR Arg = va_arg(Args, LPCWSTR);
        SIZE_T ArgLen = wcslen(Arg);
        if (ArgLen >= DWORD_MAX >> 3)
            goto cleanupBuffer;
        if (i)
            Append(L' ');
        Append(L'"');
        for (SIZE_T j = 0;; ++j)
        {
            SIZE_T NumberBackslashes = 0;

            while (j < ArgLen && Arg[j] == L'\\')
            {
                ++j;
                ++NumberBackslashes;
            }
            if (j >= ArgLen)
            {
                for (SIZE_T k = 0; k < NumberBackslashes * 2; ++k)
                    Append(L'\\');
                break;
            }
            else if (Arg[j] == L'"')
            {
                for (SIZE_T k = 0; k < NumberBackslashes * 2 + 1; ++k)
                    Append(L'\\');
                Append(Arg[j]);
            }
            else
            {
                for (SIZE_T k = 0; k < NumberBackslashes; ++k)
                    Append(L'\\');
                Append(Arg[j]);
            }
        }
        Append(L'"');
    }
    va_end(Args);
    return Output;

cleanupBuffer:
    va_end(Args);
    Free(Output);
    return NULL;
#    undef Append
}

#include "rundll32_process.h"

static _Return_type_success_(return != FALSE)
BOOL
InvokeClassInstaller(_In_ LPCWSTR Action, _In_ LPCWSTR Function, _In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA *DevInfoData)
{
    LOG(WINTUN_LOG_INFO, L"Spawning native process to %s instance", Action);

    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    DWORD RequiredChars = _countof(InstanceId);
    if (!SetupDiGetDeviceInstanceIdW(DevInfo, DevInfoData, InstanceId, RequiredChars, &RequiredChars))
    {
        LOG_LAST_ERROR(L"Failed to get adapter instance ID");
        return FALSE;
    }
    LPWSTR Arguments = ArgvToCommandLineW(1, InstanceId);
    if (!Arguments)
    {
        SetLastError(LOG_ERROR(ERROR_INVALID_PARAMETER, L"Command line too long"));
        return FALSE;
    }
    DWORD LastError;
    WCHAR Response[8 + 1];
    if (!ExecuteRunDll32(Function, Arguments, Response, _countof(Response)))
    {
        LastError = LOG_LAST_ERROR(L"Error executing worker process: %s", Arguments);
        goto cleanupArguments;
    }
    int Argc;
    LPWSTR *Argv = CommandLineToArgvW(Response, &Argc);
    if (!Argv || Argc < 1)
    {
        LastError = LOG_ERROR(ERROR_INVALID_PARAMETER, L"Incomplete response: %s", Response);
        goto cleanupArgv;
    }
    LastError = wcstoul(Argv[0], NULL, 16);
cleanupArgv:
    LocalFree(Argv);
cleanupArguments:
    Free(Arguments);
    return RET_ERROR(TRUE, LastError);
}

_Use_decl_annotations_
BOOL
RemoveInstanceViaRundll32(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData)
{
    return InvokeClassInstaller(L"remove", L"RemoveInstance", DevInfo, DevInfoData);
}

_Use_decl_annotations_
BOOL
EnableInstanceViaRundll32(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData)
{
    return InvokeClassInstaller(L"enable", L"EnableInstance", DevInfo, DevInfoData);
}

_Use_decl_annotations_
BOOL
DisableInstanceViaRundll32(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData)
{
    return InvokeClassInstaller(L"disable", L"DisableInstance", DevInfo, DevInfoData);
}

_Use_decl_annotations_
BOOL
CreateInstanceWin7ViaRundll32(LPWSTR InstanceId)
{
    LOG(WINTUN_LOG_INFO, L"Spawning native process to create instance");

    DWORD LastError;
    WCHAR Response[MAX_DEVICE_ID_LEN + 1];
    if (!ExecuteRunDll32(L"CreateInstanceWin7", L"", Response, _countof(Response)))
    {
        LastError = LOG_LAST_ERROR(L"Error executing worker process");
        goto cleanup;
    }
    int Argc;
    LPWSTR *Argv = CommandLineToArgvW(Response, &Argc);
    if (!Argv || Argc < 2)
    {
        LastError = LOG_ERROR(ERROR_INVALID_PARAMETER, L"Incomplete response: %s", Response);
        goto cleanupArgv;
    }
    LastError = wcstoul(Argv[0], NULL, 16);
    if (LastError == ERROR_SUCCESS)
        wcsncpy_s(InstanceId, MAX_DEVICE_ID_LEN, Argv[1], _TRUNCATE);
cleanupArgv:
    LocalFree(Argv);
cleanup:
    return RET_ERROR(TRUE, LastError);
}
#else
VOID Rundll32RequestShutdown(VOID) {}
BOOL Rundll32Shutdown(DWORD Timeout) { (void)Timeout; return TRUE; }
VOID Rundll32Detach(VOID) {}
#endif
