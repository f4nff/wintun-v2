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

type Batch32Leases = unsafe extern "system" fn(raw::Native, *mut raw::Lease, u32, *mut u32) -> u32;
type Batch32Commit = unsafe extern "system" fn(raw::Native, *const raw::Lease, u32, *mut u32) -> u32;
type Batch32Release = unsafe extern "system" fn(raw::Native, *const raw::Token, u32, *mut u32) -> u32;
type Batch32Cancel = unsafe extern "system" fn(raw::Native, *const raw::Token, u32) -> u32;
type Batch32Writer = unsafe extern "C" fn(
    *const u8, u32, *const raw::Plan, u32, *mut raw::SegmentOutput, u32, *mut u32,
) -> u32;
type Batch32UnsizedWriter = unsafe extern "C" fn(
    *const u8, *const raw::Plan, u32, *mut raw::SegmentOutput, u32, *mut u32,
) -> u32;

#[derive(Clone, Copy, Default)]
struct Batch32Calls {
    reserve: Option<Batch32Leases>,
    receive: Option<Batch32Leases>,
    commit: Option<Batch32Commit>,
    release: Option<Batch32Release>,
    cancel: Option<Batch32Cancel>,
    writer: Option<Batch32Writer>,
    unsized_writer: Option<Batch32UnsizedWriter>,
    reserve_calls: u32,
    reserve_requested: u32,
    reserve_accepted: u32,
    receive_calls: u32,
    receive_requested: u32,
    receive_accepted: u32,
    commit_calls: u32,
    commit_requested: u32,
    commit_accepted: u32,
    commit_last_result: u32,
    commit_last_accepted: u32,
    release_calls: u32,
    release_requested: u32,
    cancel_calls: u32,
    cancel_requested: u32,
    cancel_succeeded: u32,
    writer_calls: u32,
    writer_requested: u32,
    writer_last_first: u32,
    writer_last_count: u32,
    writer_last_result: u32,
    writer_last_written: u32,
    expected_source: usize,
    expected_length: u32,
    expected_outputs: [usize; raw::MAX_SEGMENT_BATCH],
    expected_first: u32,
    expected_count: u32,
    direct_output_buffers: bool,
}
std::thread_local! {
    // Queue calls stay on their owning Rust test thread. Each test owns its
    // Library table; parallel tests neither share these delegates nor counts.
    static BATCH32_CALLS: std::cell::Cell<Batch32Calls> = const {
        std::cell::Cell::new(Batch32Calls {
            reserve: None, receive: None, commit: None, release: None, cancel: None, writer: None, unsized_writer: None,
            reserve_calls: 0, reserve_requested: 0, reserve_accepted: 0,
            receive_calls: 0, receive_requested: 0, receive_accepted: 0,
            commit_calls: 0, commit_requested: 0, commit_accepted: 0, commit_last_result: 0, commit_last_accepted: 0,
            release_calls: 0, release_requested: 0, cancel_calls: 0, cancel_requested: 0, cancel_succeeded: 0,
            writer_calls: 0, writer_requested: 0, writer_last_first: 0, writer_last_count: 0, writer_last_result: 0, writer_last_written: 0,
            expected_source: 0, expected_length: 0, expected_outputs: [0; raw::MAX_SEGMENT_BATCH],
            expected_first: 0, expected_count: 32, direct_output_buffers: false,
        })
    };
}
unsafe extern "system" fn batch32_reserve(q: raw::Native, leases: *mut raw::Lease, n: u32, done: *mut u32) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.reserve_calls += 1; calls.reserve_requested += n;
        cell.set(calls); calls.reserve.unwrap()
    });
    let result = unsafe { next(q, leases, n, done) };
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); if result == raw::OK { calls.reserve_accepted += unsafe { *done }; } cell.set(calls);
    });
    result
}
unsafe extern "system" fn batch32_receive(q: raw::Native, leases: *mut raw::Lease, n: u32, done: *mut u32) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.receive_calls += 1; calls.receive_requested += n;
        cell.set(calls); calls.receive.unwrap()
    });
    let result = unsafe { next(q, leases, n, done) };
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); if result == raw::OK { calls.receive_accepted += unsafe { *done }; } cell.set(calls);
    });
    result
}
unsafe extern "system" fn batch32_commit(q: raw::Native, leases: *const raw::Lease, n: u32, done: *mut u32) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.commit_calls += 1; calls.commit_requested += n;
        cell.set(calls); calls.commit.unwrap()
    });
    let result = unsafe { next(q, leases, n, done) };
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.commit_last_result = result; calls.commit_last_accepted = unsafe { *done };
        if result == raw::OK { calls.commit_accepted += unsafe { *done }; } cell.set(calls);
    });
    result
}
unsafe extern "system" fn batch32_release(q: raw::Native, tokens: *const raw::Token, n: u32, done: *mut u32) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.release_calls += 1; calls.release_requested += n;
        cell.set(calls); calls.release.unwrap()
    });
    unsafe { next(q, tokens, n, done) }
}
unsafe extern "system" fn batch32_cancel(q: raw::Native, tokens: *const raw::Token, n: u32) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.cancel_calls += 1; calls.cancel_requested += n;
        cell.set(calls); calls.cancel.unwrap()
    });
    let result = unsafe { next(q, tokens, n) };
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); if result == raw::OK { calls.cancel_succeeded += n; } cell.set(calls);
    });
    result
}
unsafe extern "C" fn batch32_writer(
    source: *const u8, length: u32, plan: *const raw::Plan, first: u32,
    outputs: *mut raw::SegmentOutput, n: u32, done: *mut u32,
) -> u32 {
    let next = BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.writer_calls += 1; calls.writer_requested += n;
        calls.direct_output_buffers = source as usize == calls.expected_source && length == calls.expected_length
            && first == calls.expected_first && n == calls.expected_count && n <= 32 && !outputs.is_null()
            && unsafe { std::slice::from_raw_parts(outputs, n as usize) }.iter().enumerate()
                .all(|(i, output)| output.data as usize == calls.expected_outputs[i]);
        cell.set(calls); calls.writer.unwrap()
    });
    let result = unsafe { next(source, length, plan, first, outputs, n, done) };
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get(); calls.writer_last_first = first; calls.writer_last_count = n;
        calls.writer_last_result = result; calls.writer_last_written = unsafe { *done }; cell.set(calls);
    });
    result
}
unsafe extern "C" fn batch32_reject_unsized(
    _: *const u8, _: *const raw::Plan, _: u32, _: *mut raw::SegmentOutput, _: u32, _: *mut u32,
) -> u32 { 1 }

struct Batch32Counter;
impl Batch32Counter {
    fn attach(session: &mut Session) -> Self {
        let library = Arc::get_mut(&mut session.library).unwrap();
        BATCH32_CALLS.with(|cell| {
            assert!(cell.get().writer.is_none());
            cell.set(Batch32Calls {
                reserve: library.api.reserve_batch, receive: library.api.receive_batch,
                commit: library.api.commit_batch, release: library.api.release_batch, cancel: library.api.cancel_batch,
                writer: library.packet.write_segment_batch, unsized_writer: library.api.write_segment_batch,
                expected_count: 32,
                ..Default::default()
            });
        });
        library.api.reserve_batch = Some(batch32_reserve);
        library.api.receive_batch = Some(batch32_receive);
        library.api.commit_batch = Some(batch32_commit);
        library.api.release_batch = Some(batch32_release);
        library.api.cancel_batch = Some(batch32_cancel);
        library.packet.write_segment_batch = Some(batch32_writer);
        library.api.write_segment_batch = Some(batch32_reject_unsized);
        Self
    }
    fn finish(self, session: &mut Session) -> Batch32Calls {
        let calls = BATCH32_CALLS.with(std::cell::Cell::get);
        let library = Arc::get_mut(&mut session.library).unwrap();
        library.api.reserve_batch = calls.reserve; library.api.receive_batch = calls.receive;
        library.api.commit_batch = calls.commit; library.api.release_batch = calls.release;
        library.api.cancel_batch = calls.cancel;
        library.packet.write_segment_batch = calls.writer;
        library.api.write_segment_batch = calls.unsized_writer;
        calls
    }
}
impl Drop for Batch32Counter {
    fn drop(&mut self) { BATCH32_CALLS.with(|cell| cell.set(Batch32Calls::default())); }
}

