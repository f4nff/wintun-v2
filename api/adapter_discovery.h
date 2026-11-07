/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */
#pragma once

_Use_decl_annotations_
LPWSTR
AdapterGetDeviceObjectFileName(LPCWSTR InstanceId)
{
    ULONGLONG Started = GetTickCount64();
    DWORD Delay = 1;
    for (;;)
    {
        /* Also bound callers outside an installation operation. API calls
         * themselves cannot be interrupted, but a changing list must never
         * cause unlimited retries or keep a buffer live while waiting. */
        if (!MainOperationSleep(0)) return NULL;
        if (GetTickCount64() - Started >= 15000) { SetLastError(ERROR_TIMEOUT); return NULL; }
        ULONG InterfacesLen;
        DWORD LastError = CM_MapCrToWin32Err(
            CM_Get_Device_Interface_List_SizeW(
                &InterfacesLen,
                (GUID *)&GUID_DEVINTERFACE_NET,
                (DEVINSTID_W)InstanceId,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT),
            ERROR_GEN_FAILURE);
        if (LastError != ERROR_SUCCESS)
        {
            SetLastError(LOG_ERROR(LastError, L"Failed to query adapter %s associated instances size", InstanceId));
            return NULL;
        }
        LPWSTR Interfaces = AllocArray(InterfacesLen, sizeof(*Interfaces));
        if (!Interfaces)
            return NULL;
        CONFIGRET Result = CM_Get_Device_Interface_ListW(
            (GUID *)&GUID_DEVINTERFACE_NET,
            (DEVINSTID_W)InstanceId,
            Interfaces,
            InterfacesLen,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (Result == CR_BUFFER_SMALL)
        {
            /* Interface arrival can invalidate the preceding size snapshot.
             * Release it before a cancellable, capped backoff and requery. */
            Free(Interfaces);
            ULONGLONG Elapsed = GetTickCount64() - Started;
            if (Elapsed >= 15000) { SetLastError(ERROR_TIMEOUT); return NULL; }
            DWORD Remaining = (DWORD)(15000 - Elapsed);
            if (!MainOperationSleep(Delay < Remaining ? Delay : Remaining)) return NULL;
            if (Delay < 50) { Delay *= 2; if (Delay > 50) Delay = 50; }
            continue;
        }
        LastError = CM_MapCrToWin32Err(Result, ERROR_GEN_FAILURE);
        if (LastError != ERROR_SUCCESS)
        {
            LOG_ERROR(LastError, L"Failed to get adapter %s associated instances", InstanceId);
            Free(Interfaces);
            SetLastError(LastError);
            return NULL;
        }
        if (!Interfaces[0])
        {
            Free(Interfaces);
            SetLastError(ERROR_DEVICE_NOT_AVAILABLE);
            return NULL;
        }
        return Interfaces;
    }
}
