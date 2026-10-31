/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdio.h>
#include <string.h>
#include "../../include/packetbridge_send_diagnostics.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SEND_EVENT) == 96, "send record ABI");
_Static_assert(sizeof(PB_SEND_SNAPSHOT) == 6528, "send snapshot ABI");
static const char *PbSendReasonName(uint32_t reason)
{
    static const char *const names[] = { "unknown", "admission", "empty-nbl", "prefix-read", "control-read",
        "control-reply", "recycle", "queue-ring", "queue-full", "pool-full", "pool-error", "publication",
        "length", "payload-read", "offload-combination", "gso-plan", "gso-offset", "gso-family", "gso-count",
        "gso-payload-limit", "ip-length", "checksum-family", "checksum-data", "checksum-offset", "policy", "prior-nbl-failure" };
    return reason < sizeof(names) / sizeof(*names) ? names[reason] : "reserved";
}
static int PbPrintSendSnapshot(const PB_SEND_SNAPSHOT *s)
{
    if (s->Size != sizeof(*s) || s->Magic != PB_SEND_DIAG_MAGIC || s->Schema != PB_SEND_DIAG_SCHEMA ||
        s->RecordCapacity != PB_SEND_DIAG_CAPACITY || s->RecordCount > PB_SEND_DIAG_CAPACITY ||
        s->RecordCount != (s->LatestSequence < PB_SEND_DIAG_CAPACITY ? s->LatestSequence : PB_SEND_DIAG_CAPACITY) ||
        s->RejectionEvents < s->LatestSequence || s->SourceFingerprint[64]) return 1;
    for (unsigned i = 0; i < 64; ++i)
        if (!((s->SourceFingerprint[i] >= '0' && s->SourceFingerprint[i] <= '9') ||
            (s->SourceFingerprint[i] >= 'a' && s->SourceFingerprint[i] <= 'f'))) return 1;
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_SEND_EVENT *e = s->Records + i;
        if (e->Sequence != s->LatestSequence - s->RecordCount + 1 + i || !e->NdisStatus ||
            e->Reason >= PB_SEND_REASON_COUNT || (e->Queue != PB_SEND_QUEUE_UNKNOWN && e->Queue >= PB_MAX_QUEUES) ||
            e->NetBufferScope < 1 || e->NetBufferScope > 2) return 1;
    }
    int matching = s->FileVersion == PB_VERSION_PACKED && !strcmp(s->SourceFingerprint, PB_SOURCE_STRING);
    printf("{\n  \"tool_version\":\"%s\",\n  \"schema\":%u,\n  \"matching_build\":%s,\n", PB_VERSION_STRING, s->Schema, matching ? "true" : "false");
    printf("  \"driver_source_sha256\":\"%.64s\",\n  \"latest_sequence\":%llu,\n  \"retained_records\":%u,\n  \"overwritten_records\":%llu,\n  \"rejection_events\":%llu,\n",
        s->SourceFingerprint, (unsigned long long)s->LatestSequence, s->RecordCount,
        (unsigned long long)(s->LatestSequence - s->RecordCount), (unsigned long long)s->RejectionEvents);
    puts("  \"scope\":\"tx-rejection-branch-before-NBL-completion\",\n  \"events_are_unique_nbls\":false,\n  \"payload_logged\":false,\n  \"reason_counts\":[");
    for (unsigned i = 0; i < PB_SEND_REASON_COUNT; ++i)
        printf("    {\"reason\":%u,\"name\":\"%s\",\"count\":%llu}%s\n", i, PbSendReasonName(i), (unsigned long long)s->Reasons[i], i + 1 < PB_SEND_REASON_COUNT ? "," : "");
    puts("  ],\n  \"records\":[");
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_SEND_EVENT *e = s->Records + i;
        printf("    {\"sequence\":%llu,\"epoch\":\"0x%016llx\",\"ndis_status_raw\":\"0x%08x\",\"prior_ndis_status_raw\":\"0x%08x\",\"reason\":%u,\"reason_name\":\"%s\",",
            (unsigned long long)e->Sequence, (unsigned long long)e->Epoch, e->NdisStatus, e->PriorNdisStatus, e->Reason, PbSendReasonName(e->Reason));
        printf("\"queue\":%u,\"nb_length\":%u,\"ethernet_length\":%u,\"nb_scope\":\"%s\",\"packet_status\":%u,\"core_status\":%u,\"required_mask\":%u,\"effective_mask\":%u,\"accepted_count\":%u,\"attempted_count\":%u,",
            e->Queue, e->NetBufferLength, e->EthernetLength, e->NetBufferScope == 1 ? "first-of-nbl" : "rejected-nb",
            e->PacketStatus, e->CoreStatus, e->RequiredMask, e->EffectiveMask, e->AcceptedCount, e->AttemptedCount);
        printf("\"lso_info_raw\":\"0x%016llx\",\"uso_info_raw\":\"0x%016llx\",\"checksum_info_raw\":\"0x%016llx\"}%s\n",
            (unsigned long long)e->LsoInfo, (unsigned long long)e->UsoInfo, (unsigned long long)e->ChecksumInfo, i + 1 < s->RecordCount ? "," : "");
    }
    puts("  ]\n}"); return matching ? 0 : 3;
}
