/* SPDX-License-Identifier: GPL-2.0 */
/* Included after TUN_CTX, NdisVersion and the NBL helpers in wintun.c. */
#pragma once
#include "../../common/offload_policy.h"
#include "../../common/platform.h"
#define PB_ETH_ENCAPS (NDIS_ENCAPSULATION_IEEE_802_3 | NDIS_ENCAPSULATION_IEEE_802_3_P_AND_Q)
#include "indication_gate.h"
#include "ndis_callouts.h"
#include "return_diagnostic_query.h"
#include "pool_snapshot.h"
#include "send_diagnostic_query.h"

static ULONG PbEffectiveMask(TUN_CTX *ctx)
{
    return PbResolveOffloads((ULONG)ReadAcquire(&ctx->Probe.AllowedMask), (ULONG)ReadAcquire(&ctx->Probe.NdisMask),
        (ULONG)ReadAcquire(&ctx->Probe.EncapsulationMask), ReadAcquire(&ctx->Probe.V2SessionActive)) & PbPlatformOffloads(NdisVersion);
}

#include "offload_format.h"

static NDIS_STATUS PbRegisterOffload(TUN_CTX *ctx)
{
    NDIS_OFFLOAD hardware, current;
    NDIS_TCP_CONNECTION_OFFLOAD connection = { .Header = {
        .Type = NDIS_OBJECT_TYPE_DEFAULT, .Revision = NDIS_TCP_CONNECTION_OFFLOAD_REVISION_1,
        .Size = NDIS_SIZEOF_TCP_CONNECTION_OFFLOAD_REVISION_1 } };
    PbMakeOffloadForVersion(NdisVersion, PbPlatformOffloads(NdisVersion), &hardware);
    PbMakeOffloadForVersion(NdisVersion, 0, &current);
    ctx->Probe.NdisMask = PbPlatformOffloads(NdisVersion);
    ctx->Probe.EncapsulationMask = 0;
    /* Explicit opt-in via the diagnostic IOCTL; the initial data path stays plain. */
    ctx->Probe.AllowedMask = 0;
    NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES attributes = { .Header = {
        .Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES,
        .Revision = NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1,
        .Size = NDIS_SIZEOF_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1 },
        .DefaultOffloadConfiguration = &current, .HardwareOffloadCapabilities = &hardware,
        .DefaultTcpConnectionOffloadConfiguration = &connection, .TcpConnectionOffloadHardwareCapabilities = &connection };
    return NdisMSetMiniportAttributes(ctx->MiniportAdapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&attributes);
}

static VOID PbReportOffload(TUN_CTX *ctx)
{
    if (ctx->Probe.RegistrationStatus != NDIS_STATUS_SUCCESS) return;
    /* Serialize snapshot + indication. An older concurrent reporter cannot overwrite a later disable.
     * StatusLock is never acquired under TransitionLock or a queue lock. */
    KLOCK_QUEUE_HANDLE reportLock;
    KeAcquireInStackQueuedSpinLock(&ctx->Probe.StatusLock, &reportLock);
    NDIS_OFFLOAD current;
    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
    PbMakeOffloadForVersion(NdisVersion, PbEffectiveMask(ctx), &current);
    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
    NDIS_STATUS_INDICATION indication = { .Header = { .Type = NDIS_OBJECT_TYPE_STATUS_INDICATION,
        .Revision = NDIS_STATUS_INDICATION_REVISION_1, .Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1 },
        .SourceHandle = ctx->MiniportAdapterHandle, .StatusCode = NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG,
        .StatusBuffer = &current, .StatusBufferSize = current.Header.Size };
    NdisMIndicateStatusEx(ctx->MiniportAdapterHandle, &indication);
    KeReleaseInStackQueuedSpinLock(&reportLock);
}

