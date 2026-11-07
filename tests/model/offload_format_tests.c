/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NDIS_SUPPORT_NDIS61 1
#define NDIS_SUPPORT_NDIS630 1
#define NDIS_SUPPORT_NDIS650 1
#define NDIS_SUPPORT_NDIS670 1
#define NDIS_SUPPORT_NDIS683 1
#define NDIS_SUPPORT_NDIS685 1
#define NDIS_SUPPORT_NDIS689 1
#pragma warning(push)
#pragma warning(disable:4324)
#include <ntddndis.h>
#pragma warning(pop)
#include <assert.h>
#include <stdio.h>
#include "../../driver/v2/offload_format.h"
int main(void)
{
    const USHORT sizes[] = {0,0,NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_2,NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_3,
        NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_4,NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_5,
        NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_6,NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_7,NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_8};
    for(uint32_t minor=20;minor<=100;++minor) {
        uint32_t v=PB_NDIS_VERSION(minor);
        struct { uint64_t before; NDIS_OFFLOAD offload; uint64_t after; } guarded;
        memset(&guarded,0xa5,sizeof(guarded));
        PbMakeOffloadForVersion(v,0xffffffffu,&guarded.offload);
        NDIS_OFFLOAD *o=&guarded.offload;
        assert(guarded.before==0xa5a5a5a5a5a5a5a5ull&&guarded.after==guarded.before);
        assert(o->Header.Revision==PbPlatformOffloadRevision(v));
        assert(o->Header.Size==sizes[o->Header.Revision]);
        assert(!!o->Rsc.IPv4.Enabled==(minor>=30));
        assert(!!o->UdpSegmentation.IPv4.MaxOffLoadSize==(minor>=83));
        assert(!!o->UdpSegmentation.IPv4.SubMssFinalSegmentSupported==(minor>=84));
        assert(!!o->UdpRsc.Enabled==(minor>=89));
        for(size_t i=o->Header.Size;i<sizeof(*o);++i) assert(!((BYTE *)o)[i]);
        PbMakeOffloadForVersion(v,0,o);
        assert(!o->Rsc.IPv4.Enabled&&!o->UdpSegmentation.IPv4.MaxOffLoadSize&&!o->UdpRsc.Enabled);
        assert(!o->Checksum.IPv4Transmit.TcpChecksum&&!o->LsoV2.IPv4.MaxOffLoadSize);
    }
    puts("PASS: actual NDIS offload structures for 81 platform versions, exact revisions/sizes, zero unsupported tails, guarded bounds and plain policy.");
    return 0;
}
