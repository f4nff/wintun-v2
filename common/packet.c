/* SPDX-License-Identifier: GPL-2.0 */
#include "packet.h"
#include <string.h>

static uint16_t Read16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t Read32(const uint8_t *p) { return ((uint32_t)Read16(p) << 16) | Read16(p + 2); }
static void Write16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void Write32(uint8_t *p, uint32_t v) { Write16(p, (uint16_t)(v >> 16)); Write16(p + 2, (uint16_t)v); }

uint32_t PbChecksumAdd(uint32_t sum, const uint8_t *bytes, uint32_t length)
{
    /* Group four network-order words without wide/unaligned loads. Each group
     * fits in uint32; adding to the accumulator preserves the exact modulo
     * 2^32 result, including a caller's seed. Only the final odd byte is padded. */
    while (length >= 8) {
        sum += (uint32_t)Read16(bytes) + Read16(bytes + 2) + Read16(bytes + 4) + Read16(bytes + 6);
        bytes += 8; length -= 8;
    }
    while (length >= 2) { sum += Read16(bytes); bytes += 2; length -= 2; }
    if (length) sum += (uint32_t)*bytes << 8;
    return sum;
}

uint16_t PbChecksumFinish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t PseudoHeader(const uint8_t *packet, const PB_SEGMENT_PLAN *plan, uint32_t transportLength)
{
    uint32_t sum;
    if (plan->IpVersion == 4) {
        sum = PbChecksumAdd(0, packet + plan->PseudoSourceOffset, 4);
        sum = PbChecksumAdd(sum, packet + plan->PseudoDestinationOffset, 4);
    } else {
        sum = PbChecksumAdd(0, packet + plan->PseudoSourceOffset, 16);
        sum = PbChecksumAdd(sum, packet + plan->PseudoDestinationOffset, 16);
    }
    return sum + (transportLength >> 16) + (transportLength & 0xffffu) + plan->Protocol;
}

