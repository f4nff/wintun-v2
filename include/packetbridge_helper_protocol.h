/* SPDX-License-Identifier: GPL-2.0 */
/* Internal setupapihost response framing, measured in WCHARs. This is not a
 * public DLL ABI. Device instance IDs contain fewer than MAX_DEVICE_ID_LEN
 * characters; their capacity already includes the terminating NUL. */
#pragma once
#include <cfgmgr32.h>

#define PB_HELPER_STATUS_HEX_CHARS 8u
#define PB_HELPER_STATUS_RESPONSE_CAPACITY (PB_HELPER_STATUS_HEX_CHARS + 1u)
#define PB_HELPER_CREATE_RESPONSE_FORMAT L"%1!X! %2!s!"
/* Reserve the complete DWORD status, separator and NUL-terminated instance ID.
 * The current success status is one digit; budgeting its full width keeps the
 * producer and transport on one capacity contract. No CRLF is emitted. */
#define PB_HELPER_CREATE_RESPONSE_CAPACITY (PB_HELPER_STATUS_HEX_CHARS + 1u + MAX_DEVICE_ID_LEN)
