/* SPDX-License-Identifier: GPL-2.0 */
/* Actual V2 DLL unload test. No API implementation is linked into this host. */
#include "guard_memory.h"
#include "../../api/v2/lifecycle.h"
#include "../../api/v2/test.h"
typedef PB_RESULT (WINAPI *GET_API)(uint32_t,uint32_t,PB_INTERFACE_V2 *);
typedef PB_RESULT (WINAPI *GET_LIFE)(uint32_t,uint32_t,PB_LIFECYCLE_INTERFACE *);
typedef PB_RESULT (*OPEN)(const PB_CONFIG *,PB_SESSION **,void **,PB_LAYOUT *,uint64_t *);
typedef void (*BLOCK)(PB_SESSION *,uint32_t,HANDLE,HANDLE);
int wmain(int argc,WCHAR **argv)
{
    SAFETY_CHECK(argc==2);
    for(ULONG i=0;i<32;++i) {
        SAFETY_CHECK(!GetModuleHandleW(argv[1]));
        HMODULE library=LoadLibraryW(argv[1]);SAFETY_CHECK(library);
        GET_API get;GET_LIFE lifecycle;OPEN open;BLOCK block;
        *(FARPROC *)&get=GetProcAddress(library,"PbGetInterfaceV2");
        *(FARPROC *)&lifecycle=GetProcAddress(library,"PbGetLifecycleInterface");
        *(FARPROC *)&open=GetProcAddress(library,"PbTestOpen");
        *(FARPROC *)&block=GetProcAddress(library,"PbTestBlock");SAFETY_CHECK(get&&lifecycle&&open&&block);
        PB_INTERFACE_V2 api;PB_LIFECYCLE_INTERFACE life;
        SAFETY_CHECK(get(2,sizeof(api),&api)==PB_OK&&lifecycle(1,sizeof(life),&life)==PB_OK);
        PB_CONFIG config={sizeof(config),1,4,4,0,0};PB_SESSION *s;void *region;PB_LAYOUT layout;uint64_t epoch;
        SAFETY_CHECK(open(&config,&s,&region,&layout,&epoch)==PB_OK);
        HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);SAFETY_CHECK(entered&&gate);
        block(s,1+i%4,entered,gate);
        SAFETY_CHECK(api.Close(s,0)==PB_QUARANTINED&&WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
        life.ReleaseSession(s);SAFETY_CHECK(FreeLibrary(library));
        SAFETY_CHECK(GetModuleHandleW(argv[1])); /* Both core and callback own independent pins. */
        SAFETY_CHECK(SetEvent(gate));ULONGLONG start=GetTickCount64();
        while(GetModuleHandleW(argv[1])) { SAFETY_CHECK(GetTickCount64()-start<5000);SwitchToThread(); }
        SAFETY_CHECK(CloseHandle(entered)&&CloseHandle(gate));
    }
    puts("PASS: 32 actual V2 DLL unload cycles with blocked drain/query/device/adapter close, detached session, core and callback return-tail module references fully released.");return 0;
}
