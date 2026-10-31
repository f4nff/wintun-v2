use crate::raw;
use std::ffi::c_void;
use std::io;
use std::marker::PhantomData;
use std::mem::{MaybeUninit, size_of};
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::ptr::{self, NonNull};
use std::rc::Rc;
use std::sync::Arc;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryExW(path: *const u16, reserved: raw::Native, flags: u32) -> raw::Native;
    fn GetProcAddress(module: raw::Native, name: *const u8) -> raw::Native;
    fn FreeLibrary(module: raw::Native) -> i32;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub u32);
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "PacketBridge status {}", self.0)
    }
}
impl std::error::Error for Error {}

#[derive(Clone, Debug)]
pub struct AdapterCreateError {
    pub status: u32,
    pub diagnostics: raw::AdapterDiagnostics,
    pub details: Option<raw::AdapterDiagnosticsEx>,
}
impl std::fmt::Display for AdapterCreateError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let d = &self.diagnostics;
        write!(f, "PacketBridge adapter-create status={} stage={} attempts={} win32=0x{:08x} hresult=0x{:08x} first_stage={} first_win32=0x{:08x} first_hresult=0x{:08x} recovery=0x{:08x} problem={}/0x{:08x}/{} first_problem={}/0x{:08x}/{} removed_devices={} removed_packages={} deferred_actions={}",
            self.status,d.stage,d.attempts,d.win32_error,d.hresult,d.first_stage,d.first_win32_error,d.first_hresult,
            d.recovery_error,d.problem_code,d.problem_status,d.problem_fields_known,
            d.first_problem_code,d.first_problem_status,d.first_problem_fields_known,d.removed_devices,d.removed_packages,d.need_reboot)?;
        if let Some(x) = &self.details {
            let step = match x.recovery_step { 0 => "none", 1 => "open-devices", 2 => "locate-name", 3 => "read-bound-inf",
                4 => "resolve-inf-path", 5 => "DiUninstallDevice", 6 => "DiUninstallDriverW", _ => "unknown" };
            let source = &x.observed_source[..64];
            let source = if source.iter().all(u8::is_ascii_hexdigit) { std::str::from_utf8(source).unwrap() } else { "unavailable" };
            write!(f, " recovery_api={} matched_devices={} found_packages={} already_absent_packages={} probe_mismatch=0x{:02x} observed_source={} observed_version=0x{:08x} observed_data_abi={} observed_probe_schema={}",
                step,x.matched_devices,x.found_packages,x.already_absent_packages,x.probe_mismatch_mask,source,
                x.observed_version,x.observed_data_abi,x.observed_probe_schema)?;
        }
        Ok(())
    }
}
impl std::error::Error for AdapterCreateError {}
type Result<T> = std::result::Result<T, Error>;
fn check(code: u32) -> Result<()> {
    if code == 0 { Ok(()) } else { Err(Error(code)) }
}
fn wide(text: &std::ffi::OsStr) -> io::Result<Vec<u16>> {
    let mut value: Vec<u16> = text.encode_wide().collect();
    if value.contains(&0) {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "embedded NUL"));
    }
    value.push(0);
    Ok(value)
}
fn validate_packet_interface(packet: &raw::PacketInterface) -> io::Result<()> {
    if packet.size as usize != size_of::<raw::PacketInterface>()
        || packet.major != raw::PACKET_ABI_MAJOR
        || packet.minor < raw::PACKET_ABI_MINOR
        || packet.capabilities & raw::PACKET_REQUIRED_CAPS != raw::PACKET_REQUIRED_CAPS
        || packet.write_segment.is_none()
        || packet.write_segment_batch.is_none()
    {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "incomplete packet interface"));
    }
    Ok(())
}

