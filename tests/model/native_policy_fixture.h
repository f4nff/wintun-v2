/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../api/v2/lifecycle.h"
/* Shared only by this hosted fixture DLL and its test executable. */
enum { NP_CONTROL_ENTERED,NP_CONTROL_GATE,NP_IO_ENTERED,NP_IO_GATE,NP_PARK_ENTERED,NP_PARK_GATE,
    NP_FINAL_ENTERED,NP_FINAL_GATE,NP_DETACHED,NP_EVENT_COUNT };
enum { NP_LOW_CAPABILITY,NP_BEFORE_CONTROL,NP_LATE_SUCCESS,NP_LATE_FAILURE };
#define NP_MAX_CALLBACKS 8u
typedef struct NP_CONFIG {
    HANDLE Events[NP_EVENT_COUNT],Threads[NP_MAX_CALLBACKS];
    DWORD ThreadIds[NP_MAX_CALLBACKS];
    DWORD Mode,NdisVersion,PolicyError;
} NP_CONFIG;
typedef struct NP_SNAPSHOT {
    LONG References,Callbacks,Finished,ActiveCallbacks,Peak;
    LONG Allocations,Regions,Events,Works,Pools,Pins,TailTransfers;
    LONG PolicyCalls,AfterStopCalls,Drains,Queries,DeviceCloses,NativeActive,Loans;
    DWORD LastPolicyMask,LastPolicyError,AllowedMask;
    uint32_t Phase,Sessions,BlockStage;
    uint64_t Requested,Completed,Bytes;
    BOOL Occupied,Queued,Abandoned,Registered,ResourcesClosed,PublicReleased,CloseRequested;
} NP_SNAPSHOT;
typedef VOID (WINAPI *NP_INITIALIZE)(NP_CONFIG *);
typedef VOID (WINAPI *NP_CONFIGURE)(PB_SESSION *);
typedef VOID (WINAPI *NP_OBSERVE)(NP_SNAPSHOT *);
typedef VOID (WINAPI *NP_SET_ACTIVE)(LONG);