fn batch32_expect_outputs(batch: &ReceivedBatch<'_>, first: usize, outputs: &[Vec<u8>]) {
    assert!(!outputs.is_empty() && outputs.len() <= raw::MAX_SEGMENT_BATCH);
    BATCH32_CALLS.with(|cell| {
        let mut calls = cell.get();
        calls.expected_source = batch.items[0].data as usize;
        calls.expected_length = batch.items[0].descriptor.length;
        calls.expected_first = first as u32; calls.expected_count = outputs.len() as u32;
        calls.expected_outputs = [0; raw::MAX_SEGMENT_BATCH];
        for (i, output) in outputs.iter().enumerate() { calls.expected_outputs[i] = output.as_ptr() as usize; }
        cell.set(calls);
    });
}

fn batch32_payload(direction: u8, index: usize, length: usize) -> Vec<u8> {
    let opaque = [0, u64::MAX, 0x0123_4567_89ab_cdef, 0xfedc_ba98_7654_3210][index % 4];
    let mut bytes: Vec<u8> = (0..length).map(|offset| (index * 17 + offset * 3) as u8).collect();
    // Opaque application bytes, deliberately nonmonotonic and including both
    // u64 extremes. These are not API send timestamps or clock samples.
    bytes[..8].copy_from_slice(&opaque.to_le_bytes());
    bytes[8..12].copy_from_slice(&(index as u32).to_be_bytes());
    bytes[12] = direction;
    bytes
}
fn batch32_packet(ip: u8, protocol: u8, payload: &[u8], sequence: u32, id: u16, flags: u8) -> Vec<u8> {
    let ip_header = if ip == 4 { 20 } else { 40 };
    let transport_header = if protocol == 6 { 20 } else { 8 };
    let mut bytes = vec![0; ip_header + transport_header + payload.len()];
    if ip == 4 {
        bytes[0] = 0x45; bytes[8] = 64; bytes[9] = protocol;
        let length = bytes.len() as u16;
        bytes[2..4].copy_from_slice(&length.to_be_bytes());
        bytes[4..6].copy_from_slice(&id.to_be_bytes()); bytes[6] = 0x40;
        bytes[12..16].copy_from_slice(&[10, 0, 0, 1]); bytes[16..20].copy_from_slice(&[10, 0, 0, 2]);
        let value = checksum(&bytes[..20]); bytes[10..12].copy_from_slice(&value.to_be_bytes());
    } else {
        bytes[0] = 0x60; bytes[6] = protocol; bytes[7] = 64;
        let length = (bytes.len() - 40) as u16;
        bytes[4..6].copy_from_slice(&length.to_be_bytes());
        bytes[8..12].copy_from_slice(&[0x20, 1, 0x0d, 0xb8]); bytes[23] = 1;
        bytes[24..28].copy_from_slice(&[0x20, 1, 0x0d, 0xb8]); bytes[39] = 2;
    }
    let transport = &mut bytes[ip_header..];
    transport[..4].copy_from_slice(&[0x27, 0x11, 0x27, 0x12]);
    if protocol == 6 {
        transport[4..8].copy_from_slice(&sequence.to_be_bytes());
        transport[8..12].copy_from_slice(&42u32.to_be_bytes());
        transport[12] = 0x50; transport[13] = flags; transport[14..16].copy_from_slice(&8192u16.to_be_bytes());
    } else {
        let length = transport.len() as u16; transport[4..6].copy_from_slice(&length.to_be_bytes());
    }
    transport[transport_header..].copy_from_slice(payload);
    let mut pseudo = if ip == 4 { bytes[12..20].to_vec() } else { bytes[8..40].to_vec() };
    let length = (bytes.len() - ip_header) as u32;
    if ip == 4 { pseudo.extend_from_slice(&[0, protocol]); pseudo.extend_from_slice(&(length as u16).to_be_bytes()); }
    else { pseudo.extend_from_slice(&length.to_be_bytes()); pseudo.extend_from_slice(&[0, 0, 0, protocol]); }
    pseudo.extend_from_slice(&bytes[ip_header..]);
    let value = checksum(&pseudo); let value = if protocol == 17 && value == 0 { u16::MAX } else { value };
    let offset = ip_header + if protocol == 6 { 16 } else { 6 };
    bytes[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
    bytes
}

#[test]
fn batch32_ipv4_checked_segments_use_one_ffi_call() {
    check_batch32_checked_segments_use_one_ffi_call(4);
}

#[test]
fn batch32_ipv6_checked_segments_use_one_ffi_call() {
    check_batch32_checked_segments_use_one_ffi_call(6);
}

fn check_batch32_checked_segments_use_one_ffi_call(ip: u8) {
    const MSS: usize = 256;
    for (protocol, kind) in [(6, raw::TCP_GSO), (17, raw::UDP_GSO)] {
        let mut session = fixture();
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let payload: Vec<u8> = (0..32).flat_map(|i| batch32_payload(0xa1, i, MSS)).collect();
            let source = batch32_packet(ip, protocol, &payload, 0xffff_fff0, 0xfffe, 0x19);
            assert!(source.len() <= u16::MAX as usize);
            assert_eq!(inject(&session, &source, kind, MSS as u32), raw::OK);
            let mut batch = q.receive(32).unwrap();
            assert_eq!(batch.len(), 1);
            assert_eq!(batch.segment_count(0).unwrap(), 32);
            let original = batch.data(0).unwrap().to_vec();
            let header = if ip == 4 { 20 } else { 40 } + if protocol == 6 { 20 } else { 8 };
            let mut buffers = vec![vec![0xa5; header + MSS + 16]; 32];
            BATCH32_CALLS.with(|cell| {
                let mut calls = cell.get();
                calls.expected_source = batch.items[0].data as usize;
                calls.expected_length = source.len() as u32;
                for (i, buffer) in buffers.iter().enumerate() { calls.expected_outputs[i] = buffer.as_ptr() as usize; }
                cell.set(calls);
            });
            let lengths = {
                let mut outputs: Vec<&mut [u8]> = buffers.iter_mut().map(Vec::as_mut_slice).collect();
                batch.write_segments(0, 0, &mut outputs).unwrap()
            };
            assert_eq!(lengths, vec![header + MSS; 32]);
            for i in 0..32 {
                let id_mask = if protocol == 6 { 0x7fff } else { 0xffff };
                let expected = batch32_packet(ip, protocol, &payload[i * MSS..(i + 1) * MSS],
                    0xffff_fff0u32.wrapping_add((i * MSS) as u32), (0xfffeu32 + i as u32) as u16 & id_mask,
                    if i == 31 { 0x19 } else { 0x10 });
                assert_eq!(&buffers[i][..lengths[i]], expected.as_slice(), "ip={ip} protocol={protocol} segment={i}");
                assert!(buffers[i][lengths[i]..].iter().all(|byte| *byte == 0xa5));
            }
            assert_eq!(batch.data(0).unwrap(), original);
            assert_eq!(batch.release().unwrap(), 1);
        }
        let calls = counter.finish(&mut session);
        assert_eq!((calls.writer_calls, calls.writer_requested), (1, 32));
        assert!(calls.direct_output_buffers);
        assert_eq!((calls.reserve_calls, calls.commit_calls), (0, 0));
        assert_eq!((calls.receive_calls, calls.release_calls), (1, 1));
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
        session.close(1000).unwrap();
        println!("BATCH32 checked ip={ip} protocol={protocol} segments=32 ffi_calls=1 commit_calls=0 opaque_payload=preserved");
    }
}

