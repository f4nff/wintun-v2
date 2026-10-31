/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "v2_core.h"
#include "../include/packetbridge_probe.h"
uint32_t PbResolveOffloads(uint32_t allowed, uint32_t requested, uint32_t encapsulation, int v2Session);
PB_RESULT PbApplyOffloadParameter(uint32_t parameter, uint32_t bit, uint32_t *mask);
PB_RESULT PbApplyChecksumParameter(uint32_t parameter, uint32_t tx, uint32_t rx, uint32_t *mask);
