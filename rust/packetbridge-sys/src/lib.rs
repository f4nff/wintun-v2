//! Fixed-width PacketBridgeV2 C ABI. Raw calls require the native ownership contract.
#![no_std]
use core::ffi::c_void;

pub const API_ABI_MAJOR: u32 = 2;
pub const API_ABI_MINOR: u32 = 1;
pub const DATA_ABI_MAJOR: u32 = 2;
pub const DATA_ABI_MINOR: u32 = 1;
pub const ABI_MAJOR: u32 = API_ABI_MAJOR;
pub const MAX_BATCH: usize = 64;
pub const MAX_SEGMENT_BATCH: usize = 32;
pub const DEFAULT_QUEUES: u32 = 4;
pub const DEFAULT_SLOTS: u32 = 256;
pub const DEFAULT_RING_CAPACITY: u32 = 512;
pub const MAX_REGION: u32 = 256 * 1024 * 1024;
pub const RX_BYTE_BUDGET: u32 = 512 * 1024;
pub const TX_BYTE_BUDGET: u32 = 128 * 1024;
pub const FILE_VERSION: u32 = 0x02000000;
pub const OK: u32 = 0;
pub const EMPTY: u32 = 3;
pub const BUSY: u32 = 6;
pub const QUARANTINED: u32 = 8;
/// Receive completion descriptor only: legacy wire value 4 is packet rejection.
/// An API call returning 4 still reports queue/ownership corruption.
pub const RX_REJECTED: u32 = 4;
pub const PLAIN: u32 = 0;
pub const TCP_GSO: u32 = 1;
pub const UDP_GSO: u32 = 2;
pub const OPEN_EXPERIMENTAL_OFFLOADS: u32 = 1;
pub const OPEN_CHECKED_SEGMENTS: u32 = 2;

