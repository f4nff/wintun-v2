/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Only private state authorizes cleanup. This mapped record is a userspace hint.
 * Each consumer rechecks its condition after a coalesced event notification. */
static __inline VOID PbV2Progress(PB_KERNEL_SESSION *s, BOOLEAN terminal)
{
    if (s->Lifecycle) {
        if (terminal) InterlockedExchange((volatile LONG *)&s->Lifecycle->Terminal, 1);
        InterlockedIncrement64((volatile LONG64 *)&s->Lifecycle->Sequence);
    }
    if (s->ProgressEvent) KeSetEvent(s->ProgressEvent, IO_NO_INCREMENT, FALSE);
    if (terminal) for (ULONG i = 0; i < s->Layout.QueueCount; ++i)
        if (s->Queues[i].UserEvent) KeSetEvent(s->Queues[i].UserEvent, IO_NO_INCREMENT, FALSE);
}
