/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Construction must close/join every failed software-device callback before
 * returning. Recovery is one cold-path transaction, never a data-plane retry. */
static PB_RESULT PbCreateWithRecovery(const WCHAR *name, const WCHAR *instance, PB_ADAPTER **out,
    PB_ADAPTER_DIAGNOSTICS *diagnostics,
    PB_RESULT (*create)(const WCHAR *, const WCHAR *, PB_ADAPTER **, PB_ADAPTER_DIAGNOSTICS *, PB_DRIVER_BINDING *),
    BOOL (*repair)(const WCHAR *, const PB_DRIVER_BINDING *, PB_ADAPTER_DIAGNOSTICS *), PB_DRIVER_BINDING *binding)
{
    PB_RESULT result = PB_UNSUPPORTED;
    for (uint32_t attempt = 1; attempt <= 2; ++attempt) {
        diagnostics->Attempts = attempt;
        diagnostics->Win32Error = diagnostics->HResult = 0;
        diagnostics->ProblemCode = diagnostics->ProblemStatus = diagnostics->ProblemFieldsKnown = 0;
        result = create(name, instance, out, diagnostics, binding);
        if (result == PB_OK) { diagnostics->Stage = PB_ADAPTER_COMPLETE; break; }
        if (attempt == 2 || result != PB_UNSUPPORTED || diagnostics->Stage == PB_ADAPTER_PREPARE_PACKAGE) break;
        diagnostics->FirstWin32Error = diagnostics->Win32Error;
        diagnostics->FirstHResult = diagnostics->HResult;
        diagnostics->FirstStage = diagnostics->Stage;
        diagnostics->FirstProblemCode = diagnostics->ProblemCode;
        diagnostics->FirstProblemStatus = diagnostics->ProblemStatus;
        diagnostics->FirstProblemFieldsKnown = diagnostics->ProblemFieldsKnown;
        /* A partial native cleanup can still permit a fresh installation.
         * Retain its exact status and try the embedded package once regardless. */
        if (!repair(name, binding, diagnostics) && !diagnostics->RecoveryError) diagnostics->RecoveryError = GetLastError();
    }
    SetLastError(diagnostics->Win32Error); return result;
}