pub struct Library {
    module: NonNull<c_void>,
    api: raw::Interface,
    life: raw::LifecycleInterface,
    packet: raw::PacketInterface,
    create_adapter_ex: Option<raw::CreateAdapterEx>,
    create_adapter_ex2: Option<raw::CreateAdapterEx2>,
}
// The table is immutable; native control calls serialize, while Queue enforces one data owner.
unsafe impl Send for Library {}
unsafe impl Sync for Library {}
impl Library {
    /// Load an absolute, trusted native DLL. No PATH or current-directory search is used.
    ///
    /// # Safety
    /// The DLL must be trusted and implement the supplied C ABI and memory ownership contract.
    pub unsafe fn load(path: &Path) -> io::Result<Arc<Self>> {
        if !path.is_absolute() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "absolute DLL path required",
            ));
        }
        let path = wide(path.as_os_str())?;
        let module =
            NonNull::new(unsafe { LoadLibraryExW(path.as_ptr(), ptr::null_mut(), 0x100 | 0x800) })
                .ok_or_else(io::Error::last_os_error)?;
        let result = (|| {
            let symbol =
                unsafe { GetProcAddress(module.as_ptr(), c"PbGetInterfaceV2".as_ptr().cast()) };
            if symbol.is_null() {
                return Err(io::Error::last_os_error());
            }
            let factory: raw::GetInterface = unsafe { std::mem::transmute(symbol) };
            let mut table = MaybeUninit::<raw::Interface>::zeroed();
            if unsafe {
                factory(
                    raw::API_ABI_MAJOR,
                    size_of::<raw::Interface>() as u32,
                    table.as_mut_ptr(),
                )
            } != 0
            {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "native function table mismatch",
                ));
            }
            let api = unsafe { table.assume_init() };
            let complete = [
                api.open.is_some(),
                api.close.is_some(),
                api.open_queue.is_some(),
                api.receive_batch.is_some(),
                api.release_batch.is_some(),
                api.reserve_batch.is_some(),
                api.cancel_batch.is_some(),
                api.commit_batch.is_some(),
                api.poll_completions.is_some(),
                api.wait.is_some(),
                api.get_status.is_some(),
                api.set_policy.is_some(),
                api.plan.is_some(),
                api.write_segment.is_some(),
                api.write_segment_batch.is_some(),
                api.select_queue.is_some(),
                api.create_adapter.is_some(),
                api.close_adapter.is_some(),
                api.start_adapter_session.is_some(),
                api.adapter_luid.is_some(),
                api.drain.is_some(),
                api.wake.is_some(),
                api.capabilities.is_some(),
                api.prepare_close.is_some(),
            ]
            .into_iter()
            .all(|v| v);
            if api.size as usize != size_of::<raw::Interface>()
                || api.major != raw::API_ABI_MAJOR
                || api.minor != raw::API_ABI_MINOR
                || api.file_version != raw::FILE_VERSION
                || !complete
                || api.source[64] != 0
                || !api.source[..64].iter().all(u8::is_ascii_hexdigit)
                || api.reserved.iter().any(|v| *v != 0)
            {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "native version/ABI validation failed",
                ));
            }
            let symbol = unsafe {
                GetProcAddress(module.as_ptr(), c"PbGetLifecycleInterface".as_ptr().cast())
            };
            if symbol.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "DLL lifecycle extension required",
                ));
            }
            let factory: raw::GetLifecycleInterface = unsafe { std::mem::transmute(symbol) };
            let mut table = MaybeUninit::<raw::LifecycleInterface>::zeroed();
            if unsafe {
                factory(
                    1,
                    size_of::<raw::LifecycleInterface>() as u32,
                    table.as_mut_ptr(),
                )
            } != 0
            {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "lifecycle ABI mismatch",
                ));
            }
            let life = unsafe { table.assume_init() };
            if life.size as usize != size_of::<raw::LifecycleInterface>()
                || life.major != 1
                || life.capabilities & 1 == 0
                || life.begin_stop.is_none()
                || life.begin_policy.is_none()
                || life.poll_policy.is_none()
                || life.progress.is_none()
                || life.release.is_none()
                || life.runtime.is_none()
                || life.retained.is_none()
                || life.forget_policy.is_none()
            {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "incomplete lifecycle interface",
                ));
            }
            let symbol = unsafe {
                GetProcAddress(module.as_ptr(), c"PbGetPacketInterface".as_ptr().cast())
            };
            if symbol.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "checked packet extension required",
                ));
            }
            let factory: raw::GetPacketInterface = unsafe { std::mem::transmute(symbol) };
            // Zeroed storage also makes an incomplete returned prefix safe to reject.
            let mut table = MaybeUninit::<raw::PacketInterface>::zeroed();
            if unsafe {
                factory(raw::PACKET_ABI_MAJOR, size_of::<raw::PacketInterface>() as u32, table.as_mut_ptr())
            } != 0 {
                return Err(io::Error::new(io::ErrorKind::Unsupported, "packet ABI mismatch"));
            }
            let packet = unsafe { table.assume_init() };
            validate_packet_interface(&packet)?;
            let symbol = unsafe { GetProcAddress(module.as_ptr(), c"PbCreateAdapterEx".as_ptr().cast()) };
            let create_adapter_ex = if symbol.is_null() { None } else {
                Some(unsafe { std::mem::transmute::<*mut c_void, raw::CreateAdapterEx>(symbol) })
            };
            let symbol = unsafe { GetProcAddress(module.as_ptr(), c"PbCreateAdapterEx2".as_ptr().cast()) };
            let create_adapter_ex2 = if symbol.is_null() { None } else {
                Some(unsafe { std::mem::transmute::<*mut c_void, raw::CreateAdapterEx2>(symbol) })
            };
            Ok(Arc::new(Self { module, api, life, packet, create_adapter_ex, create_adapter_ex2 }))
        })();
        if result.is_err() {
            unsafe {
                FreeLibrary(module.as_ptr());
            }
        }
        result
    }
    pub fn source_fingerprint(&self) -> &str {
        std::str::from_utf8(&self.api.source[..64]).expect("validated ASCII")
    }
    pub fn runtime_status(&self) -> Result<raw::RuntimeStatus> {
        let mut status = raw::RuntimeStatus::default();
        check(unsafe { self.life.runtime.unwrap()(&mut status) })?;
        Ok(status)
    }
    /// Snapshot native-owned sessions. Cleanup progresses without calling this method.
    pub fn retained_sessions(&self) -> Result<Vec<raw::CloseProgress>> {
        let mut sessions = Vec::new();
        for index in 0..self.runtime_status()?.max_sessions {
            let mut progress = raw::CloseProgress::default();
            let code = unsafe { self.life.retained.unwrap()(index, &mut progress) };
            if code == raw::EMPTY {
                break;
            }
            check(code)?;
            if progress.phase >= 2 {
                sessions.push(progress);
            }
        }
        Ok(sessions)
    }
    pub fn open(self: &Arc<Self>, device_path: &Path, config: Config) -> Result<Session> {
        let path = wide(device_path.as_os_str()).map_err(|_| Error(1))?;
        let mut native = ptr::null_mut();
        check(unsafe { self.api.open.unwrap()(path.as_ptr(), &config.raw(), &mut native) })?;
        Session::adopt(self.clone(), native)
    }
    pub fn create_adapter(self: &Arc<Self>, name: &str, instance: &str) -> Result<Adapter> {
        let name = wide(std::ffi::OsStr::new(name)).map_err(|_| Error(1))?;
        let instance = wide(std::ffi::OsStr::new(instance)).map_err(|_| Error(1))?;
        let mut native = ptr::null_mut();
        check(unsafe {
            self.api.create_adapter.unwrap()(name.as_ptr(), instance.as_ptr(), &mut native)
        })?;
        Ok(Adapter {
            library: self.clone(),
            native: NonNull::new(native).ok_or(Error(4))?,
        })
    }
    /// Create with native bounded recovery and retain original Windows errors.
    /// Requires the additive entry point in the matching embedded DLL build.
    pub fn create_adapter_with_diagnostics(self: &Arc<Self>, name: &str, instance: &str)
        -> std::result::Result<(Adapter, raw::AdapterDiagnostics), AdapterCreateError>
    {
        let mut diagnostics = raw::AdapterDiagnostics {
            size: size_of::<raw::AdapterDiagnostics>() as u32, schema: raw::ADAPTER_DIAG_SCHEMA,
            ..Default::default()
        };
        let make_error = |status, win32| AdapterCreateError {
            status, diagnostics: raw::AdapterDiagnostics { win32_error: win32, ..diagnostics }, details: None
        };
        let name = wide(std::ffi::OsStr::new(name)).map_err(|_| make_error(1, 87))?;
        let instance = wide(std::ffi::OsStr::new(instance)).map_err(|_| make_error(1, 87))?;
        let mut native = ptr::null_mut();
        let mut details = None;
        let status = if let Some(create) = self.create_adapter_ex2 {
            let mut extended = raw::AdapterDiagnosticsEx { base: raw::AdapterDiagnostics {
                size: size_of::<raw::AdapterDiagnosticsEx>() as u32, schema: raw::ADAPTER_DIAG_SCHEMA_EX,
                ..Default::default()
            }, ..Default::default() };
            let status = unsafe { create(name.as_ptr(), instance.as_ptr(), &mut native, &mut extended) };
            diagnostics = extended.base;
            if diagnostics.size as usize == size_of::<raw::AdapterDiagnosticsEx>() && diagnostics.schema == raw::ADAPTER_DIAG_SCHEMA_EX {
                // Return the stable schema-1 prefix to existing Rust callers.
                diagnostics.size = size_of::<raw::AdapterDiagnostics>() as u32;
                diagnostics.schema = raw::ADAPTER_DIAG_SCHEMA;
                details = Some(extended);
            }
            status
        } else {
            let create = self.create_adapter_ex.ok_or_else(|| make_error(9, 127))?;
            unsafe { create(name.as_ptr(), instance.as_ptr(), &mut native, &mut diagnostics) }
        };
        let adapter = NonNull::new(native).map(|native| Adapter { library: self.clone(), native });
        if status != 0 { return Err(AdapterCreateError { status, diagnostics, details }); }
        if diagnostics.size as usize != size_of::<raw::AdapterDiagnostics>() || diagnostics.schema != raw::ADAPTER_DIAG_SCHEMA ||
            diagnostics.stage > 9 || diagnostics.attempts > 2 || diagnostics.need_reboot > 1 || diagnostics.reserved != 0 ||
            diagnostics.problem_fields_known & !3 != 0 || diagnostics.first_problem_fields_known & !3 != 0
            || details.as_ref().is_some_and(|x| x.reserved != [0; 3] || x.recovery_step > 6 || x.probe_mismatch_mask & !127 != 0)
        {
            return Err(AdapterCreateError { status: 4, diagnostics, details });
        }
        let adapter = adapter.ok_or(AdapterCreateError { status: 4, diagnostics, details })?;
        Ok((adapter, diagnostics))
    }
    pub fn select_queue(&self, packet: &[u8], queues: u32) -> Result<u32> {
        if queues == 0 || queues > 8 || packet.len() > u32::MAX as usize {
            return Err(Error(1));
        }
        Ok(unsafe { self.api.select_queue.unwrap()(packet.as_ptr(), packet.len() as u32, queues) })
    }
}
impl Drop for Library {
    fn drop(&mut self) {
        unsafe {
            FreeLibrary(self.module.as_ptr());
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub queues: u32,
    pub slots: u32,
    pub ring_capacity: u32,
    pub offloads: u32,
    pub experimental: bool,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            queues: 1,
            slots: 32,
            ring_capacity: 64,
            offloads: 0,
            experimental: false,
        }
    }
}
impl Config {
    fn raw(self) -> raw::Config {
        raw::Config {
            size: size_of::<raw::Config>() as u32,
            queues: self.queues,
            slots: self.slots,
            capacity: self.ring_capacity,
            offloads: self.offloads,
            flags: if self.experimental { raw::OPEN_EXPERIMENTAL_OFFLOADS | raw::OPEN_CHECKED_SEGMENTS } else { 0 },
        }
    }
}

