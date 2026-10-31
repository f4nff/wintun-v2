/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdio.h>
#include <string.h>
#include "../../include/packetbridge_return_diagnostics.h"
#include "../../include/packetbridge_version.h"
#include "../../common/v2_core.h"

_Static_assert(sizeof(PB_RETURN_EVENT) == 64, "receive return record ABI");
_Static_assert(sizeof(PB_RETURN_SNAPSHOT) == 4224, "receive return snapshot ABI");

/* Status names describe the captured word, not a cause inferred from packet
 * contents. Unknown values stay explicit. Local synthetic values retain origin. */
static const char *PbReturnStatusName(uint32_t status)
{
    switch (status) {
    case 0: return "NDIS_STATUS_SUCCESS";
    case 0xc000023eu: return "STATUS_PROTOCOL_UNREACHABLE";
    case 0xc023002au: return "NDIS_STATUS_PAUSED";
    default: return "UNCLASSIFIED";
    }
}
static const char *PbReturnClassification(const PB_RETURN_EVENT *event)
{
    if (event->Origin == PB_RETURN_ORIGIN_LOCAL_CANCEL) return "local-cancel-before-indication";
    if (!event->NblStatus) return "success";
    if (event->NblStatus == 0xc000023eu) return "protocol-unreachable";
    return "unclassified-nonzero-return";
}
static const char *PbReturnCompletionName(uint32_t status)
{
    switch (status) {
    case PB_OK: return "PB_OK";
    case PB_RX_REJECTED: return "PB_RX_REJECTED";
    case PB_STOPPED: return "PB_STOPPED";
    default: return "UNKNOWN";
    }
}

static int PbPrintReturnSnapshot(const PB_RETURN_SNAPSHOT *s)
{
    if (s->Size != sizeof(*s) || s->Magic != PB_RETURN_DIAG_MAGIC || s->Schema != PB_RETURN_DIAG_SCHEMA ||
        s->RecordCapacity != PB_RETURN_DIAG_CAPACITY || s->RecordCount > PB_RETURN_DIAG_CAPACITY ||
        s->RecordCount != (s->LatestSequence < PB_RETURN_DIAG_CAPACITY ? s->LatestSequence : PB_RETURN_DIAG_CAPACITY) ||
        s->SourceFingerprint[64]) return 1;
    for (unsigned i = 0; i < 64; ++i)
        if (!((s->SourceFingerprint[i] >= '0' && s->SourceFingerprint[i] <= '9') ||
            (s->SourceFingerprint[i] >= 'a' && s->SourceFingerprint[i] <= 'f'))) return 1;
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_RETURN_EVENT *e = s->Records + i;
        if (e->Sequence != s->LatestSequence - s->RecordCount + 1 + i ||
            (e->Origin != PB_RETURN_ORIGIN_NDIS && e->Origin != PB_RETURN_ORIGIN_LOCAL_CANCEL) ||
            !e->FirstToken.Epoch || !e->FirstToken.Generation || e->FirstToken.Queue >= PB_MAX_QUEUES || e->FirstToken.Slot >= PB_MAX_SLOTS ||
            !e->OriginalCount || e->OriginalCount > PB_MAX_BATCH ||
            e->IndicationLength < 20 || e->IndicationLength > 65535 ||
            (e->IpVersion != 4 && e->IpVersion != 6) || e->Protocol > 255 || e->ReceiveType > PB_DESC_UDP_GSO) return 1;
    }
    int matching = s->FileVersion == PB_VERSION_PACKED && !strcmp(s->SourceFingerprint, PB_SOURCE_STRING);
    printf("{\n  \"tool_version\":\"%s\",\n  \"schema\":%u,\n  \"matching_build\":%s,\n", PB_VERSION_STRING, s->Schema, matching ? "true" : "false");
    printf("  \"driver_source_sha256\":\"%.64s\",\n  \"latest_sequence\":%llu,\n", s->SourceFingerprint, (unsigned long long)s->LatestSequence);
    printf("  \"retained_records\":%u,\n  \"overwritten_records\":%llu,\n", s->RecordCount, (unsigned long long)(s->LatestSequence - s->RecordCount));
    printf("  \"ndis_nonzero_nbls\":%llu,\n  \"ndis_nonzero_packets\":%llu,\n  \"local_cancelled_packets\":%llu,\n",
        (unsigned long long)s->NdisNonzeroNbls, (unsigned long long)s->NdisNonzeroPackets, (unsigned long long)s->LocalCancelledPackets);
    puts("  \"semantics\":\"raw-NBL-status; ownership-return-is-not-peer-delivery-proof\",\n  \"completion_scope\":\"rx-packet-return\",\n  \"records\":[");
    for (uint32_t i = 0; i < s->RecordCount; ++i) {
        const PB_RETURN_EVENT *e = s->Records + i;
        printf("    {\"sequence\":%llu,\"origin\":\"%s\",\"ndis_status_observed\":%s,\"nbl_status_raw\":\"0x%08x\",\"completion_status\":%u,",
            (unsigned long long)e->Sequence, e->Origin == PB_RETURN_ORIGIN_NDIS ? "ndis-return" : "local-cancel-before-indication",
            e->Origin == PB_RETURN_ORIGIN_NDIS ? "true" : "false", e->NblStatus, e->CompletionStatus);
        printf("\"nbl_status_name\":\"%s\",\"classification\":\"%s\",\"completion_status_name\":\"%s\",",
            PbReturnStatusName(e->NblStatus), PbReturnClassification(e), PbReturnCompletionName(e->CompletionStatus));
        printf("\"epoch\":\"0x%016llx\",\"queue\":%u,\"slot\":%u,\"generation\":%u,\"original_count\":%u,\"original_bytes\":%u,",
            (unsigned long long)e->FirstToken.Epoch, e->FirstToken.Queue, e->FirstToken.Slot, e->FirstToken.Generation, e->OriginalCount, e->OriginalBytes);
        printf("\"ip_length\":%u,\"ip_declared_length\":%u,\"ip_version\":%u,\"ip_protocol\":%u,\"receive_type\":%u}%s\n",
            e->IndicationLength, e->IpDeclaredLength, e->IpVersion, e->Protocol, e->ReceiveType, i + 1 < s->RecordCount ? "," : "");
    }
    puts("  ]\n}");
    return matching ? 0 : 3;
}
