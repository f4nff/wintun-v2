/* SPDX-License-Identifier: GPL-2.0 */
/* Included after offload_probe.h; all shared offsets come from a private PB_LAYOUT. */
#pragma once
#include "platform_nbl.h"
#include "platform_mdl.h"

#include "session_fault.h"
#include "session_references.h"

#include "session_recycle.h"

#include "session_completion.h"

#include "session_nbl.h"

#include "session_receive.h"

#include "session_worker.h"

#include "session_read.h"

/* RESOURCES indications return synchronously; these control buffers never enter a shared ring. */
static NDIS_STATUS PbV2ControlReply(TUN_CTX *ctx, const UCHAR *reply, ULONG length)
{
    UCHAR *data = ExAllocatePoolZero(NonPagedPool, length, TUN_MEMORY_TAG);
    if (!data) return NDIS_STATUS_RESOURCES;
    RtlCopyMemory(data, reply, length);
    MDL *mdl = IoAllocateMdl(data, length, FALSE, FALSE, NULL);
    if (!mdl) { ExFreePoolWithTag(data, TUN_MEMORY_TAG); return NDIS_STATUS_RESOURCES; }
    MmBuildMdlForNonPagedPool(mdl);
    NET_BUFFER_LIST *nbl = NdisAllocateNetBufferAndNetBufferList(ctx->NblPool, 0, 0, mdl, 0, length);
    NDIS_STATUS result = NDIS_STATUS_RESOURCES;
    if (nbl) {
        nbl->SourceHandle = ctx->MiniportAdapterHandle;
        NdisClearNblFlag(nbl, NDIS_NBL_FLAGS_IS_IPV4 | NDIS_NBL_FLAGS_IS_IPV6 | NDIS_NBL_FLAGS_IS_TCP | NDIS_NBL_FLAGS_IS_UDP);
        NET_BUFFER_LIST_INFO(nbl, NetBufferListFrameType) = (PVOID)(ULONG_PTR)(((ULONG)reply[13] << 8) | reply[12]);
        NET_BUFFER_LIST_NEXT_NBL_EX(nbl) = NULL;
        NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = NULL;
        PB_NBL_SET(nbl, TcpRecvSegCoalesceInfo, 30, NULL);
        PB_NBL_SET(nbl, UdpRecvSegCoalesceOffloadInfo, 89, NULL);
        PB_NBL_SET(nbl, RscTcpTimestampDelta, 30, NULL);
        NET_BUFFER_LIST_INFO(nbl, Ieee8021QNetBufferListInfo) = NULL;
        NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
        NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_SUCCESS;
        PbIndicateReceive(ctx->MiniportAdapterHandle, nbl, 1, NDIS_RECEIVE_FLAGS_RESOURCES);
        NdisFreeNetBufferList(nbl);
        result = NDIS_STATUS_SUCCESS;
    }
    IoFreeMdl(mdl); ExFreePoolWithTag(data, TUN_MEMORY_TAG);
    return result;
}

#include "session_describe.h"
#include "send_diagnostics.h"

#include "session_send.h"

#include "session_cleanup.h"

