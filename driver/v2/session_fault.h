/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "session_progress.h"

static VOID PbV2Wake(PB_KERNEL_SESSION *s)
{
    for (ULONG i = 0; i < s->Layout.QueueCount; ++i) if (s->Queues[i].KernelEvent)
        KeSetEvent(s->Queues[i].KernelEvent, IO_NO_INCREMENT, FALSE);
}

static VOID PbV2Fault(PB_KERNEL_SESSION *s)
{
    InterlockedExchange(&s->Draining, 1);
    WriteRelease(&s->Ctx->Probe.V2SessionActive, 0);
    if (InterlockedCompareExchange(&s->Faulted, 1, 0)) return;
    PbV2Progress(s, TRUE);
    PbV2Wake(s);
}