PB_PACKET_STATUS PbPlanSegments(const uint8_t *packet, uint32_t length, PB_SEGMENT_TYPE type,
                               uint32_t mss, PB_SEGMENT_PLAN *plan)
{
    uint32_t offset, extensionBytes = 0;
    uint8_t protocol;
    if (!packet || !plan || length < 20 || length > 65536u || (uint32_t)type > PB_SEGMENT_UDP)
        return PB_PACKET_INVALID;
    memset(plan, 0, sizeof(*plan));
    plan->IpVersion = packet[0] >> 4;
    if (plan->IpVersion == 4) {
        offset = (uint32_t)(packet[0] & 15u) * 4;
        if (offset < 20 || offset > length || (Read16(packet + 6) & 0x3fffu))
            return PB_PACKET_INVALID;
        if (type == PB_SEGMENT_NONE && Read16(packet + 2) != length) return PB_PACKET_INVALID;
        protocol = packet[9];
        plan->PseudoSourceOffset = 12;
        plan->PseudoDestinationOffset = 16;
        for (uint32_t at = 20; at < offset;) {
            uint8_t option = packet[at];
            if (!option) break;
            if (option == 1) { at++; continue; }
            if (offset - at < 2) return PB_PACKET_INVALID;
            uint32_t size = packet[at + 1];
            if (size < 2 || size > offset - at) return PB_PACKET_INVALID;
            if (option == 131 || option == 137) {
                if (size < 7 || (size - 3u) % 4u || packet[at + 2] < 4 ||
                    packet[at + 2] > size + 1u || (packet[at + 2] - 4u) % 4u)
                    return PB_PACKET_INVALID;
                if (packet[at + 2] <= size) plan->PseudoDestinationOffset = (uint16_t)(at + size - 4u);
            }
            at += size;
        }
    } else if (plan->IpVersion == 6) {
        if (length < 40) return PB_PACKET_INVALID;
        if (type == PB_SEGMENT_NONE && (uint32_t)Read16(packet + 4) + 40u != length)
            return PB_PACKET_INVALID;
        offset = 40;
        protocol = packet[6];
        plan->PseudoSourceOffset = 8;
        plan->PseudoDestinationOffset = 24;
        while (protocol == 0 || protocol == 43 || protocol == 60) {
            uint32_t size;
            if (length - offset < 8) return PB_PACKET_INVALID;
            size = ((uint32_t)packet[offset + 1] + 1u) * 8u;
            if (size > length - offset || size > 128u - extensionBytes) return PB_PACKET_UNSUPPORTED;
            if (protocol == 43 && packet[offset + 3]) {
                uint8_t routingType = packet[offset + 2];
                if ((routingType == 0 && size >= 24 && (size - 8u) % 16u == 0 &&
                     packet[offset + 3] <= (size - 8u) / 16u) ||
                    (routingType == 2 && size == 24 && packet[offset + 3] == 1))
                    plan->PseudoDestinationOffset = (uint16_t)(offset + size - 16u);
                else if (routingType == 4 && size >= 24 &&
                         (uint32_t)packet[offset + 4] + 1u <= (size - 8u) / 16u &&
                         packet[offset + 3] <= (uint32_t)packet[offset + 4] + 1u)
                    plan->PseudoDestinationOffset = (uint16_t)(offset + 8u);
                else return PB_PACKET_UNSUPPORTED;
            } else if (protocol == 0 || protocol == 60) {
                for (uint32_t at = offset + 2u; at < offset + size;) {
                    uint8_t option = packet[at];
                    if (!option) { at++; continue; }
                    if (offset + size - at < 2) return PB_PACKET_INVALID;
                    uint32_t optionSize = (uint32_t)packet[at + 1] + 2u;
                    if (optionSize > offset + size - at) return PB_PACKET_INVALID;
                    if (option == 0xc2) return PB_PACKET_UNSUPPORTED; /* No IPv6 jumbograms. */
                    if (option == 0xc9) {
                        if (protocol != 60 || optionSize != 18) return PB_PACKET_INVALID;
                        plan->PseudoSourceOffset = (uint16_t)(at + 2u);
                    }
                    at += optionSize;
                }
            }
            protocol = packet[offset];
            offset += size;
            extensionBytes += size;
        }
        /* Fragmented, AH and ESP packets use the non-offloaded path. */
        if (protocol == 44 || protocol == 50 || protocol == 51) return PB_PACKET_UNSUPPORTED;
    } else return PB_PACKET_INVALID;

    if (protocol == 6) {
        uint32_t tcpLength;
        if (length - offset < 20 || type == PB_SEGMENT_UDP) return PB_PACKET_INVALID;
        tcpLength = (uint32_t)(packet[offset + 12] >> 4) * 4u;
        if (tcpLength < 20 || tcpLength > length - offset) return PB_PACKET_INVALID;
        if (type == PB_SEGMENT_TCP && (packet[offset + 13] & (0x20u | 0x04u | 0x02u)))
            return PB_PACKET_UNSUPPORTED;
        plan->TransportHeaderLength = (uint16_t)tcpLength;
    } else if (protocol == 17) {
        if (length - offset < 8 || type == PB_SEGMENT_TCP) return PB_PACKET_INVALID;
        if (type == PB_SEGMENT_UDP && plan->IpVersion == 6 && extensionBytes)
            return PB_PACKET_UNSUPPORTED;
        if (type == PB_SEGMENT_NONE && Read16(packet + offset + 4) != length - offset)
            return PB_PACKET_INVALID;
        plan->TransportHeaderLength = 8;
        plan->UdpChecksumDisabled = Read16(packet + offset + 6) == 0;
    } else return PB_PACKET_UNSUPPORTED;

    plan->SourceLength = length;
    plan->Protocol = protocol;
    plan->Type = (uint8_t)type;
    plan->TransportOffset = (uint16_t)offset;
    plan->HeaderLength = (uint16_t)(offset + plan->TransportHeaderLength);
    plan->PayloadLength = length - plan->HeaderLength;
    if (type == PB_SEGMENT_NONE) {
        plan->Mss = plan->PayloadLength;
        plan->SegmentCount = 1;
    } else {
        if (!mss || mss > 65535u - plan->HeaderLength || !plan->PayloadLength)
            return PB_PACKET_INVALID;
        plan->Mss = mss;
        plan->SegmentCount = 1u + (plan->PayloadLength - 1u) / mss;
    }
    return PB_PACKET_OK;
}

