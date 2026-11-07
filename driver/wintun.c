/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2026 WireGuard LLC. All Rights Reserved.
 */

#include <ntifs.h>
#include <wdm.h>
#include <wdmsec.h>
#include <ndis.h>
#include <ntstrsafe.h>
#include "undocumented.h"
#include "../common/platform.h"
#include "v2/probe_state.h"

#pragma warning(disable : 4100) /* unreferenced formal parameter */
#pragma warning(disable : 4200) /* nonstandard: zero-sized array in struct/union */
#pragma warning(disable : 4204) /* nonstandard: non-constant aggregate initializer */
#pragma warning(disable : 4221) /* nonstandard: cannot be initialized using address of automatic variable */
#pragma warning(disable : 6320) /* exception-filter expression is the constant EXCEPTION_EXECUTE_HANDLER */

#define NDIS_MINIPORT_VERSION_MIN ((NDIS_MINIPORT_MINIMUM_MAJOR_VERSION << 16) | NDIS_MINIPORT_MINIMUM_MINOR_VERSION)
#define NDIS_MINIPORT_VERSION_MAX ((NDIS_MINIPORT_MAJOR_VERSION << 16) | NDIS_MINIPORT_MINOR_VERSION)

#define TUN_VENDOR_NAME "PacketBridgeV2"
#define TUN_VENDOR_ID 0xFFFFFF00
#define TUN_LINK_SPEED 100000000000ULL /* 100gbps */

#define TUN_MAX_IP_PACKET_SIZE 0xFFFFu
#define TUN_MAX_RING_CAPACITY 0x4000000u

#if REG_DWORD == REG_DWORD_BIG_ENDIAN
#    define HTONS(x) ((USHORT)(x))
#    define HTONL(x) ((ULONG)(x))
#elif REG_DWORD == REG_DWORD_LITTLE_ENDIAN
#    define HTONS(x) ((((USHORT)(x)&0x00ff) << 8) | (((USHORT)(x)&0xff00) >> 8))
#    define HTONL(x) \
        ((((ULONG)(x)&0x000000ff) << 24) | (((ULONG)(x)&0x0000ff00) << 8) | (((ULONG)(x)&0x00ff0000) >> 8) | \
         (((ULONG)(x)&0xff000000) >> 24))
#else
#    error "Unable to determine endianness"
#endif

#define TUN_MEMORY_TAG HTONL('wtun')

/* Legacy userspace byte rings live exclusively in the compatibility DLL. */
#define TUN_IOCTL_REGISTER_RINGS CTL_CODE(51820U, 0x970U, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct _TUN_CTX
{
    volatile LONG Running;

    /* Used like RCU. When we're making use of rings, we take a shared lock. When we want to register or release the
     * rings and toggle the state, we take an exclusive lock before toggling the atomic and then releasing. It's similar
     * to setting the atomic and then calling rcu_barrier(). */
    EX_SPIN_LOCK TransitionLock;
    EX_RUNDOWN_REF ControlRundown;
    volatile LONG Halting;

    NDIS_HANDLE MiniportAdapterHandle; /* This is actually a pointer to NDIS_MINIPORT_BLOCK struct. */
    DEVICE_OBJECT *FunctionalDeviceObject;
    NDIS_STATISTICS_INFO Statistics;

    struct
    {
        LIST_ENTRY Entry;
        ERESOURCE RegistrationLock;
        FILE_OBJECT *OwningFileObject;
        HANDLE OwningProcessId;
        KEVENT Disconnected;


    } Device;

    NDIS_HANDLE NblPool;
    PB_PROBE_STATE Probe;
    PB_KERNEL_SESSION *V2;
} TUN_CTX;

static UINT NdisVersion;
#define PB_NBL_VERSION NdisVersion
static NDIS_HANDLE NdisMiniportDriverHandle;
static DRIVER_DISPATCH *NdisDispatchDeviceControl, *NdisDispatchClose, *NdisDispatchPnp;
static ERESOURCE TunDispatchCtxGuard, TunDispatchDeviceListLock;
static RTL_STATIC_LIST_HEAD(TunDispatchDeviceList);
#include "context_gate.h"
/* Binary representation of O:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)S:(ML;;NWNRNX;;;HI) */
static SECURITY_DESCRIPTOR *TunDispatchSecurityDescriptor = (SECURITY_DESCRIPTOR *)(__declspec(align(8)) UCHAR[]){
    0x01, 0x00, 0x14, 0x90, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x11, 0x00, 0x14, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x30, 0x00, 0x00, 0x02, 0x00, 0x34, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x14, 0x00, 0xff, 0x01, 0x1f, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x00, 0xff, 0x01, 0x1f, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00,
    0x00, 0x20, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00
};

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
TunIndicateStatus(_In_ NDIS_HANDLE MiniportAdapterHandle, _In_ NDIS_MEDIA_CONNECT_STATE MediaConnectState)
{
    NDIS_LINK_STATE State = { .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                                          .Revision = NDIS_LINK_STATE_REVISION_1,
                                          .Size = NDIS_SIZEOF_LINK_STATE_REVISION_1 },
                              .MediaConnectState = MediaConnectState,
                              .MediaDuplexState = MediaDuplexStateFull,
                              .XmitLinkSpeed = TUN_LINK_SPEED,
                              .RcvLinkSpeed = TUN_LINK_SPEED,
                              .PauseFunctions = NdisPauseFunctionsUnsupported };

    NDIS_STATUS_INDICATION Indication = { .Header = { .Type = NDIS_OBJECT_TYPE_STATUS_INDICATION,
                                                      .Revision = NDIS_STATUS_INDICATION_REVISION_1,
                                                      .Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1 },
                                          .SourceHandle = MiniportAdapterHandle,
                                          .StatusCode = NDIS_STATUS_LINK_STATE,
                                          .StatusBuffer = &State,
                                          .StatusBufferSize = sizeof(State) };

    NdisMIndicateStatusEx(MiniportAdapterHandle, &Indication);
}