static NTSTATUS PbV2Register(TUN_CTX *ctx, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    if (IoIs32bitProcess(irp) || stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(PB_REGISTER_V2)) return STATUS_INVALID_PARAMETER;
    PB_REGISTER_V2 r; RtlCopyMemory(&r, irp->AssociatedIrp.SystemBuffer, sizeof(r));
    PB_LAYOUT layout;
    if (r.Size != sizeof(r) || r.Major != PB_ABI_MAJOR || r.Minor != PB_ABI_MINOR || !r.Epoch ||
        PbValidateRegionAddress(r.RegionAddress, r.RegionSize) != PB_OK ||
        RtlCompareMemory(r.SourceFingerprint, PB_SOURCE_STRING, sizeof(r.SourceFingerprint)) != sizeof(r.SourceFingerprint) ||
        PbCalculateLayout(r.QueueCount, r.SlotCount, r.RingCapacity, &layout) != PB_OK || r.RegionSize != layout.RegionSize)
        return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < sizeof(r.Reserved); ++i) if (r.Reserved[i]) return STATUS_INVALID_PARAMETER;
    ExAcquireResourceExclusiveLite(&ctx->Device.RegistrationLock, TRUE);
    NTSTATUS status = STATUS_ALREADY_INITIALIZED;
    if (ctx->Device.OwningFileObject) goto done;
    PB_KERNEL_SESSION *s = ExAllocatePoolZero(NonPagedPool, sizeof(*s), TUN_MEMORY_TAG);
    if (!s) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    RtlZeroMemory(s, sizeof(*s)); s->Ctx = ctx; s->Layout = layout; s->Epoch = r.Epoch;
    KeInitializeEvent(&s->StartEvent, NotificationEvent, FALSE); KeInitializeEvent(&s->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&s->AllReturned, NotificationEvent, TRUE); KeInitializeSpinLock(&s->ReferenceLock);
    ExInitializeRundownProtection(&s->Returns);
    s->Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)r.RegionAddress, r.RegionSize, FALSE, FALSE, NULL);
    if (!s->Mdl) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    try { MmProbeAndLockPages(s->Mdl, irp->RequestorMode, IoWriteAccess); }
    except(EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); goto fail; }
    s->PagesLocked = TRUE;
    s->Region = PbV2MapMdl(s->Mdl);
    if (!s->Region) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    RtlCopyMemory(s->Region, &layout, sizeof(layout));
    s->Lifecycle = (PB_SHARED_LIFECYCLE *)(s->Region + layout.LifecycleOffset);
    RtlZeroMemory(s->Lifecycle, sizeof(*s->Lifecycle));
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)r.ProgressEvent, EVENT_MODIFY_STATE,
        *ExEventObjectType, irp->RequestorMode, &s->ProgressEvent, NULL);
    if (!NT_SUCCESS(status)) goto fail;
    for (ULONG i = 0; i < layout.QueueCount; ++i) {
        PB_KERNEL_QUEUE *q = s->Queues + i; q->Session = s; q->Index = i;
        KeInitializeSpinLock(&q->TxLock); KeInitializeSpinLock(&q->RxLock);
        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)r.KernelEvents[i], SYNCHRONIZE | EVENT_MODIFY_STATE, *ExEventObjectType, irp->RequestorMode, &q->KernelEvent, NULL);
        if (!NT_SUCCESS(status)) goto fail;
        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)r.UserEvents[i], EVENT_MODIFY_STATE, *ExEventObjectType, irp->RequestorMode, &q->UserEvent, NULL);
        if (!NT_SUCCESS(status)) goto fail;
        status = STATUS_INVALID_PARAMETER;
        if (q->KernelEvent == q->UserEvent || q->KernelEvent == s->ProgressEvent || q->UserEvent == s->ProgressEvent) goto fail;
        for (ULONG prior = 0; prior < i; ++prior) {
            PB_KERNEL_QUEUE *p = s->Queues + prior;
            if (q->KernelEvent == p->KernelEvent || q->KernelEvent == p->UserEvent ||
                q->UserEvent == p->KernelEvent || q->UserEvent == p->UserEvent) goto fail;
        }
        q->TxSlots = ExAllocatePoolZero(NonPagedPool, layout.SlotCount * sizeof(*q->TxSlots), TUN_MEMORY_TAG);
        q->RxSlots = ExAllocatePoolZero(NonPagedPool, layout.SlotCount * sizeof(*q->RxSlots), TUN_MEMORY_TAG);
        q->Pending = ExAllocatePoolZero(NonPagedPool, layout.RingCapacity * sizeof(*q->Pending), TUN_MEMORY_TAG);
        if (!q->TxSlots || !q->RxSlots || !q->Pending) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
        RtlZeroMemory(q->RxSlots, layout.SlotCount * sizeof(*q->RxSlots));
        /* Bounded, optional private copies: checksum success never depends on user-mutable bytes. */
        q->Scratch = ExAllocatePoolZero(NonPagedPool, PB_BLOCK_SIZE, TUN_MEMORY_TAG);
        for (ULONG b = 0; b < 2; ++b) q->CoalesceBuffers[b] = ExAllocatePoolZero(NonPagedPool, PB_BLOCK_SIZE, TUN_MEMORY_TAG);
        (VOID)PbPoolInitialize(&q->TxPool, q->TxSlots, layout.SlotCount, r.Epoch, (USHORT)i);
        for (ULONG ring = 0; ring < PB_QUEUE_COUNT; ++ring) {
            RtlZeroMemory(s->Region + layout.Queues[i].Rings[ring], sizeof(PB_QUEUE_SHARED));
            (VOID)PbAttachQueue(&q->Rings[ring], s->Region, &layout, i, ring, ring == PB_QUEUE_TX_READY || ring == PB_QUEUE_RX_COMPLETE);
        }
        OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        status = PsCreateSystemThread(&q->Thread, THREAD_ALL_ACCESS, &oa, NULL, NULL, PbV2Worker, q);
        if (!NT_SUCCESS(status)) goto fail;
    }
    ctx->Device.OwningFileObject = stack->FileObject; ctx->Device.OwningProcessId = PsGetCurrentProcessId();
    InitializeListHead(&ctx->Device.Entry);
    ExAcquireResourceExclusiveLite(&TunDispatchDeviceListLock, TRUE); InsertTailList(&TunDispatchDeviceList, &ctx->Device.Entry); ExReleaseResourceLite(&TunDispatchDeviceListLock);
    KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
    ctx->V2 = s; WriteRelease(&ctx->Probe.V2SessionActive, 1); WriteRelease(&ctx->Probe.AllowedMask, 0);
    KeClearEvent(&ctx->Device.Disconnected); ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
    KeSetEvent(&s->StartEvent, IO_NO_INCREMENT, FALSE);
    PbReportOffload(ctx);
    TunIndicateStatus(ctx->MiniportAdapterHandle, MediaConnectStateConnected);
    status = STATUS_SUCCESS; goto done;
