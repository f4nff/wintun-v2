/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge_v2_abi.h"

/* Independent read-only ABI; no change to the probe, RX-return or data ABI. */
#define PB_SEND_DIAG_IOCTL ((0x8337u << 16) | (3u << 14) | (0x803u << 2))
#define PB_SEND_DIAG_MAGIC 0x53544250u
#define PB_SEND_DIAG_SCHEMA 1u
#define PB_SEND_DIAG_CAPACITY 64u
#define PB_SEND_REASON_COUNT 32u
#define PB_SEND_QUEUE_UNKNOWN 0xffffffffu
enum PB_SEND_REASON {
    PB_SEND_UNKNOWN, PB_SEND_ADMISSION, PB_SEND_EMPTY_NBL, PB_SEND_PREFIX_READ,
    PB_SEND_CONTROL_READ, PB_SEND_CONTROL_REPLY, PB_SEND_RECYCLE,
    PB_SEND_QUEUE_RING, PB_SEND_QUEUE_FULL, PB_SEND_POOL_FULL, PB_SEND_POOL_ERROR,
    PB_SEND_PUBLICATION, PB_SEND_LENGTH, PB_SEND_PAYLOAD_READ,
    PB_SEND_OFFLOAD_COMBINATION, PB_SEND_GSO_PLAN, PB_SEND_GSO_OFFSET,
    PB_SEND_GSO_FAMILY, PB_SEND_GSO_COUNT, PB_SEND_GSO_PAYLOAD,
    PB_SEND_IP_LENGTH, PB_SEND_CHECKSUM_FAMILY, PB_SEND_CHECKSUM_DATA,
    PB_SEND_CHECKSUM_OFFSET, PB_SEND_POLICY, PB_SEND_PRIOR_FAILURE
};
typedef struct PB_SEND_FAILURE { uint32_t Reason, PacketStatus, CoreStatus, RequiredMask; } PB_SEND_FAILURE;
typedef struct PB_SEND_EVENT {
    uint64_t Sequence, Epoch, LsoInfo, UsoInfo, ChecksumInfo;
    uint32_t NdisStatus, PriorNdisStatus, Reason, Queue;
    uint32_t NetBufferLength, EthernetLength, PacketStatus, CoreStatus;
    uint32_t RequiredMask, EffectiveMask, AcceptedCount, AttemptedCount;
    uint32_t NetBufferScope, Reserved;
} PB_SEND_EVENT;
typedef struct PB_SEND_SNAPSHOT {
    uint32_t Size, Magic, Schema, FileVersion, RecordCount, RecordCapacity;
    uint64_t LatestSequence, RejectionEvents;
    char SourceFingerprint[65];
    uint8_t Padding[23];
    uint64_t Reasons[PB_SEND_REASON_COUNT];
    PB_SEND_EVENT Records[PB_SEND_DIAG_CAPACITY];
} PB_SEND_SNAPSHOT;
