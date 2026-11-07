/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PB_API_TESTS
#error Operation/adapter tests require the shipping runtime
#endif
#include <Windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"operation/adapter requires the shipping fingerprint layout");
enum { OPA_POPULATE_ENTERED,OPA_POPULATE_GATE,OPA_RETIRING_ENTERED,OPA_RETIRING_GATE,OPA_DETACHED,OPA_EVENT_COUNT };
typedef struct OPA_CONFIG {HANDLE Events[OPA_EVENT_COUNT],CallbackThread;DWORD CallbackThreadId;} OPA_CONFIG;
typedef struct OPA_STATE {
    ULONG Users,Jobs,NativeBlocks,Closes,SetsCreated,SetsClosed,MutexesCreated,MutexesClosed,NativeAllocations,NativeFrees;
    LONG JobReferences,CoreHandles,CoreAllocations,CoreWorks,CorePins,CorePools,Compensations,UnloadCalls;
    BOOL Unloading,Stopping,DeviceSet,InstallationMutex,RuntimeOwned,Retiring,Abandoned;
    DWORD LoadedDependencies;
} OPA_STATE;
typedef VOID (WINAPI *OPA_CONFIGURE)(OPA_CONFIG *);
typedef VOID *(WINAPI *OPA_OPEN)(VOID);
typedef BOOL (WINAPI *OPA_WAIT)(DWORD);
typedef VOID (WINAPI *OPA_READ)(OPA_STATE *);
typedef VOID (WINAPI *OPA_ACTION)(VOID);