#define NET_BUFFER_LIST_OFFSET(Nbl) (NET_BUFFER_LIST_MINIPORT_RESERVED(Nbl)[0])

/* Send: We should not modify NET_BUFFER_LIST_NEXT_NBL(Nbl) to prevent fragmented NBLs to separate.
 * Receive: NDIS may change NET_BUFFER_LIST_NEXT_NBL(Nbl) at will between the NdisMIndicateReceiveNetBufferLists() and
 * MINIPORT_RETURN_NET_BUFFER_LISTS calls. Therefore, we use our own ->Next pointer for book-keeping. */
#define NET_BUFFER_LIST_NEXT_NBL_EX(Nbl) (NET_BUFFER_LIST_MINIPORT_RESERVED(Nbl)[1])

#include "v2/offload_probe.h"
#include "v2/session.h"

static MINIPORT_SEND_NET_BUFFER_LISTS TunSendNetBufferLists;
_Use_decl_annotations_
static VOID
TunSendNetBufferLists(
    NDIS_HANDLE MiniportAdapterContext,
    NET_BUFFER_LIST *NetBufferLists,
    NDIS_PORT_NUMBER PortNumber,
    ULONG SendFlags)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;
    if (PbV2Send(Ctx, NetBufferLists)) return;
    /* A registration can race this callback. Never enter the legacy L3 ring path for an Ethernet NIC. */
    for (NET_BUFFER_LIST *nbl = NetBufferLists; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl))
        NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_MEDIA_DISCONNECTED;
    PbSetLsoCompletion(NetBufferLists);
    PbCompleteSend(Ctx->MiniportAdapterHandle, NetBufferLists);
    return;
}

static MINIPORT_CANCEL_SEND TunCancelSend;
_Use_decl_annotations_
static VOID
TunCancelSend(NDIS_HANDLE MiniportAdapterContext, PVOID CancelId)
{
}

static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;
_Use_decl_annotations_
static VOID
TunReturnNetBufferLists(NDIS_HANDLE MiniportAdapterContext, PNET_BUFFER_LIST NetBufferLists, ULONG ReturnFlags)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;
    /* Return may be reentrant from an indication. No transition lock crosses the upcall. */
    if (!ExAcquireRundownProtection(&Ctx->Probe.ReturnRundown)) {
        NT_ASSERT(FALSE);
        return;
    }
    NET_BUFFER_LIST *remaining = NetBufferLists;
    while (remaining) {
        if (!PbV2ReturnNbls(Ctx, &remaining, PB_MAX_BATCH)) {
            NT_ASSERT(FALSE); /* Only v2 owns indicated NBLs in this driver. */
            break;
        }
    }
    ExReleaseRundownProtection(&Ctx->Probe.ReturnRundown);
}

#define TUN_FORCE_UNREGISTRATION ((FILE_OBJECT *)-1)
_IRQL_requires_max_(PASSIVE_LEVEL)
static VOID
TunUnregisterBuffersForOwner(_Inout_ TUN_CTX *Ctx, _In_ FILE_OBJECT *Owner, _In_opt_ HANDLE ProcessId)
{
    if (!Owner)
        return;
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Ctx->Device.RegistrationLock, TRUE);
    if (!Ctx->Device.OwningFileObject || (Owner != TUN_FORCE_UNREGISTRATION && Ctx->Device.OwningFileObject != Owner) ||
        (ProcessId && Ctx->Device.OwningProcessId != ProcessId))
    {
        ExReleaseResourceLite(&Ctx->Device.RegistrationLock);
        KeLeaveCriticalRegion();
        return;
    }
    Ctx->Device.OwningFileObject = NULL;

    ExAcquireResourceExclusiveLite(&TunDispatchDeviceListLock, TRUE);
    RemoveEntryList(&Ctx->Device.Entry);
    ExReleaseResourceLite(&TunDispatchDeviceListLock);

    TunIndicateStatus(Ctx->MiniportAdapterHandle, MediaConnectStateDisconnected);

    if (Ctx->V2) PbV2Unregister(Ctx);
    ExReleaseResourceLite(&Ctx->Device.RegistrationLock);
    KeLeaveCriticalRegion();
}

