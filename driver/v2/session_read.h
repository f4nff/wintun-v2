/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Read only the current NET_BUFFER data, including noncontiguous MDL chains.
 * A failed read may have copied a prefix; callers must not publish its output. */
static BOOLEAN PbV2Read(NET_BUFFER *nb, ULONG skip, UCHAR *out, ULONG length)
{
    if (skip > NET_BUFFER_DATA_LENGTH(nb) || length > NET_BUFFER_DATA_LENGTH(nb) - skip) return FALSE;
    MDL *mdl = NET_BUFFER_CURRENT_MDL(nb);
    ULONG offset = NET_BUFFER_CURRENT_MDL_OFFSET(nb);
    while (mdl && length) {
        ULONG size = MmGetMdlByteCount(mdl);
        if (offset > size) return FALSE;
        ULONG available = size - offset;
        if (skip >= available) skip -= available;
        else {
            UCHAR *source = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            if (!source) return FALSE;
            ULONG take = min(available - skip, length);
            RtlCopyMemory(out, source + offset + skip, take);
            out += take; length -= take; skip = 0;
        }
        mdl = mdl->Next; offset = 0;
    }
    return length == 0;
}
