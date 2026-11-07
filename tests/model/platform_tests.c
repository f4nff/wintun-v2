/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../common/platform.h"
#ifdef NDEBUG
#error Platform tests require active assertions
#endif

static void ParameterRevisions(void)
{
    /* Independent ABI expectations. In particular, the offload structure's
     * 6.70/6.85 revisions do not add an offload-parameters revision. Inclusive
     * ranges exercise both neighbors of every parameters ABI transition. */
    static const struct { uint32_t First, Last, Revision; } ranges[] = {
        {0, 19, 0}, {20, 29, 2}, {30, 49, 3},
        {50, 82, 4}, {83, 88, 5}, {89, 255, 6}
    };
    uint32_t next = 0;
    for (uint32_t i = 0; i < sizeof(ranges)/sizeof(ranges[0]); ++i) {
        assert(ranges[i].First == next && ranges[i].First <= ranges[i].Last);
        for (uint32_t minor = ranges[i].First; minor <= ranges[i].Last; ++minor)
            assert(PbPlatformParameterRevision(PB_NDIS_VERSION(minor)) == ranges[i].Revision);
        next = ranges[i].Last + 1;
    }
    assert(next == 256);
}

int main(void)
{
    assert(PB_NDIS_MINIMUM == 0x00060014u && PB_NDIS_MAXIMUM == 0x00060059u);
    assert(PbPlatformNdis(0) == 0);
    assert(PbPlatformNdis(PB_NDIS_MINIMUM - 1) == 0);
    assert(PbPlatformNdis(0xffffffffu) == PB_NDIS_MAXIMUM);
    /* Reject the whole older encoded-version range, including earlier major
     * versions with large minor values. No capability/revision may escape. */
    for (uint32_t version = 0; version < 0x00060014u; ++version) {
        assert(!PbPlatformNdis(version) && !PbPlatformOffloads(version));
        assert(!PbPlatformOffloadRevision(version) && !PbPlatformParameterRevision(version));
    }
    ParameterRevisions();
    const uint32_t versions[] = {20,30,50,70,83,85,89};
    for (uint32_t i = 0; i < sizeof(versions)/sizeof(versions[0]); ++i) {
        uint32_t v = PB_NDIS_VERSION(versions[i]);
        assert(PbPlatformOffloadRevision(v) == i + 2);
        assert(PbPlatformOffloadRevision(v - 1) == (i ? i + 1 : 0));
    }
    uint32_t previousMask = 0, previousOffload = 0, previousParameters = 0;
    for (uint32_t minor = 0; minor <= 255; ++minor) {
        uint32_t v = PB_NDIS_VERSION(minor), mask = PbPlatformOffloads(v);
        uint32_t offload = PbPlatformOffloadRevision(v), parameters = PbPlatformParameterRevision(v);
        assert(!(mask & ~PB_PROBE_SUPPORTED));
        assert(!(previousMask & ~mask) && offload >= previousOffload && parameters >= previousParameters);
        assert(!!(mask & PB_RX_URO) == (minor >= 89));
        assert(!!(mask & PB_TX_USO4) == (minor >= 83));
        assert(!!(mask & PB_TX_USO6) == (minor >= 83));
        assert(!!(mask & PB_RX_RSC4) == (minor >= 30));
        assert(!!(mask & PB_RX_RSC6) == (minor >= 30));
        assert(!!(mask & PB_TX_LSO4) == (minor >= 20));
        assert(!!(mask & PB_RX_TCP6_CHECKSUM) == (minor >= 20));
        assert(parameters <= 6);
        previousMask = mask; previousOffload = offload; previousParameters = parameters;
    }
    assert(PbPlatformOffloads(PB_NDIS_MAXIMUM) == PB_PROBE_SUPPORTED);
    const uint32_t future[] = {0x0006005au, 0x000600ffu, 0x00060100u, 0x00070000u, 0xffffffffu};
    for (uint32_t i = 0; i < sizeof(future)/sizeof(future[0]); ++i) {
        assert(PbPlatformNdis(future[i]) == PB_NDIS_MAXIMUM);
        assert(PbPlatformOffloads(future[i]) == PB_PROBE_SUPPORTED);
        assert(PbPlatformOffloadRevision(future[i]) == 8 && PbPlatformParameterRevision(future[i]) == 6);
    }
    puts("PASS: all 393236 unsupported versions, 256 parameter ABI expectations and monotonic capability/revision versions, exact transition neighbors, five future encodings, RSC/USO/URO gating and unchanged modern capabilities.");
    return 0;
}
