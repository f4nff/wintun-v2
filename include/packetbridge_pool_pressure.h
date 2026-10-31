/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge_send_diagnostics.h"

/* Independent read-only observations; existing send/probe/data ABIs stay intact. */
#define PB_POOL_PRESSURE_IOCTL ((0x8337u << 16) | (3u << 14) | (0x804u << 2))
#define PB_POOL_PRESSURE_MAGIC 0x50544250u
#define PB_POOL_PRESSURE_SCHEMA 1u
#define PB_POOL_PRESSURE_CAPACITY 64u
#define PB_POOL_PRESSURE_STRIDE 64u
#define PB_POOL_PRESSURE_CLASSES 6u
#define PB_POOL_FULL_PROVEN 1u
#define PB_POOL_RETURN_VALID 2u
#define PB_POOL_READY_VALID 4u
#define PB_POOL_COUNTS_VALID 8u
#define PB_POOL_CURSOR_FLAGS (PB_POOL_FULL_PROVEN | PB_POOL_RETURN_VALID | PB_POOL_READY_VALID)
#define PB_POOL_ALL_FLAGS (PB_POOL_CURSOR_FLAGS | PB_POOL_COUNTS_VALID)

typedef struct PB_POOL_PRESSURE_EVENT {
    uint64_t Sequence;
    PB_SEND_EVENT Send;
    uint64_t ReadyProducer, ReadyConsumer, ReturnProducer, ReturnConsumer;
    uint32_t Slots, Pending, Outstanding, ReadyUnread, UserHeld, ReturnQueued, Unaccounted, Flags;
    uint32_t ReadyStatus, ReturnStatus;
} PB_POOL_PRESSURE_EVENT;

typedef struct PB_TX_POOL_STATE {
    uint64_t Epoch, ReadyProducer, ReadyConsumer, ReturnProducer, ReturnConsumer;
    uint32_t Queue, Slots, PoolSlots, Free, Reserved, Published, Retired, Other;
    uint32_t PoolExhausted, CountsValid, CursorFlags, ReadyStatus, ReturnStatus;
    uint32_t Running, Draining, Stopping, Faulted;
} PB_TX_POOL_STATE;

typedef struct PB_POOL_PRESSURE_SNAPSHOT {
    uint32_t Size, Magic, Schema, FileVersion, RecordCount, RecordCapacity, SampleStride, Reserved;
    uint64_t LatestSequence, PoolFullEvents, SampledEvents;
    /* Selected observations containing pending/ready/user/return/unaccounted/unknown.
     * Categories overlap; deterministic samples must not be extrapolated. */
    uint64_t Classes[PB_POOL_PRESSURE_CLASSES];
    char SourceFingerprint[65];
    uint8_t Padding[7];
    PB_POOL_PRESSURE_EVENT Records[PB_POOL_PRESSURE_CAPACITY];
    /* CurrentStatus reports observation bounds only; it never faults a session. */
    uint32_t CurrentQueueCount, CurrentStatus;
    PB_TX_POOL_STATE Current[PB_MAX_QUEUES];
} PB_POOL_PRESSURE_SNAPSHOT;