/* A plan is caller-writable metadata, not proof of the source span or its headers.
 * Use the parser as the single authority and retain its local result while writing.
 * Compare fields, not padding; batch validation is amortized across all outputs. */
static int ValidPlan(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan,
                     PB_SEGMENT_PLAN *validated)
{
    if (!plan) return 0;
    PB_SEGMENT_PLAN requested = *plan;
    if (requested.SourceLength != sourceLength) return 0;
    if (PbPlanSegments(source, sourceLength, (PB_SEGMENT_TYPE)requested.Type, requested.Mss, validated) != PB_PACKET_OK)
        return 0;
    return requested.SourceLength == validated->SourceLength && requested.PayloadLength == validated->PayloadLength &&
        requested.SegmentCount == validated->SegmentCount && requested.Mss == validated->Mss &&
        requested.HeaderLength == validated->HeaderLength && requested.TransportOffset == validated->TransportOffset &&
        requested.TransportHeaderLength == validated->TransportHeaderLength &&
        requested.PseudoSourceOffset == validated->PseudoSourceOffset && requested.PseudoDestinationOffset == validated->PseudoDestinationOffset &&
        requested.IpVersion == validated->IpVersion && requested.Protocol == validated->Protocol &&
        requested.Type == validated->Type && requested.UdpChecksumDisabled == validated->UdpChecksumDisabled;
}

typedef struct PB_PACKET_RANGE { uintptr_t Begin, End; } PB_PACKET_RANGE;
typedef struct PB_WRITE_REQUEST {
    PB_SEGMENT_PLAN Plan;
    PB_SEGMENT_OUTPUT Outputs[PB_PACKET_MAX_BATCH];
    uint32_t Count;
} PB_WRITE_REQUEST;

static int PacketRange(const void *pointer, uint32_t length, PB_PACKET_RANGE *range)
{
    uintptr_t start = (uintptr_t)pointer;
    if (!pointer || (uintptr_t)length > ~(uintptr_t)0 - start) return 0;
    range->Begin = start; range->End = start + length; return 1;
}
static int PacketOverlap(PB_PACKET_RANGE a, PB_PACKET_RANGE b)
{
    return a.Begin != a.End && b.Begin != b.End && a.Begin < b.End && b.Begin < a.End;
}

/* Snapshot and validate the whole request before parsing or writing. No caller
 * output can overwrite source, plan, output metadata, count, or another output.
 * Sort at most 32 nonempty capacities, then check neighbors. Already ordered
 * output buffers need linear work; worst-case insertion is bounded, no allocation. */
static PB_PACKET_STATUS PrepareWrite(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan,
    uint32_t first, PB_SEGMENT_OUTPUT *outputs, uint32_t count, uint32_t *written, PB_WRITE_REQUEST *request)
{
    if (!written) return PB_PACKET_INVALID;
    if (!source || !plan || !outputs || !count || count > PB_PACKET_MAX_BATCH) return PB_PACKET_INVALID;
    PB_PACKET_RANGE control[4], destinations[PB_PACKET_MAX_BATCH];
    if (!PacketRange(source, sourceLength, &control[0]) || !PacketRange(plan, sizeof(*plan), &control[1]) ||
        !PacketRange(outputs, count * (uint32_t)sizeof(*outputs), &control[2]) || !PacketRange(written, sizeof(*written), &control[3]))
        return PB_PACKET_INVALID;
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < i; ++j) if (PacketOverlap(control[i], control[j])) return PB_PACKET_INVALID;
    memcpy(request->Outputs, outputs, count * sizeof(*outputs));
    uint32_t destinationCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        PB_PACKET_RANGE range;
        if (!PacketRange(request->Outputs[i].Data, request->Outputs[i].Capacity, &range)) return PB_PACKET_INVALID;
        for (uint32_t j = 0; j < 4; ++j) if (PacketOverlap(range, control[j])) return PB_PACKET_INVALID;
        if (range.Begin != range.End) destinations[destinationCount++] = range;
    }
    for (uint32_t i = 1; i < destinationCount; ++i) {
        PB_PACKET_RANGE range = destinations[i]; uint32_t at = i;
        while (at && destinations[at - 1].Begin > range.Begin) { destinations[at] = destinations[at - 1]; --at; }
        destinations[at] = range;
    }
    for (uint32_t i = 1; i < destinationCount; ++i)
        if (destinations[i - 1].End > destinations[i].Begin) return PB_PACKET_INVALID;
    *written = 0;
    if (!ValidPlan(source, sourceLength, plan, &request->Plan) || first >= request->Plan.SegmentCount) return PB_PACKET_INVALID;
    request->Count = count < request->Plan.SegmentCount - first ? count : request->Plan.SegmentCount - first;
    return PB_PACKET_OK;
}

