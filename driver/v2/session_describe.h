/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "platform_nbl.h"
#include "../../include/packetbridge_send_diagnostics.h"
static __inline NDIS_STATUS PbDescribeFailure(PB_SEND_FAILURE *failure, ULONG reason, NDIS_STATUS status, ULONG packetStatus, ULONG required)
{
    if (failure) { failure->Reason = reason; failure->PacketStatus = packetStatus; failure->CoreStatus = 0; failure->RequiredMask = required; }
    return status;
}
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx, NET_BUFFER_LIST *nbl, NET_BUFFER *nb, ULONG ethernetLength, PB_DESCRIPTOR *d, UCHAR *bytes, PB_SEND_FAILURE *failure)
{
    ULONG length = NET_BUFFER_DATA_LENGTH(nb) - ethernetLength;
    if (!length || length > PB_BLOCK_SIZE) return PbDescribeFailure(failure, PB_SEND_LENGTH, NDIS_STATUS_INVALID_LENGTH, 0, 0);
    if (!PbV2Read(nb, ethernetLength, bytes, length)) return PbDescribeFailure(failure, PB_SEND_PAYLOAD_READ, NDIS_STATUS_RESOURCES, 0, 0);
    NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso = { .Value = NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo) };
    NDIS_UDP_SEGMENTATION_OFFLOAD_NET_BUFFER_LIST_INFO uso = { .Value = PB_NBL_GET(nbl, UdpSegmentationOffloadInfo, 83) };
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO c = { .Value = NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) };
    ULONG required = 0;
    d->Length = length;
    if (lso.Value || uso.Value) {
        if ((lso.Value && uso.Value) || NET_BUFFER_NEXT_NB(NET_BUFFER_LIST_FIRST_NB(nbl)) ||
            (lso.Value && lso.Transmit.Type != NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE))
            return PbDescribeFailure(failure, PB_SEND_OFFLOAD_COMBINATION, NDIS_STATUS_NOT_SUPPORTED, 0, 0);
        PB_SEGMENT_PLAN plan;
        ULONG mss = lso.Value ? lso.LsoV2Transmit.MSS : uso.Transmit.MSS;
        ULONG offset = lso.Value ? lso.LsoV2Transmit.TcpHeaderOffset : uso.Transmit.UdpHeaderOffset;
        ULONG ip = lso.Value ? lso.LsoV2Transmit.IPVersion : uso.Transmit.IPVersion;
        PB_PACKET_STATUS planned = PbPlanSegments(bytes, length, lso.Value ? PB_SEGMENT_TCP : PB_SEGMENT_UDP, mss, &plan);
        if (planned != PB_PACKET_OK) return PbDescribeFailure(failure, PB_SEND_GSO_PLAN, NDIS_STATUS_INVALID_DATA, planned, 0);
        if (plan.TransportOffset + ethernetLength != offset) return PbDescribeFailure(failure, PB_SEND_GSO_OFFSET, NDIS_STATUS_INVALID_DATA, 0, 0);
        if ((plan.IpVersion == 6) != !!ip) return PbDescribeFailure(failure, PB_SEND_GSO_FAMILY, NDIS_STATUS_INVALID_DATA, 0, 0);
        if (plan.SegmentCount < 2) return PbDescribeFailure(failure, PB_SEND_GSO_COUNT, NDIS_STATUS_INVALID_DATA, 0, 0);
        if (plan.PayloadLength > 32768u) return PbDescribeFailure(failure, PB_SEND_GSO_PAYLOAD, NDIS_STATUS_INVALID_DATA, 0, 0);
        d->Type = lso.Value ? PB_DESC_TCP_GSO : PB_DESC_UDP_GSO;
        d->Mss = mss; d->L4Offset = plan.TransportOffset; d->HeaderLength = plan.HeaderLength;
        d->IpVersion = plan.IpVersion; d->Protocol = plan.Protocol;
        d->Flags = (ip ? 0 : PB_CSUM_IPV4) | (lso.Value ? PB_CSUM_TCP : plan.UdpChecksumDisabled ? PB_UDP_NO_CHECKSUM : PB_CSUM_UDP);
        required = lso.Value ? (ip ? PB_TX_LSO6 : PB_TX_LSO4) : (ip ? PB_TX_USO6 : PB_TX_USO4);
    } else {
        if (length > 65535u) return PbDescribeFailure(failure, PB_SEND_LENGTH, NDIS_STATUS_INVALID_LENGTH, 0, 0);
        /* Remove Ethernet padding before delivering a single complete IP datagram. */
        ULONG ipLength = length >= 20 && bytes[0] >> 4 == 4 ? ((ULONG)bytes[2] << 8) | bytes[3] :
            length >= 40 && bytes[0] >> 4 == 6 ? 40u + (((ULONG)bytes[4] << 8) | bytes[5]) : 0;
        if (!ipLength || ipLength > length || ipLength < (bytes[0] >> 4 == 4 ? 20u : 40u))
            return PbDescribeFailure(failure, PB_SEND_IP_LENGTH, NDIS_STATUS_INVALID_DATA, 0, 0);
        length = d->Length = ipLength;
        /* NDIS defines no checksum task when neither family is selected.
         * Only interpret task selectors after that explicit family gate. */
        if ((c.Transmit.IsIPv4 || c.Transmit.IsIPv6) &&
            (c.Transmit.IpHeaderChecksum || c.Transmit.TcpChecksum || c.Transmit.UdpChecksum)) {
            ULONG ip = bytes[0] >> 4;
            uint32_t transportOffset = 0;
            if ((ip != 4 && ip != 6) || !!c.Transmit.IsIPv4 != (ip == 4) || !!c.Transmit.IsIPv6 != (ip == 6) ||
                (ip == 6 && c.Transmit.IpHeaderChecksum))
                return PbDescribeFailure(failure, PB_SEND_CHECKSUM_FAMILY, NDIS_STATUS_INVALID_DATA, 0, 0);
            PB_PACKET_STATUS checksum = c.Transmit.TcpChecksum ?
                PbFinalizeChecksumsWithOffset(bytes, length, c.Transmit.IpHeaderChecksum, c.Transmit.TcpChecksum, c.Transmit.UdpChecksum, &transportOffset) :
                PbFinalizeChecksums(bytes, length, c.Transmit.IpHeaderChecksum, c.Transmit.TcpChecksum, c.Transmit.UdpChecksum);
            if (checksum != PB_PACKET_OK) return PbDescribeFailure(failure, PB_SEND_CHECKSUM_DATA, NDIS_STATUS_INVALID_DATA, checksum, 0);
            if (c.Transmit.TcpChecksum) {
                /* The checksum pass already validated these private bytes.
                 * Compare its result, without reparsing the same IP/TCP headers. */
                if (c.Transmit.TcpHeaderOffset != transportOffset + ethernetLength)
                    return PbDescribeFailure(failure, PB_SEND_CHECKSUM_OFFSET, NDIS_STATUS_INVALID_DATA, 0, 0);
            }
            if (c.Transmit.IpHeaderChecksum) required |= PB_TX_IP4_CHECKSUM;
            if (c.Transmit.TcpChecksum) required |= ip == 4 ? PB_TX_TCP4_CHECKSUM : PB_TX_TCP6_CHECKSUM;
            if (c.Transmit.UdpChecksum) required |= ip == 4 ? PB_TX_UDP4_CHECKSUM : PB_TX_UDP6_CHECKSUM;
        }
    }
    /* Admission/lifetime is already protected by Send. A plain packet has no
     * offload requirement; keep policy observation only for actual requests. */
    if (!required) return NDIS_STATUS_SUCCESS;
    return required & ~PbEffectiveMask(ctx) ? PbDescribeFailure(failure, PB_SEND_POLICY, NDIS_STATUS_NOT_SUPPORTED, 0, required) : NDIS_STATUS_SUCCESS;
}

