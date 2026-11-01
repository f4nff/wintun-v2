/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#define SAFETY_CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
typedef struct { uint8_t *Base,*End; SIZE_T Capacity; } PB_TEST_GUARD;
static inline PB_TEST_GUARD PbTestGuardCreate(SIZE_T bytes)
{
    SYSTEM_INFO info; GetSystemInfo(&info); SIZE_T page=info.dwPageSize;
    SAFETY_CHECK(bytes<=256u*1024u*1024u);
    SIZE_T capacity=((bytes?bytes:1)+page-1)/page*page;
    uint8_t *base=VirtualAlloc(NULL,capacity+2*page,MEM_RESERVE,PAGE_NOACCESS); SAFETY_CHECK(base);
    SAFETY_CHECK(VirtualAlloc(base+page,capacity,MEM_COMMIT,PAGE_READWRITE)==base+page);
    PB_TEST_GUARD result={base,base+page+capacity,capacity}; return result;
}
static inline uint8_t *PbTestGuardSpan(PB_TEST_GUARD *guard,SIZE_T bytes)
{ SAFETY_CHECK(bytes<=guard->Capacity); return guard->End-bytes; }
static inline void PbTestGuardDestroy(PB_TEST_GUARD *guard)
{ SAFETY_CHECK(VirtualFree(guard->Base,0,MEM_RELEASE)); guard->Base=guard->End=NULL; }
static inline void *PbTestGuardAlloc(SIZE_T bytes)
{ PB_TEST_GUARD guard=PbTestGuardCreate(bytes); return PbTestGuardSpan(&guard,bytes); }
static inline void PbTestGuardFree(void *pointer)
{ MEMORY_BASIC_INFORMATION info; SAFETY_CHECK(VirtualQuery(pointer,&info,sizeof(info))==sizeof(info)); SAFETY_CHECK(VirtualFree(info.AllocationBase,0,MEM_RELEASE)); }
