/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the unchanged native-policy ledger and full actual session/lifecycle.
 * SDK waits/locks, module pins and private pool are real; driver I/O is hosted. */
#ifdef PB_API_TESTS
#error Drain/policy requires the actual non-test DLL
#endif
#include "guard_memory.h"
#include "../model/drain_policy_fixture.h"
static DP_CONFIG *Config;
static DWORD WINAPI OwnerWait(DWORD,const HANDLE *,BOOL,DWORD);
static BOOL WINAPI DrainSleep(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
#ifndef PB_DRAIN_POLICY_SESSION_SOURCE
#error Build through Build-DrainPolicyTests for explicit source binding
#endif
#include "native_policy_fixture.c"
static DWORD WINAPI OwnerWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Config && Current && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent);
    ULONG queue;for(queue=0;queue<Config->Queues && events[1]!=Current->Queues[queue].UserEvent;++queue){}
    CHECK(queue<Config->Queues && Config->Owners[queue]==(LONG)GetCurrentThreadId());
    /* A selected SDK wait entry, not an opaque kernel scheduler-park claim. */
    CHECK(SetEvent(Config->WaitEntered[queue]));
    return WaitForMultipleObjects(count,events,all,timeout);
}
static BOOL WINAPI DrainSleep(PCONDITION_VARIABLE cv,PSRWLOCK lock,DWORD timeout,ULONG flags)
{
    if(Config && Config->DrainThread==(LONG)GetCurrentThreadId()) {
        CHECK(Current && cv==&Current->Changed && lock==&Current->State && timeout==INFINITE && !flags);
        LONG call=InterlockedIncrement(&Config->SleepCalls);CHECK(SetEvent(Config->SleepEntered));
        /* A defensive single failure input; keep the API's held-lock contract.
         * On an erroneous retry call the real SDK, which releases/reacquires
         * State. No hard gate or session mutation is introduced while locked. */
        if(Config->FailSleep && call==1) {
            CHECK(InterlockedIncrement(&Config->SleepFailures)==1);SetLastError(ERROR_OPERATION_ABORTED);return FALSE;
        }
        if(Config->FailSleep) {
            InterlockedIncrement(&Config->SleepRetries);CHECK(SetEvent(Config->RetryEntered));
        }
    }
    return SleepConditionVariableSRW(cv,lock,timeout,flags);
}
__declspec(dllexport) VOID WINAPI PrepareDrainPolicy(DP_CONFIG *config)
{CHECK(config && !Config);Config=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI MarkDrainOwner(ULONG queue)
{CHECK(queue<Config->Queues && !Config->Owners[queue]);Config->Owners[queue]=(LONG)GetCurrentThreadId();}
__declspec(dllexport) VOID WINAPI PublishDrainIncoming(VOID)
{
    CHECK(Current && Current->Layout.QueueCount==Config->Queues);
    for(ULONG queue=0;queue<Config->Queues;++queue) {
        PB_DESCRIPTOR descriptor={0};descriptor.Token=(PB_TOKEN){Current->Epoch,1,0,(USHORT)queue};descriptor.Length=20;
        descriptor.DataOffset=Current->Layout.Queues[queue].TxData;memset(Current->Region+descriptor.DataOffset,(int)queue,20);
        PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,queue,PB_QUEUE_TX_READY,TRUE)==PB_OK);
        uint32_t count,notify;CHECK(PbQueuePush(&producer,&descriptor,1,&count,&notify)==PB_OK && count==1 && !notify);
    }
}
