/* SPDX-License-Identifier: GPL-2.0 */
/* Independently script wait results around a real live thread. An error is not
 * an exit witness, even if GetLastError is zero or later failures differ. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);ExitProcess(1);}}while(0)
static HANDLE Target,Gate,Entered;
static DWORD Results[64],Errors[64],Count,Index,Backoffs;
static DWORD WINAPI Thread(void *opaque)
{(void)opaque;CHECK(SetEvent(Entered)&&WaitForSingleObject(Gate,5000)==WAIT_OBJECT_0);return 0;}
static DWORD WINAPI ScriptWait(HANDLE object,DWORD timeout)
{
    CHECK(object==Target&&timeout==INFINITE);
    if(Index<Count){CHECK(WaitForSingleObject(Target,0)==WAIT_TIMEOUT);SetLastError(Errors[Index]);return Results[Index++];}
    return WaitForSingleObject(object,timeout);
}
static VOID WINAPI ScriptSleep(DWORD timeout)
{
    CHECK(timeout==1000&&Index==++Backoffs&&WaitForSingleObject(Target,0)==WAIT_TIMEOUT);
    if(Index==Count)CHECK(SetEvent(Gate));
}
#define WaitForSingleObject ScriptWait
#define Sleep ScriptSleep
#include "../../api/termination_wait.h"
#undef WaitForSingleObject
#undef Sleep
static void Run(DWORD count,DWORD expected)
{
    Count=count;Index=Backoffs=0;Gate=CreateEventW(NULL,TRUE,count==0,NULL);Entered=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Gate&&Entered);
    Target=CreateThread(NULL,0,Thread,NULL,0,NULL);CHECK(Target&&WaitForSingleObject(Entered,5000)==WAIT_OBJECT_0);
    CHECK(WaitForOwnedTermination(Target)==expected&&Index==count&&Backoffs==count&&WaitForSingleObject(Target,0)==WAIT_OBJECT_0);
    CHECK(CloseHandle(Target)&&CloseHandle(Gate)&&CloseHandle(Entered));
}
int main(void)
{
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Run(0,ERROR_SUCCESS);
    Results[0]=WAIT_FAILED;Errors[0]=ERROR_NOT_ENOUGH_MEMORY;Run(1,ERROR_NOT_ENOUGH_MEMORY);
    Errors[0]=0;Run(1,ERROR_GEN_FAILURE);
    Results[1]=WAIT_FAILED;Errors[1]=ERROR_INVALID_HANDLE;Run(2,ERROR_GEN_FAILURE);
    Errors[0]=ERROR_NOT_ENOUGH_MEMORY;Run(2,ERROR_NOT_ENOUGH_MEMORY);
    Results[0]=WAIT_TIMEOUT;Run(2,ERROR_GEN_FAILURE);
    Results[0]=WAIT_ABANDONED_0;Run(2,ERROR_GEN_FAILURE);
    for(unsigned i=0;i<64;++i){Results[i]=i&1?WAIT_TIMEOUT:WAIT_FAILED;Errors[i]=i?ERROR_INVALID_HANDLE:ERROR_NOT_ENOUGH_MEMORY;}
    Run(64,ERROR_NOT_ENOUGH_MEMORY);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    puts("PASS: real owned thread termination, normal wait, zero/mixed errors, unexpected statuses and 64 consecutive failed waits; first error retained, every failure backs off, thread remains live until explicit completion, no handle delta.");return 0;
}
