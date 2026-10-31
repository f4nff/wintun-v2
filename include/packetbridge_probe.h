/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge_types.h"

#define PB_PROBE_MAGIC 0x50425052u
#define PB_PROBE_SCHEMA 2u
#define PB_PROBE_IOCTL_QUERY ((0x8337u << 16) | (3u << 14) | (0x800u << 2))
#define PB_PROBE_IOCTL_POLICY ((0x8337u << 16) | (3u << 14) | (0x801u << 2))
#define PB_TX_LSO4 0x001u
#define PB_TX_LSO6 0x002u
#define PB_TX_USO4 0x004u
#define PB_TX_USO6 0x008u
#define PB_TX_IP4_CHECKSUM 0x010u
#define PB_TX_TCP4_CHECKSUM 0x020u
#define PB_TX_UDP4_CHECKSUM 0x040u
#define PB_TX_TCP6_CHECKSUM 0x080u
#define PB_TX_UDP6_CHECKSUM 0x100u
#define PB_PROBE_TX_SUPPORTED 0x1ffu
#define PB_RX_RSC4 0x00200u
#define PB_RX_RSC6 0x00400u
#define PB_RX_URO 0x00800u
#define PB_RX_IP4_CHECKSUM 0x01000u
#define PB_RX_TCP4_CHECKSUM 0x02000u
#define PB_RX_UDP4_CHECKSUM 0x04000u
#define PB_RX_TCP6_CHECKSUM 0x08000u
#define PB_RX_UDP6_CHECKSUM 0x10000u
#define PB_PROBE_SUPPORTED 0x1ffffu
#define PB_PROBE_RX_SUPPORTED (PB_PROBE_SUPPORTED & ~PB_PROBE_TX_SUPPORTED)

typedef struct PB_PROBE_POLICY {
    uint32_t Size;
    uint32_t Schema;
    uint32_t AllowedMask;
    uint32_t Reserved;
} PB_PROBE_POLICY;

typedef struct PB_PROBE_STATUS {
    uint32_t Size;
    uint32_t Magic;
    uint32_t Schema;
    uint32_t FileVersion;
    uint32_t NdisVersion;
    uint32_t Medium;
    uint32_t DataAbi;
    uint32_t ImplementedMask;
    uint32_t AllowedMask;
    uint32_t NdisMask;
    uint32_t EffectiveMask;
    uint32_t SessionRegistered;
    uint32_t LastIpv4Encapsulation;
    uint32_t LastIpv6Encapsulation;
    uint32_t RxOffloadsImplemented;
    uint32_t OffloadRegistrationStatus;
    uint64_t LsoNbls;
    uint64_t UsoNbls;
    uint64_t ChecksumNbls;
    uint64_t OffloadRejected;
    uint64_t SegmentsCopied;
    uint64_t OffloadParameterChanges;
    uint64_t RscPackets, RscBytes, RscEvents, RscAborts, UroPackets, UroEvents;
    char SourceFingerprint[65];
    char Padding[7];
} PB_PROBE_STATUS;
