/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
uint32_t PbExerciseMdlMapping(uint32_t,uint32_t,int,UCHAR *,UCHAR *,UCHAR *);
static BYTE *Guard(SIZE_T size)
{
    BYTE *p=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(p);DWORD old;
    CHECK(VirtualProtect(p+4096,4096,PAGE_NOACCESS,&old));return p+4096-size;
}
int main(int argc,char **argv)
{
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--win7-only")));
    BYTE *first=Guard(32),*second=Guard(32),*out=Guard(64);uint32_t cases=0;
    for(unsigned i=0;i<32;++i){first[i]=(BYTE)(i+1);second[i]=(BYTE)(i+65);}
    DWORD old;CHECK(VirtualProtect(first-4064,4096,PAGE_READONLY,&old)&&VirtualProtect(second-4064,4096,PAGE_READONLY,&old));
    const uint32_t versions[][2]={{5,1},{6,0},{6,1},{6,2},{6,3},{10,0},{10,1},{11,0},{0xffffffffu,0xffffffffu}};
    if(argc==2)cases=PbExerciseMdlMapping(6,1,0,first,second,out);
    else {
        for(unsigned i=0;i<sizeof(versions)/sizeof(versions[0]);++i)cases+=PbExerciseMdlMapping(versions[i][0],versions[i][1],0,first,second,out);
        cases+=PbExerciseMdlMapping(10,0,1,first,second,out);
    }
    CHECK(VirtualFree(first-4064,0,MEM_RELEASE)&&VirtualFree(second-4064,0,MEM_RELEASE)&&VirtualFree(out-4032,0,MEM_RELEASE));
    printf("PASS: actual WDK runtime NX opt-in and production MDL read, %u guarded cases across old/current/future OS and failed version discovery; chained/empty/cached MDLs, every offset/skip/length, both mapping failures, readonly inputs and bounded writes.\n",cases);return 0;
}
