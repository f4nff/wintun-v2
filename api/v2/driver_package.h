/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <windows.h>
#include "../../include/packetbridge_adapter_diagnostics.h"
/* Development DLLs require a separately staged driver. Final DLLs stage their signed resources. */
BOOL PbEnsureDriverPackage(PB_ADAPTER_DIAGNOSTICS *diagnostics);
typedef struct PB_DRIVER_BINDING { WCHAR Published[MAX_PATH], Store[MAX_PATH]; } PB_DRIVER_BINDING;
void PbRememberAdapterBinding(const WCHAR *name, const WCHAR *instance, PB_DRIVER_BINDING *binding);
HANDLE PbLockDriverOperations(void);
void PbUnlockDriverOperations(HANDLE mutex);
BOOL PbRepairDriverPackage(const WCHAR *name, const PB_DRIVER_BINDING *binding, PB_ADAPTER_DIAGNOSTICS *diagnostics);
