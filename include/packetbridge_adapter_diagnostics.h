/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdint.h>
#define PB_ADAPTER_DIAG_SCHEMA 1u
enum PB_ADAPTER_STAGE {
    PB_ADAPTER_VALIDATE, PB_ADAPTER_LOCK, PB_ADAPTER_STAGE_DRIVER,
    PB_ADAPTER_CREATE_DEVICE, PB_ADAPTER_WAIT_DEVICE, PB_ADAPTER_FIND_INTERFACE,
    PB_ADAPTER_REMOVE_DEVICES, PB_ADAPTER_REMOVE_PACKAGES, PB_ADAPTER_COMPLETE,
    PB_ADAPTER_PREPARE_PACKAGE
};
/* Additive user-mode entry point; does not change the V2 function table. */
typedef struct PB_ADAPTER_DIAGNOSTICS {
    uint32_t Size, Schema, Stage, Attempts;
    uint32_t Win32Error, HResult, FirstWin32Error, FirstHResult;
    uint32_t RecoveryError, RemovedDevices, RemovedPackages, NeedReboot;
    uint32_t ProblemCode, ProblemStatus, ProblemFieldsKnown, Reserved;
    uint32_t FirstStage, FirstProblemCode, FirstProblemStatus, FirstProblemFieldsKnown;
} PB_ADAPTER_DIAGNOSTICS;

/* Schema 1 remains exactly 80 bytes. The independent Ex2 entry accepts this
 * larger report; neither the V2 function table nor old callers change. */
#define PB_ADAPTER_DIAG_SCHEMA_EX 2u
enum PB_ADAPTER_RECOVERY_STEP {
    PB_RECOVERY_NONE, PB_RECOVERY_OPEN_DEVICES, PB_RECOVERY_SCAN_DEVICES,
    PB_RECOVERY_READ_INF, PB_RECOVERY_INF_PATH, PB_RECOVERY_REMOVE_DEVICE,
    PB_RECOVERY_REMOVE_DRIVER
};
typedef struct PB_ADAPTER_DIAGNOSTICS_EX {
    PB_ADAPTER_DIAGNOSTICS Base;
    uint32_t RecoveryStep, MatchedDevices, FoundPackages, AlreadyAbsentPackages;
    uint32_t ProbeMismatchMask, ObservedVersion, ObservedDataAbi, ObservedProbeSchema;
    char ObservedSource[65]; uint8_t Reserved[3];
} PB_ADAPTER_DIAGNOSTICS_EX;
