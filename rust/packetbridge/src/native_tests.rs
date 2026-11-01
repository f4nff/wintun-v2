use super::*;
use std::time::{Duration, Instant};

unsafe extern "system" fn failed_adapter_recovery(_: *const u16, _: *const u16, out: *mut raw::Native,
    diagnostics: *mut raw::AdapterDiagnostics) -> u32
{
    unsafe {
        *out = ptr::null_mut();
        *diagnostics = raw::AdapterDiagnostics {
            size: size_of::<raw::AdapterDiagnostics>() as u32, schema: raw::ADAPTER_DIAG_SCHEMA,
            stage: 4, attempts: 2, win32_error: 0xe000024b, hresult: 0x8007001f,
            first_stage: 4, first_win32_error: 31, first_hresult: 0x8007001f,
            problem_code: 52, problem_status: 0xc0000428, problem_fields_known: 3,
            first_problem_code: 52, first_problem_status: 0xc0000428, first_problem_fields_known: 3,
            removed_devices: 2, removed_packages: 3, need_reboot: 1, ..Default::default()
        };
    }
    9
}

#[test]
fn adapter_creation_extension_preserves_raw_errors_and_rejects_inputs_before_native_install() {
    assert_eq!(size_of::<raw::AdapterDiagnostics>(), 80);
    let path = std::env::var_os("PB_TEST_DLL").unwrap();
    let mut library = unsafe { Library::load(Path::new(&path)) }.unwrap();
    assert!(library.create_adapter_ex.is_some());
    let error = library.create_adapter_with_diagnostics("", "test").err().unwrap();
    assert_eq!((error.status, error.diagnostics.win32_error, error.diagnostics.attempts), (1, 87, 0));
    assert_eq!(size_of::<raw::AdapterDiagnosticsEx>(), 180);
    assert!(library.create_adapter_ex2.is_some() && error.details.is_some());
    Arc::get_mut(&mut library).unwrap().create_adapter_ex2 = None;
    Arc::get_mut(&mut library).unwrap().create_adapter_ex = Some(failed_adapter_recovery);
    let error = library.create_adapter_with_diagnostics("VPN-Link", "test").err().unwrap();
    assert_eq!((error.status, error.diagnostics.first_problem_status, error.diagnostics.removed_packages), (9, 0xc0000428, 3));
    let text = error.to_string();
    assert!(text.contains("win32=0xe000024b") && text.contains("first_win32=0x0000001f") && text.contains("0xc0000428"));
    Arc::get_mut(&mut library).unwrap().create_adapter_ex = None;
    let error = library.create_adapter_with_diagnostics("VPN-Link", "test").err().unwrap();
    assert_eq!((error.status, error.diagnostics.win32_error), (9, 127));
}

