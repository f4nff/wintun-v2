/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_return_diagnostics.h"

/* Private adapter-lifetime history. Callers serialize writers and snapshots. */
typedef struct PB_RETURN_HISTORY {
    uint64_t Sequence, NdisNonzeroNbls, NdisNonzeroPackets, LocalCancelledPackets;
    PB_RETURN_EVENT Records[PB_RETURN_DIAG_CAPACITY];
} PB_RETURN_HISTORY;

static inline uint64_t PbReturnSaturatingAdd(uint64_t value, uint32_t amount)
{
    return value > UINT64_MAX - amount ? UINT64_MAX : value + amount;
}

static inline void PbRecordReturnEvent(PB_RETURN_HISTORY *history, const PB_RETURN_EVENT *event)
{
    if (event->Origin == PB_RETURN_ORIGIN_NDIS) {
        history->NdisNonzeroNbls = PbReturnSaturatingAdd(history->NdisNonzeroNbls, 1);
        history->NdisNonzeroPackets = PbReturnSaturatingAdd(history->NdisNonzeroPackets, event->OriginalCount);
    } else {
        history->LocalCancelledPackets = PbReturnSaturatingAdd(history->LocalCancelledPackets, event->OriginalCount);
    }
    /* Never wrap sequence identity. Saturated history remains a valid final snapshot. */
    if (history->Sequence == UINT64_MAX) return;
    PB_RETURN_EVENT *out = history->Records + (history->Sequence % PB_RETURN_DIAG_CAPACITY);
    *out = *event;
    out->Sequence = ++history->Sequence;
}

static inline void PbSnapshotReturnEvents(const PB_RETURN_HISTORY *history, PB_RETURN_SNAPSHOT *out)
{
    out->RecordCapacity = PB_RETURN_DIAG_CAPACITY;
    out->RecordCount = history->Sequence < PB_RETURN_DIAG_CAPACITY ? (uint32_t)history->Sequence : PB_RETURN_DIAG_CAPACITY;
    out->LatestSequence = history->Sequence;
    out->NdisNonzeroNbls = history->NdisNonzeroNbls;
    out->NdisNonzeroPackets = history->NdisNonzeroPackets;
    out->LocalCancelledPackets = history->LocalCancelledPackets;
    uint64_t first = history->Sequence - out->RecordCount;
    for (uint32_t i = 0; i < out->RecordCount; ++i)
        out->Records[i] = history->Records[(first + i) % PB_RETURN_DIAG_CAPACITY];
}
