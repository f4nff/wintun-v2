//! Owned, batch-oriented PacketBridgeV2 access. Driver-owned memory is retained on drain timeout.
pub use packetbridge_sys as raw;
#[cfg(windows)]
mod native;
#[cfg(windows)]
pub use native::*;
pub mod fallback;
