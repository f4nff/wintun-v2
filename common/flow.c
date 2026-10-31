/* SPDX-License-Identifier: GPL-2.0 */
#include "flow.h"
uint32_t PbSelectQueue(const uint8_t *packet, uint32_t length, uint32_t queues)
{
    if (!packet || queues <= 1 || queues > 8 || length < 20) return 0;
    uint32_t start = 12, end = 20;
    if (packet[0] >> 4 == 6 && length >= 40) { start = 8; end = 40; }
    else if (packet[0] >> 4 != 4) return 0;
    uint32_t hash = 2166136261u;
    for (uint32_t i = start; i < end; ++i) hash = (hash ^ packet[i]) * 16777619u;
    return hash % queues;
}
