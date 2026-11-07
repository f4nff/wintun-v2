/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "native_policy_fixture.h"
#include "../../include/packetbridge_version.h"
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"facade stop tests require shipping fingerprint layout");
enum { FS_WAIT_ENTERED,FS_WAIT_GATE,FS_PUMP_ENTERED,FS_PUMP_GATE,FS_STOP_ENTERED,FS_STOP_GATE,FS_CLOSE_ENTERED,FS_EVENT_COUNT };
enum { FS_SEND,FS_RECEIVE };
typedef struct FS_CONFIG {HANDLE Events[FS_EVENT_COUNT];DWORD Direction;BOOL QueryFailure;} FS_CONFIG;
typedef struct FS_SNAPSHOT {
    LONG Core,Loans,Published,NativeActive,NativeClosed,PublicReleased,Registered;
    LONG Runtime,Heap,Regions,Handles,Paths,Pins,WakeSignals,WaitCalls,CommitStopped,Cancelled,RepairLoans,PublicReturns;
    LONG NativeAllocations,NativeRegions,NativeEvents,NativeWorks,NativePools,NativePins,Callbacks,Finished,Active,DeviceCloses;
    LONG CloseCalls,CloseResult;uint32_t Sessions;uint64_t Bytes,LegacyBytes;
} FS_SNAPSHOT;
typedef VOID (WINAPI *FS_PREPARE)(FS_CONFIG *);
typedef VOID *(WINAPI *FS_START)(VOID);
typedef VOID (WINAPI *FS_OBJECT)(VOID *);
typedef VOID (WINAPI *FS_ACTION)(VOID);
typedef BYTE *(WINAPI *FS_RECEIVE_COPY)(VOID *);
typedef VOID (WINAPI *FS_RETURN_COPY)(VOID *,BYTE *);
typedef VOID (WINAPI *FS_READ)(FS_SNAPSHOT *);
