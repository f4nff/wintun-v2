/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../../api/wintun.h"
#include "../../api/v2/packetbridge.h"
#include "../../api/v2/lifecycle.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s error=%lu\n",__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2); DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    const char *exports[]={"WintunAllocateSendPacket","WintunCreateAdapter","WintunEndSession","WintunOpenAdapter",
        "WintunCloseAdapter","WintunGetAdapterLUID","WintunGetReadWaitEvent","WintunGetRunningDriverVersion",
        "WintunReceivePacket","WintunReleaseReceivePacket","WintunSendPacket","WintunDeleteDriver",
        "WintunSetLogger","WintunStartSession","WintunShutdown","PbGetInterfaceV2","PbGetLifecycleInterface",
        "PbGetPacketInterface","PbCreateAdapterEx","PbCreateAdapterEx2"};
    /* Windows may initialize process-wide bcrypt/loader state on first load.
     * Verify actual module unload each time and measure 100 further cycles. */
    for(unsigned iteration=0;iteration<=100;++iteration) {
        HMODULE m=LoadLibraryExW(argv[1],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(m);
        for(unsigned i=0;i<sizeof(exports)/sizeof(exports[0]);++i)CHECK(GetProcAddress(m,exports[i]));
        CHECK(GetProcAddress(m,(LPCSTR)(ULONG_PTR)14)==GetProcAddress(m,"WintunStartSession"));
        CHECK(GetProcAddress(m,(LPCSTR)(ULONG_PTR)1)==GetProcAddress(m,"WintunAllocateSendPacket"));
        PB_RESULT (WINAPI *factory)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
        *(FARPROC *)&factory=GetProcAddress(m,"PbGetInterfaceV2"); PB_INTERFACE_V2 api;
        CHECK(factory(2,sizeof(api),&api)==PB_OK&&api.Major==2&&api.Minor>=1);
        CHECK(factory(99,sizeof(api),&api)==PB_UNSUPPORTED);
        WINTUN_START_SESSION_FUNC *start;*(FARPROC *)&start=GetProcAddress(m,"WintunStartSession");
        CHECK(!start(NULL,WINTUN_MIN_RING_CAPACITY)&&GetLastError()==ERROR_INVALID_PARAMETER);
        WINTUN_SHUTDOWN_FUNC *shutdown;*(FARPROC *)&shutdown=GetProcAddress(m,"WintunShutdown");
        CHECK(shutdown(1000));CHECK(FreeLibrary(m));
        CHECK(!GetModuleHandleW(L"wintun.dll"));
        if(!iteration) CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
    puts("PASS: shipping DLL, all 20 exports, v2 negotiation, legacy validation and 100 shutdown/unload cycles after loader warmup; zero handle delta, no installation.");return 0;
}
