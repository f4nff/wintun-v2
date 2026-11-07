/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"unpublished close requires shipping fingerprint layout");
typedef struct UC_CONFIG {
    NP_CONFIG Native;HANDLE Device,Progress;
    volatile LONG CloseAttempts,CloseFailures,CloseSuccesses,Repairs;DWORD Failure;
} UC_CONFIG;
typedef struct UC_SNAPSHOT {
    LONG References,PublicReleased,Registered,Scheduled,Callbacks,Active;
    LONG Pins,Regions,Works,Pools,NativeRegistered,NativeActive,Drains,Queries;
    uint32_t Phase,Block,Error,Sessions;uint64_t Bytes;
} UC_SNAPSHOT;
typedef VOID (WINAPI *UC_PREPARE)(UC_CONFIG *);
typedef VOID (WINAPI *UC_READ)(UC_SNAPSHOT *);
typedef VOID (WINAPI *UC_REPAIR)(VOID);
