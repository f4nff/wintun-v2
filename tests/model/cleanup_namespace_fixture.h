/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PB_API_TESTS
#error Cleanup/namespace requires the shipping runtime
#endif
#include <Windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"cleanup/namespace requires a shipping fingerprint");
enum { CN_CREATE_ENTERED,CN_CREATE_GATE,CN_CALLBACK_RETURNED,CN_DETACHED,CN_RELEASE_ENTERED,CN_RELEASE_GATE,CN_EVENT_COUNT };
typedef struct CN_CONFIG {HANDLE Events[CN_EVENT_COUNT];BOOL Cancel;} CN_CONFIG;
typedef struct CN_STATE {
    ULONG Users;
    BOOL Unloading,Stopping,Scheduled,Requested,CleanupStopping,NamespaceCached,BoundaryCached,CSAvailable;
    LONG NamespaceOwners,BoundaryOwners,MutexOwners,PairEvents,Pins,Callbacks,Returned,TakeError,LegacyCalls,OrphanCalls,UnloadCalls,MutexCreates,ReleaseHeld,Compensations;
} CN_STATE;
typedef VOID (WINAPI *CN_CONFIGURE)(CN_CONFIG *);
typedef BOOL (WINAPI *CN_QUEUE)(VOID);
typedef BOOL (WINAPI *CN_SHUTDOWN)(DWORD);
typedef VOID (WINAPI *CN_READ)(CN_STATE *);
typedef VOID (WINAPI *CN_ACTION)(VOID);
