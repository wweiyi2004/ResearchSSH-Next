//! Concrete [`SshProvider`](crate::provider::SshProvider) implementations.
//!
//! * [`mock`] — always available; an in-process simulator used to build out the
//!   UI/architecture and to test the FFI without a network or SSH server.
//! * [`russh`] — real SSH backend gated behind the `russh` cargo feature.

pub mod mock;

#[cfg(feature = "russh")]
pub mod russh;
