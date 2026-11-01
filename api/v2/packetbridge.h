/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <windows.h>
#include "../../common/v2_core.h"
#include "../../common/packet.h"
#include "../../common/flow.h"
#include "../../include/packetbridge_probe.h"

typedef struct PB_SESSION PB_SESSION;
typedef struct PB_USER_QUEUE PB_USER_QUEUE;
typedef struct PB_ADAPTER PB_ADAPTER;
#define PB_INTERFACE_V2_MAJOR 2u
#define PB_INTERFACE_V2_MINOR 1u
typedef struct PB_CONFIG {
    uint32_t Size, QueueCount, SlotCount, RingCapacity, Offloads, Flags;
} PB_CONFIG;
/* Offloads remain opt-in; queue workers should use PB_MAX_BATCH objects/call. */
#define PB_CONFIG_INIT { sizeof(PB_CONFIG), PB_DEFAULT_QUEUES, PB_DEFAULT_SLOTS, PB_DEFAULT_RING_CAPACITY, 0, 0 }
#define PB_OPEN_EXPERIMENTAL_OFFLOADS 1u
/* Acknowledge successful packet extension 1.1 negotiation before enabling offloads. */
#define PB_OPEN_CHECKED_SEGMENTS 2u
#define PB_OPEN_KNOWN_FLAGS (PB_OPEN_EXPERIMENTAL_OFFLOADS | PB_OPEN_CHECKED_SEGMENTS)
typedef struct PB_LEASE {
    PB_DESCRIPTOR Descriptor;
    uint8_t *Data;
    uint32_t Capacity;
    uint32_t Reserved;
} PB_LEASE;

/* Queue handles have one thread owner. Control calls are serialized separately.
 * Drain/PrepareClose may stop active workers. Before final Close, the caller must stop/join all
 * queue owners and other control callers; no call may enter using a handle being destroyed.
 * Successful Close invalidates the session and every queue/lease pointer. Keep the DLL loaded
 * until all public calls are joined and handles consumed. The native runtime owns independent
 * module references for deferred cleanup. Close/PrepareClose timeout covers caller waiting;
 * timeout=0 does not synchronously drain or destroy. Non-success keeps the public handle valid.
 * Lifecycle extension ReleaseSession explicitly transfers that handle to native ownership.
 * The Rust wrapper enforces queue/lease lifetimes with borrows. */
typedef struct PB_INTERFACE_V2 {
    uint32_t Size, Major, Minor, FileVersion;
    char SourceFingerprint[65]; uint8_t Reserved[7];
    PB_RESULT (WINAPI *Open)(const wchar_t *, const PB_CONFIG *, PB_SESSION **);
    PB_RESULT (WINAPI *Close)(PB_SESSION *, uint32_t);
    PB_RESULT (WINAPI *OpenQueue)(PB_SESSION *, uint32_t, PB_USER_QUEUE **);
    PB_RESULT (WINAPI *ReceiveBatch)(PB_USER_QUEUE *, PB_LEASE *, uint32_t, uint32_t *);
    PB_RESULT (WINAPI *ReleaseBatch)(PB_USER_QUEUE *, const PB_TOKEN *, uint32_t, uint32_t *);
    PB_RESULT (WINAPI *ReserveBatch)(PB_USER_QUEUE *, PB_LEASE *, uint32_t, uint32_t *);
    PB_RESULT (WINAPI *CancelBatch)(PB_USER_QUEUE *, const PB_TOKEN *, uint32_t);
    PB_RESULT (WINAPI *CommitBatch)(PB_USER_QUEUE *, const PB_LEASE *, uint32_t, uint32_t *);
    /* Successful poll retires validated tokens. Descriptor status PB_RX_REJECTED
     * (legacy wire value 4) reports packet rejection; an API PB_CORRUPT is fatal. */
    PB_RESULT (WINAPI *PollCompletions)(PB_USER_QUEUE *, PB_DESCRIPTOR *, uint32_t, uint32_t *);
    PB_RESULT (WINAPI *Wait)(PB_USER_QUEUE *, uint32_t);
    PB_RESULT (WINAPI *GetStatus)(PB_SESSION *, PB_V2_STATUS *);
    PB_RESULT (WINAPI *SetPolicy)(PB_SESSION *, uint32_t);
    PB_PACKET_STATUS (*PlanSegments)(const uint8_t *, uint32_t, PB_SEGMENT_TYPE, uint32_t, PB_SEGMENT_PLAN *);
    /* Retired slots return PB_PACKET_UNSUPPORTED; use PB_PACKET_INTERFACE. */
    PB_PACKET_STATUS (*WriteSegment)(const uint8_t *, const PB_SEGMENT_PLAN *, uint32_t, uint8_t *, uint32_t, uint32_t *);
    PB_PACKET_STATUS (*WriteSegmentBatch)(const uint8_t *, const PB_SEGMENT_PLAN *, uint32_t, PB_SEGMENT_OUTPUT *, uint32_t, uint32_t *);
    uint32_t (*SelectQueue)(const uint8_t *, uint32_t, uint32_t);
    PB_RESULT (WINAPI *CreateAdapter)(const wchar_t *, const wchar_t *, PB_ADAPTER **);
    void (WINAPI *CloseAdapter)(PB_ADAPTER *);
    PB_RESULT (WINAPI *StartAdapterSession)(PB_ADAPTER *, const PB_CONFIG *, PB_SESSION **);
    PB_RESULT (WINAPI *GetAdapterLuid)(PB_ADAPTER *, uint64_t *);
    PB_RESULT (WINAPI *Drain)(PB_SESSION *);
    PB_RESULT (WINAPI *Wake)(PB_SESSION *, uint32_t);
    PB_RESULT (WINAPI *GetCapabilities)(PB_SESSION *, PB_PROBE_STATUS *);
    PB_RESULT (WINAPI *PrepareClose)(PB_SESSION *, uint32_t);
} PB_INTERFACE_V2;
PB_RESULT WINAPI PbGetInterfaceV2(uint32_t major, uint32_t size, PB_INTERFACE_V2 *table);

/* Additive extension; the V2 table's legacy writer signatures stay unchanged. */
#define PB_PACKET_ABI_MAJOR 1u
#define PB_PACKET_ABI_MINOR 1u
#define PB_PACKET_CAP_CHECKED_SOURCE 1u
#define PB_PACKET_CAP_RETIRED_UNSIZED 2u
#define PB_PACKET_CAP_DISJOINT_OUTPUTS 4u
#define PB_PACKET_REQUIRED_CAPS (PB_PACKET_CAP_CHECKED_SOURCE | PB_PACKET_CAP_RETIRED_UNSIZED | PB_PACKET_CAP_DISJOINT_OUTPUTS)
typedef struct PB_PACKET_INTERFACE {
    uint32_t Size, Major, Minor, Capabilities;
    PB_PACKET_STATUS (*WriteSegment)(const uint8_t *, uint32_t, const PB_SEGMENT_PLAN *, uint32_t, uint8_t *, uint32_t, uint32_t *);
    PB_PACKET_STATUS (*WriteSegmentBatch)(const uint8_t *, uint32_t, const PB_SEGMENT_PLAN *, uint32_t, PB_SEGMENT_OUTPUT *, uint32_t, uint32_t *);
} PB_PACKET_INTERFACE;
PB_RESULT WINAPI PbGetPacketInterface(uint32_t major, uint32_t size, PB_PACKET_INTERFACE *table);
