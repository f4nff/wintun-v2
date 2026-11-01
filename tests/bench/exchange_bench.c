/* SPDX-License-Identifier: GPL-2.0 */
#pragma warning(disable:4505)
#include "../model/exchange_fixture.h"
int main(int argc, char **argv)
{
    BOOL defaults = argc == 5 && !strcmp(argv[1], "--defaults");
    EX_CHECK(defaults || argc == 9);
    const char *direction = argv[defaults ? 2 : 1];
    EX_CHECK(!strcmp(direction, "tx") || !strcmp(direction, "rx"));
    BOOL tx = !strcmp(direction, "tx");
    PB_CONFIG config = PB_CONFIG_INIT;
    ULONG q, slots, ring, budget, length, total, batch;
    if (defaults) {
        q = config.QueueCount; slots = config.SlotCount; ring = config.RingCapacity;
        budget = tx ? PB_TX_BYTE_BUDGET : PB_RX_BYTE_BUDGET;
        length = strtoul(argv[3], NULL, 10); total = strtoul(argv[4], NULL, 10); batch = PB_MAX_BATCH;
    } else {
        q = strtoul(argv[2], NULL, 10); slots = strtoul(argv[3], NULL, 10); ring = strtoul(argv[4], NULL, 10);
        budget = strtoul(argv[5], NULL, 10); length = strtoul(argv[6], NULL, 10);
        total = strtoul(argv[7], NULL, 10); batch = strtoul(argv[8], NULL, 10);
    }
    DWORD before, after; EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    (void)ExRun(tx, q, slots, ring, batch, budget, length, 8192, TRUE);
    EX_RESULT r = ExRun(tx, q, slots, ring, batch, budget, length, total, TRUE);
    EX_CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after);
    printf("{\"direction\":\"%s\",\"max_batch\":%u,\"batch\":%lu,\"queues\":%lu,\"slots\":%lu,\"ring\":%lu,\"byte_budget\":%lu,\"object_bytes\":%lu,\"objects\":%lu,\"seconds\":%.9f,\"mobjects_per_second\":%.6f,\"ns_per_object\":%.6f,\"cycles_per_object\":%.6f,\"cpu_ns_per_object\":%.6f,\"user_set_events\":%llu,\"peer_set_events\":%llu,\"user_waits\":%llu,\"peer_waits\":%llu,\"user_calls\":%llu,\"peer_calls\":%llu,\"user_batches\":%llu,\"peer_batches\":%llu,\"user_empty\":%llu,\"peer_empty\":%llu,\"region_bytes\":%lu,\"region_limit_bytes\":%u,\"affinity\":%llu}\n",
        direction, PB_MAX_BATCH, batch, q, slots, ring, budget, length, total, r.Seconds, total / r.Seconds / 1e6,
        r.Seconds * 1e9 / total, (double)(r.User.Cycles + r.Peer.Cycles) / total,
        (double)(r.User.Cpu100ns + r.Peer.Cpu100ns) * 100 / total,
        r.User.Sets, r.Peer.Sets, r.User.Waits, r.Peer.Waits, r.User.Calls, r.Peer.Calls,
        r.User.Batches, r.Peer.Batches, r.User.Empty, r.Peer.Empty, r.RegionBytes, PB_MAX_REGION, (unsigned long long)r.Affinity);
    return 0;
}