pub struct Adapter {
    library: Arc<Library>,
    native: NonNull<c_void>,
}
// Adapter references are counted by the DLL; metadata is immutable after creation.
unsafe impl Send for Adapter {}
unsafe impl Sync for Adapter {}
impl Adapter {
    pub fn luid(&self) -> Result<u64> {
        let mut luid = 0;
        check(unsafe { self.library.api.adapter_luid.unwrap()(self.native.as_ptr(), &mut luid) })?;
        Ok(luid)
    }
    pub fn start(&self, config: Config) -> Result<Session> {
        let mut native = ptr::null_mut();
        check(unsafe {
            self.library.api.start_adapter_session.unwrap()(
                self.native.as_ptr(),
                &config.raw(),
                &mut native,
            )
        })?;
        Session::adopt(self.library.clone(), native)
    }
}
impl Drop for Adapter {
    fn drop(&mut self) {
        unsafe {
            self.library.api.close_adapter.unwrap()(self.native.as_ptr());
        }
    }
}

pub struct Session {
    library: Arc<Library>,
    native: Option<NonNull<c_void>>,
    epoch: u64,
    queues: u32,
}
// Native control operations are synchronized. Queue objects cannot cross threads.
unsafe impl Send for Session {}
unsafe impl Sync for Session {}
impl Session {
    fn adopt(library: Arc<Library>, native: raw::Native) -> Result<Self> {
        let native = NonNull::new(native).ok_or(Error(4))?;
        let mut session = Self {
            library,
            native: Some(native),
            epoch: 0,
            queues: 0,
        };
        let status = session.status()?;
        if status.epoch == 0 || status.queues == 0 || status.queues > 8 {
            return Err(Error(4));
        }
        session.epoch = status.epoch;
        session.queues = status.queues;
        Ok(session)
    }
    fn pointer(&self) -> Result<raw::Native> {
        self.native.map(NonNull::as_ptr).ok_or(Error(7))
    }
    pub fn status(&self) -> Result<raw::Status> {
        let mut status = raw::Status::default();
        check(unsafe { self.library.api.get_status.unwrap()(self.pointer()?, &mut status) })?;
        if status.size as usize != size_of::<raw::Status>() {
            return Err(Error(4));
        }
        Ok(status)
    }
    pub fn queue_count(&self) -> u32 {
        self.queues
    }
    pub fn capabilities(&self) -> Result<raw::Capabilities> {
        let mut output = MaybeUninit::<raw::Capabilities>::uninit();
        check(unsafe {
            self.library.api.capabilities.unwrap()(self.pointer()?, output.as_mut_ptr())
        })?;
        Ok(unsafe { output.assume_init() })
    }
    pub fn queue(&self, index: u32) -> Result<Queue<'_>> {
        let mut native = ptr::null_mut();
        check(unsafe {
            self.library.api.open_queue.unwrap()(self.pointer()?, index, &mut native)
        })?;
        Ok(Queue {
            session: self,
            native: NonNull::new(native).ok_or(Error(4))?,
            index,
            single_owner: PhantomData,
        })
    }
    pub fn set_policy(&self, mask: u32) -> Result<()> {
        self.set_policy_timeout(mask, 2000)
    }
    pub fn request_policy(&self, mask: u32) -> Result<PolicyChange<'_>> {
        let mut ticket = 0;
        check(unsafe {
            self.library.life.begin_policy.unwrap()(self.pointer()?, mask, &mut ticket)
        })?;
        Ok(PolicyChange {
            session: self,
            ticket,
            acknowledged: false,
        })
    }
    pub fn set_policy_timeout(&self, mask: u32, timeout_ms: u32) -> Result<()> {
        if self.request_policy(mask)?.wait(timeout_ms)? {
            Ok(())
        } else {
            Err(Error(raw::EMPTY))
        }
    }
    pub fn close_progress(&self) -> Result<raw::CloseProgress> {
        let mut progress = raw::CloseProgress::default();
        check(unsafe { self.library.life.progress.unwrap()(self.pointer()?, &mut progress) })?;
        Ok(progress)
    }
    pub fn request_stop(&self) -> Result<()> {
        check(unsafe { self.library.life.begin_stop.unwrap()(self.pointer()?) })
    }
    /// Drain and acknowledge all references, while keeping the native session handle alive.
    pub fn prepare_close(&self, timeout_ms: u32) -> Result<()> {
        check(unsafe { self.library.api.prepare_close.unwrap()(self.pointer()?, timeout_ms) })
    }
    /// Wake a queue owner after publishing application-side work; data ownership stays on that owner.
    pub fn wake(&self, index: u32) -> Result<()> {
        check(unsafe { self.library.api.wake.unwrap()(self.pointer()?, index) })
    }
    pub fn select_queue(&self, packet: &[u8]) -> Result<u32> {
        self.library.select_queue(packet, self.queues)
    }
    pub fn close(&mut self, timeout_ms: u32) -> Result<()> {
        let Some(native) = self.native else {
            return Ok(());
        };
        check(unsafe { self.library.api.close.unwrap()(native.as_ptr(), timeout_ms) })?;
        self.native = None;
        Ok(())
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        if let Some(native) = self.native.take() {
            // The native runtime preallocates the close job and owns its module reference.
            unsafe {
                self.library.life.release.unwrap()(native.as_ptr());
            }
        }
    }
}
/// One bounded native policy operation. Dropping it abandons acknowledgement,
/// while the native runtime retains the actual request until I/O has completed.
pub struct PolicyChange<'s> {
    session: &'s Session,
    ticket: u64,
    acknowledged: bool,
}
impl PolicyChange<'_> {
    pub fn wait(&mut self, timeout_ms: u32) -> Result<bool> {
        if self.acknowledged {
            return Err(Error(5));
        }
        let mut applied = 0;
        let code = unsafe {
            self.session.library.life.poll_policy.unwrap()(
                self.session.pointer()?,
                self.ticket,
                timeout_ms,
                &mut applied,
            )
        };
        if code == raw::EMPTY {
            return Ok(false);
        }
        check(code)?;
        self.acknowledged = true;
        check(applied)?;
        Ok(true)
    }
}
impl Drop for PolicyChange<'_> {
    fn drop(&mut self) {
        if !self.acknowledged {
            if let Ok(native) = self.session.pointer() {
                unsafe {
                    self.session.library.life.forget_policy.unwrap()(native, self.ticket);
                }
            }
        }
    }
}
/// One native data owner; neither queues nor their borrowed buffers are Send or Sync.
/// ```compile_fail
/// fn requires_send<T: Send>() {}
/// requires_send::<packetbridge::Queue<'static>>();
/// ```
pub struct Queue<'s> {
    session: &'s Session,
    native: NonNull<c_void>,
    index: u32,
    single_owner: PhantomData<Rc<()>>,
}
impl Queue<'_> {
    pub fn receive(&self, maximum: usize) -> Result<ReceivedBatch<'_>> {
        let maximum = batch_size(maximum)?;
        let mut leases = [raw::Lease::default(); raw::MAX_BATCH];
        let mut count = 0;
        let code = unsafe {
            self.session.library.api.receive_batch.unwrap()(
                self.native.as_ptr(),
                leases.as_mut_ptr(),
                maximum,
                &mut count,
            )
        };
        if code != raw::EMPTY {
            check(code)?;
        }
        validate_leases(
            &leases,
            count,
            maximum,
            self.session.epoch,
            self.index,
            true,
        )?;
        Ok(ReceivedBatch {
            queue: self,
            items: leases[..count as usize].to_vec(),
        })
    }
    pub fn reserve(&self, maximum: usize) -> Result<ReservedBatch<'_>> {
        let maximum = batch_size(maximum)?;
        let mut leases = [raw::Lease::default(); raw::MAX_BATCH];
        let mut count = 0;
        check(unsafe {
            self.session.library.api.reserve_batch.unwrap()(
                self.native.as_ptr(),
                leases.as_mut_ptr(),
                maximum,
                &mut count,
            )
        })?;
        validate_leases(
            &leases,
            count,
            maximum,
            self.session.epoch,
            self.index,
            false,
        )?;
        Ok(ReservedBatch {
            queue: self,
            items: leases[..count as usize].to_vec(),
        })
    }
    pub fn completions(&self, maximum: usize) -> Result<Vec<raw::Descriptor>> {
        let maximum = batch_size(maximum)?;
        let mut descriptors = [raw::Descriptor::default(); raw::MAX_BATCH];
        let mut count = 0;
        let code = unsafe {
            self.session.library.api.poll_completions.unwrap()(
                self.native.as_ptr(),
                descriptors.as_mut_ptr(),
                maximum,
                &mut count,
            )
        };
        if code != raw::EMPTY {
            check(code)?;
        }
        if count > maximum {
            return Err(Error(4));
        }
        Ok(descriptors[..count as usize].to_vec())
    }
    pub fn wait(&self, timeout_ms: u32) -> Result<bool> {
        let code =
            unsafe { self.session.library.api.wait.unwrap()(self.native.as_ptr(), timeout_ms) };
        if code == raw::EMPTY {
            Ok(false)
        } else {
            check(code)?;
            Ok(true)
        }
    }
}
fn batch_size(size: usize) -> Result<u32> {
    if size == 0 || size > raw::MAX_BATCH {
        Err(Error(1))
    } else {
        Ok(size as u32)
    }
}
fn validate_leases(
    items: &[raw::Lease],
    count: u32,
    maximum: u32,
    epoch: u64,
    queue: u32,
    received: bool,
) -> Result<()> {
    if count > maximum {
        return Err(Error(4));
    }
    for item in &items[..count as usize] {
        if item.data.is_null()
            || item.capacity == 0
            || item.capacity > 65536
            || item.descriptor.length > item.capacity
            || (received && item.descriptor.length == 0)
            || item.descriptor.token.epoch != epoch
            || item.descriptor.token.queue as u32 != queue
        {
            return Err(Error(4));
        }
    }
    Ok(())
}