#[test]
fn batch32_ipv4_plain_packets_keep_directions_and_single_commit() {
    check_batch32_plain_packets_keep_directions_and_single_commit(4);
}

#[test]
fn batch32_ipv6_plain_packets_keep_directions_and_single_commit() {
    check_batch32_plain_packets_keep_directions_and_single_commit(6);
}

fn check_batch32_plain_packets_keep_directions_and_single_commit(ip: u8) {
    type Take = unsafe extern "C" fn(raw::Native, u32, *mut raw::Descriptor, u32, *mut u32) -> u32;
    type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
    for protocol in [6, 17] {
        let mut session = fixture_with_config(Config { queues: 1, slots: 32, ring_capacity: 32, ..Config::default() });
        let take: Take = unsafe { symbol(&session.library, c"PbTestTake") };
        let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let up: Vec<Vec<u8>> = (0..32).map(|i| batch32_packet(ip, protocol, &batch32_payload(0xa1, i, 64),
                0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18)).collect();
            let down: Vec<Vec<u8>> = (0..32).map(|i| batch32_packet(ip, protocol, &batch32_payload(0xb2, i, 64),
                0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18)).collect();
            for packet in &up { assert_eq!(inject(&session, packet, raw::PLAIN, 0), raw::OK); }
            let mut received = q.receive(32).unwrap();
            assert_eq!(received.len(), 32);
            for (i, packet) in up.iter().enumerate() {
                assert_eq!(received.data(i).unwrap(), packet);
                // The Vec contains copied lease metadata, not payload: the
                // returned slice still borrows the actual native address.
                assert_eq!(received.data(i).unwrap().as_ptr(), received.items[i].data.cast_const());
            }
            let mut reserved = q.reserve(32).unwrap();
            assert_eq!(reserved.len(), 32);
            let mut addresses = [ptr::null_mut(); 32];
            let tokens: Vec<_> = reserved.items.iter().map(|lease| lease.descriptor.token).collect();
            for (i, packet) in down.iter().enumerate() {
                addresses[i] = reserved.items[i].data;
                let output = reserved.data_mut(i).unwrap();
                assert_eq!(output.as_mut_ptr(), addresses[i]);
                output[..packet.len()].copy_from_slice(packet);
                reserved.set_length(i, packet.len()).unwrap();
            }
            assert_eq!(reserved.commit().unwrap(), 32);
            assert!(reserved.is_empty());
            let mut descriptors = [raw::Descriptor::default(); 32];
            let mut count = 0;
            assert_eq!(unsafe { take(session.pointer().unwrap(), 0, descriptors.as_mut_ptr(), 32, &mut count) }, raw::OK);
            assert_eq!(count, 32);
            for (i, descriptor) in descriptors.iter().enumerate() {
                assert_eq!(descriptor.token, tokens[i]);
                assert_eq!(descriptor.length as usize, down[i].len());
                // PbTestTake now owns this published slot as the modeled
                // kernel peer. No public caller re-borrows after Commit.
                let packet = unsafe { std::slice::from_raw_parts(addresses[i], descriptor.length as usize) };
                assert_eq!(packet, down[i]);
                assert_ne!(packet, received.data(i).unwrap());
                assert_eq!(received.data(i).unwrap(), up[i]);
            }
            assert_eq!(unsafe { complete(session.pointer().unwrap(), 0, descriptors.as_ptr(), 32) }, raw::OK);
            assert_eq!(q.completions(32).unwrap().len(), 32);
            assert_eq!(received.release().unwrap(), 32);
            assert!(received.is_empty());
        }
        let calls = counter.finish(&mut session);
        assert_eq!((calls.reserve_calls, calls.reserve_requested), (1, 32));
        assert_eq!((calls.commit_calls, calls.commit_requested), (1, 32));
        assert_eq!((calls.receive_calls, calls.receive_requested), (1, 32));
        assert_eq!((calls.release_calls, calls.release_requested), (1, 32));
        assert_eq!(calls.writer_calls, 0);
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
        session.close(1000).unwrap();
        println!("BATCH32 plain ip={ip} protocol={protocol} kernel_to_user=32 user_to_kernel=32 reserve_calls=1 commit_calls=1 receive_calls=1 release_calls=1 opaque_payload=preserved");
    }
}

#[test]
fn batch32_ipv4_short_output_preserves_prefix_and_retries_only_tail() {
    check_batch32_short_output_preserves_prefix_and_retries_only_tail(4);
}

#[test]
fn batch32_ipv6_short_output_preserves_prefix_and_retries_only_tail() {
    check_batch32_short_output_preserves_prefix_and_retries_only_tail(6);
}

fn check_batch32_short_output_preserves_prefix_and_retries_only_tail(ip: u8) {
    const MSS: usize = 256;
    for (protocol, kind) in [(6, raw::TCP_GSO), (17, raw::UDP_GSO)] {
        for short in [0, 1, 31] {
            let mut session = fixture();
            let counter = Batch32Counter::attach(&mut session);
            {
                let q = session.queue(0).unwrap();
                let payload: Vec<u8> = (0..32).flat_map(|i| batch32_payload(0xa1, i, MSS)).collect();
                let source = batch32_packet(ip, protocol, &payload, 0xffff_fff0, 0xfffe, 0x19);
                assert_eq!(inject(&session, &source, kind, MSS as u32), raw::OK);
                let mut batch = q.receive(32).unwrap();
                assert_eq!((batch.len(), batch.segment_count(0).unwrap()), (1, 32));
                let header = if ip == 4 { 20 } else { 40 } + if protocol == 6 { 20 } else { 8 };
                let length = header + MSS;
                let mut buffers = vec![vec![0xa5; length + 16]; 32];
                batch32_expect_outputs(&batch, 0, &buffers);
                let prefix = {
                    // The short declared span has a readable sentinel tail,
                    // so an erroneous native write cannot be hidden by a
                    // later Vec resize or buffer reinitialization.
                    let mut outputs: Vec<&mut [u8]> = buffers.iter_mut().enumerate()
                        .map(|(i, buffer)| if i == short { &mut buffer[..length - 1] } else { buffer.as_mut_slice() }).collect();
                    batch.write_segments(0, 0, &mut outputs)
                };
                if short == 0 { assert_eq!(prefix, Err(Error(3))); }
                else { assert_eq!(prefix.unwrap(), vec![length; short]); }
                let observed = BATCH32_CALLS.with(std::cell::Cell::get);
                assert!(observed.direct_output_buffers);
                assert_eq!((observed.writer_last_first, observed.writer_last_count, observed.writer_last_result, observed.writer_last_written),
                    (0, 32, 3, short as u32));
                for buffer in &buffers[short..] { assert!(buffer.iter().all(|byte| *byte == 0xa5)); }
                let saved_prefix = buffers[..short].to_vec(); // Test oracle only, not a payload-path allocation.
                batch32_expect_outputs(&batch, short, &buffers[short..]);
                let suffix = {
                    let mut outputs: Vec<&mut [u8]> = buffers[short..].iter_mut().map(Vec::as_mut_slice).collect();
                    batch.write_segments(0, short, &mut outputs).unwrap()
                };
                assert_eq!(suffix, vec![length; 32 - short]);
                assert_eq!(&buffers[..short], saved_prefix.as_slice());
                for i in 0..32 {
                    let mask = if protocol == 6 { 0x7fff } else { 0xffff };
                    let expected = batch32_packet(ip, protocol, &payload[i * MSS..(i + 1) * MSS],
                        0xffff_fff0u32.wrapping_add((i * MSS) as u32), (0xfffeu32 + i as u32) as u16 & mask,
                        if i == 31 { 0x19 } else { 0x10 });
                    assert_eq!(&buffers[i][..length], expected.as_slice(), "ip={ip} protocol={protocol} short={short} index={i}");
                    assert!(buffers[i][length..].iter().all(|byte| *byte == 0xa5));
                }
                assert_eq!(batch.data(0).unwrap(), source);
                let mut invalid_tail = [&mut buffers[0][..]];
                assert_eq!(batch.write_segments(0, 32, &mut invalid_tail), Err(Error(1)));
                assert_eq!(batch.release().unwrap(), 1);
            }
            let calls = counter.finish(&mut session);
            assert!(calls.direct_output_buffers);
            assert_eq!((calls.writer_calls, calls.writer_requested), (2, (64 - short) as u32));
            assert_eq!((calls.writer_last_first, calls.writer_last_count, calls.writer_last_result, calls.writer_last_written),
                (short as u32, (32 - short) as u32, 0, (32 - short) as u32));
            assert_eq!((calls.reserve_calls, calls.commit_calls, calls.cancel_calls), (0, 0, 0));
            assert_eq!((calls.release_calls, calls.release_requested), (1, 1));
            assert_eq!(session.close_progress().unwrap().user_leases, 0);
            session.close(1000).unwrap();
            println!("BATCH32_PREFIX ip={ip} protocol={protocol} short={short} prefix={short} retry={} writer_calls=2 source_and_sentinels=preserved", 32 - short);
        }
    }
}

