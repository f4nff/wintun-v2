/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../../common/v2_core.h"
#include "../../common/coalesce.h"
#include "../../common/flow.h"
struct _TUN_CTX;
typedef struct PB_KERNEL_SESSION PB_KERNEL_SESSION;
typedef struct PB_KERNEL_QUEUE {
    PB_KERNEL_SESSION *Session;
    ULONG Index;
    PB_QUEUE_VIEW Rings[PB_QUEUE_COUNT];
    PB_POOL TxPool;
    /* TxLock protects this private no-free-slot proof across send windows.
     * Validated recycling or failed publication releases invalidate it. */
    BOOLEAN TxPoolExhausted;
    PB_SLOT *TxSlots;
    PB_RX_SLOT *RxSlots;
    PB_DESCRIPTOR *Pending;
    UCHAR *Scratch, *CoalesceBuffers[2];
    volatile LONG CoalesceBusy[2];
    ULONG PendingHead, PendingCount, Credits;
    ULONG64 DrainProducer;
    /* Only the queue worker publishes this, after its last receive transaction.
     * Returned NBLs can still hold credits/references and their own rundown pins. */
    volatile LONG FaultQuiesced;
    KSPIN_LOCK TxLock, RxLock;
    PKEVENT KernelEvent, UserEvent;
    HANDLE Thread;
} PB_KERNEL_QUEUE;
struct PB_KERNEL_SESSION {
    struct _TUN_CTX *Ctx;
    PB_LAYOUT Layout;
    ULONG64 Epoch;
    MDL *Mdl;
    UCHAR *Region;
    PB_SHARED_LIFECYCLE *Lifecycle;
    PKEVENT ProgressEvent;
    BOOLEAN PagesLocked;
    KEVENT StartEvent, StopEvent;
    KEVENT AllReturned;
    KSPIN_LOCK ReferenceLock;
    EX_RUNDOWN_REF Returns;
    /* Active counts indicated NBLs plus admitted send calls, through upcall return. */
    volatile LONG Draining, DrainSnapshotReady, Stopping, Active, Faulted;
    ULONG64 Deadline;
    volatile LONG64 TxObjects, RxObjects, RxReturned, Indications, Completions;
    PB_KERNEL_QUEUE Queues[PB_MAX_QUEUES];
};
typedef struct PB_RX_RECORD {
    PB_KERNEL_QUEUE *Queue;
    MDL *Mdl;
    MDL *HeaderMdl;
    UCHAR Ethernet[14];
    ULONG Count, CoalesceIndex, TcpPayloadLength;
    ULONG IndicationLength, IpDeclaredLength;
    UCHAR IpVersion, Protocol, ReceiveType, LocallyCancelled;
    PB_DESCRIPTOR Descriptors[1];
} PB_RX_RECORD;
