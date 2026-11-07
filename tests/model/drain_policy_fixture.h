/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PB_API_TESTS
#error Drain/policy tests require shipping DLL paths
#endif
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"drain/policy requires shipping fingerprint layout");
/* Shared only by this unloadable fixture DLL and its host. */
typedef struct DP_CONFIG {
    NP_CONFIG Native;
    ULONG Queues;
    BOOL FailSleep;
    HANDLE WaitEntered[8],AfterWait[8],RetireGate,SleepEntered,RetryEntered;
    volatile LONG Owners[8],DrainThread,SleepCalls,SleepFailures,SleepRetries;
} DP_CONFIG;
typedef VOID (WINAPI *DP_PREPARE)(DP_CONFIG *);
typedef VOID (WINAPI *DP_MARK)(ULONG);
typedef VOID (WINAPI *DP_PUBLISH)(VOID);