#[test]
fn batch32_ipv4_partial_reserve_stop_rejects_commit_and_drop_cancels_loans() {
    check_batch32_partial_reserve_stop_rejects_commit_and_drop_cancels_loans(4);
}

#[test]
fn batch32_ipv6_partial_reserve_stop_rejects_commit_and_drop_cancels_loans() {
    check_batch32_partial_reserve_stop_rejects_commit_and_drop_cancels_loans(6);
}

fn check_batch32_partial_reserve_stop_rejects_commit_and_drop_cancels_loans(ip: u8) {
    type Take = unsafe extern "C" fn(raw::Native, u32, *mut raw::Descriptor, u32, *mut u32) -> u32;
    for protocol in [6, 17] {
        let mut session = fixture_with_config(Config { queues: 1, slots: 32, ring_capacity: 32, ..Config::default() });
        let take: Take = unsafe { symbol(&session.library, c"PbTestTake") };
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let mut first = q.reserve(31).unwrap();
            let mut tail = q.reserve(32).unwrap(); // The real native free pool has only one slot left.
            assert_eq!((first.len(), tail.len()), (31, 1));
            for i in 0..32 {
                let packet = batch32_packet(ip, protocol, &batch32_payload(0xb2, i, 64),
                    0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18);
                let (batch, index) = if i == 31 { (&mut tail, 0) } else { (&mut first, i) };
                batch.data_mut(index).unwrap()[..packet.len()].copy_from_slice(&packet);
                batch.set_length(index, packet.len()).unwrap();
            }
            assert_eq!(session.close_progress().unwrap().user_leases, 32);
            session.request_stop().unwrap();
            assert_eq!(session.prepare_close(0), Err(Error(raw::BUSY)));
            assert_eq!(first.commit(), Err(Error(7)));
            assert_eq!((first.len(), tail.len()), (31, 1));
            let state = session.close_progress().unwrap();
            assert_eq!(state.user_leases, 32);
            assert!(state.retained_bytes > 0);
            for i in 0..31 {
                let packet = batch32_packet(ip, protocol, &batch32_payload(0xb2, i, 64),
                    0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18);
                assert_eq!(&first.data_mut(i).unwrap()[..packet.len()], packet);
            }
            let mut descriptors = [raw::Descriptor::default(); 32];
            let mut count = u32::MAX;
            assert_eq!(unsafe { take(session.pointer().unwrap(), 0, descriptors.as_mut_ptr(), 32, &mut count) }, raw::EMPTY);
            assert_eq!(count, 0); // Failed Commit did not publish any prefix.
            drop(tail);
            assert_eq!(session.close_progress().unwrap().user_leases, 31);
            drop(first);
            assert_eq!(session.close_progress().unwrap().user_leases, 0);
        }
        session.prepare_close(1000).unwrap();
        let calls = counter.finish(&mut session);
        assert_eq!((calls.reserve_calls, calls.reserve_requested, calls.reserve_accepted), (2, 63, 32));
        assert_eq!((calls.commit_calls, calls.commit_requested, calls.commit_accepted, calls.commit_last_result, calls.commit_last_accepted), (1, 31, 0, 7, 0));
        assert_eq!((calls.cancel_calls, calls.cancel_requested, calls.cancel_succeeded), (2, 32, 32));
        assert_eq!(calls.writer_calls, 0);
        session.close(1000).unwrap();
        println!("BATCH32_RESERVE_STOP ip={ip} protocol={protocol} reserved=31+1 commit=stopped accepted=0 drop_cancel=1+31 loans=0");
    }
}

#[test]
fn batch32_ipv4_gso_loan_stays_borrowable_until_return_after_prepare_close() {
    check_batch32_gso_loan_stays_borrowable_until_return_after_prepare_close(4);
}

#[test]
fn batch32_ipv6_gso_loan_stays_borrowable_until_return_after_prepare_close() {
    check_batch32_gso_loan_stays_borrowable_until_return_after_prepare_close(6);
}

fn check_batch32_gso_loan_stays_borrowable_until_return_after_prepare_close(ip: u8) {
    const MSS: usize = 256;
    for (protocol, kind) in [(6, raw::TCP_GSO), (17, raw::UDP_GSO)] {
        let mut session = fixture();
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let payload: Vec<u8> = (0..32).flat_map(|i| batch32_payload(0xa1, i, MSS)).collect();
            let source = batch32_packet(ip, protocol, &payload, 0xffff_fff0, 0xfffe, 0x19);
            assert_eq!(inject(&session, &source, kind, MSS as u32), raw::OK);
            let batch = q.receive(32).unwrap();
            assert_eq!((batch.len(), batch.segment_count(0).unwrap()), (1, 32));
            let pointer = batch.items[0].data;
            session.request_stop().unwrap();
            assert_eq!(session.prepare_close(0), Err(Error(raw::BUSY)));
            let progress = session.close_progress().unwrap();
            assert_eq!(progress.user_leases, 1);
            assert!(progress.retained_bytes > 0);
            assert_eq!(batch.data(0).unwrap().as_ptr(), pointer.cast_const());
            assert_eq!(batch.data(0).unwrap(), source);
            assert!(matches!(q.receive(1), Err(Error(7))));
            assert!(matches!(q.reserve(1), Err(Error(7))));
            let header = if ip == 4 { 20 } else { 40 } + if protocol == 6 { 20 } else { 8 };
            let mut buffers = vec![vec![0xa5; header + MSS + 16]; 32];
            batch32_expect_outputs(&batch, 0, &buffers);
            let lengths = {
                let mut outputs: Vec<&mut [u8]> = buffers.iter_mut().map(Vec::as_mut_slice).collect();
                batch.write_segments(0, 0, &mut outputs).unwrap()
            };
            assert_eq!(lengths, vec![header + MSS; 32]);
            for i in 0..32 {
                let mask = if protocol == 6 { 0x7fff } else { 0xffff };
                let expected = batch32_packet(ip, protocol, &payload[i * MSS..(i + 1) * MSS],
                    0xffff_fff0u32.wrapping_add((i * MSS) as u32), (0xfffeu32 + i as u32) as u16 & mask,
                    if i == 31 { 0x19 } else { 0x10 });
                assert_eq!(&buffers[i][..lengths[i]], expected);
                assert!(buffers[i][lengths[i]..].iter().all(|byte| *byte == 0xa5));
            }
            assert_eq!(batch.data(0).unwrap(), source);
            drop(batch); // Actual retirement remains allowed after admission stops.
            assert_eq!(session.close_progress().unwrap().user_leases, 0);
        }
        session.prepare_close(1000).unwrap();
        let calls = counter.finish(&mut session);
        assert!(calls.direct_output_buffers);
        assert_eq!((calls.writer_calls, calls.writer_requested), (1, 32));
        assert_eq!((calls.receive_calls, calls.receive_requested, calls.receive_accepted), (2, 33, 1));
        assert_eq!((calls.reserve_calls, calls.reserve_requested, calls.reserve_accepted), (1, 1, 0));
        assert_eq!((calls.commit_calls, calls.cancel_calls), (0, 0));
        assert_eq!((calls.release_calls, calls.release_requested), (1, 1));
        session.close(1000).unwrap();
        println!("BATCH32_GSO_STOP ip={ip} protocol={protocol} held=1 generated=32 writer_calls=1 drop_return=1 loans=0 mapping=retained_until_return");
    }
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