static VOID
TunUnregisterBuffers(_Inout_ TUN_CTX *Ctx, _In_ FILE_OBJECT *Owner)
{
    TunUnregisterBuffersForOwner(Ctx, Owner, NULL);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static VOID
TunProcessNotification(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create)
{
    if (Create)
        return;
    for (;;) {
        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&TunDispatchCtxGuard, TRUE);
        ExAcquireResourceSharedLite(&TunDispatchDeviceListLock, TRUE);
        TUN_CTX *Ctx = NULL;
        for (LIST_ENTRY *Entry = TunDispatchDeviceList.Flink; Entry != &TunDispatchDeviceList; Entry = Entry->Flink)
        {
            TUN_CTX *Candidate = CONTAINING_RECORD(Entry, TUN_CTX, Device.Entry);
            if (Candidate->Device.OwningProcessId == ProcessId && TunReferenceCtxLocked(Candidate)) { Ctx = Candidate; break; }
        }
        ExReleaseResourceLite(&TunDispatchDeviceListLock);
        ExReleaseResourceLite(&TunDispatchCtxGuard);
        KeLeaveCriticalRegion();
        if (!Ctx) break;
        /* Recheck the PID under RegistrationLock: the old owner may already have been replaced. */
        TunUnregisterBuffersForOwner(Ctx, TUN_FORCE_UNREGISTRATION, ProcessId);
        TunDereferenceCtx(Ctx);
    }
}

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
static DRIVER_DISPATCH_PAGED TunDispatchDeviceControl;
_Use_decl_annotations_
static NTSTATUS
TunDispatchDeviceControl(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    IO_STACK_LOCATION *Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->Parameters.DeviceIoControl.IoControlCode != TUN_IOCTL_REGISTER_RINGS
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_PROBE_IOCTL_QUERY
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_PROBE_IOCTL_POLICY
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_RETURN_DIAG_IOCTL
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_SEND_DIAG_IOCTL
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_POOL_PRESSURE_IOCTL
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_V2_IOCTL_REGISTER
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_V2_IOCTL_DRAIN
        && Stack->Parameters.DeviceIoControl.IoControlCode != PB_V2_IOCTL_QUERY
        )
        return NdisDispatchDeviceControl(DeviceObject, Irp);

    Irp->IoStatus.Information = 0;

    SECURITY_SUBJECT_CONTEXT SubjectContext;
    SeCaptureSubjectContext(&SubjectContext);
    NTSTATUS Status;
    ACCESS_MASK GrantedAccess;
    BOOLEAN HasAccess = SeAccessCheck(
        TunDispatchSecurityDescriptor,
        &SubjectContext,
        FALSE,
        FILE_WRITE_DATA,
        0,
        NULL,
        IoGetFileObjectGenericMapping(),
        Irp->RequestorMode,
        &GrantedAccess,
        &Status);
    SeReleaseSubjectContext(&SubjectContext);
    if (!HasAccess)
        goto cleanup;
    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case PB_V2_IOCTL_REGISTER:
    case PB_V2_IOCTL_DRAIN:
    case PB_V2_IOCTL_QUERY: {
        KeEnterCriticalRegion();
        TUN_CTX *Ctx = TunReferenceDeviceCtx(DeviceObject);
        Status = Ctx ? PbV2Control(Ctx, Irp, Stack->Parameters.DeviceIoControl.IoControlCode) : STATUS_DEVICE_NOT_READY;
        if (Ctx) TunDereferenceCtx(Ctx);
        KeLeaveCriticalRegion();
        break;
    }
    case PB_PROBE_IOCTL_QUERY:
    case PB_PROBE_IOCTL_POLICY:
    case PB_RETURN_DIAG_IOCTL:
    case PB_POOL_PRESSURE_IOCTL:
    case PB_SEND_DIAG_IOCTL: {
        KeEnterCriticalRegion();
        TUN_CTX *Ctx = TunReferenceDeviceCtx(DeviceObject);
        Status = Ctx ? PbProbeIoctl(Ctx, Irp, Stack->Parameters.DeviceIoControl.IoControlCode) : STATUS_DEVICE_NOT_READY;
        if (Ctx) TunDereferenceCtx(Ctx);
        KeLeaveCriticalRegion();
        break;
    }
    case TUN_IOCTL_REGISTER_RINGS: {
        KeEnterCriticalRegion();
        TUN_CTX *Ctx = TunReferenceDeviceCtx(DeviceObject);
        Status = NDIS_STATUS_ADAPTER_NOT_READY;
        if (Ctx)
            Status = STATUS_NOT_SUPPORTED; /* Ethernet frontend requires the L3 v2 boundary. */
        if (Ctx) TunDereferenceCtx(Ctx);
        KeLeaveCriticalRegion();
        break;
    }
    }
cleanup:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

