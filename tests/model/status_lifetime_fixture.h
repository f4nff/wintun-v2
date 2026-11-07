/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"status lifetime tests require the shipping fingerprint layout");
enum { SL_READ_ENTERED,SL_READ_GATE,SL_EXCLUSIVE_ATTEMPTED,SL_EXCLUSIVE_HELD,SL_EXCLUSIVE_GATE,SL_EVENT_COUNT };
typedef struct SL_CONFIG {HANDLE Events[SL_EVENT_COUNT];BOOL Fails;} SL_CONFIG;
typedef struct SL_SNAPSHOT {
    LONG Core,ReaderHeld,ExclusiveImmediate,ExclusiveHeld,IoCalls;
    LONG Allocations,Regions,Events,Works,Pools,Pins,Callbacks,Finished,Active;
    uint32_t Sessions;uint64_t Bytes;
} SL_SNAPSHOT;
typedef VOID (WINAPI *SL_CONFIGURE)(SL_CONFIG *);
typedef VOID (WINAPI *SL_MARK)(VOID);
typedef VOID (WINAPI *SL_READ)(SL_SNAPSHOT *);
