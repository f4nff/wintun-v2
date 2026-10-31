/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* No transition/queue lock is held here. Keep upper callbacks at DISPATCH so PASSIVE control
 * requests are queued instead of synchronously waiting for their own indication to return. */
static VOID PbIndicateReceive(NDIS_HANDLE adapter, NET_BUFFER_LIST *lists, ULONG count, ULONG flags)
{
    KIRQL saved;
    KeRaiseIrql(DISPATCH_LEVEL, &saved);
    NdisMIndicateReceiveNetBufferLists(adapter, lists, NDIS_DEFAULT_PORT_NUMBER, count, flags | NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
    KeLowerIrql(saved);
}
static VOID PbCompleteSend(NDIS_HANDLE adapter, NET_BUFFER_LIST *lists)
{
    KIRQL saved;
    KeRaiseIrql(DISPATCH_LEVEL, &saved);
    NdisMSendNetBufferListsComplete(adapter, lists, NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL);
    KeLowerIrql(saved);
}