_Dispatch_type_(IRP_MJ_CLOSE)
static DRIVER_DISPATCH_PAGED TunDispatchClose;
_Use_decl_annotations_
static NTSTATUS
TunDispatchClose(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    KeEnterCriticalRegion();
    TUN_CTX *Ctx = TunReferenceDeviceCtx(DeviceObject);
    if (Ctx)
        TunUnregisterBuffers(Ctx, IoGetCurrentIrpStackLocation(Irp)->FileObject);
    if (Ctx) TunDereferenceCtx(Ctx);
    KeLeaveCriticalRegion();
    return NdisDispatchClose(DeviceObject, Irp);
}

_Dispatch_type_(IRP_MJ_PNP)
static DRIVER_DISPATCH_PAGED TunDispatchPnp;
_Use_decl_annotations_
static NTSTATUS
TunDispatchPnp(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    /* NDIS owns PnP sequencing. Never close foreign process handles from a racy system-handle snapshot.
     * Pause/Halt drain the session; user handles keep their normal I/O-manager lifetime. */
    return NdisDispatchPnp(DeviceObject, Irp);
}

static MINIPORT_RESTART TunRestart;
_Use_decl_annotations_
static NDIS_STATUS
TunRestart(NDIS_HANDLE MiniportAdapterContext, PNDIS_MINIPORT_RESTART_PARAMETERS MiniportRestartParameters)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;
    WriteRelease(&Ctx->Running, TRUE);
    KeSetEvent(&Ctx->Probe.RunningEvent, IO_NO_INCREMENT, FALSE);
    return NDIS_STATUS_SUCCESS;
}

static MINIPORT_PAUSE TunPause;
_Use_decl_annotations_
static NDIS_STATUS
TunPause(NDIS_HANDLE MiniportAdapterContext, PNDIS_MINIPORT_PAUSE_PARAMETERS MiniportPauseParameters)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;

    KIRQL irql = ExAcquireSpinLockExclusive(&Ctx->TransitionLock);
    WriteRelease(&Ctx->Running, FALSE);
    KeClearEvent(&Ctx->Probe.RunningEvent);
    ExReleaseSpinLockExclusive(&Ctx->TransitionLock, irql);


    PbV2Pause(Ctx);

    return NDIS_STATUS_SUCCESS;
}

static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;
_Use_decl_annotations_
static VOID
TunDevicePnPEventNotify(NDIS_HANDLE MiniportAdapterContext, PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
}

static MINIPORT_INITIALIZE TunInitializeEx;
_Use_decl_annotations_
static NDIS_STATUS
TunInitializeEx(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportDriverContext,
    PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    NDIS_STATUS Status;
    BOOLEAN RegistrationLockInitialized = FALSE;

    if (!MiniportAdapterHandle)
        return NDIS_STATUS_FAILURE;

/* Leaking memory 'Ctx'. Note: 'Ctx' is freed in TunHaltEx or on failure. */
#pragma warning(suppress : 6014)
#pragma warning(suppress : 28160)
    TUN_CTX *Ctx = ExAllocatePoolZero(NonPagedPool, sizeof(*Ctx), TUN_MEMORY_TAG);
    if (!Ctx)
        return NDIS_STATUS_FAILURE;

    Ctx->MiniportAdapterHandle = MiniportAdapterHandle;
    ExInitializeRundownProtection(&Ctx->ControlRundown);
    ExInitializeRundownProtection(&Ctx->Probe.ReturnRundown);
    KeInitializeEvent(&Ctx->Probe.RunningEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&Ctx->Probe.StatusLock);
    KeInitializeSpinLock(&Ctx->Probe.ReturnDiagnosticsLock);
    KeInitializeSpinLock(&Ctx->Probe.SendDiagnosticsLock);
    ExInitializeFastMutex(&Ctx->Probe.OffloadMutex);
    KeInitializeSpinLock(&Ctx->Probe.IndicationLock);
    KeInitializeEvent(&Ctx->Probe.IndicationIdle[0], NotificationEvent, TRUE);
    KeInitializeEvent(&Ctx->Probe.IndicationIdle[1], NotificationEvent, TRUE);
    Ctx->Probe.LocalMac[0] = 2;
    Ctx->Probe.LocalMac[1] = 0x50;
    ULONG identity = (ULONG)MiniportInitParameters->NetLuid.Info.NetLuidIndex;
    for (ULONG i = 0; i < 4; ++i) Ctx->Probe.LocalMac[2 + i] = (UCHAR)(identity >> (8 * i));
    RtlCopyMemory(Ctx->Probe.PeerMac, Ctx->Probe.LocalMac, 6);
    Ctx->Probe.PeerMac[1] ^= 0x80;

    NdisMGetDeviceProperty(MiniportAdapterHandle, NULL, &Ctx->FunctionalDeviceObject, NULL, NULL, NULL);
    if (Status = NDIS_STATUS_FAILURE, !Ctx->FunctionalDeviceObject)
        goto cleanupFreeCtx;
#pragma warning(suppress : 28175)
    ASSERT(!Ctx->FunctionalDeviceObject->Reserved);
    /* Reverse engineering indicates that we'd be better off calling
     * NdisWdfGetAdapterContextFromAdapterHandle(functional_device),
     * which points to our TUN_CTX object directly, but this isn't
     * available before Windows 10, so for now we just stick it into
     * this reserved field. Revisit this when we drop support for old
     * Windows versions. */
    /* Publish Reserved only after every initialization failure point has passed. */

    Ctx->Statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Ctx->Statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
    Ctx->Statistics.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
    Ctx->Statistics.SupportedStatistics =
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS | NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR | NDIS_STATISTICS_FLAGS_VALID_XMIT_DISCARDS |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_XMIT;
    KeInitializeEvent(&Ctx->Device.Disconnected, NotificationEvent, TRUE);
    Status = ExInitializeResourceLite(&Ctx->Device.RegistrationLock);
    if (!NT_SUCCESS(Status)) goto cleanupFreeCtx;
    RegistrationLockInitialized = TRUE;

    NET_BUFFER_LIST_POOL_PARAMETERS NblPoolParameters = {
        .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                    .Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1,
                    .Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1 },
        .ProtocolId = NDIS_PROTOCOL_ID_DEFAULT,
        .fAllocateNetBuffer = TRUE,
        .PoolTag = TUN_MEMORY_TAG
    };
