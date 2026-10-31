/* SPDX-License-Identifier: GPL-2.0 */
/* Verify the production callout wrappers with DDI stand-ins; no real NDIS is invoked. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ExitProcess(1); } } while(0)
typedef VOID *NDIS_HANDLE;
typedef struct { ULONG Unused; } NET_BUFFER_LIST;
typedef UCHAR KIRQL;
#define DISPATCH_LEVEL 2
#define NDIS_DEFAULT_PORT_NUMBER 0
#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL 1
#define NDIS_RECEIVE_FLAGS_RESOURCES 2
#define NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL 1
static KIRQL CurrentIrql;
static ULONG Receives, Sends, ExpectedFlags;
static VOID KeRaiseIrql(KIRQL level,KIRQL *saved) { CHECK(CurrentIrql<=level && level==DISPATCH_LEVEL); *saved=CurrentIrql; CurrentIrql=level; }
static VOID KeLowerIrql(KIRQL saved) { CHECK(CurrentIrql>=saved); CurrentIrql=saved; }
static VOID NdisMIndicateReceiveNetBufferLists(NDIS_HANDLE adapter,NET_BUFFER_LIST *lists,ULONG port,ULONG count,ULONG flags)
{ CHECK(adapter && lists && port==NDIS_DEFAULT_PORT_NUMBER && count==32 && CurrentIrql==DISPATCH_LEVEL && flags==ExpectedFlags); Receives++; }
static VOID NdisMSendNetBufferListsComplete(NDIS_HANDLE adapter,NET_BUFFER_LIST *lists,ULONG flags)
{ CHECK(adapter && lists && CurrentIrql==DISPATCH_LEVEL && flags==NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL); Sends++; }
#include "../../driver/v2/ndis_callouts.h"
int main(VOID)
{
    NET_BUFFER_LIST lists={0};
    for (KIRQL level=0;level<=DISPATCH_LEVEL;++level) {
        CurrentIrql=level;
        ExpectedFlags=NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL;
        PbIndicateReceive(&lists,&lists,32,0); CHECK(CurrentIrql==level);
        ExpectedFlags=NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL|NDIS_RECEIVE_FLAGS_RESOURCES;
        PbIndicateReceive(&lists,&lists,32,NDIS_RECEIVE_FLAGS_RESOURCES); CHECK(CurrentIrql==level);
        PbCompleteSend(&lists,&lists); CHECK(CurrentIrql==level);
    }
    CHECK(Receives==6 && Sends==3);
    puts("PASS: actual NDIS callout wrappers, DISPATCH flags, resource flag preservation and IRQL restoration from 0/1/2 (hosted DDI stand-ins).");
    return 0;
}
