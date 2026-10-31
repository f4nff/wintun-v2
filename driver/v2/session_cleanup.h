/* SPDX-License-Identifier: GPL-2.0 */
/* Included by the driver and by the hosted resource-lifetime regression harness. */
#pragma once
#include "session_progress.h"

static VOID PbV2Free(PB_KERNEL_SESSION *s)
{
    if (!s) return;
    PbV2Progress(s, TRUE);
    InterlockedExchange(&s->Stopping, 1); KeSetEvent(&s->StartEvent, IO_NO_INCREMENT, FALSE); KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
    for (ULONG i = 0; i < s->Layout.QueueCount; ++i) if (s->Queues[i].Thread) {
        ZwWaitForSingleObject(s->Queues[i].Thread, FALSE, NULL); ZwClose(s->Queues[i].Thread);
    }
    /* Returns may be reentrant. They never acquire TransitionLock. */
    KeWaitForSingleObject(&s->AllReturned, Executive, KernelMode, FALSE, NULL);
    ExWaitForRundownProtectionRelease(&s->Returns);
    if (s->ProgressEvent) ObDereferenceObject(s->ProgressEvent);
    for (ULONG i = 0; i < s->Layout.QueueCount; ++i) {
        if (s->Queues[i].KernelEvent) ObDereferenceObject(s->Queues[i].KernelEvent);
        if (s->Queues[i].UserEvent) ObDereferenceObject(s->Queues[i].UserEvent);
        if (s->Queues[i].Pending) ExFreePoolWithTag(s->Queues[i].Pending, TUN_MEMORY_TAG);
        if (s->Queues[i].TxSlots) ExFreePoolWithTag(s->Queues[i].TxSlots, TUN_MEMORY_TAG);
        if (s->Queues[i].RxSlots) ExFreePoolWithTag(s->Queues[i].RxSlots, TUN_MEMORY_TAG);
        if (s->Queues[i].Scratch) ExFreePoolWithTag(s->Queues[i].Scratch, TUN_MEMORY_TAG);
        for (ULONG b = 0; b < 2; ++b) if (s->Queues[i].CoalesceBuffers[b]) ExFreePoolWithTag(s->Queues[i].CoalesceBuffers[b], TUN_MEMORY_TAG);
    }
    if (s->PagesLocked) MmUnlockPages(s->Mdl);
    if (s->Mdl) IoFreeMdl(s->Mdl);
    ExFreePoolWithTag(s, TUN_MEMORY_TAG);
}

