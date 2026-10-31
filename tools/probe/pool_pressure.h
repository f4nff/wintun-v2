/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdio.h>
#include <string.h>
#include "../../include/packetbridge_pool_pressure.h"
#include "../../include/packetbridge_version.h"
#include "../../common/v2_core.h"
_Static_assert(sizeof(PB_POOL_PRESSURE_EVENT) == 176, "pool pressure event ABI");
_Static_assert(sizeof(PB_TX_POOL_STATE) == 112, "current pool state ABI");
_Static_assert(sizeof(PB_POOL_PRESSURE_SNAPSHOT) == 12344, "pool pressure snapshot ABI");
static int PbValidPoolPressure(const PB_POOL_PRESSURE_SNAPSHOT *s)
{
    if (s->Size != sizeof(*s) || s->Magic != PB_POOL_PRESSURE_MAGIC || s->Schema != PB_POOL_PRESSURE_SCHEMA ||
        s->RecordCapacity != PB_POOL_PRESSURE_CAPACITY || s->SampleStride != PB_POOL_PRESSURE_STRIDE || s->Reserved ||
        s->LatestSequence > s->SampledEvents || s->SampledEvents > s->PoolFullEvents ||
        s->RecordCount != (s->LatestSequence < PB_POOL_PRESSURE_CAPACITY ? (uint32_t)s->LatestSequence : PB_POOL_PRESSURE_CAPACITY) ||
        s->SourceFingerprint[64] || s->CurrentQueueCount > PB_MAX_QUEUES || s->CurrentStatus > PB_UNSUPPORTED) return 0;
    for (unsigned i = 0; i < 64; ++i) {
        char c = s->SourceFingerprint[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    for (unsigned i = 0; i < PB_POOL_PRESSURE_CLASSES; ++i) if (s->Classes[i] > s->SampledEvents) return 0;
    for (uint32_t i = 0; i < s->CurrentQueueCount; ++i) {
        const PB_TX_POOL_STATE *q = s->Current + i;
        if (!q->Epoch || q->Queue != i || !q->Slots || q->Slots > PB_MAX_SLOTS || q->PoolExhausted > 1 ||
            q->CountsValid > 1 || (q->CursorFlags & ~(PB_POOL_RETURN_VALID | PB_POOL_READY_VALID)) ||
            q->ReadyStatus > PB_UNSUPPORTED || q->ReturnStatus > PB_UNSUPPORTED ||
            q->Running > 1 || q->Draining > 1 || q->Stopping > 1 || q->Faulted > 1) return 0;
        if (q->CountsValid) {
            if (q->PoolSlots != q->Slots || (uint64_t)q->Free + q->Reserved + q->Published + q->Retired + q->Other != q->Slots) return 0;
        } else if (q->Free || q->Reserved || q->Published || q->Retired || q->Other) return 0;
    }
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_POOL_PRESSURE_EVENT *e = s->Records + i;
        if (e->Sequence != s->LatestSequence - s->RecordCount + i + 1u || !e->Send.Sequence || !e->Send.Epoch ||
            e->Send.Reason != PB_SEND_POOL_FULL || e->Send.Queue >= PB_MAX_QUEUES || !e->Send.NdisStatus ||
            e->Send.Reserved || e->Slots > PB_MAX_SLOTS || e->Pending > e->Slots || (e->Flags & ~PB_POOL_ALL_FLAGS) ||
            e->ReadyStatus > PB_UNSUPPORTED || e->ReturnStatus > PB_UNSUPPORTED) return 0;
        if (e->Flags & PB_POOL_COUNTS_VALID) {
            if (e->Flags != PB_POOL_ALL_FLAGS || e->ReadyStatus || e->ReturnStatus ||
                (uint64_t)e->Pending + e->Outstanding + e->Unaccounted != e->Slots ||
                (uint64_t)e->ReadyUnread + e->UserHeld + e->ReturnQueued != e->Outstanding ||
                e->ReadyProducer - e->ReturnConsumer != e->Outstanding ||
                e->ReadyProducer - e->ReadyConsumer != e->ReadyUnread ||
                e->ReturnProducer - e->ReturnConsumer != e->ReturnQueued) return 0;
        } else if (e->Outstanding || e->ReadyUnread || e->UserHeld || e->ReturnQueued || e->Unaccounted) return 0;
    }
    return 1;
}
static int PbPrintPoolPressure(const PB_POOL_PRESSURE_SNAPSHOT *s)
{
    if (!PbValidPoolPressure(s)) return 1;
    int matching = s->FileVersion == PB_VERSION_PACKED && !strcmp(s->SourceFingerprint, PB_SOURCE_STRING);
    printf("{\n  \"tool_version\":\"%s\",\"schema\":%u,\"matching_build\":%s,\"driver_source_sha256\":\"%s\",\n",
        PB_VERSION_STRING, s->Schema, matching ? "true" : "false", s->SourceFingerprint);
    printf("  \"pool_full_events\":%llu,\"sampled_observations\":%llu,\"latest_sample_sequence\":%llu,\"sample_stride\":%u,\"retained_records\":%u,\n",
        (unsigned long long)s->PoolFullEvents, (unsigned long long)s->SampledEvents,
        (unsigned long long)s->LatestSequence, s->SampleStride, s->RecordCount);
    puts("  \"sampling\":\"first-and-every-64th-pool-full\",\"snapshot_atomic\":false,\"sample_classes_overlap\":true,\"extrapolation\":false,\"payload_logged\":false,\"events_are_unique_nbls\":false,");
    puts("  \"units\":\"slots/descriptors-not-original-packets\",");
    puts("  \"observation\":\"after-rejection-before-handoff; return-before-ready snapshots; queued returns may arrive after admission check\",");
    printf("  \"sample_contains\":{\"pending\":%llu,\"ready\":%llu,\"user_or_late_return\":%llu,\"returned\":%llu,\"unaccounted\":%llu,\"unknown\":%llu},\n  \"records\":[\n",
        (unsigned long long)s->Classes[0], (unsigned long long)s->Classes[1], (unsigned long long)s->Classes[2],
        (unsigned long long)s->Classes[3], (unsigned long long)s->Classes[4], (unsigned long long)s->Classes[5]);
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_POOL_PRESSURE_EVENT *e = s->Records + i;
        printf("    {\"sample_sequence\":%llu,\"send_sequence\":%llu,\"epoch\":\"0x%016llx\",\"queue\":%u,\"ndis_status_raw\":\"0x%08x\",\"prior_ndis_status_raw\":\"0x%08x\",\"nb_length\":%u,",
            (unsigned long long)e->Sequence, (unsigned long long)e->Send.Sequence, (unsigned long long)e->Send.Epoch,
            e->Send.Queue, e->Send.NdisStatus, e->Send.PriorNdisStatus, e->Send.NetBufferLength);
        printf("\"slots\":%u,\"pending_publication_slots\":%u,\"outstanding_published_slots\":%u,\"ready_unread_slots\":%u,\"user_held_or_late_return_slots\":%u,\"returned_unrecycled_slots\":%u,\"unaccounted_slots\":%u,\"counts_valid\":%s,\"flags\":%u,",
            e->Slots, e->Pending, e->Outstanding, e->ReadyUnread, e->UserHeld, e->ReturnQueued, e->Unaccounted,
            e->Flags & PB_POOL_COUNTS_VALID ? "true" : "false", e->Flags);
        printf("\"ready_producer\":%llu,\"ready_consumer\":%llu,\"return_producer\":%llu,\"return_consumer\":%llu,\"ready_status\":%u,\"return_status\":%u,",
            (unsigned long long)e->ReadyProducer, (unsigned long long)e->ReadyConsumer,
            (unsigned long long)e->ReturnProducer, (unsigned long long)e->ReturnConsumer, e->ReadyStatus, e->ReturnStatus);
        printf("\"lso_info_raw\":\"0x%016llx\",\"uso_info_raw\":\"0x%016llx\",\"checksum_info_raw\":\"0x%016llx\"}%s\n",
            (unsigned long long)e->Send.LsoInfo, (unsigned long long)e->Send.UsoInfo,
            (unsigned long long)e->Send.ChecksumInfo, i + 1 < s->RecordCount ? "," : "");
    }
    printf("  ],\n  \"current_observation_status\":%u,\"current_queues\":[\n", s->CurrentStatus);
    for (uint32_t i = 0; i < s->CurrentQueueCount; ++i) {
        const PB_TX_POOL_STATE *q = s->Current + i;
        printf("    {\"queue\":%u,\"epoch\":\"0x%016llx\",\"slots\":%u,\"pool_slots\":%u,\"free\":%u,\"reserved\":%u,\"published\":%u,\"retired\":%u,\"other\":%u,\"counts_valid\":%s,\"pool_exhausted\":%s,",
            q->Queue, (unsigned long long)q->Epoch, q->Slots, q->PoolSlots, q->Free, q->Reserved, q->Published,
            q->Retired, q->Other, q->CountsValid ? "true" : "false", q->PoolExhausted ? "true" : "false");
        printf("\"ready_producer\":%llu,\"ready_consumer\":%llu,\"return_producer\":%llu,\"return_consumer\":%llu,\"cursor_flags\":%u,\"ready_status\":%u,\"return_status\":%u,\"running\":%u,\"draining\":%u,\"stopping\":%u,\"faulted\":%u}%s\n",
            (unsigned long long)q->ReadyProducer, (unsigned long long)q->ReadyConsumer,
            (unsigned long long)q->ReturnProducer, (unsigned long long)q->ReturnConsumer, q->CursorFlags,
            q->ReadyStatus, q->ReturnStatus, q->Running, q->Draining, q->Stopping, q->Faulted, i + 1 < s->CurrentQueueCount ? "," : "");
    }
    puts("  ]\n}"); return matching ? 0 : 3;
}