unsafe fn symbol<T: Copy>(library: &Library, name: &std::ffi::CStr) -> T {
    let pointer = unsafe { GetProcAddress(library.module.as_ptr(), name.as_ptr().cast()) };
    assert!(!pointer.is_null());
    assert_eq!(size_of::<T>(), size_of::<raw::Native>());
    unsafe { std::mem::transmute_copy(&pointer) }
}
unsafe extern "system" fn failed_named_recovery(_: *const u16, _: *const u16, out: *mut raw::Native,
    diagnostics: *mut raw::AdapterDiagnosticsEx) -> u32
{
    unsafe {
        assert_eq!(((*diagnostics).base.size, (*diagnostics).base.schema), (180, 2));
        let status = failed_adapter_recovery(ptr::null(), ptr::null(), out, &mut (*diagnostics).base);
        (*diagnostics).base.size = 180; (*diagnostics).base.schema = 2;
        (*diagnostics).recovery_step = 6; (*diagnostics).matched_devices = 2;
        (*diagnostics).found_packages = 1; (*diagnostics).probe_mismatch_mask = 64;
        (&mut (*diagnostics).observed_source)[..64].fill(b'a');
        status
    }
}
#[test]
fn named_adapter_report_preserves_old_prefix_and_identifies_native_failure_api() {
    let path = std::env::var_os("PB_TEST_DLL").unwrap();
    let mut library = unsafe { Library::load(Path::new(&path)) }.unwrap();
    Arc::get_mut(&mut library).unwrap().create_adapter_ex2 = Some(failed_named_recovery);
    let error = library.create_adapter_with_diagnostics("VPN-Link", "test").err().unwrap();
    assert_eq!((error.diagnostics.size, error.diagnostics.schema), (80, 1));
    let details = error.details.as_ref().unwrap();
    assert_eq!((details.base.size, details.base.schema, details.probe_mismatch_mask), (180, 2, 64));
    let text = error.to_string();
    assert!(text.contains("recovery_api=DiUninstallDriverW") && text.contains("matched_devices=2") &&
        text.contains("observed_source=aaaaaaaa") && text.contains("0xc0000428"));
}
fn fixture_with_config(config: Config) -> Session {
    let path = std::env::var_os("PB_TEST_DLL")
        .expect("build Tests first; PB_TEST_DLL must name pbapi-testshim.dll");
    let library = unsafe { Library::load(Path::new(&path)) }.unwrap();
    type Open = unsafe extern "C" fn(
        *const raw::Config,
        *mut raw::Native,
        *mut raw::Native,
        *mut raw::Layout,
        *mut u64,
    ) -> u32;
    let open: Open = unsafe { symbol(&library, c"PbTestOpen") };
    let config = config.raw();
    let (mut native, mut region, mut epoch) = (ptr::null_mut(), ptr::null_mut(), 0);
    let mut layout = raw::Layout::default();
    assert_eq!(
        unsafe { open(&config, &mut native, &mut region, &mut layout, &mut epoch) },
        0
    );
    assert_eq!(layout.region_size as usize % 4096, 0);
    Session::adopt(library, native).unwrap()
}
fn fixture() -> Session {
    fixture_with_config(Config {
        queues: 1,
        slots: 2,
        ring_capacity: 2,
        ..Config::default()
    })
}
#[test]
fn throughput_defaults_support_64_object_ffi_batches_and_keep_segment_limit_separate() {
    let config = Config::default();
    assert_eq!((config.queues, config.slots, config.ring_capacity), (4, 256, 512));
    assert_eq!((raw::MAX_BATCH, raw::MAX_SEGMENT_BATCH), (64, 32));
    let mut session = fixture_with_config(config);
    assert_eq!(session.queue_count(), 4);
    type Take = unsafe extern "C" fn(raw::Native, u32, *mut raw::Descriptor, u32, *mut u32) -> u32;
    type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
    let take: Take = unsafe { symbol(&session.library, c"PbTestTake") };
    let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
    for qi in 0..4 {
        let q = session.queue(qi).unwrap();
        assert!(matches!(q.reserve(65), Err(Error(1))));
        assert!(matches!(q.receive(65), Err(Error(1))));
        let mut reserved = q.reserve(64).unwrap();
        assert_eq!(reserved.len(), 64);
        for i in 0..64 {
            reserved.data_mut(i).unwrap()[..20].fill(i as u8);
            reserved.set_length(i, 20).unwrap();
        }
        assert_eq!(reserved.commit().unwrap(), 64);
        drop(reserved);
        let mut descriptors = [raw::Descriptor::default(); 64];
        let mut count = 0;
        assert_eq!(unsafe { take(session.pointer().unwrap(), qi, descriptors.as_mut_ptr(), 64, &mut count) }, 0);
        assert_eq!(count, 64);
        descriptors.reverse();
        assert_eq!(unsafe { complete(session.pointer().unwrap(), qi, descriptors.as_ptr(), 64) }, 0);
        assert_eq!(q.completions(64).unwrap().len(), 64);
    }
    {
        let q = session.queue(0).unwrap();
        for _ in 0..64 { assert_eq!(inject(&session, &[0x45; 20], raw::PLAIN, 0), 0); }
        let batch = q.receive(64).unwrap();
        assert_eq!(batch.len(), 64);
        let mut buffers = [[0u8; 20]; 33];
        let mut outputs: Vec<&mut [u8]> = buffers.iter_mut().map(|b| &mut b[..]).collect();
        assert!(matches!(batch.write_segments(0, 0, &mut outputs), Err(Error(1))));
    }
    session.close(1000).unwrap();
}
fn inject(session: &Session, bytes: &[u8], kind: u32, mss: u32) -> u32 {
    type Send = unsafe extern "C" fn(raw::Native, u32, *const u8, u32, u32, u32) -> u32;
    let send: Send = unsafe { symbol(&session.library, c"PbTestSend") };
    unsafe {
        send(
            session.pointer().unwrap(),
            0,
            bytes.as_ptr(),
            bytes.len() as u32,
            kind,
            mss,
        )
    }
}
fn active(session: &Session, refs: u32) {
    let set: unsafe extern "C" fn(raw::Native, u32) =
        unsafe { symbol(&session.library, c"PbTestActive") };
    unsafe {
        set(session.pointer().unwrap(), refs);
    }
}
fn await_waiter(session: &Session) {
    let get: unsafe extern "C" fn(raw::Native) -> u32 =
        unsafe { symbol(&session.library, c"PbTestWaiters") };
    let start = Instant::now();
    while unsafe { get(session.pointer().unwrap()) } == 0 {
        assert!(start.elapsed() < Duration::from_secs(2));
        std::thread::yield_now();
    }
}
#[test]
fn faulted_native_session_still_accepts_batch_drop_retirement() {
    let mut session = fixture();
    {
        let q = session.queue(0).unwrap();
        assert_eq!(inject(&session, &[0x45; 20], 0, 0), 0);
        let received = q.receive(1).unwrap();
        let reserved = q.reserve(1).unwrap();
        type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
        let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
        let mut bad = raw::Descriptor::default();
        bad.token.epoch = session.epoch.wrapping_add(1);
        bad.token.generation = 1;
        assert_eq!(
            unsafe { complete(session.pointer().unwrap(), 0, &bad, 1) },
            0
        );
        assert!(matches!(q.completions(1), Err(Error(4))));
        drop(received);
        drop(reserved);
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
    }
    session.prepare_close(1000).unwrap();
    session.close(1000).unwrap();
}