pub struct ReceivedBatch<'q> {
    queue: &'q Queue<'q>,
    items: Vec<raw::Lease>,
}
impl ReceivedBatch<'_> {
    pub fn len(&self) -> usize {
        self.items.len()
    }
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
    pub fn metadata(&self, index: usize) -> Option<&raw::Descriptor> {
        self.items.get(index).map(|v| &v.descriptor)
    }
    pub fn data(&self, index: usize) -> Option<&[u8]> {
        self.items
            .get(index)
            .map(|v| unsafe { std::slice::from_raw_parts(v.data, v.descriptor.length as usize) })
    }
    pub fn segment_count(&self, index: usize) -> Result<usize> {
        let item = self.items.get(index).ok_or(Error(1))?;
        if item.descriptor.kind == raw::PLAIN {
            return Ok(1);
        }
        Ok(self.plan(item)?.count as usize)
    }
    fn plan(&self, item: &raw::Lease) -> Result<raw::Plan> {
        let mut plan = raw::Plan::default();
        check(unsafe {
            self.queue.session.library.api.plan.unwrap()(
                item.data,
                item.descriptor.length,
                item.descriptor.kind,
                item.descriptor.mss,
                &mut plan,
            )
        })?;
        if plan.header != item.descriptor.header_length || plan.l4 != item.descriptor.l4_offset {
            return Err(Error(4));
        }
        Ok(plan)
    }
    /// Generate at most 32 wire packets, before protocol framing/encryption. Short output capacity preserves a prefix.
    pub fn write_segments(
        &self,
        index: usize,
        first: usize,
        outputs: &mut [&mut [u8]],
    ) -> Result<Vec<usize>> {
        batch_size(outputs.len())?;
        let item = self.items.get(index).ok_or(Error(1))?;
        if item.descriptor.kind == raw::PLAIN {
            let data = self.data(index).ok_or(Error(1))?;
            if first != 0 || outputs[0].len() < data.len() {
                return Err(Error(1));
            }
            outputs[0][..data.len()].copy_from_slice(data);
            return Ok(vec![data.len()]);
        }
        let plan = self.plan(item)?;
        if first >= plan.count as usize {
            return Err(Error(1));
        }
        let mut native = [raw::SegmentOutput::default(); raw::MAX_BATCH];
        for (out, buffer) in native.iter_mut().zip(outputs.iter_mut()) {
            out.data = buffer.as_mut_ptr();
            out.capacity = u32::try_from(buffer.len()).map_err(|_| Error(1))?;
        }
        let mut count = 0;
        let code = unsafe {
            self.queue.session.library.packet.write_segment_batch.unwrap()(
                item.data,
                item.descriptor.length,
                &plan,
                first as u32,
                native.as_mut_ptr(),
                outputs.len() as u32,
                &mut count,
            )
        };
        if code != 3 {
            check(code)?;
        } // Packet NO_SPACE may leave a successfully generated prefix.
        if count as usize > outputs.len() {
            return Err(Error(4));
        }
        if count == 0 && code != 0 {
            return Err(Error(code));
        }
        for item in native.iter().take(count as usize) {
            if item.length > item.capacity {
                return Err(Error(4));
            }
        }
        Ok(native[..count as usize]
            .iter()
            .map(|v| v.length as usize)
            .collect())
    }
    pub fn release(&mut self) -> Result<usize> {
        self.release_prefix(self.items.len())
    }
    /// Retire only fully consumed descriptors, preserving all remaining loans.
    /// The accepted prefix is removed before returning; callers must rebase
    /// their descriptor cursor by the returned count, including partial release.
    pub fn release_prefix(&mut self, requested: usize) -> Result<usize> {
        if requested > self.items.len() {
            return Err(Error(1));
        }
        if requested == 0 {
            return Ok(0);
        }
        let mut tokens = [raw::Token::default(); raw::MAX_BATCH];
        for (out, item) in tokens.iter_mut().zip(self.items.iter().take(requested)) {
            *out = item.descriptor.token;
        }
        let mut count = 0;
        check(unsafe {
            self.queue.session.library.api.release_batch.unwrap()(
                self.queue.native.as_ptr(),
                tokens.as_ptr(),
                requested as u32,
                &mut count,
            )
        })?;
        if count as usize > requested {
            return Err(Error(4));
        }
        self.items.drain(..count as usize);
        Ok(count as usize)
    }
}
impl Drop for ReceivedBatch<'_> {
    fn drop(&mut self) {
        let _ = self.release();
    }
}

