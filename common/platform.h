/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_probe.h"

/* NdisGetVersion uses decimal minor numbers in the low word (6.89 = 0x60059).
 * Keep platform decisions pure so every version boundary can be hosted-tested. */
#define PB_NDIS_VERSION(minor) (0x00060000u | (minor))
#define PB_NDIS_MINIMUM PB_NDIS_VERSION(20)
#define PB_NDIS_MAXIMUM PB_NDIS_VERSION(89)
static inline uint32_t PbPlatformNdis(uint32_t runtime)
{
    if (runtime < PB_NDIS_MINIMUM) return 0;
    return runtime > PB_NDIS_MAXIMUM ? PB_NDIS_MAXIMUM : runtime;
}
static inline uint32_t PbPlatformOffloads(uint32_t runtime)
{
    if (!PbPlatformNdis(runtime)) return 0;
    uint32_t mask = PB_PROBE_SUPPORTED;
    if (runtime < PB_NDIS_VERSION(30)) mask &= ~(PB_RX_RSC4 | PB_RX_RSC6);
    if (runtime < PB_NDIS_VERSION(83)) mask &= ~(PB_TX_USO4 | PB_TX_USO6);
    if (runtime < PB_NDIS_VERSION(89)) mask &= ~PB_RX_URO;
    return mask;
}
static inline uint32_t PbPlatformOffloadRevision(uint32_t runtime)
{
    if (runtime >= PB_NDIS_VERSION(89)) return 8;
    if (runtime >= PB_NDIS_VERSION(85)) return 7;
    if (runtime >= PB_NDIS_VERSION(83)) return 6;
    if (runtime >= PB_NDIS_VERSION(70)) return 5;
    if (runtime >= PB_NDIS_VERSION(50)) return 4;
    if (runtime >= PB_NDIS_VERSION(30)) return 3;
    return runtime >= PB_NDIS_MINIMUM ? 2 : 0;
}
static inline uint32_t PbPlatformParameterRevision(uint32_t runtime)
{
    if (runtime >= PB_NDIS_VERSION(89)) return 6;
    if (runtime >= PB_NDIS_VERSION(83)) return 5;
    if (runtime >= PB_NDIS_VERSION(50)) return 4;
    if (runtime >= PB_NDIS_VERSION(30)) return 3;
    return runtime >= PB_NDIS_MINIMUM ? 2 : 0;
}