static PB_PACKET_STATUS WriteValidated(const uint8_t *source, const PB_SEGMENT_PLAN *plan, uint32_t index,
                                      uint8_t *destination, uint32_t capacity, uint32_t *written)
{
    uint32_t payloadOffset, payloadLength, totalLength, transportLength;
    uint8_t *transport;
    uint16_t checksum;
    *written = 0;
    /* The parser and checked index bound index*MSS by PayloadLength <= 65536. */
    payloadOffset = plan->Type == PB_SEGMENT_NONE ? 0 : index * plan->Mss;
    if (payloadOffset > plan->PayloadLength) return PB_PACKET_INVALID;
    payloadLength = plan->PayloadLength - payloadOffset;
    if (plan->Type != PB_SEGMENT_NONE && payloadLength > plan->Mss) payloadLength = plan->Mss;
    totalLength = plan->HeaderLength + payloadLength;
    if (totalLength > capacity || totalLength > 65535u) return PB_PACKET_NO_SPACE;
    memcpy(destination, source, plan->HeaderLength);
    memcpy(destination + plan->HeaderLength, source + plan->HeaderLength + payloadOffset, payloadLength);
    if (plan->IpVersion == 4) {
        uint32_t idMask = plan->Type == PB_SEGMENT_TCP ? 0x7fffu : 0xffffu;
        Write16(destination + 2, (uint16_t)totalLength);
        if (plan->Type != PB_SEGMENT_NONE)
            Write16(destination + 4, (uint16_t)((Read16(source + 4) + index) & idMask));
        Write16(destination + 10, 0);
        Write16(destination + 10, PbChecksumFinish(PbChecksumAdd(0, destination, plan->TransportOffset)));
    } else Write16(destination + 4, (uint16_t)(totalLength - 40));
    transport = destination + plan->TransportOffset;
    transportLength = plan->TransportHeaderLength + payloadLength;
    if (plan->Protocol == 6) {
        Write32(transport + 4, Read32(source + plan->TransportOffset + 4) + payloadOffset);
        if (index + 1 < plan->SegmentCount) transport[13] &= 0xf6u;
        if (index) transport[13] &= 0x7fu;
        Write16(transport + 16, 0);
        checksum = PbChecksumFinish(PbChecksumAdd(PseudoHeader(destination, plan, transportLength), transport, transportLength));
        Write16(transport + 16, checksum);
    } else {
        Write16(transport + 4, (uint16_t)transportLength);
        Write16(transport + 6, 0);
        if (!plan->UdpChecksumDisabled) {
            checksum = PbChecksumFinish(PbChecksumAdd(PseudoHeader(destination, plan, transportLength), transport, transportLength));
            Write16(transport + 6, checksum ? checksum : 0xffffu);
        }
    }
    *written = totalLength;
    return PB_PACKET_OK;
}

PB_PACKET_STATUS PbWriteSegmentChecked(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan, uint32_t index,
                               uint8_t *destination, uint32_t capacity, uint32_t *written)
{
    PB_SEGMENT_OUTPUT output = {destination, capacity, 0}; PB_WRITE_REQUEST request;
    PB_PACKET_STATUS result = PrepareWrite(source, sourceLength, plan, index, &output, 1, written, &request);
    if (result != PB_PACKET_OK) return result;
    return WriteValidated(source, &request.Plan, index, request.Outputs[0].Data, request.Outputs[0].Capacity, written);
}

