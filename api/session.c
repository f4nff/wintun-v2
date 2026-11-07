/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include "adapter.h"
#include "logger.h"
#include "main.h"
#include "wintun.h"
#include <Windows.h>
#include <devioctl.h>
#include <stdlib.h>
#include "v2/compat.h"
#include "v2/lifecycle.h"
#include <wchar.h>
#include "termination_wait.h"

#pragma warning(disable : 4200) /* nonstandard: zero-sized array in struct/union */

#define TUN_ALIGNMENT sizeof(ULONG)
#define TUN_ALIGN(Size) (((ULONG)(Size) + ((ULONG)TUN_ALIGNMENT - 1)) & ~((ULONG)TUN_ALIGNMENT - 1))
#define TUN_IS_ALIGNED(Size) (!((ULONG)(Size) & ((ULONG)TUN_ALIGNMENT - 1)))
#define TUN_MAX_PACKET_SIZE TUN_ALIGN(sizeof(TUN_PACKET) + WINTUN_MAX_IP_PACKET_SIZE)
#define TUN_RING_CAPACITY(Size) ((Size) - sizeof(TUN_RING) - (TUN_MAX_PACKET_SIZE - TUN_ALIGNMENT))
#define TUN_RING_SIZE(Capacity) (sizeof(TUN_RING) + (Capacity) + (TUN_MAX_PACKET_SIZE - TUN_ALIGNMENT))
#define TUN_RING_WRAP(Value, Capacity) ((Value) & (Capacity - 1))
#define LOCK_SPIN_COUNT 0x10000
#define TUN_PACKET_RELEASE ((DWORD)0x80000000)

typedef struct _TUN_PACKET
{
    ULONG Size;
    UCHAR Data[];
} TUN_PACKET;

typedef struct _TUN_RING
{
    volatile ULONG Head;
    volatile ULONG Tail;
    volatile LONG Alertable;
    UCHAR Data[];
} TUN_RING;

#define TUN_IOCTL_REGISTER_RINGS CTL_CODE(51820U, 0x970U, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct _TUN_REGISTER_RINGS
{
    struct
    {
        ULONG RingSize;
        TUN_RING *Ring;
        HANDLE TailMoved;
    } Send, Receive;
} TUN_REGISTER_RINGS;

typedef struct _TUN_SESSION
{
    ULONG Capacity;
    struct
    {
        ULONG Tail;
        ULONG TailRelease;
        ULONG PacketsToRelease;
        CRITICAL_SECTION Lock;
    } Receive;
    struct
    {
        ULONG Head;
        ULONG HeadRelease;
        ULONG PacketsToRelease;
        CRITICAL_SECTION Lock;
    } Send;
    TUN_REGISTER_RINGS Descriptor;
    HANDLE Thread, Wake, Ready;
    HMODULE Module;
    WCHAR *Path;
    PB_INTERFACE_V2 Api;
    PB_LIFECYCLE_INTERFACE Life;
    PB_SESSION *Native;
    volatile LONG Stopping;
    DWORD StartError;
    SIZE_T RingBytes;
    BOOL RingCharged;
} TUN_SESSION;

#include "compat_worker.h"

WINTUN_START_SESSION_FUNC WintunStartSession;
_Use_decl_annotations_
TUN_SESSION *WINAPI
WintunStartSession(WINTUN_ADAPTER *Adapter, DWORD Capacity)
{
    if (!Adapter || Capacity < WINTUN_MIN_RING_CAPACITY || Capacity > WINTUN_MAX_RING_CAPACITY ||
        (Capacity & (Capacity - 1))) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!MainRuntimeAcquire()) return NULL;
    DWORD LastError = ERROR_NOT_ENOUGH_MEMORY;
    TUN_SESSION *Session = Zalloc(sizeof(*Session));
    if (!Session) { DWORD error = GetLastError(); MainRuntimeRelease(); SetLastError(error); return NULL; }
    Session->Capacity = Capacity;
    Session->RingBytes = (SIZE_T)TUN_RING_SIZE(Capacity) * 2;
    if (!CompatReserveBytes(Session->RingBytes)) goto cleanup;
    Session->RingCharged = TRUE;
    Session->Path = _wcsdup(Adapter->InterfaceFilename);
    if (!Session->Path) goto cleanup;
    BYTE *region = VirtualAlloc(NULL, Session->RingBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!region) { LastError = GetLastError(); goto cleanup; }
    Session->Descriptor.Send.Ring = (TUN_RING *)region;
    Session->Descriptor.Receive.Ring = (TUN_RING *)(region + TUN_RING_SIZE(Capacity));
    Session->Descriptor.Send.TailMoved = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Session->Descriptor.Send.TailMoved) { LastError = GetLastError(); goto cleanup; }
    Session->Wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Session->Wake) { LastError = GetLastError(); goto cleanup; }
    Session->Ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Session->Ready) { LastError = GetLastError(); goto cleanup; }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)WintunStartSession, &Session->Module)) {
        LastError = GetLastError(); goto cleanup;
    }
    if (PbGetInterfaceV2(PB_INTERFACE_V2_MAJOR, sizeof(Session->Api), &Session->Api) != PB_OK ||
        PbGetLifecycleInterface(PB_LIFECYCLE_ABI, sizeof(Session->Life), &Session->Life) != PB_OK) {
        LastError = ERROR_REVISION_MISMATCH; goto cleanup;
    }
    InitializeCriticalSection(&Session->Receive.Lock);
    InitializeCriticalSection(&Session->Send.Lock);
    Session->Thread = CreateThread(NULL, 0, CompatWorker, Session, 0, NULL);
    if (!Session->Thread) {
        LastError = GetLastError();
        DeleteCriticalSection(&Session->Receive.Lock); DeleteCriticalSection(&Session->Send.Lock); goto cleanup;
    }
    if (WaitForSingleObject(Session->Ready, INFINITE) != WAIT_OBJECT_0 || Session->StartError) {
        LastError = Session->StartError ? Session->StartError : GetLastError();
        InterlockedExchange(&Session->Stopping, 1); SetEvent(Session->Wake);
        (void)WaitForOwnedTermination(Session->Thread); CloseHandle(Session->Thread);
        DeleteCriticalSection(&Session->Receive.Lock); DeleteCriticalSection(&Session->Send.Lock); goto cleanup;
    }
    CloseHandle(Session->Ready); Session->Ready = NULL;
    return Session;