static BOOLEAN PbUpdateParameter(UCHAR parameter, ULONG bit, ULONG *mask)
{
    uint32_t value = *mask;
    if (PbApplyOffloadParameter(parameter, bit, &value) != PB_OK) return FALSE;
    *mask = value; return TRUE;
}

static BOOLEAN PbChecksumParameter(UCHAR parameter, ULONG tx, ULONG rx, ULONG *mask)
{
    uint32_t value = *mask;
    if (PbApplyChecksumParameter(parameter, tx, rx, &value) != PB_OK) return FALSE;
    *mask = value; return TRUE;
}

static NDIS_STATUS PbSetOffloadParameters(TUN_CTX *ctx, NDIS_OID_REQUEST *request)
{
    const NDIS_OFFLOAD_PARAMETERS *p = request->DATA.SET_INFORMATION.InformationBuffer;
    ULONG size = request->DATA.SET_INFORMATION.InformationBufferLength;
    ULONG mask = (ULONG)ReadAcquire(&ctx->Probe.NdisMask);
    if (ctx->Probe.RegistrationStatus != NDIS_STATUS_SUCCESS) return NDIS_STATUS_NOT_SUPPORTED;
    if (size < NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_1 || p->Header.Size > size ||
        p->Header.Size < NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_1 || p->Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
        p->Header.Revision < NDIS_OFFLOAD_PARAMETERS_REVISION_1 || p->Header.Revision > PbPlatformParameterRevision(NdisVersion))
        return NDIS_STATUS_INVALID_LENGTH;
    static const USHORT sizes[] = { 0, NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_1,
        NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_2, NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3,
        NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_4, NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_5,
        NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_6 };
    if (p->Header.Size != sizes[p->Header.Revision]) return NDIS_STATUS_INVALID_LENGTH;
    if ((p->Flags & ~NDIS_OFFLOAD_PARAMETERS_SKIP_REGISTRY_UPDATE) ||
        (p->Flags && NdisVersion < PB_NDIS_VERSION(30))) return NDIS_STATUS_NOT_SUPPORTED;
    if (!PbChecksumParameter(p->IPv4Checksum, PB_TX_IP4_CHECKSUM, PB_RX_IP4_CHECKSUM, &mask) ||
        !PbChecksumParameter(p->TCPIPv4Checksum, PB_TX_TCP4_CHECKSUM, PB_RX_TCP4_CHECKSUM, &mask) ||
        !PbChecksumParameter(p->UDPIPv4Checksum, PB_TX_UDP4_CHECKSUM, PB_RX_UDP4_CHECKSUM, &mask) ||
        !PbChecksumParameter(p->TCPIPv6Checksum, PB_TX_TCP6_CHECKSUM, PB_RX_TCP6_CHECKSUM, &mask) ||
        !PbChecksumParameter(p->UDPIPv6Checksum, PB_TX_UDP6_CHECKSUM, PB_RX_UDP6_CHECKSUM, &mask) ||
        !PbUpdateParameter(p->LsoV2IPv4, PB_TX_LSO4, &mask) ||
        !PbUpdateParameter(p->LsoV2IPv6, PB_TX_LSO6, &mask)) return NDIS_STATUS_NOT_SUPPORTED;
    if ((p->LsoV1 && p->LsoV1 != NDIS_OFFLOAD_PARAMETERS_LSOV1_DISABLED) ||
        (p->IPsecV1 && p->IPsecV1 != NDIS_OFFLOAD_PARAMETERS_IPSECV1_DISABLED) ||
        (p->TcpConnectionIPv4 && p->TcpConnectionIPv4 != NDIS_OFFLOAD_PARAMETERS_CONNECTION_OFFLOAD_DISABLED) ||
        (p->TcpConnectionIPv6 && p->TcpConnectionIPv6 != NDIS_OFFLOAD_PARAMETERS_CONNECTION_OFFLOAD_DISABLED))
        return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_2 &&
        ((p->IPsecV2 && p->IPsecV2 != NDIS_OFFLOAD_PARAMETERS_IPSECV2_DISABLED) ||
         (p->IPsecV2IPv4 && p->IPsecV2IPv4 != NDIS_OFFLOAD_PARAMETERS_IPSECV2_DISABLED)))
        return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3 &&
        (!PbUpdateParameter(p->RscIPv4, PB_RX_RSC4, &mask) || !PbUpdateParameter(p->RscIPv6, PB_RX_RSC6, &mask))) return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3 &&
        (p->EncapsulatedPacketTaskOffload || p->EncapsulationTypes)) return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_4 &&
        p->EncapsulationProtocolParameters.Value) return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_5 &&
        (!PbUpdateParameter(p->UdpSegmentation.IPv4, PB_TX_USO4, &mask) ||
         !PbUpdateParameter(p->UdpSegmentation.IPv6, PB_TX_USO6, &mask))) return NDIS_STATUS_NOT_SUPPORTED;
    if (p->Header.Size >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_6 &&
        !PbUpdateParameter(p->UdpRsc.Enabled, PB_RX_URO, &mask))
        return NDIS_STATUS_NOT_SUPPORTED;
    if (mask & ~PbPlatformOffloads(NdisVersion)) return NDIS_STATUS_NOT_SUPPORTED;
    ExAcquireFastMutex(&ctx->Probe.OffloadMutex);
    KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    WriteRelease(&ctx->Probe.NdisMask, (LONG)mask);
    ULONG oldBank = PbRotateIndications(ctx);
    ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
    PbWaitIndications(ctx, oldBank);
    InterlockedIncrement64(&ctx->Probe.OffloadParameterChanges);
    request->DATA.SET_INFORMATION.BytesRead = p->Header.Size;
    PbReportOffload(ctx);
    ExReleaseFastMutex(&ctx->Probe.OffloadMutex);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS PbSetEncapsulation(TUN_CTX *ctx, NDIS_OID_REQUEST *request)
{
    const NDIS_OFFLOAD_ENCAPSULATION *p = request->DATA.SET_INFORMATION.InformationBuffer;
    if (request->DATA.SET_INFORMATION.InformationBufferLength < sizeof(*p) || p->Header.Size != sizeof(*p) ||
        p->Header.Type != NDIS_OBJECT_TYPE_OFFLOAD_ENCAPSULATION ||
        p->Header.Revision != NDIS_OFFLOAD_ENCAPSULATION_REVISION_1 ||
        p->IPv4.Enabled > NDIS_OFFLOAD_SET_OFF || p->IPv6.Enabled > NDIS_OFFLOAD_SET_OFF)
        return NDIS_STATUS_INVALID_LENGTH;
    if ((p->IPv4.Enabled == NDIS_OFFLOAD_SET_ON && (!(p->IPv4.EncapsulationType & NDIS_ENCAPSULATION_IEEE_802_3) || (p->IPv4.EncapsulationType & ~PB_ETH_ENCAPS) || (p->IPv4.HeaderSize != PB_ETH_HEADER && p->IPv4.HeaderSize != 18))) ||
        (p->IPv6.Enabled == NDIS_OFFLOAD_SET_ON && (!(p->IPv6.EncapsulationType & NDIS_ENCAPSULATION_IEEE_802_3) || (p->IPv6.EncapsulationType & ~PB_ETH_ENCAPS) || (p->IPv6.HeaderSize != PB_ETH_HEADER && p->IPv6.HeaderSize != 18))))
        return NDIS_STATUS_NOT_SUPPORTED;
    const ULONG ipv4Mask = PB_TX_LSO4 | PB_TX_USO4 | PB_TX_IP4_CHECKSUM | PB_TX_TCP4_CHECKSUM | PB_TX_UDP4_CHECKSUM |
        PB_RX_RSC4 | PB_RX_IP4_CHECKSUM | PB_RX_TCP4_CHECKSUM | PB_RX_UDP4_CHECKSUM;
    const ULONG ipv6Mask = PB_TX_LSO6 | PB_TX_USO6 | PB_TX_TCP6_CHECKSUM | PB_TX_UDP6_CHECKSUM |
        PB_RX_RSC6 | PB_RX_TCP6_CHECKSUM | PB_RX_UDP6_CHECKSUM;
    ExAcquireFastMutex(&ctx->Probe.OffloadMutex);
    KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    ULONG mask = (ULONG)ReadAcquire(&ctx->Probe.EncapsulationMask);
    ctx->Probe.LastIpv4Encapsulation = p->IPv4.EncapsulationType;
    ctx->Probe.LastIpv6Encapsulation = p->IPv6.EncapsulationType;
    if (p->IPv4.Enabled == NDIS_OFFLOAD_SET_ON) mask |= ipv4Mask;
    else if (p->IPv4.Enabled == NDIS_OFFLOAD_SET_OFF) mask &= ~ipv4Mask;
    if (p->IPv6.Enabled == NDIS_OFFLOAD_SET_ON) mask |= ipv6Mask;
    else if (p->IPv6.Enabled == NDIS_OFFLOAD_SET_OFF) mask &= ~ipv6Mask;
    if ((mask & (ipv4Mask | ipv6Mask)) == (ipv4Mask | ipv6Mask)) mask |= PB_RX_URO;
    else mask &= ~PB_RX_URO;
    WriteRelease(&ctx->Probe.EncapsulationMask, (LONG)mask);
    ULONG oldBank = PbRotateIndications(ctx);
    ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
    PbWaitIndications(ctx, oldBank);
    request->DATA.SET_INFORMATION.BytesRead = sizeof(*p);
    PbReportOffload(ctx);
    ExReleaseFastMutex(&ctx->Probe.OffloadMutex);
    return NDIS_STATUS_SUCCESS;
}

