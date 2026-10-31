/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge.h"
#include "../../include/packetbridge_adapter_diagnostics.h"
PB_RESULT WINAPI PbAdapterCreate(const wchar_t *name, const wchar_t *instance, PB_ADAPTER **out);
PB_RESULT WINAPI PbCreateAdapterEx(const wchar_t *name, const wchar_t *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *diagnostics);
PB_RESULT WINAPI PbCreateAdapterEx2(const wchar_t *name, const wchar_t *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS_EX *diagnostics);
void WINAPI PbAdapterRelease(PB_ADAPTER *adapter);
void PbAdapterRetain(PB_ADAPTER *adapter);
const wchar_t *PbAdapterPath(PB_ADAPTER *adapter);
PB_RESULT WINAPI PbAdapterLuid(PB_ADAPTER *adapter, uint64_t *luid);
BOOL PbRuntimeReserveAdapter(void);
void PbRuntimeReleaseAdapter(void);