cleanup:
    CompatFree(Session); SetLastError(LastError); return NULL;
}

WINTUN_END_SESSION_FUNC WintunEndSession;
_Use_decl_annotations_
VOID WINAPI WintunEndSession(TUN_SESSION *Session)
{
    if (!Session) return;
    InterlockedExchange(&Session->Stopping, 1); SetEvent(Session->Wake);
    /* Old API requires callers to have stopped/joined packet users first.
     * Never free rings while the queue owner can still touch them. */
    (void)WaitForOwnedTermination(Session->Thread); CloseHandle(Session->Thread);
    DeleteCriticalSection(&Session->Send.Lock); DeleteCriticalSection(&Session->Receive.Lock);
    CompatFree(Session);
}

WINTUN_GET_READ_WAIT_EVENT_FUNC WintunGetReadWaitEvent;
_Use_decl_annotations_
HANDLE WINAPI
WintunGetReadWaitEvent(TUN_SESSION *Session)
{
    if (!Session) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return Session->Descriptor.Send.TailMoved;
}

WINTUN_RECEIVE_PACKET_FUNC WintunReceivePacket;
_Use_decl_annotations_
BYTE *WINAPI
WintunReceivePacket(TUN_SESSION *Session, DWORD *PacketSize)
{
    DWORD LastError;
    if (!Session || !PacketSize) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    EnterCriticalSection(&Session->Send.Lock);
    if (Session->Send.Head >= Session->Capacity)
    {
        LastError = ERROR_HANDLE_EOF;
        goto cleanup;
    }
    const ULONG BuffTail = ReadULongAcquire(&Session->Descriptor.Send.Ring->Tail);
    if (BuffTail >= Session->Capacity)
    {
        LastError = ERROR_HANDLE_EOF;
        goto cleanup;
    }
    if (Session->Send.Head == BuffTail)
    {
        LastError = ERROR_NO_MORE_ITEMS;
        goto cleanup;
    }
    const ULONG BuffContent = TUN_RING_WRAP(BuffTail - Session->Send.Head, Session->Capacity);
    if (BuffContent < sizeof(TUN_PACKET))
    {
        LastError = ERROR_INVALID_DATA;
        goto cleanup;
    }
    TUN_PACKET *BuffPacket = (TUN_PACKET *)&Session->Descriptor.Send.Ring->Data[Session->Send.Head];
    if (BuffPacket->Size > WINTUN_MAX_IP_PACKET_SIZE)
    {
        LastError = ERROR_INVALID_DATA;
        goto cleanup;
    }
    const ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + BuffPacket->Size);
    if (AlignedPacketSize > BuffContent)
    {
        LastError = ERROR_INVALID_DATA;
        goto cleanup;
    }
    *PacketSize = BuffPacket->Size;
    BYTE *Packet = BuffPacket->Data;
    Session->Send.Head = TUN_RING_WRAP(Session->Send.Head + AlignedPacketSize, Session->Capacity);
    Session->Send.PacketsToRelease++;
    LeaveCriticalSection(&Session->Send.Lock);
    return Packet;
cleanup:
    LeaveCriticalSection(&Session->Send.Lock);
    SetLastError(LastError);
    return NULL;
}

