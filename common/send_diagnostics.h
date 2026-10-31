/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_send_diagnostics.h"
#include "../include/packetbridge_pool_pressure.h"
typedef struct PB_SEND_HISTORY {
    uint64_t Sequence, RejectionEvents, Reasons[PB_SEND_REASON_COUNT];
    PB_SEND_EVENT Records[PB_SEND_DIAG_CAPACITY];
    uint64_t PressureSequence, PressureSamples, PressureClasses[PB_POOL_PRESSURE_CLASSES];
    PB_POOL_PRESSURE_EVENT Pressure[PB_POOL_PRESSURE_CAPACITY];
} PB_SEND_HISTORY;
static inline void PbRecordSendEvent(PB_SEND_HISTORY *history, const PB_SEND_EVENT *event)
{
    if (history->RejectionEvents != UINT64_MAX) history->RejectionEvents++;
    uint32_t reason = event->Reason < PB_SEND_REASON_COUNT ? event->Reason : PB_SEND_UNKNOWN;
    if (history->Reasons[reason] != UINT64_MAX) history->Reasons[reason]++;
    if (history->Sequence == UINT64_MAX) return;
    PB_SEND_EVENT *out = history->Records + history->Sequence % PB_SEND_DIAG_CAPACITY;
    *out = *event; out->Sequence = ++history->Sequence;
}

/* Caller holds the same history lock as the original rejection record. */
static inline void PbRecordPoolPressure(PB_SEND_HISTORY *history, PB_POOL_PRESSURE_EVENT *event)
{
    if (history->PressureSamples != UINT64_MAX) history->PressureSamples++;
    if (history->PressureSequence == UINT64_MAX) return;
    event->Sequence = ++history->PressureSequence;
    history->Pressure[(event->Sequence - 1u) % PB_POOL_PRESSURE_CAPACITY] = *event;
    uint32_t counts[PB_POOL_PRESSURE_CLASSES] = { event->Pending, event->ReadyUnread,
        event->UserHeld, event->ReturnQueued, event->Unaccounted, !(event->Flags & PB_POOL_COUNTS_VALID) };
    for (uint32_t i = 0; i < PB_POOL_PRESSURE_CLASSES; ++i)
        if (counts[i] && history->PressureClasses[i] != UINT64_MAX) history->PressureClasses[i]++;
}

static inline void PbSnapshotPoolPressure(const PB_SEND_HISTORY *history, PB_POOL_PRESSURE_SNAPSHOT *out)
{
    out->RecordCapacity = PB_POOL_PRESSURE_CAPACITY; out->SampleStride = PB_POOL_PRESSURE_STRIDE;
    out->LatestSequence = history->PressureSequence; out->PoolFullEvents = history->Reasons[PB_SEND_POOL_FULL];
    out->SampledEvents = history->PressureSamples;
    out->RecordCount = history->PressureSequence < PB_POOL_PRESSURE_CAPACITY ? (uint32_t)history->PressureSequence : PB_POOL_PRESSURE_CAPACITY;
    for (uint32_t i = 0; i < PB_POOL_PRESSURE_CLASSES; ++i) out->Classes[i] = history->PressureClasses[i];
    uint64_t first = history->PressureSequence - out->RecordCount;
    for (uint32_t i = 0; i < out->RecordCount; ++i)
        out->Records[i] = history->Pressure[(first + i) % PB_POOL_PRESSURE_CAPACITY];
}
static inline void PbSnapshotSendEvents(const PB_SEND_HISTORY *history, PB_SEND_SNAPSHOT *out)
{
    out->RecordCapacity = PB_SEND_DIAG_CAPACITY;
    out->RecordCount = history->Sequence < PB_SEND_DIAG_CAPACITY ? (uint32_t)history->Sequence : PB_SEND_DIAG_CAPACITY;
    out->LatestSequence = history->Sequence; out->RejectionEvents = history->RejectionEvents;
    for (uint32_t i = 0; i < PB_SEND_REASON_COUNT; ++i) out->Reasons[i] = history->Reasons[i];
    uint64_t first = history->Sequence - out->RecordCount;
    for (uint32_t i = 0; i < out->RecordCount; ++i) out->Records[i] = history->Records[(first + i) % PB_SEND_DIAG_CAPACITY];
}
