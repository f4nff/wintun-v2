/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../include/packetbridge_adapter_diagnostics.h"
static inline PB_ADAPTER_DIAGNOSTICS_EX *PbAdapterDetails(PB_ADAPTER_DIAGNOSTICS *diagnostics)
{
    return diagnostics->Size == sizeof(PB_ADAPTER_DIAGNOSTICS_EX) && diagnostics->Schema == PB_ADAPTER_DIAG_SCHEMA_EX
        ? (PB_ADAPTER_DIAGNOSTICS_EX *)diagnostics : NULL;
}
static inline void PbRecoveryFailure(PB_ADAPTER_DIAGNOSTICS *diagnostics, uint32_t step, DWORD error)
{
    if (!diagnostics->RecoveryError) {
        diagnostics->RecoveryError = error ? error : ERROR_GEN_FAILURE;
        PB_ADAPTER_DIAGNOSTICS_EX *details = PbAdapterDetails(diagnostics);
        if (details) details->RecoveryStep = step;
    }
}
