/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "adapter.h"
#include <wchar.h>
#include <string.h>
static BOOL CopyOperationName(WCHAR *destination, LPCWSTR source)
{
    SIZE_T length = source ? wcsnlen_s(source, PB_MAX_ADAPTER_NAME) : 0;
    if (!destination || !length || length >= PB_MAX_ADAPTER_NAME) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    memcpy(destination, source, (length + 1) * sizeof(WCHAR)); return TRUE;
}
