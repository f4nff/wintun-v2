/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge_types.h"

#define PB_ABI_MAGIC 0x32514250u
#define PB_ABI_MAJOR 2u
#define PB_ABI_MINOR 1u
#define PB_MAX_QUEUES 8u
#define PB_MAX_BATCH 64u
#define PB_MAX_REGION (256u * 1024u * 1024u)
#define PB_DEFAULT_QUEUES 4u
#define PB_DEFAULT_SLOTS 256u
#define PB_DEFAULT_RING_CAPACITY 512u
#define PB_RX_BYTE_BUDGET (512u * 1024u)
#define PB_TX_BYTE_BUDGET (128u * 1024u)
#define PB_BLOCK_SIZE 65536u
#define PB_MAX_SLOTS 512u
#define PB_QUEUE_TX_READY 0u
#define PB_QUEUE_TX_RECYCLE 1u
#define PB_QUEUE_RX_SUBMIT 2u
#define PB_QUEUE_RX_COMPLETE 3u
#define PB_QUEUE_COUNT 4u
#define PB_DESC_PLAIN 0u
#define PB_DESC_TCP_GSO 1u
#define PB_DESC_UDP_GSO 2u
#define PB_CSUM_IPV4 1u
#define PB_CSUM_TCP 2u
#define PB_CSUM_UDP 4u
#define PB_UDP_NO_CHECKSUM 8u

typedef struct PB_TOKEN {
    uint64_t Epoch;
    uint32_t Generation;
    uint16_t Slot;
    uint16_t Queue;
} PB_TOKEN;

typedef struct PB_DESCRIPTOR {
    PB_TOKEN Token;
    uint32_t Type;
    uint32_t Length;
    uint32_t DataOffset;
    uint32_t Flags;
    uint32_t Mss;
    uint16_t L4Offset;
    uint16_t HeaderLength;
    uint8_t IpVersion;
    uint8_t Protocol;
    uint16_t Reserved16;
    uint32_t Status;
    uint64_t Reserved64[2];
} PB_DESCRIPTOR;

/* Each counter occupies its own cache line. The owning side keeps a private cursor. */
typedef struct PB_QUEUE_SHARED {
    volatile uint64_t Producer;
    uint8_t ProducerPadding[56];
    volatile uint64_t Consumer;
    volatile uint32_t Armed;
    uint8_t ConsumerPadding[52];
} PB_QUEUE_SHARED;

typedef struct PB_QUEUE_LAYOUT {
    uint32_t Rings[PB_QUEUE_COUNT];
    uint32_t TxData;
    uint32_t RxData;
    uint32_t Reserved[2];
} PB_QUEUE_LAYOUT;

/* v2.1: explicit kernel-published hints, never authority to reclaim resources. */
typedef struct PB_SHARED_LIFECYCLE {
    volatile uint64_t Sequence;
    volatile uint32_t Terminal;
    uint32_t Reserved;
} PB_SHARED_LIFECYCLE;

/* Layout is calculated in private memory. The driver never trusts the shared copy. */
typedef struct PB_LAYOUT {
    uint32_t Size;
    uint32_t Magic;
    uint16_t Major;
    uint16_t Minor;
    uint32_t QueueCount;
    uint32_t SlotCount;
    uint32_t RingCapacity;
    uint32_t BlockSize;
    uint32_t RegionSize;
    uint32_t LifecycleOffset;
    uint32_t LifecycleSize;
    PB_QUEUE_LAYOUT Queues[PB_MAX_QUEUES];
} PB_LAYOUT;

#define PB_V2_IOCTL_REGISTER ((0x8337u << 16) | (3u << 14) | (0x810u << 2))
/* DRAIN captures each RX submission boundary. Later submissions are not admitted
 * and cannot extend drain. STOPPED still does not replace the device-close barrier. */
#define PB_V2_IOCTL_DRAIN ((0x8337u << 16) | (3u << 14) | (0x811u << 2))
#define PB_V2_IOCTL_QUERY ((0x8337u << 16) | (3u << 14) | (0x812u << 2))

typedef struct PB_REGISTER_V2 {
    uint32_t Size;
    uint16_t Major;
    uint16_t Minor;
    uint64_t Epoch;
    uint64_t RegionAddress;
    uint64_t KernelEvents[PB_MAX_QUEUES];
    uint64_t UserEvents[PB_MAX_QUEUES];
    uint32_t QueueCount;
    uint32_t SlotCount;
    uint32_t RingCapacity;
    uint32_t RegionSize;
    uint64_t ProgressEvent;
    char SourceFingerprint[65];
    uint8_t Reserved[7];
} PB_REGISTER_V2;

typedef struct PB_V2_STATUS {
    uint32_t Size;
    uint32_t State;
    uint32_t QueueCount;
    /* Native lifetime references: indicated NBLs and admitted send calls/upcalls. */
    uint32_t ActiveReferences;
    uint64_t Epoch;
    uint64_t TxObjects;
    uint64_t RxObjects;
    uint64_t RxReturned;
    uint64_t Indications;
    uint64_t Completions;
} PB_V2_STATUS;
