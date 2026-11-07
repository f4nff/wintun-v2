/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PB_API_TESTS
#error Actual native DLL required
#endif
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"shipping source fingerprint required");
typedef struct PS_CONFIG {NP_CONFIG Native;ULONG Pollers;HANDLE Waiting[8],Resumed[8],Gates[8];volatile LONG Threads[8],Gated[8],Sleeps[8];} PS_CONFIG;
typedef VOID (WINAPI *PS_PREPARE)(PS_CONFIG *);
typedef VOID (WINAPI *PS_MARK)(ULONG);
typedef VOID (WINAPI *PS_WAIT_CALLBACK)(VOID);