fn check_batch32_gso_prefix_retirement_keeps_rebased_tail_and_reused_slot_independent(ip: u8) {
    const MSS: usize = 256;
    const PER_SOURCE: usize = 16;
    for (protocol, kind) in [(6, raw::TCP_GSO), (17, raw::UDP_GSO)] {
        let mut session = fixture();
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let payloads: [Vec<u8>; 2] = std::array::from_fn(|source| {
                (source * PER_SOURCE..(source + 1) * PER_SOURCE)
                    .flat_map(|index| batch32_payload(0xa1 + source as u8, index, MSS)).collect()
            });
            let sequences = [0xffff_fff0u32, 0x1234_5670];
            let ids = [0xfffeu16, 0x7ff8];
            let flags = [0x19u8, 0x99];
            let sources: [Vec<u8>; 2] = std::array::from_fn(|source| {
                batch32_packet(ip, protocol, &payloads[source], sequences[source], ids[source], flags[source])
            });
            for source in &sources {
                assert_eq!(inject(&session, source, kind, MSS as u32), raw::OK);
            }
            let mut batch = q.receive(32).unwrap();
            assert_eq!(batch.len(), 2);
            assert_eq!(session.close_progress().unwrap().user_leases, 2);
            let first_token = batch.metadata(0).unwrap().token;
            let first_address = batch.data(0).unwrap().as_ptr() as usize;
            let tail_address = batch.data(1).unwrap().as_ptr() as usize;
            assert_ne!(first_address, tail_address);
            let metadata_fields = |descriptor: &raw::Descriptor| [
                descriptor.token.epoch, descriptor.token.generation as u64,
                descriptor.token.slot as u64, descriptor.token.queue as u64,
                descriptor.kind as u64, descriptor.length as u64, descriptor.data_offset as u64,
                descriptor.flags as u64, descriptor.mss as u64, descriptor.l4_offset as u64,
                descriptor.header_length as u64, descriptor.ip_version as u64, descriptor.protocol as u64,
                descriptor.reserved16 as u64, descriptor.status as u64,
                descriptor.reserved64[0], descriptor.reserved64[1],
            ];
            let plan_fields = |plan: raw::Plan| [
                plan.source, plan.payload, plan.count, plan.mss, plan.header as u32,
                plan.l4 as u32, plan.l4_header as u32, plan.pseudo_source as u32,
                plan.pseudo_destination as u32, plan.ip as u32, plan.protocol as u32,
                plan.kind as u32, plan.udp_zero as u32,
            ];
            let tail_metadata = metadata_fields(batch.metadata(1).unwrap());
            let tail_plan = plan_fields(batch.plan(&batch.items[1]).unwrap());
            assert_eq!(batch.segment_count(0).unwrap(), PER_SOURCE);
            assert_eq!(batch.segment_count(1).unwrap(), PER_SOURCE);
            let header = if ip == 4 { 20 } else { 40 } + if protocol == 6 { 20 } else { 8 };
            let length = header + MSS;
            let mut buffers: [Vec<Vec<u8>>; 2] = std::array::from_fn(|_| vec![vec![0xa5; length + 16]; 32]);
            let write_and_check = |batch: &ReceivedBatch<'_>, source: usize, outputs: &mut [Vec<u8>]| {
                batch32_expect_outputs(batch, 0, outputs);
                let lengths = {
                    let mut spans: Vec<&mut [u8]> = outputs.iter_mut().map(Vec::as_mut_slice).collect();
                    batch.write_segments(0, 0, &mut spans).unwrap()
                };
                // The caller offers 32 buffers, but this descriptor has only
                // 16 segments. Rust must return the actual C written prefix.
                assert_eq!(lengths, vec![length; PER_SOURCE]);
                let observed = BATCH32_CALLS.with(std::cell::Cell::get);
                assert!(observed.direct_output_buffers);
                assert_eq!((observed.writer_last_first, observed.writer_last_count,
                    observed.writer_last_result, observed.writer_last_written), (0, 32, 0, PER_SOURCE as u32));
                for index in 0..PER_SOURCE {
                    let mask = if protocol == 6 { 0x7fff } else { 0xffff };
                    let segment_flags = (flags[source] & !0x89)
                        | if index == 0 { flags[source] & 0x80 } else { 0 }
                        | if index + 1 == PER_SOURCE { flags[source] & 0x09 } else { 0 };
                    let expected = batch32_packet(ip, protocol, &payloads[source][index * MSS..(index + 1) * MSS],
                        sequences[source].wrapping_add((index * MSS) as u32),
                        (ids[source] as u32 + index as u32) as u16 & mask, segment_flags);
                    assert_eq!(&outputs[index][..length], expected.as_slice(),
                        "ip={ip} protocol={protocol} source={source} segment={index}");
                    assert!(outputs[index][length..].iter().all(|byte| *byte == 0xa5));
                }
                for output in &outputs[PER_SOURCE..] {
                    assert!(output.iter().all(|byte| *byte == 0xa5));
                }
                assert_eq!(batch.data(0).unwrap(), sources[source]);
            };
            write_and_check(&batch, 0, &mut buffers[0]);
            assert_eq!(batch.data(1).unwrap(), sources[1]);
            // After this successful return, never read the first loan's data
            // again. Only its copied token/address and owned output survive.
            assert_eq!(batch.release_prefix(1).unwrap(), 1);
            assert_eq!(batch.len(), 1);
            assert_eq!(session.close_progress().unwrap().user_leases, 1);
            assert_eq!(batch.data(0).unwrap().as_ptr() as usize, tail_address);
            assert_eq!(metadata_fields(batch.metadata(0).unwrap()), tail_metadata);
            assert_eq!(plan_fields(batch.plan(&batch.items[0]).unwrap()), tail_plan);
            let replacement = batch32_packet(ip, protocol, &batch32_payload(0xc3, 99, 64),
                0x7654_3210, 0x1234, 0x10);
            assert_ne!(replacement, sources[0]);
            assert_eq!(inject(&session, &replacement, raw::PLAIN, 0), raw::OK);
            let mut reused = q.receive(32).unwrap();
            assert_eq!(reused.len(), 1);
            let reused_token = reused.metadata(0).unwrap().token;
            assert_eq!(reused_token, raw::Token {
                generation: first_token.generation.checked_add(1).unwrap(), ..first_token
            });
            assert_eq!(reused.metadata(0).unwrap().kind, raw::PLAIN);
            assert_eq!(reused.data(0).unwrap().as_ptr() as usize, first_address);
            assert_ne!(reused.data(0).unwrap().as_ptr() as usize, tail_address);
            assert_eq!(reused.data(0).unwrap(), replacement);
            assert_eq!(session.close_progress().unwrap().user_leases, 2);
            assert_eq!(batch.len(), 1);
            assert!(batch.metadata(1).is_none() && batch.data(1).is_none());
            assert_eq!(batch.data(0).unwrap().as_ptr() as usize, tail_address);
            assert_eq!(metadata_fields(batch.metadata(0).unwrap()), tail_metadata);
            assert_eq!(plan_fields(batch.plan(&batch.items[0]).unwrap()), tail_plan);
            assert_eq!(batch.segment_count(0).unwrap(), PER_SOURCE);
            let first_output_before_tail = buffers[0].clone(); // Test oracle only.
            write_and_check(&batch, 1, &mut buffers[1]);
            assert_eq!(buffers[0], first_output_before_tail);
            assert_eq!(reused.data(0).unwrap(), replacement);
            assert_eq!(batch.release().unwrap(), 1);
            assert_eq!(session.close_progress().unwrap().user_leases, 1);
            assert_eq!(reused.data(0).unwrap(), replacement);
            assert_eq!(reused.release().unwrap(), 1);
            assert!(batch.is_empty() && reused.is_empty());
        }
        let calls = counter.finish(&mut session);
        assert_eq!((calls.writer_calls, calls.writer_requested), (2, 64));
        assert!(calls.direct_output_buffers);
        assert_eq!((calls.receive_calls, calls.receive_requested, calls.receive_accepted), (2, 64, 3));
        assert_eq!((calls.release_calls, calls.release_requested), (3, 3));
        assert_eq!((calls.reserve_calls, calls.commit_calls, calls.cancel_calls), (0, 0, 0));
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
        session.close(1000).unwrap();
        println!("BATCH32_REBASE ip={ip} protocol={protocol} sources=2 segments=32 writer_calls=2 requested=64 written=16+16 receives=2 loans=3 releases=3 reused_generation=2 source_and_sentinels=preserved");
    }
}

