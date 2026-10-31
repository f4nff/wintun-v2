/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_types.h"

typedef enum PB_PACKET_STATUS {
    PB_PACKET_OK = 0,
    PB_PACKET_INVALID,
    PB_PACKET_UNSUPPORTED,
    PB_PACKET_NO_SPACE
} PB_PACKET_STATUS;

typedef enum PB_SEGMENT_TYPE {
    PB_SEGMENT_NONE = 0,
    PB_SEGMENT_TCP = 1,
    PB_SEGMENT_UDP = 2
} PB_SEGMENT_TYPE;

typedef struct PB_SEGMENT_PLAN {
    uint32_t SourceLength;
    uint32_t PayloadLength;
    uint32_t SegmentCount;
    uint32_t Mss;
    uint16_t HeaderLength;
    uint16_t TransportOffset;
    uint16_t TransportHeaderLength;
    uint16_t PseudoSourceOffset;
    uint16_t PseudoDestinationOffset;
    uint8_t IpVersion;
    uint8_t Protocol;
    uint8_t Type;
    uint8_t UdpChecksumDisabled;
} PB_SEGMENT_PLAN;
typedef struct PB_SEGMENT_OUTPUT { uint8_t *Data; uint32_t Capacity, Length; } PB_SEGMENT_OUTPUT;
#define PB_PACKET_MAX_BATCH 32u

PB_PACKET_STATUS PbPlanSegments(const uint8_t *packet, uint32_t length, PB_SEGMENT_TYPE type,
                               uint32_t mss, PB_SEGMENT_PLAN *plan);
/* sourceLength is the actual caller-owned readable span, independent of plan.
 * Source, plan, output metadata, written and every declared output capacity must
 * be disjoint and pointer ranges must not wrap. Invalid arguments/ranges/overlap return
 * INVALID without writes, including to written. Otherwise written is reset to
 * zero before packet/plan validation. Batch NO_SPACE preserves a successful prefix.
 * Zero output capacity returns NO_SPACE. Keep all inputs stable during the call;
 * this API does not probe arbitrary pointers or synchronize concurrent mutation. */
PB_PACKET_STATUS PbWriteSegmentChecked(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan, uint32_t index,
                               uint8_t *destination, uint32_t capacity, uint32_t *written);
PB_PACKET_STATUS PbFinalizeChecksums(uint8_t *packet, uint32_t length, int ipChecksum, int tcpChecksum,
                                    int udpChecksum);
/* Same checksum validation and writes; optionally returns this call's checked
 * transport offset on success only. IPv4 header-only checksum returns zero
 * because that path does not parse L4. A non-NULL output must be distinct,
 * writable uint32_t storage. Keep packet stable for the supplied length. */
PB_PACKET_STATUS PbFinalizeChecksumsWithOffset(uint8_t *packet, uint32_t length, int ipChecksum, int tcpChecksum,
                                              int udpChecksum, uint32_t *transportOffset);
PB_PACKET_STATUS PbWriteSegmentBatchChecked(const uint8_t *source, uint32_t sourceLength, const PB_SEGMENT_PLAN *plan, uint32_t first,
    PB_SEGMENT_OUTPUT *outputs, uint32_t count, uint32_t *written);
/* Retired ABI slots: return UNSUPPORTED and clear written (if non-NULL).
 * Source, plan and output metadata are never dereferenced. Use Checked variants. */
PB_PACKET_STATUS PbWriteSegment(const uint8_t *source, const PB_SEGMENT_PLAN *plan, uint32_t index,
                               uint8_t *destination, uint32_t capacity, uint32_t *written);
PB_PACKET_STATUS PbWriteSegmentBatch(const uint8_t *source, const PB_SEGMENT_PLAN *plan, uint32_t first,
    PB_SEGMENT_OUTPUT *outputs, uint32_t count, uint32_t *written);
uint32_t PbChecksumAdd(uint32_t sum, const uint8_t *bytes, uint32_t length);
uint16_t PbChecksumFinish(uint32_t sum);
