/* SPDX-License-Identifier: GPL-2.0 */
#include "coalesce.h"
#include <string.h>
static uint16_t R16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t R32(const uint8_t *p) { return ((uint32_t)R16(p) << 16) | R16(p + 2); }
static void W16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static PB_PACKET_STATUS Eligible(const uint8_t *bytes, uint32_t length, PB_SEGMENT_PLAN *p)
{
    PB_PACKET_STATUS result = PbPlanSegments(bytes, length, PB_SEGMENT_NONE, 0, p);
    if (result != PB_PACKET_OK) return result;
    if (!p->PayloadLength || p->TransportOffset != (p->IpVersion == 4 ? 20u : 40u)) return PB_PACKET_UNSUPPORTED;
    if (p->IpVersion == 4 && ((bytes[6] & 0x80u) || PbChecksumFinish(PbChecksumAdd(0, bytes, 20)))) return PB_PACKET_INVALID;
    const uint8_t *l4 = bytes + p->TransportOffset;
    if (p->Protocol == 6) {
        /* Data with ACK/PSH is eligible. Preserve PSH by ORing all admitted
         * segments, per RSC rules. Options, pure ACKs and other control/ECN
         * flags stay individual; ACK/window changes remain group boundaries. */
        if (l4[12] != 0x50 || (l4[13] & ~0x08u) != 0x10 || R16(l4 + 18)) return PB_PACKET_UNSUPPORTED;
    } else if (!R16(l4 + 6)) return PB_PACKET_UNSUPPORTED; /* Preserve zero-checksum datagrams individually. */
    uint32_t transportLength = length - p->TransportOffset;
    uint32_t sum = PbChecksumAdd(0, bytes + (p->IpVersion == 4 ? 12 : 8), p->IpVersion == 4 ? 8 : 32);
    sum += transportLength + p->Protocol;
    if (PbChecksumFinish(PbChecksumAdd(sum, l4, transportLength))) return PB_PACKET_INVALID;
    return PB_PACKET_OK;
}

PB_PACKET_STATUS PbCoalesceBegin(PB_COALESCE *s, uint8_t *output, uint32_t capacity, const uint8_t *packet, uint32_t length)
{
    PB_SEGMENT_PLAN p;
    if (!s || !output || !packet || length > 65535) return PB_PACKET_INVALID;
    PB_PACKET_STATUS result = Eligible(packet, length, &p);
    if (result != PB_PACKET_OK) return result;
    if (length > capacity) return PB_PACKET_NO_SPACE;
    memset(s, 0, sizeof(*s));
    s->Buffer = output; s->Capacity = capacity < 65535 ? capacity : 65535; s->Length = length;
    s->Count = 1; s->SegmentSize = p.PayloadLength; s->PayloadLength = p.PayloadLength;
    s->HeaderLength = p.HeaderLength; s->L4Offset = p.TransportOffset; s->IpVersion = p.IpVersion; s->Protocol = p.Protocol;
    if (p.Protocol == 6) s->NextSequence = R32(packet + p.TransportOffset + 4) + p.PayloadLength;
    memcpy(output, packet, length);
    return PB_PACKET_OK;
}

PB_PACKET_STATUS PbCoalesceAppend(PB_COALESCE *s, const uint8_t *packet, uint32_t length)
{
    PB_SEGMENT_PLAN p;
    if (!s || !s->Buffer || !s->Count || s->Finalized || s->ShortFinal || !packet) return PB_PACKET_INVALID;
    PB_PACKET_STATUS result = Eligible(packet, length, &p);
    if (result != PB_PACKET_OK) return result;
    if (p.IpVersion != s->IpVersion || p.Protocol != s->Protocol || p.HeaderLength != s->HeaderLength) return PB_PACKET_UNSUPPORTED;
    const uint8_t *previous = s->Buffer, *a = previous + s->L4Offset, *b = packet + p.TransportOffset;
    if (memcmp(a, b, 4)) return PB_PACKET_UNSUPPORTED;
    if (p.IpVersion == 4) {
        if (previous[1] != packet[1] || (previous[6] & 0x40) != (packet[6] & 0x40) || memcmp(previous + 12, packet + 12, 8))
            return PB_PACKET_UNSUPPORTED;
        if (p.Protocol == 17 && previous[8] != packet[8]) return PB_PACKET_UNSUPPORTED;
    } else {
        if (memcmp(previous, packet, 4) || memcmp(previous + 8, packet + 8, 32)) return PB_PACKET_UNSUPPORTED;
        if (p.Protocol == 17 && previous[7] != packet[7]) return PB_PACKET_UNSUPPORTED;
    }
    if (p.Protocol == 6) {
        if (R32(b + 4) != s->NextSequence || memcmp(a + 8, b + 8, 4) || memcmp(a + 14, b + 14, 2)) return PB_PACKET_UNSUPPORTED;
    } else if (p.PayloadLength > s->SegmentSize) return PB_PACKET_UNSUPPORTED;
    if (s->Count >= 32 || p.PayloadLength > s->Capacity - s->Length) return PB_PACKET_NO_SPACE;
    /* Mutate the tracked header only after every admission check succeeded. */
    if (p.Protocol == 6) s->Buffer[s->L4Offset + 13] |= b[13] & 0x08u;
    memcpy(s->Buffer + s->Length, packet + p.HeaderLength, p.PayloadLength);
    s->Length += p.PayloadLength; s->PayloadLength += p.PayloadLength; s->Count++;
    if (p.Protocol == 6) {
        s->NextSequence += p.PayloadLength;
        uint32_t ttl = p.IpVersion == 4 ? 8u : 7u;
        if (packet[ttl] < s->Buffer[ttl]) s->Buffer[ttl] = packet[ttl];
    } else if (p.PayloadLength < s->SegmentSize) s->ShortFinal = 1;
    return PB_PACKET_OK;
}

PB_PACKET_STATUS PbCoalesceFinish(PB_COALESCE *s)
{
    if (!s || !s->Buffer || !s->Count || s->Finalized) return PB_PACKET_INVALID;
    if (s->Count > 1) {
        if (s->IpVersion == 4) W16(s->Buffer + 2, s->Length);
        else W16(s->Buffer + 4, s->Length - 40);
        if (s->Protocol == 17) {
            W16(s->Buffer + s->L4Offset + 4, s->Length - s->L4Offset);
            W16(s->Buffer + s->L4Offset + 6, 0);
            if (s->IpVersion == 4) W16(s->Buffer + 10, 0);
            /* URO uses successful checksum OOB with zero values, per the NDIS contract. */
        } else {
            PB_PACKET_STATUS result = PbFinalizeChecksums(s->Buffer, s->Length, s->IpVersion == 4, 1, 0);
            if (result != PB_PACKET_OK) return result;
        }
    }
    s->Finalized = 1;
    return PB_PACKET_OK;
}
