/* SPDX-License-Identifier: GPL-2.0 */
/* Production callback/wait helper, real thread/events, mocked software-device close barrier. */
#include "guard_memory.h"
#include <swdevice.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
typedef struct { HSWDEVICE Device; } PB_ADAPTER;
typedef struct { HANDLE Thread,Cancel,Gate; VOID *Context; ULONG Mode; } MOCK_DEVICE;
static VOID WINAPI Created(HSWDEVICE,HRESULT,PVOID,PCWSTR);
static DWORD WINAPI CallbackThread(VOID *);
static ULONG CloseCalls,Callbacks;
static HANDLE CurrentEvent;
static ULONG CurrentMode;
static DWORD WINAPI MockWait(HANDLE event,DWORD timeout)
{ if(CurrentMode==4 && event==CurrentEvent) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; } return WaitForSingleObject(event,timeout); }
static VOID WINAPI MockClose(HSWDEVICE handle)
{
    MOCK_DEVICE *device=(MOCK_DEVICE *)handle; CloseCalls++;
    if(device->Thread) {
        SAFETY_CHECK(SetEvent(device->Mode==3?device->Gate:device->Cancel));
        SAFETY_CHECK(WaitForSingleObject(device->Thread,5000)==WAIT_OBJECT_0);
        SAFETY_CHECK(CloseHandle(device->Thread));
    }
    SAFETY_CHECK(CloseHandle(device->Cancel) && CloseHandle(device->Gate)); free(device);
}
#define SwDeviceClose MockClose
#define WaitForSingleObject MockWait
#define PB_ADAPTER_CREATE_TIMEOUT_MS 10u
#include "../../api/v2/adapter_create_wait.h"
#undef SwDeviceClose
#undef WaitForSingleObject
static DWORD WINAPI CallbackThread(VOID *arg)
{
    MOCK_DEVICE *device=arg; HANDLE events[]={device->Cancel,device->Gate};
    DWORD selected=WaitForMultipleObjects(2,events,FALSE,5000); SAFETY_CHECK(selected==WAIT_OBJECT_0 || selected==WAIT_OBJECT_0+1);
    if(selected==WAIT_OBJECT_0+1) { Callbacks++; Created((HSWDEVICE)device,S_OK,device->Context,L"SWD\\PacketBridgeV2\\test"); }
    return 0;
}
int main(void)
{
    DWORD before,after; SAFETY_CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(ULONG mode=0;mode<8;++mode) {
        PB_CREATE ctx={.Event=CreateEventW(NULL,TRUE,FALSE,NULL),.Result=E_PENDING}; SAFETY_CHECK(ctx.Event);
        MOCK_DEVICE *device=calloc(1,sizeof(*device)); SAFETY_CHECK(device);
        device->Cancel=CreateEventW(NULL,TRUE,FALSE,NULL); device->Gate=CreateEventW(NULL,TRUE,FALSE,NULL);
        SAFETY_CHECK(device->Cancel && device->Gate); device->Context=&ctx; device->Mode=mode;
        PB_ADAPTER adapter={(HSWDEVICE)device}; CurrentEvent=ctx.Event; CurrentMode=mode; ULONG oldCloses=CloseCalls;
        if(mode==0 || mode==1 || mode==5 || mode==6 || mode==7) {
            WCHAR huge[1025]; for(ULONG i=0;i<1024;++i) huge[i]=L'x'; huge[1024]=0;
            Created(adapter.Device,mode==1 || mode==7?E_FAIL:S_OK,&ctx,mode==5?NULL:mode==6 || mode==7?huge:L"SWD\\PacketBridgeV2\\test");
        } else { device->Thread=CreateThread(NULL,0,CallbackThread,device,0,NULL); SAFETY_CHECK(device->Thread); }
        HRESULT result=PbWaitForAdapterCreate(&adapter,&ctx);
        if(mode==0) SAFETY_CHECK(result==S_OK && adapter.Device);
        else if(mode==1 || mode==7) SAFETY_CHECK(result==E_FAIL && adapter.Device);
        else if(mode==5 || mode==6) SAFETY_CHECK(result==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && adapter.Device);
        else { SAFETY_CHECK(result==HRESULT_FROM_WIN32(mode==4?ERROR_INVALID_HANDLE:ERROR_TIMEOUT) && !adapter.Device && CloseCalls==oldCloses+1); }
        if(adapter.Device) MockClose(adapter.Device);
        SAFETY_CHECK(CloseCalls==oldCloses+1 && CloseHandle(ctx.Event));
    }
    SAFETY_CHECK(Callbacks==1 && GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("PASS: production adapter callback/wait, eight success/failure/timeout/cancel-race/invalid-instance cases, original failure HRESULT, callback close barrier, joined threads and zero handle delta.");
    return 0;
}
