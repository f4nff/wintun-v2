/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include "../../api/adapter_name.h"
#include "../safety/guard_memory.h"
int main(void)
{
    PB_TEST_GUARD source=PbTestGuardCreate(260),dest=PbTestGuardCreate(258);
    WCHAR *in=(WCHAR *)PbTestGuardSpan(&source,260),*out=(WCHAR *)PbTestGuardSpan(&dest,258);
    SAFETY_CHECK(!CopyOperationName(out,NULL)&&GetLastError()==ERROR_INVALID_PARAMETER);
    for(unsigned length=0;length<=129;++length) {
        for(unsigned i=0;i<length;++i)in[i]=(WCHAR)(0x4e00+i);
        in[length]=0;memset(out,0xa5,258);
        BOOL okay=CopyOperationName(out,in);
        SAFETY_CHECK(okay==(length>=1&&length<=128));
        if(okay)SAFETY_CHECK(!memcmp(in,out,(length+1)*sizeof(WCHAR)));
        else for(unsigned i=0;i<258;++i)SAFETY_CHECK(((BYTE *)out)[i]==0xa5);
    }
    /* Unterminated source exactly at the documented bounded-read limit. */
    in=(WCHAR *)PbTestGuardSpan(&source,258);for(unsigned i=0;i<129;++i)in[i]=L'x';
    SAFETY_CHECK(!CopyOperationName(out,in));
    PbTestGuardDestroy(&source);PbTestGuardDestroy(&dest);
    puts("PASS: guarded shared-installer names 0..129, Unicode, legacy 127/v2 128 boundary, null and unterminated inputs; no truncated success or out-of-bounds access.");return 0;
}