/* Leaking memory 'Ctx->NblPool'. Note: 'Ctx->NblPool' is freed in TunHaltEx or on failure. */
#pragma warning(suppress : 6014)
    Ctx->NblPool = NdisAllocateNetBufferListPool(MiniportAdapterHandle, &NblPoolParameters);
    if (Status = NDIS_STATUS_FAILURE, !Ctx->NblPool)
        goto cleanupFreeCtx;

    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES AdapterRegistrationAttributes = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_630
                                    ? NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1
                                    : NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_630
                                ? NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1
                                : NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2 },
        .AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND | NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK,
        .InterfaceType = NdisInterfaceInternal,
        .MiniportAdapterContext = Ctx
    };
    if (Status = NDIS_STATUS_FAILURE,
        !NT_SUCCESS(NdisMSetMiniportAttributes(
            MiniportAdapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&AdapterRegistrationAttributes)))
        goto cleanupFreeNblPool;

    NDIS_PM_CAPABILITIES PmCapabilities = {
        .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_630 ? NDIS_PM_CAPABILITIES_REVISION_1
                                                                       : NDIS_PM_CAPABILITIES_REVISION_2,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_630 ? NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1
                                                                   : NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_2 },
        .MinMagicPacketWakeUp = NdisDeviceStateUnspecified,
        .MinPatternWakeUp = NdisDeviceStateUnspecified,
        .MinLinkChangeWakeUp = NdisDeviceStateUnspecified
    };
    static NDIS_OID SupportedOids[] = { OID_GEN_MAXIMUM_TOTAL_SIZE,
                                        OID_TCP_OFFLOAD_PARAMETERS,
                                        OID_OFFLOAD_ENCAPSULATION,
                                        OID_TCP_RSC_STATISTICS,
                                        OID_802_3_PERMANENT_ADDRESS,
                                        OID_802_3_CURRENT_ADDRESS,
                                        OID_802_3_MAXIMUM_LIST_SIZE,
                                        OID_802_3_MULTICAST_LIST,
                                        OID_GEN_CURRENT_LOOKAHEAD,
                                        OID_GEN_TRANSMIT_BUFFER_SPACE,
                                        OID_GEN_RECEIVE_BUFFER_SPACE,
                                        OID_GEN_TRANSMIT_BLOCK_SIZE,
                                        OID_GEN_RECEIVE_BLOCK_SIZE,
                                        OID_GEN_VENDOR_DESCRIPTION,
                                        OID_GEN_VENDOR_ID,
                                        OID_GEN_VENDOR_DRIVER_VERSION,
                                        OID_GEN_XMIT_OK,
                                        OID_GEN_RCV_OK,
                                        OID_GEN_CURRENT_PACKET_FILTER,
                                        OID_GEN_STATISTICS,
                                        OID_GEN_INTERRUPT_MODERATION,
                                        OID_GEN_LINK_PARAMETERS,
                                        OID_PNP_SET_POWER,
                                        OID_PNP_QUERY_POWER };
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES AdapterGeneralAttributes = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES,
                    .Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2,
                    .Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2 },
        .MediaType = NdisMedium802_3,
        .PhysicalMediumType = NdisPhysicalMediumUnspecified,
        .MtuSize = TUN_MAX_IP_PACKET_SIZE,
        .MaxXmitLinkSpeed = TUN_LINK_SPEED,
        .MaxRcvLinkSpeed = TUN_LINK_SPEED,
        .RcvLinkSpeed = TUN_LINK_SPEED,
        .XmitLinkSpeed = TUN_LINK_SPEED,
        .MediaConnectState = MediaConnectStateDisconnected,
        .MediaDuplexState = MediaDuplexStateFull,
        .LookaheadSize = TUN_MAX_IP_PACKET_SIZE,
        .MacOptions =
            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_NO_LOOPBACK,
        .SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_ALL_MULTICAST |
                                  NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_ALL_LOCAL |
                                  NDIS_PACKET_TYPE_ALL_FUNCTIONAL,
        .AccessType = NET_IF_ACCESS_BROADCAST,
        .DirectionType = NET_IF_DIRECTION_SENDRECEIVE,
        .ConnectionType = NET_IF_CONNECTION_DEDICATED,
        .IfType = IF_TYPE_ETHERNET_CSMACD,
        .IfConnectorPresent = FALSE,
        .SupportedStatistics = Ctx->Statistics.SupportedStatistics,
        .SupportedPauseFunctions = NdisPauseFunctionsUnsupported,
        .AutoNegotiationFlags =
            NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED | NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED |
            NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED | NDIS_LINK_STATE_PAUSE_FUNCTIONS_AUTO_NEGOTIATED,
        .SupportedOidList = SupportedOids,
        .SupportedOidListLength = sizeof(SupportedOids),
        .PowerManagementCapabilitiesEx = &PmCapabilities
    };
    AdapterGeneralAttributes.MacAddressLength = 6;
    RtlCopyMemory(AdapterGeneralAttributes.PermanentMacAddress, Ctx->Probe.LocalMac, 6);
    RtlCopyMemory(AdapterGeneralAttributes.CurrentMacAddress, Ctx->Probe.LocalMac, 6);
    AdapterGeneralAttributes.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
        NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS;
    if (Status = NDIS_STATUS_FAILURE,
        !NT_SUCCESS(NdisMSetMiniportAttributes(
            MiniportAdapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&AdapterGeneralAttributes)))
        goto cleanupFreeNblPool;

    Ctx->Probe.RegistrationStatus = PbRegisterOffload(Ctx);
    if (Ctx->Probe.RegistrationStatus != NDIS_STATUS_SUCCESS) {
        /* Keep an inspectable plain adapter when offload registration is rejected. */
        Ctx->Probe.NdisMask = 0;
        Ctx->Probe.AllowedMask = 0;
    }

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&TunDispatchCtxGuard, TRUE);
#pragma warning(suppress : 28175)
    Ctx->FunctionalDeviceObject->Reserved = Ctx;
    ExReleaseResourceLite(&TunDispatchCtxGuard);
    KeLeaveCriticalRegion();
    return NDIS_STATUS_SUCCESS;

