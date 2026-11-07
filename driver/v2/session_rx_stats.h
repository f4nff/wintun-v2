/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "platform_nbl.h"
/* Private to one receive transaction. Publish only if this chain is indicated;
 * a late fault discards these sums together with its unindicated chain. */
typedef struct PB_RX_INDICATION_STATS {
    ULONG RscPackets, RscEvents, UroPackets, UroEvents;
    ULONG64 RscBytes;
} PB_RX_INDICATION_STATS;

static __forceinline VOID PbV2AccumulateRxStats(PB_RX_INDICATION_STATS *stats, NET_BUFFER_LIST *nbl)
{
    PB_RX_RECORD *record = (PB_RX_RECORD *)((ULONG_PTR)NET_BUFFER_LIST_NEXT_NBL_EX(nbl) & ~(ULONG_PTR)1);
    if (record->Count < 2) return;
    NDIS_RSC_NBL_INFO rsc = { .Value = PB_NBL_GET(nbl, TcpRecvSegCoalesceInfo, 30) };
    if (rsc.Info.CoalescedSegCount) {
        stats->RscPackets += record->Count; stats->RscBytes += record->TcpPayloadLength; stats->RscEvents++;
    } else {
        stats->UroPackets += record->Count; stats->UroEvents++;
    }
}

static __forceinline VOID PbV2PublishRxStats(TUN_CTX *ctx, const PB_RX_INDICATION_STATS *stats)
{
    if (stats->RscEvents) {
        InterlockedAdd64(&ctx->Probe.RscPackets, stats->RscPackets);
        InterlockedAdd64(&ctx->Probe.RscBytes, stats->RscBytes);
        InterlockedAdd64(&ctx->Probe.RscEvents, stats->RscEvents);
    }
    if (stats->UroEvents) {
        InterlockedAdd64(&ctx->Probe.UroPackets, stats->UroPackets);
        InterlockedAdd64(&ctx->Probe.UroEvents, stats->UroEvents);
    }
}
