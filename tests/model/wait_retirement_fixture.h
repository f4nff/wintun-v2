/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"wait retirement requires shipping fingerprint layout");
typedef struct WR_CONFIG {
    NP_CONFIG Native;ULONG Queues,Last;BOOL CancelFirst,Fault;
    HANDLE Entered[8],WaitGate,AfterWait[8],Retire[8],TailEntered,TailGate,DataAttempt,EarlyQuery;
    volatile LONG Owners[8],TailHeld;
} WR_CONFIG;
typedef VOID (WINAPI *WR_PREPARE)(WR_CONFIG *);
typedef VOID (WINAPI *WR_ACTION)(VOID);
typedef VOID (WINAPI *WR_MARK)(ULONG);
typedef VOID (WINAPI *WR_INSPECT)(ULONG);