cleanupFreeNblPool:
    NdisFreeNetBufferListPool(Ctx->NblPool);
cleanupFreeCtx:
    if (RegistrationLockInitialized) ExDeleteResourceLite(&Ctx->Device.RegistrationLock);
    ExFreePoolWithTag(Ctx, TUN_MEMORY_TAG);
    return Status;
}

static MINIPORT_HALT TunHaltEx;
_Use_decl_annotations_
static VOID
TunHaltEx(NDIS_HANDLE MiniportAdapterContext, NDIS_HALT_ACTION HaltAction)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;

    KIRQL irql = ExAcquireSpinLockExclusive(&Ctx->TransitionLock);
    WriteRelease(&Ctx->Running, FALSE);
    KeClearEvent(&Ctx->Probe.RunningEvent);
    ExReleaseSpinLockExclusive(&Ctx->TransitionLock, irql);

    TunRevokeContext(Ctx);

    TunUnregisterBuffers(Ctx, TUN_FORCE_UNREGISTRATION);

    ExWaitForRundownProtectionRelease(&Ctx->Probe.ReturnRundown);

    ExReleaseSpinLockExclusive(
        &Ctx->TransitionLock,
        ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure above change is visible to all readers. */
    NdisFreeNetBufferListPool(Ctx->NblPool);

#pragma warning(suppress : 6387)
    WritePointerNoFence(&Ctx->MiniportAdapterHandle, NULL);
    ExDeleteResourceLite(&Ctx->Device.RegistrationLock);
    ExFreePoolWithTag(Ctx, TUN_MEMORY_TAG);
}

