/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "session_progress.h"
/* The caller owns a worker lifetime or Returns rundown pin. Active covers NBLs
 * and admitted send calls, including unlocked upcalls. ReferenceLock serializes
 * every Active write and its AllReturned event transition. QUERY observes Active
 * without this lock, so retain/release publish with the matching release store;
 * no read-modify-write is needed inside the already serialized transaction. */
static VOID PbV2RetainReferences(PB_KERNEL_SESSION *s, ULONG count)
{
    NT_ASSERT(count && count <= PB_MAX_BATCH);
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&s->ReferenceLock, &lock);
    /* Positive ownership already keeps the event reset. Serialize the first
     * retain's clear with the last release's set under this same lock. */
    LONG active = ReadAcquire(&s->Active);
    NT_ASSERT(active >= 0 && active <= MAXLONG - (LONG)count);
    WriteRelease(&s->Active, active + (LONG)count);
    if (!active) KeClearEvent(&s->AllReturned);
    KeReleaseInStackQueuedSpinLock(&lock);
}
static VOID PbV2RetainReceives(PB_KERNEL_SESSION *s, ULONG nbls, ULONG objects)
{
    NT_ASSERT(objects >= nbls && objects <= PB_MAX_BATCH);
    PbV2RetainReferences(s, nbls); InterlockedAdd64(&s->RxObjects, objects);
}
static VOID PbV2ReleaseReferences(PB_KERNEL_SESSION *s, ULONG count)
{
    NT_ASSERT(count && count <= PB_MAX_BATCH);
    KLOCK_QUEUE_HANDLE lock;
    KeAcquireInStackQueuedSpinLock(&s->ReferenceLock, &lock);
    LONG active = ReadAcquire(&s->Active);
    NT_ASSERT(active >= (LONG)count);
    active -= (LONG)count;
    WriteRelease(&s->Active, active);
    if (!active) {
        KeSetEvent(&s->AllReturned, IO_NO_INCREMENT, FALSE);
        if (ReadAcquire(&s->Draining)) PbV2Progress(s, FALSE);
    }
    KeReleaseInStackQueuedSpinLock(&lock);
}
static VOID PbV2ReleaseReference(PB_KERNEL_SESSION *s) { PbV2ReleaseReferences(s, 1); }
