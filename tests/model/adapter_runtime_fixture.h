/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"adapter/runtime requires the shipping fingerprint layout");
enum { AR_UNLOAD_ENTERED,AR_UNLOAD_GATE,AR_SHUTDOWN_ENTERED,AR_EVENT_COUNT };
enum { AR_FAIL_PROBE=1,AR_FAIL_REGISTER,AR_FAIL_POLICY };
typedef struct AR_CONFIG {NP_CONFIG Native;HANDLE Events[AR_EVENT_COUNT];DWORD Failure;} AR_CONFIG;
typedef struct AR_STATE {
    ULONG Users,Unloading,Stopping,WrapperReferences,RuntimeOwned,LoadedDependencies;
    LONG Wrappers,LegacyCloses,LegacyBlocks,NativeSessions,NativePins,NativeMaps,NativeClosed,Parked,UnloadingHeld,UnloadCalls;
    LONG Core,ResourcesClosed,ActiveCallbacks,Callbacks,Finished,Repairs;
    uint32_t Phase;uint64_t Bytes;
} AR_STATE;
typedef VOID *(WINAPI *AR_CREATE)(AR_CONFIG *);
typedef VOID (WINAPI *AR_ADAPTER)(VOID *);
typedef BOOL (WINAPI *AR_ACTION)(VOID);
typedef VOID (WINAPI *AR_READ)(AR_STATE *);
typedef VOID (WINAPI *AR_VOID)(VOID);