static MINIPORT_SHUTDOWN TunShutdownEx;
_Use_decl_annotations_
static VOID
TunShutdownEx(NDIS_HANDLE MiniportAdapterContext, NDIS_SHUTDOWN_ACTION ShutdownAction)
{
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWrite(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_ ULONG Value)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
    *(ULONG *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = Value;
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWrite32or64(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_ ULONG64 Value)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG64);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG64))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG64);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
        *(ULONG *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = (ULONG)(Value & 0xffffffff);
        return NDIS_STATUS_SUCCESS;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG64);
    *(ULONG64 *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = Value;
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWriteBuf(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_bytecount_(Size) const void *Buf, _In_ ULONG Size)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < Size)
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = Size;
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = Size;
    NdisMoveMemory(OidRequest->DATA.QUERY_INFORMATION.InformationBuffer, Buf, Size);
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQuery(_Inout_ TUN_CTX *Ctx, _Inout_ NDIS_OID_REQUEST *OidRequest)
{
    ASSERT(
        OidRequest->RequestType == NdisRequestQueryInformation ||
        OidRequest->RequestType == NdisRequestQueryStatistics);

    switch (OidRequest->DATA.QUERY_INFORMATION.Oid)
    {
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS:
        return TunOidQueryWriteBuf(OidRequest, Ctx->Probe.LocalMac, 6);
    case OID_802_3_MAXIMUM_LIST_SIZE:
        return TunOidQueryWrite(OidRequest, 32);
    case OID_802_3_MULTICAST_LIST:
        return TunOidQueryWriteBuf(OidRequest, NULL, 0);
    case OID_TCP_RSC_STATISTICS: {
        NDIS_RSC_STATISTICS_INFO statistics = { .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
            .Revision = NDIS_RSC_STATISTICS_REVISION_1, .Size = NDIS_SIZEOF_RSC_STATISTICS_REVISION_1 },
            .CoalescedPkts = ReadULong64Acquire((ULONG64 *)&Ctx->Probe.RscPackets),
            .CoalescedOctets = ReadULong64Acquire((ULONG64 *)&Ctx->Probe.RscBytes),
            .CoalesceEvents = ReadULong64Acquire((ULONG64 *)&Ctx->Probe.RscEvents),
            .Aborts = ReadULong64Acquire((ULONG64 *)&Ctx->Probe.RscAborts) };
        return TunOidQueryWriteBuf(OidRequest, &statistics, sizeof(statistics));
    }
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_IP_PACKET_SIZE + PB_ETH_HEADER);

    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_RING_CAPACITY);

    case OID_GEN_RECEIVE_BUFFER_SPACE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_RING_CAPACITY);

    case OID_GEN_VENDOR_ID:
        return TunOidQueryWrite(OidRequest, HTONL(TUN_VENDOR_ID));

    case OID_GEN_VENDOR_DESCRIPTION:
        return TunOidQueryWriteBuf(OidRequest, TUN_VENDOR_NAME, (ULONG)sizeof(TUN_VENDOR_NAME));

    case OID_GEN_VENDOR_DRIVER_VERSION:
        return TunOidQueryWrite(OidRequest, (WINTUN_VERSION_MAJ << 16) | WINTUN_VERSION_MIN);

    case OID_GEN_XMIT_OK:
        return TunOidQueryWrite32or64(
            OidRequest,
            ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCOutUcastPkts) +
                ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCOutMulticastPkts) +
                ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCOutBroadcastPkts));

    case OID_GEN_RCV_OK:
        return TunOidQueryWrite32or64(
            OidRequest,
            ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCInUcastPkts) +
                ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCInMulticastPkts) +
                ReadNoFence64((LONG64 *)&Ctx->Statistics.ifHCInBroadcastPkts));

    case OID_GEN_STATISTICS:
        return TunOidQueryWriteBuf(OidRequest, &Ctx->Statistics, (ULONG)sizeof(Ctx->Statistics));

    case OID_GEN_INTERRUPT_MODERATION: {
        static const NDIS_INTERRUPT_MODERATION_PARAMETERS InterruptParameters = {
            .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                        .Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1,
                        .Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1 },
            .InterruptModeration = NdisInterruptModerationNotSupported
        };
        return TunOidQueryWriteBuf(OidRequest, &InterruptParameters, (ULONG)sizeof(InterruptParameters));
    }

    case OID_PNP_QUERY_POWER:
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_SUCCESS;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
    return NDIS_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static NDIS_STATUS
TunOidSet(_Inout_ TUN_CTX *Ctx, _Inout_ NDIS_OID_REQUEST *OidRequest)
{
    ASSERT(OidRequest->RequestType == NdisRequestSetInformation);

    OidRequest->DATA.SET_INFORMATION.BytesNeeded = OidRequest->DATA.SET_INFORMATION.BytesRead = 0;

    switch (OidRequest->DATA.SET_INFORMATION.Oid)
    {
    case OID_802_3_MULTICAST_LIST:
        if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength % 6 ||
            OidRequest->DATA.SET_INFORMATION.InformationBufferLength > 32 * 6) return NDIS_STATUS_INVALID_LENGTH;
        OidRequest->DATA.SET_INFORMATION.BytesRead = OidRequest->DATA.SET_INFORMATION.InformationBufferLength;
        return NDIS_STATUS_SUCCESS;
    case OID_TCP_OFFLOAD_PARAMETERS:
        return PbSetOffloadParameters(Ctx, OidRequest);
    case OID_OFFLOAD_ENCAPSULATION:
        return PbSetEncapsulation(Ctx, OidRequest);
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_GEN_CURRENT_LOOKAHEAD:
        if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength != 4)
        {
            OidRequest->DATA.SET_INFORMATION.BytesNeeded = 4;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        OidRequest->DATA.SET_INFORMATION.BytesRead = 4;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_LINK_PARAMETERS:
        OidRequest->DATA.SET_INFORMATION.BytesRead = OidRequest->DATA.SET_INFORMATION.InformationBufferLength;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_INTERRUPT_MODERATION:
        return NDIS_STATUS_INVALID_DATA;

    case OID_PNP_SET_POWER:
        if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength != sizeof(NDIS_DEVICE_POWER_STATE))
        {
            OidRequest->DATA.SET_INFORMATION.BytesNeeded = sizeof(NDIS_DEVICE_POWER_STATE);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(NDIS_DEVICE_POWER_STATE);
        return NDIS_STATUS_SUCCESS;
    }

    return NDIS_STATUS_NOT_SUPPORTED;
}

