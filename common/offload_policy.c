/* SPDX-License-Identifier: GPL-2.0 */
#include "offload_policy.h"
uint32_t PbResolveOffloads(uint32_t allowed, uint32_t requested, uint32_t encapsulation, int v2Session)
{
    uint32_t mask = allowed & requested & encapsulation & PB_PROBE_SUPPORTED;
    if (!v2Session) return 0;
    /* LSO calculates checksums for its own segments independently of the
     * ordinary TX checksum configuration. Its own gates are intersected
     * above; RX coalescing retains its validation prerequisites below. */
    const uint32_t rsc4 = PB_RX_IP4_CHECKSUM | PB_RX_TCP4_CHECKSUM;
    const uint32_t uro = PB_RX_IP4_CHECKSUM | PB_RX_UDP4_CHECKSUM | PB_RX_UDP6_CHECKSUM;
    if ((mask & rsc4) != rsc4) mask &= ~PB_RX_RSC4;
    if (!(mask & PB_RX_TCP6_CHECKSUM)) mask &= ~PB_RX_RSC6;
    if ((mask & uro) != uro) mask &= ~PB_RX_URO;
    return mask;
}
PB_RESULT PbApplyOffloadParameter(uint32_t parameter, uint32_t bit, uint32_t *mask)
{
    if (!mask || parameter > 2) return PB_INVALID;
    if (parameter == 1) *mask &= ~bit;
    else if (parameter == 2) *mask |= bit;
    return PB_OK;
}
PB_RESULT PbApplyChecksumParameter(uint32_t parameter, uint32_t tx, uint32_t rx, uint32_t *mask)
{
    if (!mask || parameter > 4) return PB_INVALID;
    if (parameter) {
        *mask &= ~(tx | rx);
        if (parameter == 2 || parameter == 4) *mask |= tx;
        if (parameter == 3 || parameter == 4) *mask |= rx;
    }
    return PB_OK;
}
