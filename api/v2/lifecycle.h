/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge.h"
#define PB_LIFECYCLE_ABI 1u
#define PB_LIFECYCLE_CAPS 1u
#define PB_LIFE_RUNNING 1u
#define PB_LIFE_STOP_REQUESTED 2u
#define PB_LIFE_DRAINING 3u
#define PB_LIFE_PREPARED 4u
#define PB_LIFE_RELEASING 5u
#define PB_LIFE_CLOSED 6u
#define PB_BLOCK_NONE 0u
#define PB_BLOCK_POLICY 1u
#define PB_BLOCK_DRAIN 2u
#define PB_BLOCK_LEASES 3u
#define PB_BLOCK_KERNEL 4u
#define PB_BLOCK_DEVICE_CLOSE 5u
#define PB_BLOCK_ADAPTER_CLOSE 6u
#define PB_BLOCK_API_USERS 7u
#define PB_LIFE_QUARANTINED 1u
typedef struct PB_CLOSE_PROGRESS {
    uint32_t Size, Phase, Flags, Result;
    uint64_t SessionId, PolicyRequested, PolicyCompleted, RetainedBytes;
    uint32_t UserLeases, Published, NativeReferences, LastError, BlockStage, Reserved;
    uint64_t Started, LastProgress, Rechecks;
} PB_CLOSE_PROGRESS;
typedef struct PB_RUNTIME_STATUS {
    /* RetainedBytes is the conservative admission charge, released after all
     * native barriers complete; per-session progress reports remaining mapping bytes. */
    uint32_t Size, Sessions;
    uint64_t RetainedBytes;
    uint32_t MaxSessions, Reserved;
    uint64_t MaxRetainedBytes;
    uint32_t Adapters, MaxAdapters;
} PB_RUNTIME_STATUS;
/* Independent extension ABI. A factory writes only the requested known prefix.
 * The 1.x prefix methods keep their 96-byte progress and 40-byte runtime record
 * layouts frozen. New record layouts require appended versioned methods; never
 * resize the output of an existing prefix method in a minor update.
 * BeginStop acknowledges local admission stop, not NDIS drain completion.
 * ReleaseSession consumes the public handle; caller must have joined every user
 * and returned all leases. It never waits for external I/O or allocates a job.
 * PollPolicy acknowledges and releases one outstanding request when it returns OK.
 * All raw calls require a live, externally owned public handle. */
typedef struct PB_LIFECYCLE_INTERFACE {
    uint32_t Size, Major, Minor, Capabilities;
    PB_RESULT (WINAPI *BeginStop)(PB_SESSION *);
    PB_RESULT (WINAPI *BeginSetPolicy)(PB_SESSION *, uint32_t, uint64_t *);
    PB_RESULT (WINAPI *PollPolicy)(PB_SESSION *, uint64_t, uint32_t, PB_RESULT *);
    PB_RESULT (WINAPI *GetCloseProgress)(PB_SESSION *, PB_CLOSE_PROGRESS *);
    void (WINAPI *ReleaseSession)(PB_SESSION *);
    PB_RESULT (WINAPI *GetRuntimeStatus)(PB_RUNTIME_STATUS *);
    PB_RESULT (WINAPI *GetRetained)(uint32_t, PB_CLOSE_PROGRESS *);
    void (WINAPI *ForgetPolicy)(PB_SESSION *, uint64_t);
} PB_LIFECYCLE_INTERFACE;
PB_RESULT WINAPI PbGetLifecycleInterface(uint32_t, uint32_t, PB_LIFECYCLE_INTERFACE *);