#[test]
fn batch32_ipv4_gso_prefix_retirement_keeps_rebased_tail_and_reused_slot_independent() {
    check_batch32_gso_prefix_retirement_keeps_rebased_tail_and_reused_slot_independent(4);
}

#[test]
fn batch32_ipv6_gso_prefix_retirement_keeps_rebased_tail_and_reused_slot_independent() {
    check_batch32_gso_prefix_retirement_keeps_rebased_tail_and_reused_slot_independent(6);
}

fn check_batch32_commit_validation_failure_keeps_loans_for_corrected_retry(ip: u8) {
    type Take = unsafe extern "C" fn(raw::Native, u32, *mut raw::Descriptor, u32, *mut u32) -> u32;
    type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
    for protocol in [6, 17] {
        let mut session = fixture_with_config(Config { queues: 1, slots: 32, ring_capacity: 32, ..Config::default() });
        let take: Take = unsafe { symbol(&session.library, c"PbTestTake") };
        let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let packets: Vec<Vec<u8>> = (0..32).map(|i| batch32_packet(ip, protocol, &batch32_payload(0xb2, i, 64),
                0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18)).collect();
            let mut reserved = q.reserve(32).unwrap();
            assert_eq!(reserved.len(), 32);
            let addresses: Vec<_> = reserved.items.iter().map(|lease| lease.data).collect();
            let tokens: Vec<_> = reserved.items.iter().map(|lease| lease.descriptor.token).collect();
            for (i, packet) in packets.iter().enumerate() {
                let data = reserved.data_mut(i).unwrap();
                assert_eq!(data.as_mut_ptr(), addresses[i]);
                data[..packet.len()].copy_from_slice(packet);
                data[packet.len()..packet.len()+16].fill(0xa5);
                if i != 31 { reserved.set_length(i, packet.len()).unwrap(); }
            }
            let metadata: Vec<_> = reserved.items.iter().map(|lease| format!("{:?}", lease.descriptor)).collect();
            assert_eq!(reserved.items[31].descriptor.length, 0);
            assert_eq!(reserved.commit(), Err(Error(1)));
            assert_eq!(reserved.len(), 32);
            let state = session.close_progress().unwrap();
            assert_eq!((state.user_leases, state.published), (32, 0));
            BATCH32_CALLS.with(|cell| {
                let calls = cell.get();
                assert_eq!((calls.commit_calls, calls.commit_requested, calls.commit_accepted,
                    calls.commit_last_result, calls.commit_last_accepted), (1, 32, 0, 1, 0));
            });
            for (i, packet) in packets.iter().enumerate() {
                assert_eq!(format!("{:?}", reserved.items[i].descriptor), metadata[i]);
                assert_eq!(reserved.items[i].descriptor.token, tokens[i]);
                let data = reserved.data_mut(i).unwrap();
                assert_eq!(data.as_mut_ptr(), addresses[i]);
                assert_eq!(&data[..packet.len()], packet);
                assert_eq!(&data[packet.len()..packet.len()+16], &[0xa5;16]);
            }
            let mut descriptors = [raw::Descriptor::default(); 32];
            let mut count = u32::MAX;
            assert_eq!(unsafe { take(session.pointer().unwrap(), 0, descriptors.as_mut_ptr(), 32, &mut count) }, raw::EMPTY);
            assert_eq!(count, 0);
            reserved.set_length(31, packets[31].len()).unwrap();
            assert_eq!(reserved.commit().unwrap(), 32);
            assert!(reserved.is_empty());
            let state = session.close_progress().unwrap();
            assert_eq!((state.user_leases, state.published), (0, 32));
            assert_eq!(unsafe { take(session.pointer().unwrap(), 0, descriptors.as_mut_ptr(), 32, &mut count) }, raw::OK);
            assert_eq!(count, 32);
            for (i, descriptor) in descriptors.iter().enumerate() {
                assert_eq!(descriptor.token, tokens[i]);
                assert_eq!(descriptor.length as usize, packets[i].len());
                let peer_packet = unsafe { std::slice::from_raw_parts(addresses[i], descriptor.length as usize) };
                assert_eq!(peer_packet, packets[i]);
            }
            assert_eq!(unsafe { complete(session.pointer().unwrap(), 0, descriptors.as_ptr(), 32) }, raw::OK);
            assert_eq!(q.completions(32).unwrap().len(), 32);
            let reused = q.reserve(32).unwrap();
            assert_eq!(reused.len(), 32);
            // ReserveBatch does not promise slot order. Match each new loan
            // to its original slot rather than the caller's batch index.
            let mut seen_slots = [false; 32];
            for lease in &reused.items {
                let slot = lease.descriptor.token.slot as usize;
                assert!(slot < seen_slots.len() && !seen_slots[slot]);
                seen_slots[slot] = true;
                let original = tokens.iter().position(|token| token.slot == lease.descriptor.token.slot).unwrap();
                assert_eq!(lease.descriptor.token, raw::Token { generation: 2, ..tokens[original] });
                assert_eq!(lease.data, addresses[original]);
            }
            assert!(seen_slots.iter().all(|seen| *seen));
            drop(reused);
            assert_eq!((session.close_progress().unwrap().user_leases, session.close_progress().unwrap().published), (0, 0));
        }
        let calls = counter.finish(&mut session);
        assert_eq!((calls.reserve_calls, calls.reserve_requested, calls.reserve_accepted), (2, 64, 64));
        assert_eq!((calls.commit_calls, calls.commit_requested, calls.commit_accepted,
            calls.commit_last_result, calls.commit_last_accepted), (2, 64, 32, raw::OK, 32));
        assert_eq!((calls.cancel_calls, calls.cancel_requested, calls.cancel_succeeded), (1, 32, 32));
        assert_eq!((calls.writer_calls, calls.receive_calls, calls.release_calls), (0, 0, 0));
        session.close(1000).unwrap();
        println!("COMMIT_RETRY32 ip={ip} protocol={protocol} invalid32_accepted0 retry32_accepted32 payloads_unchanged32 generation2_cancel32 reserves2 commits2 cancel1 writer0 loans0 published0");
    }
}

