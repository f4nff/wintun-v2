/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifdef PACKETBRIDGE_KERNEL
/* Avoid mixing the WDK kernel CRT with the user-mode MSVC stdint.h. */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef unsigned __int64 uintptr_t;
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffull
#endif
#else
#include <stdint.h>
#endif
