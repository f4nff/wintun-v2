/* SPDX-License-Identifier: GPL-2.0 */
#pragma warning(disable:4505)
#include "exchange_fixture.h"
int main(VOID)
{
    PB_CONFIG config = PB_CONFIG_INIT; PB_LAYOUT layout;
    EX_CHECK(config.Size == sizeof(config) && config.QueueCount == 4 && config.SlotCount == 256 &&
        config.RingCapacity == 512 && !config.Offloads && !config.Flags);
    _Static_assert(PB_MAX_BATCH == 64 && PB_MAX_REGION == 256u * 1024u * 1024u && PB_PACKET_MAX_BATCH == 32, "transport and packet limits");
    _Static_assert(PB_RX_BYTE_BUDGET == 512u * 1024u && PB_TX_BYTE_BUDGET == 128u * 1024u, "directional work budgets");
    EX_CHECK(PbCalculateLayout(config.QueueCount, config.SlotCount, config.RingCapacity, &layout) == PB_OK);
    EX_CHECK(layout.RegionSize == 134762496u && layout.RegionSize < PB_MAX_REGION);
    DWORD before, after; EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    const ULONG lengths[] = { 1500, 32768 };
    for (ULONG tx = 0; tx < 2; ++tx) for (ULONG i = 0; i < _countof(lengths); ++i) {
        EX_RESULT r = ExRun((BOOL)tx, config.QueueCount, config.SlotCount, config.RingCapacity,
            PB_MAX_BATCH, tx ? PB_TX_BYTE_BUDGET : PB_RX_BYTE_BUDGET, lengths[i], 65539, FALSE);
        EX_CHECK(r.User.Objects == 65539 && r.Peer.Objects == 65539);
    }
    EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after);
    puts("PASS: production defaults 4 queues / 256 slots / ring 512 / batch 64, 128.52-MiB layout, RX/TX 512/128-KiB budgets; four concurrent exact-data retirement runs and zero handle delta.");
    return 0;
}
