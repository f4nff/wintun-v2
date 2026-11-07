/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../api/v2/lifecycle.h"
/* Host-owned gates and thread handles outlive every fixture callback. This is
 * test-only IPC within one process, not a published PacketBridge interface. */
enum { CH_IDLE_ENTERED, CH_IDLE_GATE, CH_POLICY_ENTERED, CH_POLICY_GATE,
    CH_FINAL_ENTERED, CH_FINAL_GATE, CH_DETACHED, CH_EVENT_COUNT };
#define CH_MAX_CALLBACKS 8u
typedef struct CH_CONFIG {
    HANDLE Events[CH_EVENT_COUNT];
    HANDLE Threads[CH_MAX_CALLBACKS];
    DWORD ThreadIds[CH_MAX_CALLBACKS];
} CH_CONFIG;
typedef struct CH_SNAPSHOT {
    LONG References, Active, Started, Finished, Peak;
    LONG Allocations, Regions, Events, Works, Pools, Pins, TailTransfers;
    uint32_t Sessions, Phase;
    uint64_t Bytes, Requested, Completed;
    BOOL Registered, Scheduled, Occupied, Queued, Abandoned, Closed;
    BOOL ModulePresent, TailPresent, IdleHeld, FinalHeld;
} CH_SNAPSHOT;
typedef VOID (WINAPI *CH_CONFIGURE)(PB_SESSION *,CH_CONFIG *);
typedef VOID (WINAPI *CH_OBSERVE)(CH_SNAPSHOT *);
typedef VOID (WINAPI *CH_FAIL_WAIT)(VOID);
typedef VOID (WINAPI *CH_REPAIR)(VOID);
