/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* A policy prefilter over the existing complete private snapshot. It authorizes
 * no metadata: CoalesceBegin still validates lengths, eligibility and checksums. */
static ULONG PbV2CoalesceMode(const UCHAR *bytes, ULONG length)
{
    ULONG ip, protocol;
    if (length >= 20 && bytes[0] >> 4 == 4) { ip = 4; protocol = bytes[9]; }
    else if (length >= 40 && bytes[0] >> 4 == 6) { ip = 6; protocol = bytes[6]; }
    else return 0;
    return protocol == 17 ? PB_RX_URO : protocol == 6 ? (ip == 4 ? PB_RX_RSC4 : PB_RX_RSC6) : 0;
}

/* This was an inline block in ReceiveBatch. Keep source isolation without adding
 * a call for each unmerged candidate (including when both output buffers are busy). */
static __forceinline ULONG PbV2TryCoalesce(PB_KERNEL_QUEUE *q, const PB_DESCRIPTOR *items, const BOOLEAN *valid,
    ULONG count, ULONG enabled, ULONG *outCopied, PB_COALESCE *outCoal)
{
    NT_ASSERT(count && count <= PB_MAX_BATCH && valid[0]);
    PB_KERNEL_SESSION *s = q->Session; TUN_CTX *ctx = s->Ctx;
    ULONG group = 1, copied = MAXULONG; PB_COALESCE coal = { 0 };
    if (q->Scratch && count > 1 && valid[1] && (enabled & (PB_RX_RSC4 | PB_RX_RSC6 | PB_RX_URO))) {
        for (ULONG b = 0; b < 2; ++b) if (q->CoalesceBuffers[b] && !InterlockedCompareExchange(&q->CoalesceBusy[b], 1, 0)) { copied = b; break; }
        if (copied < 2) {
            RtlCopyMemory(q->Scratch, s->Region + items[0].DataOffset, items[0].Length);
            if ((enabled & PbV2CoalesceMode(q->Scratch, items[0].Length)) &&
                PbCoalesceBegin(&coal, q->CoalesceBuffers[copied], 65535, q->Scratch, items[0].Length) == PB_PACKET_OK &&
                (enabled & (coal.Protocol == 17 ? PB_RX_URO : coal.IpVersion == 4 ? PB_RX_RSC4 : PB_RX_RSC6))) {
                /* The core closes UDP groups after a short final segment.
                 * Do not snapshot another packet merely to reject it from
                 * that already-closed private state; the next group owns it. */
                while (!coal.ShortFinal && group < count && valid[group]) {
                    const PB_DESCRIPTOR *next = items + group;
                    RtlCopyMemory(q->Scratch, s->Region + next->DataOffset, next->Length);
                    PB_PACKET_STATUS appended = PbCoalesceAppend(&coal, q->Scratch, next->Length);
                    if (appended != PB_PACKET_OK) {
                        if (coal.Protocol == 6 && appended != PB_PACKET_NO_SPACE) InterlockedIncrement64(&ctx->Probe.RscAborts);
                        break;
                    }
                    group++;
                }
                if (PbCoalesceFinish(&coal) != PB_PACKET_OK) group = 1;
            }
            if (group == 1) { InterlockedExchange(&q->CoalesceBusy[copied], 0); copied = MAXULONG; }
        }
    }
    *outCopied = copied; *outCoal = coal; return group;
}