#[test]
fn batch32_ipv4_commit_validation_failure_keeps_loans_for_corrected_retry() { check_batch32_commit_validation_failure_keeps_loans_for_corrected_retry(4); }
#[test]
fn batch32_ipv6_commit_validation_failure_keeps_loans_for_corrected_retry() { check_batch32_commit_validation_failure_keeps_loans_for_corrected_retry(6); }

fn check_batch32_plain_receive_rejects_copied_tokens_before_prefix_reuse(ip: u8) {
    for protocol in [6, 17] {
        let mut session = fixture_with_config(Config { queues: 1, slots: 32, ring_capacity: 32, ..Config::default() });
        let counter = Batch32Counter::attach(&mut session);
        {
            let q = session.queue(0).unwrap();
            let packets: Vec<Vec<u8>> = (0..32).map(|i| batch32_packet(ip, protocol, &batch32_payload(0xa1, i, 64),
                0xffff_fff0u32.wrapping_add((i * 64) as u32), i as u16, 0x18)).collect();
            for packet in &packets { assert_eq!(inject(&session, packet, raw::PLAIN, 0), raw::OK); }
            let mut received = q.receive(32).unwrap();
            assert_eq!(received.len(), 32);
            let tokens: Vec<_> = (0..32).map(|i| received.metadata(i).unwrap().token).collect();
            let addresses: Vec<_> = (0..32).map(|i| received.data(i).unwrap().as_ptr() as usize).collect();
            let metadata: Vec<_> = (0..32).map(|i| format!("{:?}", received.metadata(i).unwrap())).collect();
            // This is a read-only hosted layout witness. No ring cursor or
            // private count is changed to manufacture backpressure.
            let region_address = addresses[0].checked_sub(received.metadata(0).unwrap().data_offset as usize).unwrap();
            assert_eq!(region_address & 63, 0);
            let region = region_address as *const u8;
            let layout = unsafe { ptr::read(region.cast::<raw::Layout>()) };
            assert_eq!((layout.size as usize, layout.queues, layout.slots, layout.capacity, layout.block_size),
                (size_of::<raw::Layout>(), 1, 32, 32, 65536));
            assert_eq!(layout.queue[0].rings[1] & 63, 0);
            assert!(layout.queue[0].rings[1] as usize + 128 <= layout.region_size as usize);
            let recycle = unsafe { region.add(layout.queue[0].rings[1] as usize) };
            let cursors = || unsafe { (ptr::read_volatile(recycle.cast::<u64>()), ptr::read_volatile(recycle.add(64).cast::<u64>())) };
            assert_eq!(cursors(), (0, 0));
            // Unsafe raw ABI misuse is confined to independent copied tokens.
            // Neither Rust lease metadata nor borrowed payload is modified.
            for duplicate in [false, true] {
                let mut bad = tokens.clone();
                if duplicate { bad[31] = bad[0]; } else { bad[31].epoch = bad[31].epoch.wrapping_add(1); }
                let mut accepted = u32::MAX;
                let result = unsafe { session.library.api.release_batch.unwrap()(q.native.as_ptr(), bad.as_ptr(), 32, &mut accepted) };
                assert_eq!((result, accepted), (if duplicate { 1 } else { 5 }, 0));
                assert_eq!(cursors(), (0, 0));
                assert_eq!(received.len(), 32);
                let progress = session.close_progress().unwrap();
                assert_eq!((progress.user_leases, progress.published), (32, 0));
                for (i, packet) in packets.iter().enumerate() {
                    assert_eq!(received.data(i).unwrap().as_ptr() as usize, addresses[i]);
                    assert_eq!(received.data(i).unwrap(), packet);
                    assert_eq!(format!("{:?}", received.metadata(i).unwrap()), metadata[i]);
                }
            }
            // From here on, never read a transferred gen1 prefix address;
            // only the copied tokens and addresses survive as test metadata.
            assert_eq!(received.release_prefix(31).unwrap(), 31);
            assert_eq!((received.len(), cursors()), (1, (31, 0)));
            assert_eq!(received.metadata(0).unwrap().token, tokens[31]);
            assert_eq!(received.data(0).unwrap().as_ptr() as usize, addresses[31]);
            assert_eq!(received.data(0).unwrap(), packets[31]);
            assert_eq!(format!("{:?}", received.metadata(0).unwrap()), metadata[31]);
            let replacements: Vec<Vec<u8>> = (0..31).map(|i| batch32_packet(ip, protocol, &batch32_payload(0xb2, i, 64),
                0x1234_0000u32.wrapping_add((i * 64) as u32), (i + 100) as u16, 0x18)).collect();
            for packet in &replacements { assert_eq!(inject(&session, packet, raw::PLAIN, 0), raw::OK); }
            assert_eq!(cursors(), (31, 31));
            let mut reused = q.receive(32).unwrap();
            assert_eq!(reused.len(), 31);
            let mut seen = [false; 32];
            for (i, packet) in replacements.iter().enumerate() {
                let token = reused.metadata(i).unwrap().token;
                let original = tokens[..31].iter().position(|old| old.slot == token.slot).unwrap();
                let slot = token.slot as usize;
                assert!(slot < seen.len() && !seen[slot]); seen[slot] = true;
                assert_eq!(token, raw::Token { generation: 2, ..tokens[original] });
                assert_eq!(reused.data(i).unwrap().as_ptr() as usize, addresses[original]);
                assert_eq!(reused.data(i).unwrap(), packet);
            }
            assert_eq!(seen.iter().filter(|seen| **seen).count(), 31);
            assert!(!seen[tokens[31].slot as usize]);
            assert_eq!(session.close_progress().unwrap().user_leases, 32);
            assert_eq!(received.data(0).unwrap(), packets[31]);
            assert_eq!(format!("{:?}", received.metadata(0).unwrap()), metadata[31]);
            assert_eq!(received.release().unwrap(), 1);
            assert!(received.is_empty());
            for (i, packet) in replacements.iter().enumerate() { assert_eq!(reused.data(i).unwrap(), packet); }
            assert_eq!(reused.release().unwrap(), 31);
            assert!(reused.is_empty());
            assert_eq!(cursors(), (63, 31));
        }
        let calls = counter.finish(&mut session);
        assert_eq!((calls.receive_calls, calls.receive_requested, calls.receive_accepted), (2, 64, 63));
        assert_eq!((calls.release_calls, calls.release_requested), (5, 127));
        assert_eq!((calls.reserve_calls, calls.commit_calls, calls.cancel_calls, calls.writer_calls), (0, 0, 0, 0));
        assert_eq!(session.close_progress().unwrap().user_leases, 0);
        session.close(1000).unwrap();
        println!("RECEIVE_TOKEN_RETRY32 ip={ip} protocol={protocol} invalid_last_epoch_stale1 duplicate_invalid1 accepted0 prefix31 tail1 reused31 generation2 release_calls5 returned63 loans0");
    }
}

