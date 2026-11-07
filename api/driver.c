/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include <winsock2.h>
#include <Windows.h>
#include <winternl.h>
#include <cfgmgr32.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <ndisguid.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <wchar.h>

#include "driver.h"
#include "adapter.h"
#include "logger.h"
#include "namespace.h"
#include "resource.h"
#include "registry.h"
#include "ntdll.h"
#include "rundll32.h"
#include "wintun-inf.h"

#pragma warning(disable : 4221) /* nonstandard: address of automatic in initializer */

struct _SP_DEVINFO_DATA_LIST
{
    SP_DEVINFO_DATA Data;
    struct _SP_DEVINFO_DATA_LIST *Next;
};

#include "driver_toggle.h"

#include "driver_version.h"

#include "driver_install.h"

static ULONG_PTR RunDeleteDriver(const VOID *input) { (void)input; return DeleteDriverInternal(); }
_Use_decl_annotations_
BOOL WINAPI WintunDeleteDriver(VOID) { return (BOOL)MainRunOperation(RunDeleteDriver, NULL, NULL, 0); }
