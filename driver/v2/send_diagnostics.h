/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "send_pressure.h"
/* Exception-only snapshot before LSO completion mutates OOB and before NBL
 * ownership is returned. No MDL/payload reads, allocation, clock or upcall. */
static __declspec(noinline) VOID PbV2RejectSend(TUN_CTX *ctx, uint64_t epoch, NET_BUFFER_LIST *nbl, NET_BUFFER *nb,
    NDIS_STATUS status, ULONG prior, ULONG queue, ULONG ethernet, const PB_SEND_FAILURE *failure,
    ULONG accepted, ULONG attempted, ULONG scope, PB_KERNEL_QUEUE *pressureQueue, ULONG pending)
{
    PB_SEND_EVENT event = { 0 };
    event.Epoch = epoch; event.NdisStatus = (uint32_t)status; event.PriorNdisStatus = prior;
    event.LsoInfo = (ULONG_PTR)NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo);
    event.UsoInfo = (ULONG_PTR)NET_BUFFER_LIST_INFO(nbl, UdpSegmentationOffloadInfo);
    event.ChecksumInfo = (ULONG_PTR)NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo);
    event.Reason = failure->Reason; event.Queue = queue;
    event.NetBufferLength = nb ? NET_BUFFER_DATA_LENGTH(nb) : 0; event.EthernetLength = ethernet;
    event.PacketStatus = failure->PacketStatus; event.CoreStatus = failure->CoreStatus;
    event.RequiredMask = failure->RequiredMask; event.EffectiveMask = PbEffectiveMask(ctx);
    event.AcceptedCount = accepted; event.AttemptedCount = attempted; event.NetBufferScope = scope;
    KIRQL irql;
    KeAcquireSpinLock(&ctx->Probe.SendDiagnosticsLock, &irql);
    PbRecordSendEvent(&ctx->Probe.SendDiagnostics, &event);
    PB_SEND_HISTORY *history = &ctx->Probe.SendDiagnostics;
    /* First and each 64th subsequent pool-full event only. All original raw
     * rejections remain counted/stored; no clocks, pool walk or extra lock. */
    if (event.Reason == PB_SEND_POOL_FULL && pressureQueue &&
        (history->Reasons[PB_SEND_POOL_FULL] & (PB_POOL_PRESSURE_STRIDE - 1u)) == 1u) {
        PB_POOL_PRESSURE_EVENT pressure = { 0 };
        pressure.Send = event; pressure.Send.Sequence = history->Sequence;
        PbV2CapturePoolPressure(pressureQueue, pending, &pressure);
        PbRecordPoolPressure(history, &pressure);
    }
    InterlockedIncrement64(&ctx->Probe.OffloadRejected);
    KeReleaseSpinLock(&ctx->Probe.SendDiagnosticsLock, irql);
}
