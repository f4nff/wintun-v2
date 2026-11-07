/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* DriverEntry initializes WDK's downlevel NX opt-in before any mapping. Its
 * MmMapLockedPagesSpecifyCache inline adds ExDefaultMdlProtection: zero before
 * Windows 8, MdlMappingNoExecute on newer systems. An unconditional NX flag
 * bypasses that compatibility decision. Keep both MDL mapping sites here. */
#if !POOL_NX_OPTIN || POOL_NX_OPTOUT || POOL_NX_OPTIN_AUTO
#error PacketBridge requires runtime POOL_NX_OPTIN for downlevel MDL mappings
#endif
static __forceinline UCHAR *PbV2MapMdl(MDL *mdl)
{
    return MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
}
