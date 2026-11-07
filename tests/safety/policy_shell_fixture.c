/* SPDX-License-Identifier: GPL-2.0 */
/* Unchanged native-policy host/ledger; actual non-test DLL policy wait and physical close. */
#ifdef PB_API_TESTS
#error Actual native DLL required
#endif
#include "guard_memory.h"
#include "../model/policy_shell_fixture.h"
static PS_CONFIG *Config;
static BOOL WINAPI PolicySleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
#ifndef PB_POLICY_SHELL_SESSION_SOURCE
#error Build through Build-PolicyShellTests for explicit source binding
#endif
#include "native_policy_fixture.c"
static BOOL WINAPI PolicySleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    ULONG index=0;BOOL selected=FALSE;
    if(Config)for(;index<Config->Pollers;++index)if(Config->Threads[index]==(LONG)GetCurrentThreadId()){selected=TRUE;break;}
    if(selected){CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);InterlockedIncrement(&Config->Sleeps[index]);CHECK(SetEvent(Config->Waiting[index]));}
    BOOL result=SleepConditionVariableSRW(cv,lock,timeout,flags);DWORD error=GetLastError();
    if(selected && Current->Progress.PolicyCompleted==1 && !InterlockedExchange(Config->Gated+index,1)) {
        /* Another reader can win State reacquisition after a genuine wake.
         * Restore the SDK-held lock before production rechecks state. */
        ReleaseSRWLockExclusive(lock);CHECK(SetEvent(Config->Resumed[index]));
        CHECK(WaitForSingleObject(Config->Gates[index],10000)==WAIT_OBJECT_0);AcquireSRWLockExclusive(lock);
    }
    if(!result)SetLastError(error);return result;
}
__declspec(dllexport) VOID WINAPI PreparePolicyShell(PS_CONFIG *config)
{CHECK(config && !Config);Config=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI MarkPolicyReader(ULONG index)
{CHECK(index<Config->Pollers && !Config->Threads[index]);Config->Threads[index]=(LONG)GetCurrentThreadId();}
__declspec(dllexport) VOID WINAPI WaitPolicyShellCallback(VOID)
{
    /* The externally owned public reference protects this shell and Work.
     * Finish the real callback before measuring its remaining sole owner. */
    CHECK(Current && !Current->PublicReleased);WaitForThreadpoolWorkCallbacks(Current->Work,FALSE);
    CHECK(!ActiveCallbacks && CallbackCount && Finished==CallbackCount);
}
