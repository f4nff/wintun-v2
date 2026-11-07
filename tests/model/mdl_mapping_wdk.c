/* SPDX-License-Identifier: GPL-2.0 */
/* This translation unit uses the real WDK MDL, runtime initialization and
 * mapping inlines. Only the two imported kernel calls are hosted stand-ins. */
#include <ntddk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s os=%lu.%lu\n",__FILE__,__LINE__,#x,Major,Minor);exit(1); } } while(0)
static ULONG Major,Minor,Calls,FailAt,Unsupported;
static BOOLEAN VersionFailed;
static PVOID NTAPI TestRoutine(PUNICODE_STRING name)
{
    static const WCHAR expected[]=L"NtQuerySystemInformation";
    CHECK(name->Length==sizeof(expected)-sizeof(WCHAR)&&!memcmp(name->Buffer,expected,name->Length));
    return NULL; /* Native pool-zeroing discovery is independent of NX mapping. */
}
static NTSTATUS NTAPI TestVersion(PRTL_OSVERSIONINFOW v)
{
    CHECK(v->dwOSVersionInfoSize==sizeof(*v));
    if(VersionFailed)return STATUS_UNSUCCESSFUL;
    v->dwMajorVersion=Major;v->dwMinorVersion=Minor;v->dwBuildNumber=7601;return STATUS_SUCCESS;
}
static PVOID NTAPI TestMap(PMDL mdl,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,PVOID address,ULONG crash,ULONG priority)
{
    CHECK(mode==KernelMode&&cache==MmCached&&!address&&!crash);Calls++;
    BOOLEAN nx=!VersionFailed&&(Major>6||(Major==6&&Minor>=2));
    ULONG expected=NormalPagePriority|(nx?MdlMappingNoExecute:0);
    if(priority!=expected){Unsupported++;return NULL;}
    return FailAt==Calls?NULL:mdl->MappedSystemVa;
}
/* Resolve dllimport through the same IAT indirection emitted for the driver. */
NTSTATUS (NTAPI *__imp_RtlGetVersion)(PRTL_OSVERSIONINFOW)=TestVersion;
PVOID (NTAPI *__imp_MmGetSystemRoutineAddress)(PUNICODE_STRING)=TestRoutine;
PVOID (NTAPI *__imp_MmMapLockedPagesSpecifyCache)(PMDL,KPROCESSOR_MODE,MEMORY_CACHING_TYPE,PVOID,ULONG,ULONG)=TestMap;
typedef struct TEST_NET_BUFFER {MDL *Mdl;ULONG Offset,Length;} NET_BUFFER;
#define NET_BUFFER_DATA_LENGTH(nb) ((nb)->Length)
#define NET_BUFFER_CURRENT_MDL(nb) ((nb)->Mdl)
#define NET_BUFFER_CURRENT_MDL_OFFSET(nb) ((nb)->Offset)
#ifndef PB_MDL_READ_SOURCE
#define PB_MDL_READ_SOURCE "../../driver/v2/session_read.h"
#endif
#include PB_MDL_READ_SOURCE
uint32_t PbExerciseMdlMapping(uint32_t major,uint32_t minor,int versionFailure,UCHAR *first,UCHAR *second,UCHAR *out)
{
    Major=major;Minor=minor;VersionFailed=(BOOLEAN)versionFailure;
    ExDefaultMdlProtection=0;ExDefaultNonPagedPoolType=NonPagedPoolExecute;
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    BOOLEAN nx=!VersionFailed&&(Major>6||(Major==6&&Minor>=2));
    CHECK(ExDefaultMdlProtection==(nx?(ULONG)MdlMappingNoExecute:0u));
    CHECK((ULONG)ExDefaultNonPagedPoolType==(ULONG)(nx?NonPagedPoolNx:NonPagedPoolExecute));
    MDL a={0},b={0},empty={0};a.ByteCount=b.ByteCount=32;a.MappedSystemVa=first;b.MappedSystemVa=second;a.Next=&empty;empty.Next=&b;
    UCHAR expected[64];memcpy(expected,first,32);memcpy(expected+32,second,32);uint32_t cases=0;
#ifdef PB_MDL_DIRECT_MAPPING
    Calls=FailAt=Unsupported=0;CHECK(PbV2MapMdl(&a)==first&&Calls==1&&!Unsupported);cases++;
    Calls=0;FailAt=1;CHECK(!PbV2MapMdl(&a)&&Calls==1&&!Unsupported);cases++;FailAt=0;
#endif
    NET_BUFFER nb={&a,0,64};
    for(ULONG offset=0;offset<=32;++offset) {
        nb.Offset=offset;nb.Length=64-offset;
        for(ULONG skip=0;skip<=nb.Length;++skip)for(ULONG size=0;size<=nb.Length-skip;++size) {
            memset(out,0xcc,64);Calls=Unsupported=FailAt=0;
            CHECK(PbV2Read(&nb,skip,out,size));CHECK(!Unsupported&&!memcmp(out,expected+offset+skip,size));
            for(ULONG j=size;j<64;++j)CHECK(out[j]==0xcc);
            CHECK(Calls<=2);cases++;
        }
    }
    nb.Offset=0;nb.Length=64;
    for(ULONG failure=1;failure<=2;++failure) {
        memset(out,0xcc,64);Calls=Unsupported=0;FailAt=failure;
        CHECK(!PbV2Read(&nb,0,out,64)&&Calls==failure&&!Unsupported);
        CHECK(failure!=2||!memcmp(out,first,32));cases++;
    }
    FailAt=0;
    ULONG mappedFlags[]={MDL_MAPPED_TO_SYSTEM_VA,MDL_SOURCE_IS_NONPAGED_POOL};
    for(ULONG i=0;i<2;++i) {
        a.MdlFlags=b.MdlFlags=(CSHORT)mappedFlags[i];Calls=0;
        CHECK(PbV2Read(&nb,0,out,64)&&!Calls&&!memcmp(out,expected,64));cases++;
    }
    a.MdlFlags=b.MdlFlags=0;Calls=0;
    CHECK(!PbV2Read(&nb,65,out,0)&&!PbV2Read(&nb,0,out,65)&&!Calls);cases+=2;
    nb.Offset=33;CHECK(!PbV2Read(&nb,0,out,1)&&!Calls);cases++;
    nb.Offset=0;b.Next=NULL;nb.Length=65;CHECK(!PbV2Read(&nb,0,out,65));cases++;
    nb.Mdl=NULL;nb.Length=0;Calls=0;CHECK(PbV2Read(&nb,0,out,0)&&!Calls);cases++;
    nb.Length=1;CHECK(!PbV2Read(&nb,0,out,1)&&!Calls);cases++;
    return cases;
}
