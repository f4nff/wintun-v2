//! Select API profiles within the unified DLL/SYS package. `Legacy` means the
//! Wintun compatibility API over the same v2 driver. It cannot bypass native
//! platform/build checks or switch to an unrelated official Wintun driver.
//! Fall back only after the corresponding native operation is acknowledged.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Auto,
    Legacy,
    Plain,
    Offloads,
}
impl Mode {
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "auto" => Some(Self::Auto),
            "legacy" => Some(Self::Legacy),
            "plain" => Some(Self::Plain),
            "offloads" => Some(Self::Offloads),
            _ => None,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    Legacy,
    Modern { mask: u32 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Running(Profile),
    PolicyPending { previous: u32, next: u32 },
    Draining,
    Quarantined,
    ReplacePending,
    Stopped,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    None,
    SetPolicy(u32),
    StopAdmissionAndDrain,
    RetainMappings,
    ReplaceWithLegacy,
    Stop,
}
#[derive(Clone, Copy, Debug)]
pub struct Availability {
    pub os_supported: bool,
    pub matching_modern: bool,
    pub validated_mask: u32,
    pub legacy_verified: bool,
}
pub fn select(mode: Mode, available: Availability) -> Option<Profile> {
    let modern = available.os_supported && available.matching_modern;
    if !modern {
        return None;
    }
    match mode {
        Mode::Legacy => available.legacy_verified.then_some(Profile::Legacy),
        Mode::Plain => modern.then_some(Profile::Modern { mask: 0 }),
        Mode::Offloads => modern.then_some(Profile::Modern {
            mask: available.validated_mask,
        }),
        Mode::Auto if modern => Some(Profile::Modern {
            mask: available.validated_mask,
        }),
        Mode::Auto => available.legacy_verified.then_some(Profile::Legacy),
    }
}
pub struct Controller {
    state: State,
    legacy_verified: bool,
    deadline: u64,
    failures: u32,
}
impl Controller {
    pub fn new(profile: Profile, legacy_verified: bool) -> Self {
        Self {
            state: State::Running(profile),
            legacy_verified,
            deadline: 0,
            failures: 0,
        }
    }
    pub fn state(&self) -> State {
        self.state
    }
    pub fn failures(&self) -> u32 {
        self.failures
    }
    pub fn prefer_legacy_on_restart(&self) -> bool {
        self.failures >= 3 && self.legacy_verified
    }
    pub fn disable(&mut self, bits: u32) -> Action {
        if let State::Running(Profile::Modern { mask }) = self.state {
            let next = mask & !bits;
            if next != mask {
                self.state = State::PolicyPending {
                    previous: mask,
                    next,
                };
                return Action::SetPolicy(next);
            }
        }
        Action::None
    }
    pub fn policy_applied(&mut self, success: bool, now: u64) -> Action {
        if let State::PolicyPending { previous: _, next } = self.state {
            if success {
                self.state = State::Running(Profile::Modern { mask: next });
                Action::None
            } else {
                self.fault(now)
            }
        } else {
            Action::None
        }
    }
    pub fn fault(&mut self, now: u64) -> Action {
        self.failures = self.failures.saturating_add(1);
        match self.state {
            State::Draining | State::Quarantined | State::ReplacePending => Action::None,
            State::Stopped | State::Running(Profile::Legacy) => {
                self.state = State::Stopped;
                Action::Stop
            }
            _ => {
                self.state = State::Draining;
                self.deadline = now.wrapping_add(2000);
                Action::StopAdmissionAndDrain
            }
        }
    }
    /// `native_prepared` requires PrepareClose success for this same session.
    /// Zero reference/lease counters alone do not acknowledge native drain.
    pub fn observe_drain(
        &mut self,
        now: u64,
        ndis_references: u32,
        user_leases: u32,
        native_prepared: bool,
    ) -> Action {
        if !matches!(self.state, State::Draining | State::Quarantined) {
            return Action::None;
        }
        if native_prepared && ndis_references == 0 && user_leases == 0 {
            if self.legacy_verified {
                self.state = State::ReplacePending;
                Action::ReplaceWithLegacy
            } else {
                self.state = State::Stopped;
                Action::Stop
            }
        } else if now.wrapping_sub(self.deadline) < (1u64 << 63) {
            self.state = State::Quarantined;
            Action::RetainMappings
        } else {
            Action::None
        }
    }
    pub fn replaced(&mut self, success: bool) -> Action {
        if self.state != State::ReplacePending {
            return Action::None;
        }
        self.state = if success {
            State::Running(Profile::Legacy)
        } else {
            State::Stopped
        };
        if success { Action::None } else { Action::Stop }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn compatibility_cannot_bypass_native_platform_or_build_checks() {
        let mut available = Availability {
            os_supported: false,
            matching_modern: true,
            validated_mask: 0x1ff,
            legacy_verified: true,
        };
        assert_eq!(select(Mode::Auto, available), None);
        assert_eq!(select(Mode::Legacy, available), None);
        assert_eq!(select(Mode::Offloads, available), None);
        available.os_supported = true;
        available.matching_modern = false;
        assert_eq!(select(Mode::Auto, available), None);
        available.matching_modern = true;
        assert_eq!(select(Mode::Legacy, available), Some(Profile::Legacy));
        available.legacy_verified = false;
        assert_eq!(select(Mode::Legacy, available), None);
        assert_eq!(select(Mode::Auto, available), Some(Profile::Modern { mask: 0x1ff }));
    }
    #[test]
    fn plain_mode_never_enables_requested_but_unvalidated_offloads() {
        let available = Availability {
            os_supported: true,
            matching_modern: true,
            validated_mask: 0,
            legacy_verified: true,
        };
        assert_eq!(
            select(Mode::Offloads, available),
            Some(Profile::Modern { mask: 0 })
        );
        assert_eq!(
            select(Mode::Plain, available),
            Some(Profile::Modern { mask: 0 })
        );
        assert_eq!(Mode::parse("offload"), None);
    }
    #[test]
    fn disabled_is_published_only_after_native_acknowledgement() {
        let mut c = Controller::new(Profile::Modern { mask: 7 }, true);
        assert_eq!(c.disable(2), Action::SetPolicy(5));
        assert_eq!(
            c.state(),
            State::PolicyPending {
                previous: 7,
                next: 5
            }
        );
        assert_eq!(c.policy_applied(true, 0), Action::None);
        assert_eq!(c.state(), State::Running(Profile::Modern { mask: 5 }));
        assert_eq!(c.disable(7), Action::SetPolicy(0));
        assert_eq!(c.policy_applied(false, 1), Action::StopAdmissionAndDrain);
        assert_eq!(c.state(), State::Draining);
    }
    #[test]
    fn timeout_retains_mapping_until_last_reference_and_lease_return() {
        let mut c = Controller::new(Profile::Modern { mask: 7 }, true);
        assert_eq!(c.fault(1000), Action::StopAdmissionAndDrain);
        assert_eq!(c.observe_drain(2999, 1, 0, true), Action::None);
        assert_eq!(c.observe_drain(3000, 1, 0, true), Action::RetainMappings);
        assert_eq!(c.observe_drain(4000, 0, 1, true), Action::RetainMappings);
        assert_eq!(c.observe_drain(4001, 0, 0, true), Action::ReplaceWithLegacy);
        assert_eq!(c.replaced(true), Action::None);
        assert_eq!(c.state(), State::Running(Profile::Legacy));
    }
    #[test]
    fn repeated_fault_does_not_renew_drain_deadline_or_start_duplicate_switch() {
        let mut c = Controller::new(Profile::Modern { mask: 0 }, true);
        c.fault(0);
        c.fault(1900);
        c.fault(1999);
        assert!(c.prefer_legacy_on_restart());
        assert_eq!(c.observe_drain(2000, 2, 0, true), Action::RetainMappings);
        assert_eq!(c.observe_drain(2001, 0, 0, true), Action::ReplaceWithLegacy);
        assert_eq!(c.observe_drain(2002, 0, 0, true), Action::None);
        assert_eq!(c.replaced(false), Action::Stop);
    }
    #[test]
    fn no_legacy_package_means_stop_without_a_direct_network_mode() {
        let mut c = Controller::new(Profile::Modern { mask: 0 }, false);
        c.fault(0);
        assert_eq!(c.observe_drain(1, 0, 0, true), Action::Stop);
        assert_eq!(c.state(), State::Stopped);
    }
    #[test]
    fn zero_counts_require_native_prepare_acknowledgement_before_replacement() {
        let mut c = Controller::new(Profile::Modern { mask: 7 }, true);
        assert_eq!(c.fault(100), Action::StopAdmissionAndDrain);
        assert_eq!(c.observe_drain(101, 0, 0, false), Action::None);
        assert_eq!(c.state(), State::Draining);
        assert_eq!(c.observe_drain(2100, 0, 0, false), Action::RetainMappings);
        assert_eq!(c.observe_drain(2101, 0, 0, true), Action::ReplaceWithLegacy);
    }
    #[test]
    fn late_policy_success_cannot_revive_a_draining_controller() {
        let mut c = Controller::new(Profile::Modern { mask: 7 }, true);
        assert_eq!(c.disable(2), Action::SetPolicy(5));
        assert_eq!(c.fault(10), Action::StopAdmissionAndDrain);
        assert_eq!(c.policy_applied(true, 11), Action::None);
        assert_eq!(c.state(), State::Draining);
    }
}
