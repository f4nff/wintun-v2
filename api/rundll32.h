/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#pragma once

#include <Windows.h>
#include <SetupAPI.h>
#include "adapter.h"

/* Shutdown signals every retained helper before joining cleanup callbacks.
 * A timeout never destroys a process or its resources while it is still active. */
VOID Rundll32RequestShutdown(VOID);
BOOL Rundll32Shutdown(DWORD Timeout);
VOID Rundll32Detach(VOID);

_Return_type_success_(return != FALSE)
BOOL
RemoveInstanceViaRundll32(_In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA *DevInfoData);

_Return_type_success_(return != FALSE)
BOOL
EnableInstanceViaRundll32(_In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA *DevInfoData);

_Return_type_success_(return != FALSE)
BOOL
DisableInstanceViaRundll32(_In_ HDEVINFO DevInfo, _In_ SP_DEVINFO_DATA *DevInfoData);

_Return_type_success_(return != FALSE)
BOOL
CreateInstanceWin7ViaRundll32(_Out_writes_z_(MAX_DEVICE_ID_LEN) LPWSTR InstanceId);
