/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../common/platform.h"
#ifndef PB_NBL_VERSION
/* Hosted DDI fixtures model the current platform unless explicitly overridden. */
#define PB_NBL_VERSION PB_NDIS_MAXIMUM
#define PB_NBL_HOST_CURRENT 1
#endif
/* New WDK headers describe a larger NBL information array than older kernels
 * allocate. Even clearing an unavailable field is an out-of-bounds write. */
#define PB_NBL_GET(nbl, field, minor) \
    (PB_NBL_VERSION >= PB_NDIS_VERSION(minor) ? NET_BUFFER_LIST_INFO(nbl, field) : NULL)
#ifdef PB_NBL_HOST_CURRENT
#define PB_NBL_SET(nbl, field, minor, value) (NET_BUFFER_LIST_INFO(nbl, field) = (value))
#else
#define PB_NBL_SET(nbl, field, minor, value) do { \
    if (PB_NBL_VERSION >= PB_NDIS_VERSION(minor)) NET_BUFFER_LIST_INFO(nbl, field) = (value); \
} while (0)
#endif
