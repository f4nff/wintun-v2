/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Private byte rings preserve the old ABI's capacity, pointer and publication
 * order without teaching the kernel a second protocol. Only this joined worker
 * owns the native queue. No thread-pool task/coroutine is spawned per packet. */
#define COMPAT_MAX_RING_BYTES (256ll * 1024ll * 1024ll)
static volatile LONG64 CompatRingBytes;
static TUN_PACKET *CompatIssuedPacket(TUN_SESSION *s, TUN_RING *ring, const BYTE *data, ULONG start, ULONG end)
{
    uintptr_t base = (uintptr_t)ring->Data, address = (uintptr_t)data;
    if (address < base + sizeof(TUN_PACKET)) return NULL;
    uintptr_t offset = address - base - sizeof(TUN_PACKET);
    if (offset >= s->Capacity || !TUN_IS_ALIGNED(offset)) return NULL;
    ULONG span = TUN_RING_WRAP(end-start, s->Capacity);
    ULONG distance = TUN_RING_WRAP((ULONG)offset-start, s->Capacity);
    if (distance >= span) return NULL;
    TUN_PACKET *packet = (TUN_PACKET *)(ring->Data + offset);
    ULONG length = packet->Size & ~TUN_PACKET_RELEASE;
    if (length > WINTUN_MAX_IP_PACKET_SIZE || TUN_ALIGN(sizeof(*packet)+length) > span-distance) return NULL;
    return packet;
}
static BOOL CompatReserveBytes(SIZE_T bytes)
{
    LONG64 old = InterlockedCompareExchange64(&CompatRingBytes, 0, 0);
    do {
        if (bytes > COMPAT_MAX_RING_BYTES || old > COMPAT_MAX_RING_BYTES - (LONG64)bytes) return FALSE;
        LONG64 observed = InterlockedCompareExchange64(&CompatRingBytes, old + (LONG64)bytes, old);
        if (observed == old) return TRUE;
        old = observed;
    } while (TRUE);
}
static void CompatFree(TUN_SESSION *s)
{
    HMODULE module = s->Module;
    if (s->Ready) CloseHandle(s->Ready);
    if (s->Wake) CloseHandle(s->Wake);
    if (s->Descriptor.Send.TailMoved) CloseHandle(s->Descriptor.Send.TailMoved);
    if (s->Descriptor.Send.Ring) VirtualFree(s->Descriptor.Send.Ring, 0, MEM_RELEASE);
    if (s->RingCharged) InterlockedAdd64(&CompatRingBytes, -(LONG64)s->RingBytes);
    free(s->Path); Free(s); MainRuntimeRelease();
    if (module) FreeLibrary(module);
}
#ifndef PB_COMPAT_OPEN
#define PB_COMPAT_OPEN(s, config) (s)->Api.Open((s)->Path, config, &(s)->Native)
#endif
#include "compat_pump.h"
static DWORD WINAPI CompatWorker(void *opaque)
{
    TUN_SESSION *s = opaque;
    PB_CONFIG config = { sizeof(config), 1, 64, 128, 0, 0 };
    PB_USER_QUEUE *q = NULL;
    COMPAT_PENDING pending = { 0 };
    uint32_t count = 0;
    PB_RESULT result = PB_COMPAT_OPEN(s, &config);
    if (result == PB_OK) result = s->Api.OpenQueue(s->Native, 0, &q);
    s->StartError = result == PB_OK ? ERROR_SUCCESS : GetLastError();
    if (result != PB_OK && !s->StartError) s->StartError = ERROR_NOT_SUPPORTED;
    SetEvent(s->Ready);
    while (result == PB_OK && !ReadAcquire(&s->Stopping)) {
        BOOL progress = FALSE;
        PB_DESCRIPTOR completed[PB_MAX_BATCH]; count = 0;
        result = s->Api.PollCompletions(q, completed, PB_MAX_BATCH, &count);
        if (result != PB_OK && result != PB_EMPTY) break;
        progress = count != 0;
        result = CompatSendBatch(s, q, &progress);
        if (result != PB_OK) break;
        result = CompatReceiveBatch(s, q, &pending, &progress);
        if (result != PB_OK) break;
        if (!progress) {
            /* Do not arm/check TX_READY when a local byte-ring loan is blocked:
             * unread packets would make Wait return immediately and spin. */
            result = PbCompatWait(q, s->Wake, !pending.Count, INFINITE);
            if (result == PB_EMPTY) result = PB_OK;
        }
    }
    WriteULongRelease(&s->Descriptor.Receive.Ring->Head, MAXDWORD);
    WriteULongRelease(&s->Descriptor.Send.Ring->Tail, MAXDWORD);
    SetEvent(s->Descriptor.Send.TailMoved);
    if (s->Native) {
        s->Life.BeginStop(s->Native);
        CompatReturnPending(s, q, &pending);
        if (s->Api.Close(s->Native, 5000) != PB_OK) s->Life.ReleaseSession(s->Native);
    }
    return 0;
}