pub struct ReservedBatch<'q> {
    queue: &'q Queue<'q>,
    items: Vec<raw::Lease>,
}
impl ReservedBatch<'_> {
    pub fn len(&self) -> usize {
        self.items.len()
    }
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
    pub fn data_mut(&mut self, index: usize) -> Option<&mut [u8]> {
        self.items
            .get_mut(index)
            .map(|v| unsafe { std::slice::from_raw_parts_mut(v.data, v.capacity as usize) })
    }
    pub fn set_length(&mut self, index: usize, length: usize) -> Result<()> {
        let item = self.items.get_mut(index).ok_or(Error(1))?;
        if length == 0 || length > item.capacity as usize {
            return Err(Error(1));
        }
        item.descriptor.length = length as u32;
        Ok(())
    }
    pub fn cancel(&mut self, index: usize) -> Result<()> {
        let token = self.items.get(index).ok_or(Error(1))?.descriptor.token;
        check(unsafe {
            self.queue.session.library.api.cancel_batch.unwrap()(
                self.queue.native.as_ptr(),
                &token,
                1,
            )
        })?;
        self.items.remove(index);
        Ok(())
    }
    pub fn commit(&mut self) -> Result<usize> {
        if self.items.is_empty() {
            return Ok(0);
        }
        let mut count = 0;
        check(unsafe {
            self.queue.session.library.api.commit_batch.unwrap()(
                self.queue.native.as_ptr(),
                self.items.as_ptr(),
                self.items.len() as u32,
                &mut count,
            )
        })?;
        if count as usize > self.items.len() {
            return Err(Error(4));
        }
        self.items.drain(..count as usize);
        Ok(count as usize)
    }
}
impl Drop for ReservedBatch<'_> {
    fn drop(&mut self) {
        if self.items.is_empty() {
            return;
        }
        let mut tokens = [raw::Token::default(); raw::MAX_BATCH];
        for (out, item) in tokens.iter_mut().zip(&self.items) {
            *out = item.descriptor.token;
        }
        let _ = unsafe {
            self.queue.session.library.api.cancel_batch.unwrap()(
                self.queue.native.as_ptr(),
                tokens.as_ptr(),
                self.items.len() as u32,
            )
        };
    }
}

#[cfg(all(test, feature = "native-tests"))]
#[path = "native_tests.rs"]
mod tests;