static NTSTATUS PbProbeIoctl(TUN_CTX *ctx, IRP *irp, ULONG code)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    if (code == PB_POOL_PRESSURE_IOCTL) {
        return PbQueryPoolPressure(ctx, irp->AssociatedIrp.SystemBuffer,
            stack->Parameters.DeviceIoControl.OutputBufferLength, stack->Parameters.DeviceIoControl.InputBufferLength,
            &irp->IoStatus.Information);
    }
    if (code == PB_SEND_DIAG_IOCTL) {
        return PbQuerySendDiagnostics(ctx, irp->AssociatedIrp.SystemBuffer,
            stack->Parameters.DeviceIoControl.OutputBufferLength, stack->Parameters.DeviceIoControl.InputBufferLength,
            &irp->IoStatus.Information);
    }
    if (code == PB_RETURN_DIAG_IOCTL) {
        return PbQueryReturnDiagnostics(ctx, irp->AssociatedIrp.SystemBuffer,
            stack->Parameters.DeviceIoControl.OutputBufferLength, stack->Parameters.DeviceIoControl.InputBufferLength,
            &irp->IoStatus.Information);
    }
    if (code == PB_PROBE_IOCTL_QUERY) {
        PB_PROBE_STATUS status = { 0 };
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(status)) return STATUS_BUFFER_TOO_SMALL;
        status.Size = sizeof(status); status.Magic = PB_PROBE_MAGIC; status.Schema = PB_PROBE_SCHEMA;
        status.FileVersion = PB_VERSION_PACKED; status.NdisVersion = NdisVersion; status.Medium = NdisMedium802_3;
        status.DataAbi = PB_ABI_MAJOR;
        status.ImplementedMask = PbPlatformOffloads(NdisVersion);
        KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);
        status.AllowedMask = (ULONG)ReadAcquire(&ctx->Probe.AllowedMask);
        status.NdisMask = (ULONG)ReadAcquire(&ctx->Probe.NdisMask);
        status.EffectiveMask = PbEffectiveMask(ctx);
        status.SessionRegistered = ctx->V2 != NULL;
        status.LastIpv4Encapsulation = ctx->Probe.LastIpv4Encapsulation;
        status.LastIpv6Encapsulation = ctx->Probe.LastIpv6Encapsulation;
        ExReleaseSpinLockShared(&ctx->TransitionLock, irql);
        status.OffloadRegistrationStatus = ctx->Probe.RegistrationStatus;
        status.LsoNbls = ReadULong64Acquire((ULONG64 *)&ctx->Probe.LsoNbls);
        status.UsoNbls = ReadULong64Acquire((ULONG64 *)&ctx->Probe.UsoNbls);
        status.ChecksumNbls = ReadULong64Acquire((ULONG64 *)&ctx->Probe.ChecksumNbls);
        status.OffloadRejected = ReadULong64Acquire((ULONG64 *)&ctx->Probe.OffloadRejected);
        status.SegmentsCopied = ReadULong64Acquire((ULONG64 *)&ctx->Probe.SegmentsCopied);
        status.OffloadParameterChanges = ReadULong64Acquire((ULONG64 *)&ctx->Probe.OffloadParameterChanges);
        status.RxOffloadsImplemented = 1;
        status.RscPackets = ReadULong64Acquire((ULONG64 *)&ctx->Probe.RscPackets);
        status.RscBytes = ReadULong64Acquire((ULONG64 *)&ctx->Probe.RscBytes);
        status.RscEvents = ReadULong64Acquire((ULONG64 *)&ctx->Probe.RscEvents);
        status.RscAborts = ReadULong64Acquire((ULONG64 *)&ctx->Probe.RscAborts);
        status.UroPackets = ReadULong64Acquire((ULONG64 *)&ctx->Probe.UroPackets);
        status.UroEvents = ReadULong64Acquire((ULONG64 *)&ctx->Probe.UroEvents);
        C_ASSERT(sizeof(PB_SOURCE_STRING) <= sizeof(status.SourceFingerprint));
        RtlCopyMemory(status.SourceFingerprint, PB_SOURCE_STRING, sizeof(PB_SOURCE_STRING));
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &status, sizeof(status));
        irp->IoStatus.Information = sizeof(status);
        return STATUS_SUCCESS;
    }
    if (code == PB_PROBE_IOCTL_POLICY) {
        const PB_PROBE_POLICY *policy = irp->AssociatedIrp.SystemBuffer;
        if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(*policy) ||
            policy->Size != sizeof(*policy) || policy->Schema != PB_PROBE_SCHEMA || policy->Reserved ||
            (policy->AllowedMask & ~PB_PROBE_SUPPORTED)) return STATUS_INVALID_PARAMETER;
        if ((policy->AllowedMask & ~PbPlatformOffloads(NdisVersion)) ||
            (policy->AllowedMask && ctx->Probe.RegistrationStatus != NDIS_STATUS_SUCCESS))
            return STATUS_NOT_SUPPORTED;
        ExAcquireFastMutex(&ctx->Probe.OffloadMutex);
        KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
        if (policy->AllowedMask && !ReadAcquire(&ctx->Probe.V2SessionActive)) {
            ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
            ExReleaseFastMutex(&ctx->Probe.OffloadMutex); return STATUS_DEVICE_NOT_READY;
        }
        WriteRelease(&ctx->Probe.AllowedMask, (LONG)policy->AllowedMask);
        ULONG oldBank = PbRotateIndications(ctx);
        ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
        PbWaitIndications(ctx, oldBank);
        PbReportOffload(ctx);
        ExReleaseFastMutex(&ctx->Probe.OffloadMutex);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls)
{
    for (NET_BUFFER_LIST *nbl = nbls; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO original = { .Value = NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo) };
        if (original.Value && original.Transmit.Type == NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE) {
            NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO complete = { 0 };
            complete.LsoV2TransmitComplete.Type = NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE;
            NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo) = complete.Value;
        }
    }
}
