/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
static NTSTATUS PbQuerySendDiagnostics(TUN_CTX *ctx, VOID *buffer, ULONG outputSize, ULONG inputSize, ULONG_PTR *written)
{
    *written = 0;
    if (inputSize) return STATUS_INVALID_PARAMETER;
    if (!buffer || outputSize < sizeof(PB_SEND_SNAPSHOT)) return STATUS_BUFFER_TOO_SMALL;
    C_ASSERT(sizeof(PB_SEND_EVENT) == 96);
    C_ASSERT(sizeof(PB_SEND_SNAPSHOT) == 6528);
    PB_SEND_SNAPSHOT *out = buffer;
    RtlZeroMemory(out, sizeof(*out));
    out->Size = sizeof(*out); out->Magic = PB_SEND_DIAG_MAGIC;
    out->Schema = PB_SEND_DIAG_SCHEMA; out->FileVersion = PB_VERSION_PACKED;
    C_ASSERT(sizeof(PB_SOURCE_STRING) <= sizeof(out->SourceFingerprint));
    RtlCopyMemory(out->SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING));
    KIRQL irql;
    KeAcquireSpinLock(&ctx->Probe.SendDiagnosticsLock, &irql);
    PbSnapshotSendEvents(&ctx->Probe.SendDiagnostics, out);
    KeReleaseSpinLock(&ctx->Probe.SendDiagnosticsLock, irql);
    *written = sizeof(*out); return STATUS_SUCCESS;
}
static NTSTATUS PbQueryPoolPressure(TUN_CTX *ctx, VOID *buffer, ULONG outputSize, ULONG inputSize, ULONG_PTR *written)
{
    *written = 0;
    if (inputSize) return STATUS_INVALID_PARAMETER;
    if (!buffer || outputSize < sizeof(PB_POOL_PRESSURE_SNAPSHOT)) return STATUS_BUFFER_TOO_SMALL;
    C_ASSERT(sizeof(PB_POOL_PRESSURE_EVENT) == 176);
    C_ASSERT(sizeof(PB_TX_POOL_STATE) == 112);
    C_ASSERT(sizeof(PB_POOL_PRESSURE_SNAPSHOT) == 12344);
    PB_POOL_PRESSURE_SNAPSHOT *out = buffer;
    RtlZeroMemory(out, sizeof(*out)); out->Size = sizeof(*out); out->Magic = PB_POOL_PRESSURE_MAGIC;
    out->Schema = PB_POOL_PRESSURE_SCHEMA; out->FileVersion = PB_VERSION_PACKED;
    RtlCopyMemory(out->SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING));
    KIRQL irql; KeAcquireSpinLock(&ctx->Probe.SendDiagnosticsLock, &irql);
    PbSnapshotPoolPressure(&ctx->Probe.SendDiagnostics, out);
    KeReleaseSpinLock(&ctx->Probe.SendDiagnosticsLock, irql);
#ifndef PB_POOL_PRESSURE_HISTORY_ONLY
    PbSnapshotCurrentTxPools(ctx, out);
#endif
    *written = sizeof(*out); return STATUS_SUCCESS;
}