pub const ADAPTER_DIAG_SCHEMA: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct AdapterDiagnostics {
    pub size: u32, pub schema: u32, pub stage: u32, pub attempts: u32,
    pub win32_error: u32, pub hresult: u32, pub first_win32_error: u32, pub first_hresult: u32,
    pub recovery_error: u32, pub removed_devices: u32, pub removed_packages: u32, pub need_reboot: u32,
    pub problem_code: u32, pub problem_status: u32, pub problem_fields_known: u32, pub reserved: u32,
    pub first_stage: u32, pub first_problem_code: u32, pub first_problem_status: u32, pub first_problem_fields_known: u32,
}
pub type CreateAdapterEx = unsafe extern "system" fn(*const u16, *const u16, *mut Native, *mut AdapterDiagnostics) -> u32;
pub const ADAPTER_DIAG_SCHEMA_EX: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AdapterDiagnosticsEx {
    pub base: AdapterDiagnostics,
    pub recovery_step: u32, pub matched_devices: u32, pub found_packages: u32, pub already_absent_packages: u32,
    pub probe_mismatch_mask: u32, pub observed_version: u32, pub observed_data_abi: u32, pub observed_probe_schema: u32,
    pub observed_source: [u8; 65], pub reserved: [u8; 3],
}
impl Default for AdapterDiagnosticsEx {
    fn default() -> Self {
        Self { base: AdapterDiagnostics::default(), recovery_step: 0, matched_devices: 0, found_packages: 0,
            already_absent_packages: 0, probe_mismatch_mask: 0, observed_version: 0, observed_data_abi: 0,
            observed_probe_schema: 0, observed_source: [0; 65], reserved: [0; 3] }
    }
}
pub type CreateAdapterEx2 = unsafe extern "system" fn(*const u16, *const u16, *mut Native, *mut AdapterDiagnosticsEx) -> u32;

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Token {
    pub epoch: u64,
    pub generation: u32,
    pub slot: u16,
    pub queue: u16,
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Descriptor {
    pub token: Token,
    pub kind: u32,
    pub length: u32,
    pub data_offset: u32,
    pub flags: u32,
    pub mss: u32,
    pub l4_offset: u16,
    pub header_length: u16,
    pub ip_version: u8,
    pub protocol: u8,
    pub reserved16: u16,
    pub status: u32,
    pub reserved64: [u64; 2],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Lease {
    pub descriptor: Descriptor,
    pub data: *mut u8,
    pub capacity: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Config {
    pub size: u32,
    pub queues: u32,
    pub slots: u32,
    pub capacity: u32,
    pub offloads: u32,
    pub flags: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Status {
    pub size: u32,
    pub state: u32,
    pub queues: u32,
    pub active_references: u32,
    pub epoch: u64,
    pub tx_objects: u64,
    pub rx_objects: u64,
    pub rx_returned: u64,
    pub indications: u64,
    pub completions: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Plan {
    pub source: u32,
    pub payload: u32,
    pub count: u32,
    pub mss: u32,
    pub header: u16,
    pub l4: u16,
    pub l4_header: u16,
    pub pseudo_source: u16,
    pub pseudo_destination: u16,
    pub ip: u8,
    pub protocol: u8,
    pub kind: u8,
    pub udp_zero: u8,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SegmentOutput {
    pub data: *mut u8,
    pub capacity: u32,
    pub length: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct QueueLayout {
    pub rings: [u32; 4],
    pub tx_data: u32,
    pub rx_data: u32,
    pub reserved: [u32; 2],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Layout {
    pub size: u32,
    pub magic: u32,
    pub major: u16,
    pub minor: u16,
    pub queues: u32,
    pub slots: u32,
    pub capacity: u32,
    pub block_size: u32,
    pub region_size: u32,
    pub lifecycle_offset: u32,
    pub lifecycle_size: u32,
    pub queue: [QueueLayout; 8],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Capabilities {
    pub size: u32,
    pub magic: u32,
    pub schema: u32,
    pub file_version: u32,
    pub ndis_version: u32,
    pub medium: u32,
    pub data_abi: u32,
    pub implemented: u32,
    pub allowed: u32,
    pub requested: u32,
    pub effective: u32,
    pub session_registered: u32,
    pub ipv4_encapsulation: u32,
    pub ipv6_encapsulation: u32,
    pub rx_implemented: u32,
    pub registration_status: u32,
    pub lso_nbls: u64,
    pub uso_nbls: u64,
    pub checksum_nbls: u64,
    pub rejected: u64,
    pub segments_copied: u64,
    pub parameter_changes: u64,
    pub rsc_packets: u64,
    pub rsc_bytes: u64,
    pub rsc_events: u64,
    pub rsc_aborts: u64,
    pub uro_packets: u64,
    pub uro_events: u64,
    pub source: [u8; 65],
    pub padding: [u8; 7],
}
pub type Native = *mut c_void;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Interface {
    pub size: u32,
    pub major: u32,
    pub minor: u32,
    pub file_version: u32,
    pub source: [u8; 65],
    pub reserved: [u8; 7],
    pub open: Option<unsafe extern "system" fn(*const u16, *const Config, *mut Native) -> u32>,
    pub close: Option<unsafe extern "system" fn(Native, u32) -> u32>,
    pub open_queue: Option<unsafe extern "system" fn(Native, u32, *mut Native) -> u32>,
    pub receive_batch: Option<unsafe extern "system" fn(Native, *mut Lease, u32, *mut u32) -> u32>,
    pub release_batch:
        Option<unsafe extern "system" fn(Native, *const Token, u32, *mut u32) -> u32>,
    pub reserve_batch: Option<unsafe extern "system" fn(Native, *mut Lease, u32, *mut u32) -> u32>,
    pub cancel_batch: Option<unsafe extern "system" fn(Native, *const Token, u32) -> u32>,
    pub commit_batch: Option<unsafe extern "system" fn(Native, *const Lease, u32, *mut u32) -> u32>,
    pub poll_completions:
        Option<unsafe extern "system" fn(Native, *mut Descriptor, u32, *mut u32) -> u32>,
    pub wait: Option<unsafe extern "system" fn(Native, u32) -> u32>,
    pub get_status: Option<unsafe extern "system" fn(Native, *mut Status) -> u32>,
    pub set_policy: Option<unsafe extern "system" fn(Native, u32) -> u32>,
    pub plan: Option<unsafe extern "C" fn(*const u8, u32, u32, u32, *mut Plan) -> u32>,
    pub write_segment:
        Option<unsafe extern "C" fn(*const u8, *const Plan, u32, *mut u8, u32, *mut u32) -> u32>,
    pub write_segment_batch: Option<
        unsafe extern "C" fn(*const u8, *const Plan, u32, *mut SegmentOutput, u32, *mut u32) -> u32,
    >,
    pub select_queue: Option<unsafe extern "C" fn(*const u8, u32, u32) -> u32>,
    pub create_adapter:
        Option<unsafe extern "system" fn(*const u16, *const u16, *mut Native) -> u32>,
    pub close_adapter: Option<unsafe extern "system" fn(Native)>,
    pub start_adapter_session:
        Option<unsafe extern "system" fn(Native, *const Config, *mut Native) -> u32>,
    pub adapter_luid: Option<unsafe extern "system" fn(Native, *mut u64) -> u32>,
    pub drain: Option<unsafe extern "system" fn(Native) -> u32>,
    pub wake: Option<unsafe extern "system" fn(Native, u32) -> u32>,
    pub capabilities: Option<unsafe extern "system" fn(Native, *mut Capabilities) -> u32>,
    pub prepare_close: Option<unsafe extern "system" fn(Native, u32) -> u32>,
}
pub type GetInterface = unsafe extern "system" fn(u32, u32, *mut Interface) -> u32;

pub const PACKET_ABI_MAJOR: u32 = 1;
pub const PACKET_ABI_MINOR: u32 = 1;
pub const PACKET_CAP_CHECKED_SOURCE: u32 = 1;
pub const PACKET_CAP_RETIRED_UNSIZED: u32 = 2;
pub const PACKET_CAP_DISJOINT_OUTPUTS: u32 = 4;
pub const PACKET_REQUIRED_CAPS: u32 = PACKET_CAP_CHECKED_SOURCE | PACKET_CAP_RETIRED_UNSIZED | PACKET_CAP_DISJOINT_OUTPUTS;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PacketInterface {
    pub size: u32,
    pub major: u32,
    pub minor: u32,
    pub capabilities: u32,
    pub write_segment: Option<
        unsafe extern "C" fn(*const u8, u32, *const Plan, u32, *mut u8, u32, *mut u32) -> u32,
    >,
    pub write_segment_batch: Option<
        unsafe extern "C" fn(*const u8, u32, *const Plan, u32, *mut SegmentOutput, u32, *mut u32) -> u32,
    >,
}
pub type GetPacketInterface = unsafe extern "system" fn(u32, u32, *mut PacketInterface) -> u32;

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct CloseProgress {
    pub size: u32,
    pub phase: u32,
    pub flags: u32,
    pub result: u32,
    pub session_id: u64,
    pub policy_requested: u64,
    pub policy_completed: u64,
    pub retained_bytes: u64,
    pub user_leases: u32,
    pub published: u32,
    pub native_references: u32,
    pub last_error: u32,
    pub block_stage: u32,
    pub reserved: u32,
    pub started: u64,
    pub last_progress: u64,
    pub rechecks: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct RuntimeStatus {
    pub size: u32,
    pub sessions: u32,
    pub retained_bytes: u64,
    pub max_sessions: u32,
    pub reserved: u32,
    pub max_retained_bytes: u64,
    pub adapters: u32,
    pub max_adapters: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LifecycleInterface {
    pub size: u32,
    pub major: u32,
    pub minor: u32,
    pub capabilities: u32,
    pub begin_stop: Option<unsafe extern "system" fn(Native) -> u32>,
    pub begin_policy: Option<unsafe extern "system" fn(Native, u32, *mut u64) -> u32>,
    pub poll_policy: Option<unsafe extern "system" fn(Native, u64, u32, *mut u32) -> u32>,
    pub progress: Option<unsafe extern "system" fn(Native, *mut CloseProgress) -> u32>,
    pub release: Option<unsafe extern "system" fn(Native)>,
    pub runtime: Option<unsafe extern "system" fn(*mut RuntimeStatus) -> u32>,
    pub retained: Option<unsafe extern "system" fn(u32, *mut CloseProgress) -> u32>,
    pub forget_policy: Option<unsafe extern "system" fn(Native, u64)>,
}
pub type GetLifecycleInterface =
    unsafe extern "system" fn(u32, u32, *mut LifecycleInterface) -> u32;

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of};
    #[test]
    fn matches_c_abi_sizes_and_offsets() {
        assert_eq!(RX_REJECTED, 4); // Preserve the existing descriptor wire contract.
        assert_eq!(size_of::<Token>(), 16);
        assert_eq!(align_of::<Token>(), 8);
        assert_eq!(size_of::<Descriptor>(), 64);
        assert_eq!(offset_of!(Descriptor, status), 44);
        assert_eq!(size_of::<Lease>(), 80);
        assert_eq!(offset_of!(Lease, data), 64);
        assert_eq!(size_of::<Config>(), 24);
        assert_eq!(size_of::<Status>(), 64);
        assert_eq!(size_of::<Plan>(), 32);
        assert_eq!(offset_of!(Plan, ip), 26);
        assert_eq!(size_of::<SegmentOutput>(), 16);
        assert_eq!(size_of::<Interface>(), 280);
        assert_eq!(size_of::<PacketInterface>(), 32);
        assert_eq!(offset_of!(PacketInterface, write_segment), 16);
        assert_eq!(size_of::<LifecycleInterface>(), 80);
        assert_eq!(size_of::<CloseProgress>(), 96);
        assert_eq!(size_of::<RuntimeStatus>(), 40);
        assert_eq!(size_of::<Capabilities>(), 232);
        assert_eq!(size_of::<Layout>(), 296);
        assert_eq!(offset_of!(Interface, open), 88);
        assert_eq!(offset_of!(Interface, select_queue), 208);
    }
}