WINTUN_RELEASE_RECEIVE_PACKET_FUNC WintunReleaseReceivePacket;
_Use_decl_annotations_
VOID WINAPI
WintunReleaseReceivePacket(TUN_SESSION *Session, const BYTE *Packet)
{
    if (!Session) { SetLastError(ERROR_INVALID_HANDLE); return; }
    EnterCriticalSection(&Session->Send.Lock);
    TUN_PACKET *ReleasedBuffPacket = CompatIssuedPacket(Session, Session->Descriptor.Send.Ring, Packet,
        Session->Send.HeadRelease, Session->Send.Head);
    if (!ReleasedBuffPacket || (ReleasedBuffPacket->Size & TUN_PACKET_RELEASE)) {
        LeaveCriticalSection(&Session->Send.Lock); SetLastError(ERROR_INVALID_PARAMETER); return;
    }
    ReleasedBuffPacket->Size |= TUN_PACKET_RELEASE;
    while (Session->Send.PacketsToRelease)
    {
        const TUN_PACKET *BuffPacket = (TUN_PACKET *)&Session->Descriptor.Send.Ring->Data[Session->Send.HeadRelease];
        if ((BuffPacket->Size & TUN_PACKET_RELEASE) == 0)
            break;
        const ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + (BuffPacket->Size & ~TUN_PACKET_RELEASE));
        Session->Send.HeadRelease = TUN_RING_WRAP(Session->Send.HeadRelease + AlignedPacketSize, Session->Capacity);
        Session->Send.PacketsToRelease--;
    }
    if (Session->Descriptor.Send.Ring->Head != Session->Send.HeadRelease) {
        WriteULongRelease(&Session->Descriptor.Send.Ring->Head, Session->Send.HeadRelease);
        SetEvent(Session->Wake);
    }
    LeaveCriticalSection(&Session->Send.Lock);
}

WINTUN_ALLOCATE_SEND_PACKET_FUNC WintunAllocateSendPacket;
_Use_decl_annotations_
BYTE *WINAPI
WintunAllocateSendPacket(TUN_SESSION *Session, DWORD PacketSize)
{
    DWORD LastError;
    if (!Session || PacketSize > WINTUN_MAX_IP_PACKET_SIZE) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    EnterCriticalSection(&Session->Receive.Lock);
    if (Session->Receive.Tail >= Session->Capacity)
    {
        LastError = ERROR_HANDLE_EOF;
        goto cleanup;
    }
    const ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + PacketSize);
    const ULONG BuffHead = ReadULongAcquire(&Session->Descriptor.Receive.Ring->Head);
    if (BuffHead >= Session->Capacity)
    {
        LastError = ERROR_HANDLE_EOF;
        goto cleanup;
    }
    const ULONG BuffSpace = TUN_RING_WRAP(BuffHead - Session->Receive.Tail - TUN_ALIGNMENT, Session->Capacity);
    if (AlignedPacketSize > BuffSpace)
    {
        LastError = ERROR_BUFFER_OVERFLOW;
        goto cleanup;
    }
    TUN_PACKET *BuffPacket = (TUN_PACKET *)&Session->Descriptor.Receive.Ring->Data[Session->Receive.Tail];
    BuffPacket->Size = PacketSize | TUN_PACKET_RELEASE;
    BYTE *Packet = BuffPacket->Data;
    Session->Receive.Tail = TUN_RING_WRAP(Session->Receive.Tail + AlignedPacketSize, Session->Capacity);
    Session->Receive.PacketsToRelease++;
    LeaveCriticalSection(&Session->Receive.Lock);
    return Packet;
cleanup:
    LeaveCriticalSection(&Session->Receive.Lock);
    SetLastError(LastError);
    return NULL;
}

WINTUN_SEND_PACKET_FUNC WintunSendPacket;
_Use_decl_annotations_
VOID WINAPI
WintunSendPacket(TUN_SESSION *Session, const BYTE *Packet)
{
    if (!Session) { SetLastError(ERROR_INVALID_HANDLE); return; }
    EnterCriticalSection(&Session->Receive.Lock);
    TUN_PACKET *ReleasedBuffPacket = CompatIssuedPacket(Session, Session->Descriptor.Receive.Ring, Packet,
        Session->Receive.TailRelease, Session->Receive.Tail);
    if (!ReleasedBuffPacket || !(ReleasedBuffPacket->Size & TUN_PACKET_RELEASE)) {
        LeaveCriticalSection(&Session->Receive.Lock); SetLastError(ERROR_INVALID_PARAMETER); return;
    }
    ReleasedBuffPacket->Size &= ~TUN_PACKET_RELEASE;
    while (Session->Receive.PacketsToRelease)
    {
        const TUN_PACKET *BuffPacket =
            (TUN_PACKET *)&Session->Descriptor.Receive.Ring->Data[Session->Receive.TailRelease];
        if (BuffPacket->Size & TUN_PACKET_RELEASE)
            break;
        const ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + BuffPacket->Size);
        Session->Receive.TailRelease =
            TUN_RING_WRAP(Session->Receive.TailRelease + AlignedPacketSize, Session->Capacity);
        Session->Receive.PacketsToRelease--;
    }
    if (Session->Descriptor.Receive.Ring->Tail != Session->Receive.TailRelease)
    {
        WriteULongRelease(&Session->Descriptor.Receive.Ring->Tail, Session->Receive.TailRelease);
        MemoryBarrier();
        SetEvent(Session->Wake);
    }
    LeaveCriticalSection(&Session->Receive.Lock);
}
