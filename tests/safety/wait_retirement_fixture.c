/* SPDX-License-Identifier: GPL-2.0 */
/* Reuse the complete native-policy ledger and actual non-test DLL lifecycle.
 * Hosted producer publishes once; no kernel/NDIS or native slot reuse claim. */
#ifdef PB_API_TESTS
#error Wait retirement requires shipping native session paths
#endif
#include "guard_memory.h"
#include "../model/wait_retirement_fixture.h"
static WR_CONFIG *Config;
static volatile LONG Armed;
static DWORD WINAPI OwnerWait(DWORD,const HANDLE *,BOOL,DWORD);
static VOID WINAPI RetireRelease(PSRWLOCK);
static VOID WINAPI RetireAcquire(PSRWLOCK);
static BOOL WINAPI RetireIo(HANDLE,DWORD,VOID *,DWORD,VOID *,DWORD,DWORD *,LPOVERLAPPED);
#ifndef PB_WAIT_RETIREMENT_SESSION_SOURCE
#error Build through Build-WaitRetirementTests for explicit source binding
#endif
#include "native_policy_fixture.c"
static DWORD WINAPI OwnerWait(DWORD count,const HANDLE *events,BOOL all,DWORD timeout)
{
    CHECK(Config && Current && count==2 && !all && timeout==INFINITE && events[0]==Current->ClosingEvent);
    ULONG queue;for(queue=0;queue<Config->Queues && events[1]!=Current->Queues[queue].UserEvent;++queue){}
    CHECK(queue<Config->Queues && Config->Owners[queue]==(LONG)GetCurrentThreadId());
    CHECK(SetEvent(Config->Entered[queue]) && WaitForSingleObject(Config->WaitGate,10000)==WAIT_OBJECT_0);
    return WaitForMultipleObjects(count,events,all,timeout);
}
static VOID WINAPI RetireRelease(PSRWLOCK lock)
{
    if(Current && lock==&Current->DataGate && GetCurrentThreadId()==(DWORD)Config->Owners[Config->Last] && Armed &&
        !LoansNow() && InterlockedCompareExchange(&Armed,0,1)==1) {
        /* All finite retirement writes, including output count, are finished.
         * This owner still holds DataGate and Lifetime shared. Other owners
         * have already joined; no producer can modify this snapshot. */
        InterlockedExchange(&Config->TailHeld,1);CHECK(SetEvent(Config->TailEntered));
        CHECK(WaitForSingleObject(Config->TailGate,10000)==WAIT_OBJECT_0);InterlockedExchange(&Config->TailHeld,0);
    }
    ReleaseSRWLockShared(lock);
}
static VOID WINAPI RetireAcquire(PSRWLOCK lock)
{
    if(Current && lock==&Current->DataGate && Config->TailHeld && GetCurrentThreadId()==(DWORD)CallbackThread) {
        CHECK(!TryAcquireSRWLockExclusive(lock) && SetEvent(Config->DataAttempt));AcquireSRWLockExclusive(lock);return;
    }
    ObservedAcquire(lock);
}
static BOOL WINAPI RetireIo(HANDLE device,DWORD code,VOID *input,DWORD inputSize,VOID *output,DWORD outputSize,DWORD *returned,LPOVERLAPPED overlap)
{
    if(code==PB_V2_IOCTL_QUERY && Config->TailHeld) {
        CHECK(GetCurrentThreadId()==(DWORD)CallbackThread && !LoansNow() && NativeActive==1);
        CHECK(SetEvent(Config->EarlyQuery));
    }
    return HostedIo(device,code,input,inputSize,output,outputSize,returned,overlap);
}
__declspec(dllexport) VOID WINAPI PrepareRetirement(WR_CONFIG *config)
{CHECK(config && !Config);Config=config;Initialize(&config->Native);}
__declspec(dllexport) VOID WINAPI MarkRetirementOwner(ULONG queue)
{CHECK(queue<Config->Queues && !Config->Owners[queue]);Config->Owners[queue]=(LONG)GetCurrentThreadId();}
__declspec(dllexport) VOID WINAPI PublishRetirementIncoming(VOID)
{
    static const ULONG counts[]={1,2,3,7,8,31,63,64};CHECK(Current && Current->Layout.QueueCount==Config->Queues);
    for(ULONG queue=0;queue<Config->Queues;++queue) {
        PB_DESCRIPTOR descriptors[64]={0};
        for(ULONG i=0;i<counts[queue];++i) {
            descriptors[i].Token=(PB_TOKEN){Current->Epoch,1,(USHORT)i,(USHORT)queue};descriptors[i].Length=20;
            descriptors[i].DataOffset=Current->Layout.Queues[queue].TxData+i*PB_BLOCK_SIZE;memset(Current->Region+descriptors[i].DataOffset,(int)queue,20);
        }
        PB_QUEUE_VIEW producer;CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,queue,PB_QUEUE_TX_READY,TRUE)==PB_OK);
        uint32_t count,notify;CHECK(PbQueuePush(&producer,descriptors,counts[queue],&count,&notify)==PB_OK && count==counts[queue] && !notify);
    }
}
__declspec(dllexport) VOID WINAPI PublishRetirementFault(VOID)
{
    CHECK(Config->Fault && Current);PB_QUEUE_VIEW producer;
    CHECK(PbAttachQueue(&producer,Current->Region,&Current->Layout,0,PB_QUEUE_RX_COMPLETE,TRUE)==PB_OK);
    /* Defensive malformed peer input, not a valid application publication. */
    PB_DESCRIPTOR descriptor={.Token={Current->Epoch+1,1,0,0}};uint32_t count,notify;
    CHECK(PbQueuePush(&producer,&descriptor,1,&count,&notify)==PB_OK && count==1 && notify);CHECK(SetEvent(Current->Queues[0].UserEvent));
}
__declspec(dllexport) VOID WINAPI ArmRetirementTail(VOID)
{CHECK(Current && ParkHeld);InterlockedExchange(&Armed,1);}
__declspec(dllexport) VOID WINAPI InspectRetirement(ULONG stage)
{
    CHECK(Current && Regions==1 && !Current->ResourcesClosed && Current->Registered && NativeActive==1 && !LoansNow());
    if(!stage)CHECK(Config->TailHeld && !TryAcquireSRWLockExclusive(&Current->DataGate));
    else CHECK(ParkHeld && Queries && Current->Progress.Phase<PB_LIFE_PREPARED);
    CHECK(!Config->Fault || Current->Fatal);CHECK(!DeviceCloses);
}
