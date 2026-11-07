/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../common/platform.h"
#define PB_ETH_ENCAPS (NDIS_ENCAPSULATION_IEEE_802_3 | NDIS_ENCAPSULATION_IEEE_802_3_P_AND_Q)
static inline void PbMakeOffloadForVersion(uint32_t version, uint32_t mask, NDIS_OFFLOAD *offload)
{
    RtlZeroMemory(offload, sizeof(*offload));
    offload->Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
    static const USHORT sizes[] = { 0, 0, NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_2,
        NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_3, NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_4,
        NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_5, NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_6,
        NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_7, NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_8 };
    offload->Header.Revision = (UCHAR)PbPlatformOffloadRevision(version);
    offload->Header.Size = sizes[offload->Header.Revision];
    mask &= PbPlatformOffloads(version);
    offload->Checksum.IPv4Transmit.Encapsulation = PB_ETH_ENCAPS;
    offload->Checksum.IPv4Transmit.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    offload->Checksum.IPv4Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    offload->Checksum.IPv4Transmit.IpChecksum = !!(mask & PB_TX_IP4_CHECKSUM);
    offload->Checksum.IPv4Transmit.TcpChecksum = !!(mask & PB_TX_TCP4_CHECKSUM);
    offload->Checksum.IPv4Transmit.UdpChecksum = !!(mask & PB_TX_UDP4_CHECKSUM);
    offload->Checksum.IPv6Transmit.Encapsulation = PB_ETH_ENCAPS;
    offload->Checksum.IPv6Transmit.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
    offload->Checksum.IPv6Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    offload->Checksum.IPv6Transmit.TcpChecksum = !!(mask & PB_TX_TCP6_CHECKSUM);
    offload->Checksum.IPv6Transmit.UdpChecksum = !!(mask & PB_TX_UDP6_CHECKSUM);
    offload->Checksum.IPv4Receive.Encapsulation = PB_ETH_ENCAPS;
    offload->Checksum.IPv4Receive.IpChecksum = !!(mask & PB_RX_IP4_CHECKSUM);
    offload->Checksum.IPv4Receive.TcpChecksum = !!(mask & PB_RX_TCP4_CHECKSUM);
    offload->Checksum.IPv4Receive.UdpChecksum = !!(mask & PB_RX_UDP4_CHECKSUM);
    offload->Checksum.IPv6Receive.Encapsulation = PB_ETH_ENCAPS;
    offload->Checksum.IPv6Receive.TcpChecksum = !!(mask & PB_RX_TCP6_CHECKSUM);
    offload->Checksum.IPv6Receive.UdpChecksum = !!(mask & PB_RX_UDP6_CHECKSUM);
    offload->Rsc.IPv4.Enabled = !!(mask & PB_RX_RSC4);
    offload->Rsc.IPv6.Enabled = !!(mask & PB_RX_RSC6);
    offload->UdpRsc.Enabled = !!(mask & PB_RX_URO);
    if (mask & PB_TX_LSO4) {
        offload->LsoV2.IPv4.Encapsulation = PB_ETH_ENCAPS;
        offload->LsoV2.IPv4.MaxOffLoadSize = 32768;
        offload->LsoV2.IPv4.MinSegmentCount = 2;
    }
    if (mask & PB_TX_LSO6) {
        offload->LsoV2.IPv6.Encapsulation = PB_ETH_ENCAPS;
        offload->LsoV2.IPv6.MaxOffLoadSize = 32768;
        offload->LsoV2.IPv6.MinSegmentCount = 2;
        offload->LsoV2.IPv6.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
        offload->LsoV2.IPv6.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    }
    if (mask & PB_TX_USO4) {
        offload->UdpSegmentation.IPv4.Encapsulation = PB_ETH_ENCAPS;
        offload->UdpSegmentation.IPv4.MaxOffLoadSize = 32768;
        offload->UdpSegmentation.IPv4.MinSegmentCount = 2;
        offload->UdpSegmentation.IPv4.SubMssFinalSegmentSupported = version >= PB_NDIS_VERSION(84);
    }
    if (mask & PB_TX_USO6) {
        offload->UdpSegmentation.IPv6.Encapsulation = PB_ETH_ENCAPS;
        offload->UdpSegmentation.IPv6.MaxOffLoadSize = 32768;
        offload->UdpSegmentation.IPv6.MinSegmentCount = 2;
        offload->UdpSegmentation.IPv6.SubMssFinalSegmentSupported = version >= PB_NDIS_VERSION(84);
    }
}

