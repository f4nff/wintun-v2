/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
/* Private host/fixture protocol. The caller holds its ordinary LoadLibrary
 * reference until all diagnostic calls and pool threads have been joined. */
typedef struct RO_SNAPSHOT {
    LONG Core,References,Phase,Closed,Registered,DiagnosticHeld;
    LONG Allocations,Regions,Events,Works,Pools,Pins,Callbacks,Finished,Active;
    LONG DestroyThread,PoolCloseThread,CoreUnpinThread,CallbackThread;
    uint32_t Sessions;uint64_t Bytes,SessionId;
} RO_SNAPSHOT;
typedef VOID (WINAPI *RO_CONFIGURE)(HANDLE,HANDLE);
typedef VOID (WINAPI *RO_MARK)(VOID);
typedef VOID (WINAPI *RO_READ)(RO_SNAPSHOT *);
