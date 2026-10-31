/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge_v2_abi.h"

/* Independent read-only diagnostic ABI. Existing probe/data layouts stay unchanged. */
#define PB_RETURN_DIAG_IOCTL ((0x8337u << 16) | (3u << 14) | (0x802u << 2))
#define PB_RETURN_DIAG_MAGIC 0x52524250u
#define PB_RETURN_DIAG_SCHEMA 1u
#define PB_RETURN_DIAG_CAPACITY 64u
#define PB_RETURN_ORIGIN_NDIS 1u
#define PB_RETURN_ORIGIN_LOCAL_CANCEL 2u

typedef struct PB_RETURN_EVENT {
    uint64_t Sequence;
    PB_TOKEN FirstToken;
    uint32_t NblStatus;
    uint32_t CompletionStatus;
    uint32_t Origin;
    uint32_t OriginalCount;
    uint32_t OriginalBytes;
    uint32_t IndicationLength;
    uint32_t IpVersion;
    uint32_t Protocol;
    uint32_t ReceiveType;
    uint32_t IpDeclaredLength;
} PB_RETURN_EVENT;

typedef struct PB_RETURN_SNAPSHOT {
    uint32_t Size, Magic, Schema, FileVersion;
    uint32_t RecordCount, RecordCapacity;
    uint64_t LatestSequence;
    uint64_t NdisNonzeroNbls, NdisNonzeroPackets, LocalCancelledPackets;
    char SourceFingerprint[65];
    uint8_t Padding[7];
    PB_RETURN_EVENT Records[PB_RETURN_DIAG_CAPACITY];
} PB_RETURN_SNAPSHOT;
