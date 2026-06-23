//! Sensitive byte buffers.
//!
//! Passwords and private-key material are wrapped in [`Secret`], which zeroizes
//! its contents on drop via the `zeroize` crate. The QML layer never touches
//! these bytes; they enter the core only through the dedicated FFI entry point
//! (`rscore_session_set_password`) and are copied straight into a zeroizing
//! buffer. This is the reserved hook for real credential handling — the Mock
//! provider simply discards the secret.

use zeroize::Zeroizing;

/// A byte buffer whose contents are wiped from memory when dropped.
#[derive(Clone)]
pub struct Secret(Zeroizing<Vec<u8>>);

impl Secret {
    /// Copy `bytes` into a zeroizing buffer.
    pub fn from_bytes(bytes: &[u8]) -> Self {
        Secret(Zeroizing::new(bytes.to_vec()))
    }

    /// Borrow the protected bytes.
    pub fn expose(&self) -> &[u8] {
        &self.0
    }

    /// Length in bytes.
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// Whether the secret is empty.
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

// Never print secret contents.
impl std::fmt::Debug for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Secret([{} bytes redacted])", self.0.len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secret_roundtrips_and_redacts() {
        let s = Secret::from_bytes(b"hunter2");
        assert_eq!(s.expose(), b"hunter2");
        assert_eq!(s.len(), 7);
        assert!(!s.is_empty());
        let dbg = format!("{s:?}");
        assert!(dbg.contains("redacted"));
        assert!(!dbg.contains("hunter2"));
    }
}
