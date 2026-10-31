/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packet.h"

/* Strict, adjacent-only reference coalescer. Buffer must remain private until NDIS returns it. */
typedef struct PB_COALESCE {
    uint8_t *Buffer;
    uint32_t Capacity, Length, Count, SegmentSize, NextSequence, PayloadLength;
    uint16_t HeaderLength, L4Offset;
    uint8_t IpVersion, Protocol, ShortFinal, Finalized;
} PB_COALESCE;

PB_PACKET_STATUS PbCoalesceBegin(PB_COALESCE *state, uint8_t *output, uint32_t capacity, const uint8_t *packet, uint32_t length);
PB_PACKET_STATUS PbCoalesceAppend(PB_COALESCE *state, const uint8_t *packet, uint32_t length);
PB_PACKET_STATUS PbCoalesceFinish(PB_COALESCE *state);
