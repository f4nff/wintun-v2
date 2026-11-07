/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "platform_nbl.h"
#include "session_return.h"

static NET_BUFFER_LIST *PbV2Receive(PB_KERNEL_QUEUE *q, const PB_DESCRIPTOR *d, ULONG count, ULONG copied, const PB_COALESCE *coal)
{
    /* Internal callers already validate descriptors; keep allocation/copy bounds explicit here too. */
    if (!d || !count || count > PB_MAX_BATCH || (coal ? copied >= 2 || count < 2 : copied < 2 || count != 1))
        return NULL;
    PB_KERNEL_SESSION *s = q->Session;
    TUN_CTX *ctx = s->Ctx;
    ULONG length = coal ? coal->Length : d->Length;
    if (length < 20 || length > 65535u || (coal && !q->CoalesceBuffers[copied])) return NULL;
    if (!coal && (d->DataOffset > s->Layout.RegionSize || length > s->Layout.RegionSize - d->DataOffset)) return NULL;
    UCHAR *bytes = copied < 2 ? q->CoalesceBuffers[copied] : s->Region + d->DataOffset;
    /* Snapshot the only bytes used to construct NDIS metadata. Payload remains untrusted network data. */
    UCHAR prefix[40]; ULONG prefixLength = min(length, sizeof(prefix));
    RtlCopyMemory(prefix, bytes, prefixLength);
    UCHAR first = prefix[0];
    ULONG flags; USHORT type;
    if (length >= 20 && first >> 4 == 4) { flags = NDIS_NBL_FLAGS_IS_IPV4; type = HTONS(NDIS_ETH_TYPE_IPV4); }
    else if (length >= 40 && first >> 4 == 6) { flags = NDIS_NBL_FLAGS_IS_IPV6; type = HTONS(NDIS_ETH_TYPE_IPV6); }
    else return NULL;
    PB_RX_RECORD *record = ExAllocatePoolZero(NonPagedPool, FIELD_OFFSET(PB_RX_RECORD, Descriptors) + count * sizeof(*d), TUN_MEMORY_TAG);
    if (!record) return NULL;
    VOID *address = copied < 2 ? bytes : (UCHAR *)MmGetMdlVirtualAddress(s->Mdl) + d->DataOffset;
    record->Mdl = IoAllocateMdl(address, length, FALSE, FALSE, NULL);
    if (!record->Mdl) { ExFreePoolWithTag(record, TUN_MEMORY_TAG); return NULL; }
    if (copied < 2) MmBuildMdlForNonPagedPool(record->Mdl);
    else IoBuildPartialMdl(s->Mdl, record->Mdl, address, length);
    PbEthernetHeader(record->Ethernet, ctx->Probe.LocalMac, ctx->Probe.PeerMac, prefix, prefixLength);
    record->HeaderMdl = IoAllocateMdl(record->Ethernet, PB_ETH_HEADER, FALSE, FALSE, NULL);
    if (!record->HeaderMdl) { IoFreeMdl(record->Mdl); ExFreePoolWithTag(record, TUN_MEMORY_TAG); return NULL; }
    MmBuildMdlForNonPagedPool(record->HeaderMdl);
    record->HeaderMdl->Next = record->Mdl;
    NET_BUFFER_LIST *nbl = NdisAllocateNetBufferAndNetBufferList(ctx->NblPool, 0, 0, record->HeaderMdl, 0, length + PB_ETH_HEADER);
    if (!nbl) { IoFreeMdl(record->HeaderMdl); IoFreeMdl(record->Mdl); ExFreePoolWithTag(record, TUN_MEMORY_TAG); return NULL; }
    record->Queue = q; record->Count = count; record->CoalesceIndex = copied;
    record->TcpPayloadLength = coal && coal->Protocol == 6 ? coal->PayloadLength : 0;
    record->IndicationLength = length;
    record->IpVersion = first >> 4;
    record->IpDeclaredLength = record->IpVersion == 4 ? ((ULONG)prefix[2] << 8) | prefix[3] :
        40u + (((ULONG)prefix[4] << 8) | prefix[5]);
    record->Protocol = coal ? coal->Protocol : prefix[record->IpVersion == 4 ? 9 : 6];
    record->ReceiveType = coal ? (coal->Protocol == 6 ? PB_DESC_TCP_GSO : PB_DESC_UDP_GSO) : PB_DESC_PLAIN;
    record->LocallyCancelled = FALSE;
    RtlCopyMemory(record->Descriptors, d, count * sizeof(*d));
    NET_BUFFER_LIST_NEXT_NBL_EX(nbl) = (PVOID)((ULONG_PTR)record | 1);
    NET_BUFFER_LIST_OFFSET(nbl) = NULL;
    nbl->SourceHandle = ctx->MiniportAdapterHandle;
    NdisClearNblFlag(nbl, NDIS_NBL_FLAGS_IS_IPV4 | NDIS_NBL_FLAGS_IS_IPV6 | NDIS_NBL_FLAGS_IS_TCP | NDIS_NBL_FLAGS_IS_UDP);
    NdisSetNblFlag(nbl, flags);
    NET_BUFFER_LIST_INFO(nbl, NetBufferListFrameType) = (PVOID)(ULONG_PTR)type;
    NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_SUCCESS;
    PB_NBL_SET(nbl, TcpRecvSegCoalesceInfo, 30, NULL);
    PB_NBL_SET(nbl, UdpRecvSegCoalesceOffloadInfo, 89, NULL);
    NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = NULL;
    PB_NBL_SET(nbl, RscTcpTimestampDelta, 30, NULL);
    NET_BUFFER_LIST_INFO(nbl, Ieee8021QNetBufferListInfo) = NULL;
    NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
    if (coal && count > 1) {
        NdisSetNblFlag(nbl, coal->Protocol == 6 ? NDIS_NBL_FLAGS_IS_TCP : NDIS_NBL_FLAGS_IS_UDP);
        NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum = { 0 };
        checksum.Receive.IpChecksumSucceeded = coal->IpVersion == 4;
        if (coal->Protocol == 6) {
            NDIS_RSC_NBL_INFO info = { 0 }; info.Info.CoalescedSegCount = (USHORT)count;
            PB_NBL_SET(nbl, TcpRecvSegCoalesceInfo, 30, info.Value);
            checksum.Receive.TcpChecksumSucceeded = TRUE;
        } else {
            NDIS_UDP_RSC_OFFLOAD_NET_BUFFER_LIST_INFO info = { 0 };
            info.Receive.SegCount = (USHORT)count; info.Receive.SegSize = (USHORT)coal->SegmentSize;
            PB_NBL_SET(nbl, UdpRecvSegCoalesceOffloadInfo, 89, info.Value);
            checksum.Receive.UdpChecksumSucceeded = TRUE;
            checksum.Receive.IpChecksumValueInvalid = coal->IpVersion == 4;
        }
        NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = checksum.Value;
    }
    /* Still private to this worker. ReceiveBatch retains the completed chain before any return/upcall. */
    return nbl;
}
