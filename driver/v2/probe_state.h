/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../include/packetbridge_probe.h"
#include "../../include/packetbridge_version.h"
#include "../../common/packet.h"
#include "session_state.h"
#include "../../common/ethernet.h"
#include "../../common/return_diagnostics.h"
#include "../../common/send_diagnostics.h"

typedef struct PB_PROBE_STATE {
    volatile LONG AllowedMask;
    volatile LONG NdisMask;
    volatile LONG EncapsulationMask;
    volatile LONG V2SessionActive;
    LONG ReceiveCleanupCount;
    EX_RUNDOWN_REF ReturnRundown;
    KEVENT RunningEvent;
    KSPIN_LOCK StatusLock;
    KSPIN_LOCK ReturnDiagnosticsLock;
    PB_RETURN_HISTORY ReturnDiagnostics;
    KSPIN_LOCK SendDiagnosticsLock;
    PB_SEND_HISTORY SendDiagnostics;
    FAST_MUTEX OffloadMutex;
    KSPIN_LOCK IndicationLock;
    KEVENT IndicationIdle[2];
    ULONG IndicationBank;
    LONG IndicationCount[2];
    UCHAR LocalMac[6], PeerMac[6];
    NDIS_STATUS RegistrationStatus;
    ULONG LastIpv4Encapsulation;
    ULONG LastIpv6Encapsulation;
    volatile LONG64 LsoNbls, UsoNbls, ChecksumNbls, OffloadRejected, SegmentsCopied, OffloadParameterChanges;
    volatile LONG64 RscPackets, RscBytes, RscEvents, RscAborts, UroPackets, UroEvents;
} PB_PROBE_STATE;
