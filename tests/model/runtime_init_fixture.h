/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PB_API_TESTS
#error Runtime initialization must use the shipping main implementation
#endif
#include <Windows.h>
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"runtime initialization requires the shipping fingerprint layout");
enum { RI_FAILURE_ENTERED,RI_FAILURE_GATE,RI_SECOND_ENTERED,RI_FAILED_RETURNED,RI_FAILED_GATE,RI_EVENT_COUNT };
typedef struct RI_CONFIG {DWORD Point,Error;HANDLE Events[RI_EVENT_COUNT];} RI_CONFIG;
typedef struct RI_STATE {
    LONG Heaps,Tokens,Locals,NamespaceReady,Calls,Failures,UnloadCalls,ClientLeases,InFlight,Repairs;
    ULONG Users;BOOL Unloading,RuntimeReady,FileDescriptor,PipeDescriptor,ModuleHeapPresent;
    DWORD LoadedDependencies;
} RI_STATE;
typedef VOID (WINAPI *RI_CONFIGURE)(const RI_CONFIG *);
typedef BOOL (WINAPI *RI_ACQUIRE)(VOID);
typedef VOID (WINAPI *RI_ACTION)(VOID);
typedef VOID (WINAPI *RI_READ)(RI_STATE *);
