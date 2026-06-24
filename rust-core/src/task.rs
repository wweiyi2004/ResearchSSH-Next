//! Side-channel task execution results.
//!
//! This module is intentionally generic: package discovery, resource snapshots,
//! script runs, and future non-interactive commands all use the same output
//! shape instead of writing marker commands into the interactive terminal.

use crate::RsErrorCode;

/// Output captured from one non-interactive remote command.
#[derive(Debug, Clone, Default)]
pub struct ExecOutput {
    /// Bytes captured from stdout.
    pub stdout: Vec<u8>,
    /// Bytes captured from stderr.
    pub stderr: Vec<u8>,
    /// Process exit status, or `-1` when the backend could not observe one.
    pub exit_status: i32,
}

/// Payload for a completed side-channel command.
#[derive(Debug, Clone)]
pub enum ExecPayload {
    /// Command completed and returned captured streams.
    Output(ExecOutput),
    /// Command could not be run or was cancelled.
    Error {
        /// Stable error code.
        code: RsErrorCode,
        /// Human-readable detail.
        message: String,
    },
}

/// Result of one side-channel command, correlated by request id.
#[derive(Debug, Clone)]
pub struct ExecResult {
    /// Request id returned to the caller when the command was queued.
    pub request_id: u64,
    /// Captured output or error.
    pub payload: ExecPayload,
}