#[test]
fn batch32_ipv4_plain_receive_rejects_copied_tokens_before_prefix_reuse() { check_batch32_plain_receive_rejects_copied_tokens_before_prefix_reuse(4); }
#[test]
fn batch32_ipv6_plain_receive_rejects_copied_tokens_before_prefix_reuse() { check_batch32_plain_receive_rejects_copied_tokens_before_prefix_reuse(6); }

fn check_batch32_fault_drop_keeps_other_direction_borrowed_and_native_barrier(ip: u8) {
    type Complete = unsafe extern "C" fn(raw::Native, u32, *const raw::Descriptor, u32) -> u32;
    for protocol in [6, 17] {
        for receive_first in [false, true] {
            let mut session = fixture_with_config(Config { queues: 1, slots: 32, ring_capacity: 32, ..Config::default() });
            let complete: Complete = unsafe { symbol(&session.library, c"PbTestComplete") };
            let counter = Batch32Counter::attach(&mut session);
            {
                let q = session.queue(0).unwrap();
                let packets: [Vec<Vec<u8>>; 2] = std::array::from_fn(|direction| {
                    (0..32).map(|index| batch32_packet(ip, protocol, &batch32_payload(0xa1 + direction as u8, index, 64),
                        0xffff_fff0u32.wrapping_add((index * 64) as u32), index as u16, 0x18)).collect()
                });
                for packet in &packets[0] { assert_eq!(inject(&session, packet, raw::PLAIN, 0), raw::OK); }
                let mut received = q.receive(32).unwrap();
                let mut reserved = q.reserve(32).unwrap();
                assert_eq!((received.len(), reserved.len()), (32, 32));
                let rx_addresses: Vec<_> = (0..32).map(|i| received.data(i).unwrap().as_ptr() as usize).collect();
                let tx_addresses: Vec<_> = reserved.items.iter().map(|lease| lease.data as usize).collect();
                let rx_metadata: Vec<_> = (0..32).map(|i| format!("{:?}", received.metadata(i).unwrap())).collect();
                let tx_tokens: Vec<_> = reserved.items.iter().map(|lease| lease.descriptor.token).collect();
                for (index, packet) in packets[1].iter().enumerate() {
                    reserved.data_mut(index).unwrap()[..packet.len()].copy_from_slice(packet);
                    reserved.set_length(index, packet.len()).unwrap();
                }
                assert_eq!(session.close_progress().unwrap().user_leases, 64);
                assert_eq!(received.release_prefix(1).unwrap(), 1);
                reserved.cancel(0).unwrap();
                assert_eq!((received.len(), reserved.len()), (31, 31));
                assert_eq!(session.close_progress().unwrap().user_leases, 62);
                // The returned prefixes are no longer borrowed. Only copied
                // integer addresses/metadata remain; they are not dereferenced.
                let verify_received = |batch: &ReceivedBatch<'_>| {
                    assert_eq!(batch.len(), 31);
                    for index in 0..31 {
                        assert_eq!(batch.data(index).unwrap().as_ptr() as usize, rx_addresses[index+1]);
                        assert_eq!(batch.data(index).unwrap(), packets[0][index+1]);
                        assert_eq!(format!("{:?}", batch.metadata(index).unwrap()), rx_metadata[index+1]);
                    }
                };
                let verify_reserved = |batch: &mut ReservedBatch<'_>| {
                    assert_eq!(batch.len(), 31);
                    for index in 0..31 {
                        assert_eq!(batch.items[index].descriptor.token, tx_tokens[index+1]);
                        assert_eq!(batch.items[index].data as usize, tx_addresses[index+1]);
                        assert_eq!(batch.items[index].descriptor.length as usize, packets[1][index+1].len());
                        assert_eq!(&batch.data_mut(index).unwrap()[..packets[1][index+1].len()], packets[1][index+1]);
                    }
                };
                verify_received(&received); verify_reserved(&mut reserved);
                active(&session, 1);
                // Defensive shared-wire input: actual public Poll validates
                // the wrong epoch and calls actual FaultSession. No PB result,
                // private Fatal, slot or DLL loan state is forged. active()
                // controls only the hosted native-reference boundary.
                let mut bad = raw::Descriptor::default();
                bad.token.epoch = session.epoch.wrapping_add(1);
                bad.token.generation = 1;
                assert_eq!(unsafe { complete(session.pointer().unwrap(), 0, &bad, 1) }, raw::OK);
                assert_eq!(q.completions(1).unwrap_err(), Error(4));
                assert_eq!(session.prepare_close(0), Err(Error(raw::BUSY)));
                let progress = session.close_progress().unwrap();
                assert_eq!(progress.user_leases, 62);
                assert!(progress.retained_bytes > 0);
                assert_eq!(progress.published, 0);
                verify_received(&received); verify_reserved(&mut reserved);
                let mut received = Some(received);
                let mut reserved = Some(reserved);
                if receive_first {
                    drop(received.take().unwrap());
                    assert_eq!(session.close_progress().unwrap().user_leases, 31);
                    verify_reserved(reserved.as_mut().unwrap());
                    assert_eq!(session.prepare_close(0), Err(Error(raw::BUSY)));
                    drop(reserved.take().unwrap());
                } else {
                    drop(reserved.take().unwrap());
                    assert_eq!(session.close_progress().unwrap().user_leases, 31);
                    verify_received(received.as_ref().unwrap());
                    assert_eq!(session.prepare_close(0), Err(Error(raw::BUSY)));
                    drop(received.take().unwrap());
                }
                assert!(received.is_none() && reserved.is_none());
                assert_eq!(session.close_progress().unwrap().user_leases, 0);
                assert_eq!(session.prepare_close(0), Err(Error(raw::QUARANTINED)));
                let progress = session.close_progress().unwrap();
                assert_eq!(progress.user_leases, 0);
                assert!(progress.retained_bytes > 0);
                assert_eq!(progress.published, 0);
                assert_eq!(session.status().unwrap().active_references, 1);
            }
            let calls = counter.finish(&mut session);
            assert_eq!((calls.receive_calls, calls.receive_requested, calls.receive_accepted), (1, 32, 32));
            assert_eq!((calls.reserve_calls, calls.reserve_requested, calls.reserve_accepted), (1, 32, 32));
            assert_eq!((calls.release_calls, calls.release_requested), (2, 32));
            assert_eq!((calls.cancel_calls, calls.cancel_requested, calls.cancel_succeeded), (2, 32, 32));
            assert_eq!((calls.commit_calls, calls.writer_calls), (0, 0));
            active(&session, 0);
            session.prepare_close(1000).unwrap();
            assert_eq!(session.status().unwrap().active_references, 0);
            session.close(1000).unwrap();
            println!("FAULT_DROP62 ip={ip} protocol={protocol} receive_first={receive_first} initial64 prefix1_cancel1 remaining62 first_drop31 second_drop0 native_active1_quarantined release2 cancel2 writer0 commit0");
        }
    }
}

#[test]
fn batch32_ipv4_fault_drop_keeps_other_direction_borrowed_and_native_barrier() { check_batch32_fault_drop_keeps_other_direction_borrowed_and_native_barrier(4); }
#[test]
fn batch32_ipv6_fault_drop_keeps_other_direction_borrowed_and_native_barrier() { check_batch32_fault_drop_keeps_other_direction_borrowed_and_native_barrier(6); }
