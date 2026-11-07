/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "packetbridge.h"
/* Internal, borrowed external wake. The caller owns a live session and joins
 * the sole queue owner before closing. No new public ABI/function-table slot. */
PB_RESULT PbCompatWait(PB_USER_QUEUE *, HANDLE external, BOOL receive, uint32_t timeout);
