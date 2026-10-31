/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

/* Ctx is referenced by the dispatch path. This query owns no session or NBL. */
static NTSTATUS PbQueryReturnDiagnostics(TUN_CTX *ctx, VOID *buffer, ULONG outputSize, ULONG inputSize, ULONG_PTR *written)
{
    *written = 0;
    if (inputSize) return STATUS_INVALID_PARAMETER;
    if (!buffer || outputSize < sizeof(PB_RETURN_SNAPSHOT)) return STATUS_BUFFER_TOO_SMALL;
    C_ASSERT(sizeof(PB_RETURN_EVENT) == 64);
    C_ASSERT(sizeof(PB_RETURN_SNAPSHOT) == 4224);
    /* METHOD_BUFFERED output is nonpaged. Keep history off the kernel stack. */
    PB_RETURN_SNAPSHOT *out = buffer;
    RtlZeroMemory(out, sizeof(*out));
    out->Size = sizeof(*out); out->Magic = PB_RETURN_DIAG_MAGIC;
    out->Schema = PB_RETURN_DIAG_SCHEMA; out->FileVersion = PB_VERSION_PACKED;
    C_ASSERT(sizeof(PB_SOURCE_STRING) <= sizeof(out->SourceFingerprint));
    RtlCopyMemory(out->SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING));
    KIRQL irql;
    KeAcquireSpinLock(&ctx->Probe.ReturnDiagnosticsLock, &irql);
    PbSnapshotReturnEvents(&ctx->Probe.ReturnDiagnostics, out);
    KeReleaseSpinLock(&ctx->Probe.ReturnDiagnosticsLock, irql);
    *written = sizeof(*out);
    return STATUS_SUCCESS;
}