static MINIPORT_OID_REQUEST TunOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        return TunOidQuery(MiniportAdapterContext, OidRequest);

    case NdisRequestSetInformation:
        return TunOidSet(MiniportAdapterContext, OidRequest);

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_CANCEL_OID_REQUEST TunCancelOidRequest;
_Use_decl_annotations_
static VOID
TunCancelOidRequest(NDIS_HANDLE MiniportAdapterContext, PVOID RequestId)
{
}

static MINIPORT_DIRECT_OID_REQUEST TunDirectOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunDirectOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
    case NdisRequestSetInformation:
        return NDIS_STATUS_NOT_SUPPORTED;

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_CANCEL_DIRECT_OID_REQUEST TunCancelDirectOidRequest;
_Use_decl_annotations_
static VOID
TunCancelDirectOidRequest(NDIS_HANDLE MiniportAdapterContext, PVOID RequestId)
{
}

static MINIPORT_SYNCHRONOUS_OID_REQUEST TunSynchronousOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunSynchronousOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
    case NdisRequestSetInformation:
        return NDIS_STATUS_NOT_SUPPORTED;

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_UNLOAD TunUnload;
_Use_decl_annotations_
static VOID
TunUnload(PDRIVER_OBJECT DriverObject)
{
    PsSetCreateProcessNotifyRoutine(TunProcessNotification, TRUE);
    NdisMDeregisterMiniportDriver(NdisMiniportDriverHandle);
    ExDeleteResourceLite(&TunDispatchCtxGuard);
    ExDeleteResourceLite(&TunDispatchDeviceListLock);
}

DRIVER_INITIALIZE DriverEntry;
#ifdef ALLOC_PRAGMA
#    pragma alloc_text(INIT, DriverEntry)
#endif
_Use_decl_annotations_
NTSTATUS
DriverEntry(DRIVER_OBJECT *DriverObject, UNICODE_STRING *RegistryPath)
{
    NTSTATUS Status;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    NdisVersion = NdisGetVersion();
    if (NdisVersion < PB_NDIS_MINIMUM)
        return NDIS_STATUS_UNSUPPORTED_REVISION;
    if (NdisVersion < NDIS_MINIPORT_VERSION_MIN)
        return NDIS_STATUS_UNSUPPORTED_REVISION;
    if (NdisVersion > NDIS_MINIPORT_VERSION_MAX)
        NdisVersion = NDIS_MINIPORT_VERSION_MAX;

    Status = ExInitializeResourceLite(&TunDispatchCtxGuard);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ExInitializeResourceLite(&TunDispatchDeviceListLock);
    if (!NT_SUCCESS(Status)) { ExDeleteResourceLite(&TunDispatchCtxGuard); return Status; }

    NDIS_MINIPORT_DRIVER_CHARACTERISTICS miniport = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_680
                                    ? NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2
                                    : NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_680
                                ? NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2
                                : NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3 },

        .MajorNdisVersion = (UCHAR)((NdisVersion & 0x00ff0000) >> 16),
        .MinorNdisVersion = (UCHAR)(NdisVersion & 0x000000ff),

        .MajorDriverVersion = WINTUN_VERSION_MAJ,
        .MinorDriverVersion = WINTUN_VERSION_MIN,

        .InitializeHandlerEx = TunInitializeEx,
        .HaltHandlerEx = TunHaltEx,
        .UnloadHandler = TunUnload,
        .PauseHandler = TunPause,
        .RestartHandler = TunRestart,
        .OidRequestHandler = TunOidRequest,
        .SendNetBufferListsHandler = TunSendNetBufferLists,
        .ReturnNetBufferListsHandler = TunReturnNetBufferLists,
        .CancelSendHandler = TunCancelSend,
        .DevicePnPEventNotifyHandler = TunDevicePnPEventNotify,
        .ShutdownHandlerEx = TunShutdownEx,
        .CancelOidRequestHandler = TunCancelOidRequest,
        .DirectOidRequestHandler = TunDirectOidRequest,
        .CancelDirectOidRequestHandler = TunCancelDirectOidRequest,
        .SynchronousOidRequestHandler = TunSynchronousOidRequest
    };

    Status = PsSetCreateProcessNotifyRoutine(TunProcessNotification, FALSE);
    if (!NT_SUCCESS(Status))
        goto cleanupResources;

    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL, &miniport, &NdisMiniportDriverHandle);
    if (!NT_SUCCESS(Status))
        goto cleanupNotifier;

    NdisDispatchDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    NdisDispatchClose = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    NdisDispatchPnp = DriverObject->MajorFunction[IRP_MJ_PNP];
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TunDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = TunDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_PNP] = TunDispatchPnp;

    return STATUS_SUCCESS;

cleanupNotifier:
    PsSetCreateProcessNotifyRoutine(TunProcessNotification, TRUE);
cleanupResources:
    ExDeleteResourceLite(&TunDispatchCtxGuard);
    ExDeleteResourceLite(&TunDispatchDeviceListLock);
    return Status;
}
