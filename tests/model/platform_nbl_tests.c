/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../safety/guard_memory.h"
typedef struct { void *Info[16]; } NBL;
static uint32_t Version;
#define PB_NBL_VERSION Version
#define NET_BUFFER_LIST_INFO(n,f) ((n)->Info[f])
#include "../../driver/v2/platform_nbl.h"
int main(void)
{
    PB_TEST_GUARD guard=PbTestGuardCreate(16*sizeof(void *));
    for(uint32_t minor=20;minor<=100;++minor) {
        Version=PB_NDIS_VERSION(minor);
        /* Place the modeled old kernel's shorter NBL exactly against NOACCESS.
         * Unavailable metadata indices would fault even on a read/zero write. */
        size_t available=minor<30?1:minor<83?4:minor<89?8:16;
        NBL *n=(NBL *)PbTestGuardSpan(&guard,available*sizeof(void *));
        memset(n,0,available*sizeof(void *));
        PB_NBL_SET(n,2,30,(void *)1); PB_NBL_SET(n,5,83,(void *)2); PB_NBL_SET(n,10,89,(void *)3);
        SAFETY_CHECK(PB_NBL_GET(n,2,30)==(minor>=30?(void *)1:NULL));
        SAFETY_CHECK(PB_NBL_GET(n,5,83)==(minor>=83?(void *)2:NULL));
        SAFETY_CHECK(PB_NBL_GET(n,10,89)==(minor>=89?(void *)3:NULL));
    }
    PbTestGuardDestroy(&guard);
    puts("PASS: guarded old-kernel NBL prefixes across 81 versions; no unavailable metadata read/write."); return 0;
}