fail:
    PbV2Free(s);
done:
    ExReleaseResourceLite(&ctx->Device.RegistrationLock);
    return status;
}

#include "session_unregister.h"

#include "session_pause.h"

#include "session_query.h"

static NTSTATUS PbV2Control(TUN_CTX *ctx, IRP *irp, ULONG code)
{
    if (code == PB_V2_IOCTL_REGISTER) return PbV2Register(ctx, irp);
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    if (code == PB_V2_IOCTL_QUERY) {
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PB_V2_STATUS)) return STATUS_BUFFER_TOO_SMALL;
        NTSTATUS result = PbV2Query(ctx, irp->AssociatedIrp.SystemBuffer);
        if (NT_SUCCESS(result)) irp->IoStatus.Information = sizeof(PB_V2_STATUS);
        return result;
    }
    ExAcquireResourceExclusiveLite(&ctx->Device.RegistrationLock, TRUE);
    NTSTATUS result = STATUS_DEVICE_NOT_READY;
    PB_KERNEL_SESSION *s = ctx->V2;
    if (!s) goto done;
    if (code == PB_V2_IOCTL_DRAIN) {
        if (stack->FileObject != ctx->Device.OwningFileObject) { result = STATUS_ACCESS_DENIED; goto done; }
        ExAcquireFastMutex(&ctx->Probe.OffloadMutex);
        KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);
        /* Query no longer takes RegistrationLock: publish the deadline and state in its snapshot lock. */
        PbV2BeginDrain(s, KeQueryInterruptTime() / 10000u + 2000u);
        InterlockedExchange(&ctx->Probe.AllowedMask, 0);
        WriteRelease(&ctx->Probe.V2SessionActive, 0); /* A second control handle cannot re-enable a draining session. */
        ULONG oldBank = PbRotateIndications(ctx);
        ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);
        PbV2Progress(s, TRUE);
        PbWaitIndications(ctx, oldBank);
        PbReportOffload(ctx); PbV2Wake(s);
        ExReleaseFastMutex(&ctx->Probe.OffloadMutex);
        result = STATUS_SUCCESS;
    }
done:
    ExReleaseResourceLite(&ctx->Device.RegistrationLock); return result;
}
