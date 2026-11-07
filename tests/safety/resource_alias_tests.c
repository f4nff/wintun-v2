/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../api/resource.h"
HINSTANCE ResourceModule;
SECURITY_ATTRIBUTES SecurityAttributes;
DWORD LoggerLogV(int level,LPCWSTR format,va_list args)
{(void)level;(void)format;(void)args;return GetLastError();}
DWORD LoggerErrorV(DWORD error,LPCWSTR format,va_list args)
{(void)format;(void)args;SetLastError(error);return error;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2);ResourceModule=LoadLibraryExW(argv[1],NULL,LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);CHECK(ResourceModule);
    const WCHAR *names[]={L"packetbridge.sys",L"packetbridge.inf",L"packetbridge.cat"};
    for(unsigned i=0;i<3;++i){DWORD a=0,b=0;const void *p=ResourceGetAddress(names[i],&a),*q=ResourceGetAddress(MAKEINTRESOURCEW(101+i),&b);CHECK(p&&p==q&&a==b&&a);}
    DWORD bytes=99;CHECK(!ResourceGetAddress(L"missing",&bytes));
    CHECK(FreeLibrary(ResourceModule));puts("PASS: legacy installer aliases and numeric v2 resources address the same single SYS/INF/CAT package.");return 0;
}
