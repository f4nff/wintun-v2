/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

/* The build supplies these from wintun.props, the single PE version source. */
#ifndef WINTUN_VERSION_MAJ
#error Build using tools/build/build.ps1 so the version is supplied from wintun.props
#endif

#define PB_VERSION_MAJOR WINTUN_VERSION_MAJ
#define PB_VERSION_MINOR WINTUN_VERSION_MIN
#define PB_VERSION_PATCH WINTUN_VERSION_REL
#define PB_VERSION_BUILD 0
#define PB_VERSION_PACKED ((PB_VERSION_MAJOR << 24) | (PB_VERSION_MINOR << 16) | (PB_VERSION_PATCH << 8))
#define PB_STRINGIZE_INNER(x) #x
#define PB_STRINGIZE(x) PB_STRINGIZE_INNER(x)
#define PB_VERSION_STRING PB_STRINGIZE(PB_VERSION_MAJOR) "." PB_STRINGIZE(PB_VERSION_MINOR) "." PB_STRINGIZE(PB_VERSION_PATCH)
#ifndef PB_SOURCE_FINGERPRINT
#define PB_SOURCE_FINGERPRINT unknown
#endif
#define PB_SOURCE_STRING PB_STRINGIZE(PB_SOURCE_FINGERPRINT)

#define PB_PRODUCT_NAME "PacketBridgeV2"
#define PB_PRODUCT_NAME_W L"PacketBridgeV2"
#define PB_DRIVER_FILENAME "packetbridge.sys"
#define PB_DRIVER_FILENAME_W L"packetbridge.sys"