#[test]
fn real_c_batch_drop_returns_each_held_buffer_before_reuse() {
    let mut session = fixture();
    {
        let q = session.queue(0).unwrap();
        assert_eq!(inject(&session, &[0x45; 20], 0, 0), 0);
        assert_eq!(inject(&session, &[0x46; 20], 0, 0), 0);
        assert_eq!(inject(&session, &[0x47; 20], 0, 0), 2);
        let batch = q.receive(32).unwrap();
        assert_eq!(batch.len(), 2);
        assert_eq!(batch.data(0).unwrap(), &[0x45; 20]);
        drop(batch);
        assert_eq!(inject(&session, &[0x48; 20], 0, 0), 0);
        assert_eq!(inject(&session, &[0x49; 20], 0, 0), 0);
        let batch = q.receive(32).unwrap();
        assert_eq!(batch.metadata(0).unwrap().token.generation, 2);
        drop(batch);
    }
    session.close(1000).unwrap();
}
#[test]
fn real_c_prefix_return_reuses_only_completed_slots_while_tail_stays_borrowed() {
    let mut session = fixture();
    {
        let q = session.queue(0).unwrap();
        assert_eq!(inject(&session, &[0x45; 20], 0, 0), 0);
        assert_eq!(inject(&session, &[0x46; 20], 0, 0), 0);
        let mut batch = q.receive(32).unwrap();
        let first = batch.metadata(0).unwrap().token;
        let tail = batch.metadata(1).unwrap().token;
        assert_eq!(batch.release_prefix(3), Err(Error(1)));
        assert_eq!(batch.release_prefix(0).unwrap(), 0);
        assert_eq!(batch.len(), 2);
        assert_eq!(batch.release_prefix(1).unwrap(), 1);
        assert_eq!(batch.len(), 1);
        assert_eq!(batch.metadata(0).unwrap().token.slot, tail.slot);
        assert_eq!(inject(&session, &[0x47; 20], 0, 0), 0);
        assert_eq!(inject(&session, &[0x48; 20], 0, 0), 2); // PB_FULL
        let next = q.receive(32).unwrap();
        assert_eq!(next.metadata(0).unwrap().token.slot, first.slot);
        assert_eq!(
            next.metadata(0).unwrap().token.generation,
            first.generation + 1
        );
        assert_eq!(batch.data(0).unwrap(), &[0x46; 20]);
        drop(next);
        drop(batch);
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
    }
    session.close(1000).unwrap();
}
#[test]
fn real_c_cancel_and_out_of_order_completion_preserve_ownership() {
    let mut session = fixture();
    {
        let q = session.queue(0).unwrap();
        let reserved = q.reserve(8).unwrap();
        assert_eq!(reserved.len(), 2);
        drop(reserved);
        let mut reserved = q.reserve(2).unwrap();
        for i in 0..2 {
            reserved.data_mut(i).unwrap()[..20].fill(0x45);
            reserved.set_length(i, 20).unwrap();
        }
        assert_eq!(reserved.commit().unwrap(), 2);
        assert!(reserved.is_empty());
        drop(reserved);
        assert!(matches!(q.reserve(1), Err(Error(2))));
        type Take =
            unsafe extern "C" fn(raw::Native, u32, *mut raw::Descriptor, u32, *mut u32) -> u32;
        type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
        let take: Take = unsafe { symbol(&session.library, c"PbTestTake") };
        let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
        let mut descriptors = [raw::Descriptor::default(); 2];
        let mut count = 0;
        assert_eq!(
            unsafe {
                take(
                    session.pointer().unwrap(),
                    0,
                    descriptors.as_mut_ptr(),
                    2,
                    &mut count,
                )
            },
            0
        );
        assert_eq!(count, 2);
        descriptors.swap(0, 1);
        descriptors[0].status = 1;
        assert_eq!(
            unsafe { complete(session.pointer().unwrap(), 0, descriptors.as_ptr(), 2) },
            0
        );
        let completions = q.completions(32).unwrap();
        assert_eq!(completions.len(), 2);
        assert_eq!(completions[0].status, 1);
        assert_eq!(completions[0].token, descriptors[0].token);
        let batch = q.reserve(2).unwrap();
        assert_eq!(batch.len(), 2);
        drop(batch);
    }
    session.close(1000).unwrap();
}
#[test]
fn real_c_timeout_keeps_session_until_native_reference_returns() {
    let mut session = fixture();
    active(&session, 1);
    assert_eq!(session.close(0), Err(Error(8)));
    assert_eq!(session.status().unwrap().active_references, 1);
    active(&session, 0);
    session.close(1000).unwrap();
}
#[test]
fn prepare_close_retains_a_queryable_handle_and_waits_for_refs_and_leases() {
    let mut session = fixture();
    let caps = session.capabilities().unwrap();
    assert_eq!((caps.data_abi, caps.file_version), (2, raw::FILE_VERSION));
    {
        let q = session.queue(0).unwrap();
        let mut reserved = q.reserve(2).unwrap();
        reserved.cancel(0).unwrap();
        assert_eq!(reserved.len(), 1);
        assert_eq!(session.prepare_close(0), Err(Error(6)));
        drop(reserved);
    }
    active(&session, 1);
    assert_eq!(session.prepare_close(0), Err(Error(8)));
    active(&session, 0);
    session.prepare_close(1000).unwrap();
    assert_eq!(session.status().unwrap().active_references, 0);
    session.prepare_close(1000).unwrap();
    session.close(1000).unwrap();
}
#[test]
fn cross_thread_wake_is_control_only_and_does_not_transfer_queue_ownership() {
    let mut session = fixture();
    std::thread::scope(|scope| {
        let worker = scope.spawn(|| {
            let q = session.queue(0).unwrap();
            assert!(q.wait(u32::MAX).unwrap());
        });
        await_waiter(&session);
        let other_owner = session.queue(0).unwrap();
        assert!(matches!(other_owner.receive(1), Err(Error(6))));
        session.wake(0).unwrap();
        worker.join().unwrap();
    });
    session.close(1000).unwrap();
}
#[test]
fn shared_stop_interrupts_infinite_wait_before_taking_exclusive_control() {
    let mut session = fixture();
    std::thread::scope(|scope| {
        let worker = scope.spawn(|| {
            let q = session.queue(0).unwrap();
            assert_eq!(q.wait(u32::MAX), Err(Error(7)));
        });
        await_waiter(&session);
        session.request_stop().unwrap();
        worker.join().unwrap();
    });
    session.close(1000).unwrap();
}
fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = 0u32;
    for chunk in bytes.chunks(2) {
        sum += u16::from_be_bytes([chunk[0], *chunk.get(1).unwrap_or(&0)]) as u32;
    }
    while sum >> 16 != 0 {
        sum = (sum & 65535) + (sum >> 16);
    }
    !(sum as u16)
}
#[test]
fn real_c_gso_crosses_ffi_once_per_batch_and_preserves_wire_checksums() {
    let mut session = fixture();
    unsafe extern "C" fn reject_legacy_writer(
        _: *const u8,
        _: *const raw::Plan,
        _: u32,
        _: *mut raw::SegmentOutput,
        _: u32,
        _: *mut u32,
    ) -> u32 {
        1
    }
    // Poison the legacy slot: successful output must use the checked extension.
    Arc::get_mut(&mut session.library)
        .unwrap()
        .api
        .write_segment_batch = Some(reject_legacy_writer);
    {
        let q = session.queue(0).unwrap();
        let mut source = vec![0u8; 2540];
        source[0] = 0x45;
        source[8] = 64;
        source[9] = 6;
        source[12..16].copy_from_slice(&[10, 0, 0, 1]);
        source[16..20].copy_from_slice(&[10, 0, 0, 2]);
        source[20..24].copy_from_slice(&[0x12, 0x34, 0x56, 0x78]);
        source[24..28].copy_from_slice(&0xfffffff0u32.to_be_bytes());
        source[32] = 0x50;
        source[33] = 0x19;
        source[40..].fill(0xa5);
        assert_eq!(inject(&session, &source, raw::TCP_GSO, 1000), 0);
        let batch = q.receive(32).unwrap();
        assert_eq!(batch.segment_count(0).unwrap(), 3);
        let (mut a, mut b, mut c) = ([0u8; 1500], [0u8; 1500], [0u8; 1500]);
        let lengths = batch
            .write_segments(0, 0, &mut [&mut a, &mut b, &mut c])
            .unwrap();
        assert_eq!(lengths, vec![1040, 1040, 540]);
        for (i, bytes) in [a.as_slice(), b.as_slice(), c.as_slice()]
            .into_iter()
            .enumerate()
        {
            let bytes = &bytes[..lengths[i]];
            assert_eq!(checksum(&bytes[..20]), 0);
            let mut pseudo = Vec::from(&bytes[12..20]);
            pseudo.extend_from_slice(&[0, 6]);
            pseudo.extend_from_slice(&((bytes.len() - 20) as u16).to_be_bytes());
            pseudo.extend_from_slice(&bytes[20..]);
            assert_eq!(checksum(&pseudo), 0);
            assert!(bytes[40..].iter().all(|b| *b == 0xa5));
            assert_eq!(
                u32::from_be_bytes(bytes[24..28].try_into().unwrap()),
                0xfffffff0u32.wrapping_add((i * 1000) as u32)
            );
            assert_eq!(bytes[33], if i == 2 { 0x19 } else { 0x10 });
        }
    }
    session.close(1000).unwrap();
}
#[test]
fn checked_packet_extension_rejects_forged_source_length_without_output() {
    let session = fixture();
    let mut source = [0u8; 64];
    source[0] = 0x45;
    source[9] = 6;
    source[32] = 0x50;
    let mut plan = raw::Plan::default();
    assert_eq!(
        unsafe {
            session.library.api.plan.unwrap()(source.as_ptr(), 64, raw::TCP_GSO, 12, &mut plan)
        },
        0
    );
    plan.source = 76;
    plan.payload = 36;
    plan.count = 3;
    let mut output = [0xa5; 64];
    let mut written = u32::MAX;
    assert_eq!(
        unsafe {
            session.library.packet.write_segment.unwrap()(
                source.as_ptr(),
                64,
                &plan,
                2,
                output.as_mut_ptr(),
                64,
                &mut written,
            )
        },
        1
    );
    assert_eq!(written, 0);
    let mut item = raw::SegmentOutput {
        data: output.as_mut_ptr(),
        capacity: 64,
        length: u32::MAX,
    };
    assert_eq!(
        unsafe {
            session.library.packet.write_segment_batch.unwrap()(
                source.as_ptr(),
                64,
                &plan,
                2,
                &mut item,
                1,
                &mut written,
            )
        },
        1
    );
    assert_eq!((written, item.length), (0, u32::MAX));
    assert_eq!(output, [0xa5; 64]);
}
#[test]
fn packet_negotiation_requires_retirement_and_disjoint_output_guarantees() {
    let session = fixture();
    let original = session.library.packet;
    assert!(validate_packet_interface(&original).is_ok());
    for capability in [
        raw::PACKET_CAP_CHECKED_SOURCE,
        raw::PACKET_CAP_RETIRED_UNSIZED,
        raw::PACKET_CAP_DISJOINT_OUTPUTS,
    ] {
        let mut table = original;
        table.capabilities &= !capability;
        assert!(validate_packet_interface(&table).is_err());
    }
    let mut table = original;
    table.minor = 0;
    assert!(validate_packet_interface(&table).is_err());
    table = original;
    table.major += 1;
    assert!(validate_packet_interface(&table).is_err());
    table = original;
    table.size = 16;
    assert!(validate_packet_interface(&table).is_err());
    table = original;
    table.write_segment_batch = None;
    assert!(validate_packet_interface(&table).is_err());
    table = original;
    table.write_segment = None;
    assert!(validate_packet_interface(&table).is_err());
    table = original;
    table.minor += 1;
    table.capabilities |= 0x8000_0000;
    assert!(validate_packet_interface(&table).is_ok());
}
#[test]
fn relative_dll_paths_are_rejected_before_native_loading() {
    assert!(unsafe { Library::load(Path::new("packetbridge.dll")) }.is_err());
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateEventW(
        attributes: raw::Native,
        manual: i32,
        initial: i32,
        name: *const u16,
    ) -> raw::Native;
    fn SetEvent(event: raw::Native) -> i32;
    fn WaitForSingleObject(event: raw::Native, timeout: u32) -> u32;
    fn CloseHandle(handle: raw::Native) -> i32;
}
fn block(session: &Session, stage: u32, entered: raw::Native, gate: raw::Native) {
    let configure: unsafe extern "C" fn(raw::Native, u32, raw::Native, raw::Native) =
        unsafe { symbol(&session.library, c"PbTestBlock") };
    unsafe {
        configure(session.pointer().unwrap(), stage, entered, gate);
    }
}
fn events() -> (raw::Native, raw::Native) {
    let entered = unsafe { CreateEventW(ptr::null_mut(), 1, 0, ptr::null()) };
    let gate = unsafe { CreateEventW(ptr::null_mut(), 1, 0, ptr::null()) };
    assert!(!entered.is_null() && !gate.is_null());
    (entered, gate)
}
#[test]
fn drop_hands_off_a_blocked_drain_without_waiting_for_io() {
    let session = fixture();
    let library = session.library.clone();
    let id = session.epoch;
    let (entered, gate) = events();
    block(&session, 1, entered, gate);
    session.request_stop().unwrap();
    assert_eq!(unsafe { WaitForSingleObject(entered, 2000) }, 0);
    let (send, receive) = std::sync::mpsc::channel();
    let dropping = std::thread::spawn(move || {
        drop(session);
        send.send(()).unwrap();
    });
    let returned = receive.recv_timeout(Duration::from_secs(2));
    assert_ne!(unsafe { SetEvent(gate) }, 0);
    dropping.join().unwrap();
    assert!(returned.is_ok());
    let start = Instant::now();
    let retained: unsafe extern "C" fn(u64) -> u32 = unsafe { symbol(&library, c"PbTestRetained") };
    while unsafe { retained(id) } != 0 {
        assert!(start.elapsed() < Duration::from_secs(2));
        std::thread::yield_now();
    }
    assert_ne!(unsafe { CloseHandle(entered) }, 0);
    assert_ne!(unsafe { CloseHandle(gate) }, 0);
}
#[test]
fn dropping_a_policy_ticket_releases_its_slot_after_native_completion() {
    let mut session = fixture();
    let (entered, gate) = events();
    block(&session, 0, entered, gate);
    let mut change = session.request_policy(0).unwrap();
    assert_eq!(unsafe { WaitForSingleObject(entered, 2000) }, 0);
    assert!(!change.wait(0).unwrap());
    drop(change);
    assert!(matches!(session.request_policy(0), Err(Error(6))));
    assert_ne!(unsafe { SetEvent(gate) }, 0);
    let start = Instant::now();
    loop {
        let p = session.close_progress().unwrap();
        if p.policy_requested == p.policy_completed {
            break;
        }
        assert!(start.elapsed() < Duration::from_secs(2));
        std::thread::yield_now();
    }
    session.set_policy_timeout(0, 2000).unwrap();
    session.close(2000).unwrap();
    assert_ne!(unsafe { CloseHandle(entered) }, 0);
    assert_ne!(unsafe { CloseHandle(gate) }, 0);
}