PB_PACKET_STATUS PbFinalizeChecksumsWithOffset(uint8_t *packet, uint32_t length, int ipChecksum, int tcpChecksum,
                                              int udpChecksum, uint32_t *transportOffset)
{
    PB_SEGMENT_PLAN plan;
    if (ipChecksum && !tcpChecksum && !udpChecksum) {
        uint32_t headerLength;
        if (!packet || length < 20 || length > 65535u || packet[0] >> 4 != 4)
            return PB_PACKET_INVALID;
        headerLength = (uint32_t)(packet[0] & 15u) * 4u;
        if (headerLength < 20 || headerLength > length || Read16(packet + 2) != length)
            return PB_PACKET_INVALID;
        Write16(packet + 10, 0);
        Write16(packet + 10, PbChecksumFinish(PbChecksumAdd(0, packet, headerLength)));
        if (transportOffset) *transportOffset = 0;
        return PB_PACKET_OK;
    }
    PB_PACKET_STATUS status = PbPlanSegments(packet, length, PB_SEGMENT_NONE, 0, &plan);
    uint32_t transportLength;
    uint16_t checksum;
    if (status != PB_PACKET_OK) return status;
    if ((tcpChecksum && plan.Protocol != 6) || (udpChecksum && plan.Protocol != 17))
        return PB_PACKET_INVALID;
    if (ipChecksum && plan.IpVersion == 4) {
        Write16(packet + 10, 0);
        Write16(packet + 10, PbChecksumFinish(PbChecksumAdd(0, packet, plan.TransportOffset)));
    }
    transportLength = length - plan.TransportOffset;
    if (tcpChecksum || udpChecksum) {
        uint32_t checksumOffset = plan.TransportOffset + (tcpChecksum ? 16u : 6u);
        Write16(packet + checksumOffset, 0);
        checksum = PbChecksumFinish(PbChecksumAdd(PseudoHeader(packet, &plan, transportLength), packet + plan.TransportOffset, transportLength));
        Write16(packet + checksumOffset, udpChecksum && !checksum ? 0xffffu : checksum);
    }
    if (transportOffset) *transportOffset = plan.TransportOffset;
    return PB_PACKET_OK;
}

PB_PACKET_STATUS PbFinalizeChecksums(uint8_t *packet, uint32_t length, int ipChecksum, int tcpChecksum,
                                    int udpChecksum)
{
    return PbFinalizeChecksumsWithOffset(packet, length, ipChecksum, tcpChecksum, udpChecksum, NULL);
}

PB_PACKET_STATUS PbWriteSegmentBatchChecked(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan, uint32_t first,
    PB_SEGMENT_OUTPUT *outputs, uint32_t count, uint32_t *written)
{
    PB_WRITE_REQUEST request;
    PB_PACKET_STATUS result = PrepareWrite(source, sourceLength, plan, first, outputs, count, written, &request);
    if (result != PB_PACKET_OK) return result;
    for (uint32_t i = 0; i < request.Count; ++i) {
        uint32_t length;
        result = WriteValidated(source, &request.Plan, first + i, request.Outputs[i].Data, request.Outputs[i].Capacity, &length);
        if (result != PB_PACKET_OK) return result;
        outputs[i].Length = length; (*written)++;
    }
    return PB_PACKET_OK;
}

/* Retired ABI slots never inspect source, plan, destination, or batch metadata.
 * A wrapper that invents a readable span from plan->SourceLength recreates the
 * original defect. Old binaries get an explicit migration failure instead. */
PB_PACKET_STATUS PbWriteSegment(const uint8_t *source, const PB_SEGMENT_PLAN *plan, uint32_t index,
                               uint8_t *destination, uint32_t capacity, uint32_t *written)
{
    (void)source; (void)plan; (void)index; (void)destination; (void)capacity;
    if (written) *written = 0;
    return PB_PACKET_UNSUPPORTED;
}

PB_PACKET_STATUS PbWriteSegmentBatch(const uint8_t *source, const PB_SEGMENT_PLAN *plan, uint32_t first,
    PB_SEGMENT_OUTPUT *outputs, uint32_t count, uint32_t *written)
{
    (void)source; (void)plan; (void)first; (void)outputs; (void)count;
    if (written) *written = 0;
    return PB_PACKET_UNSUPPORTED;
}
