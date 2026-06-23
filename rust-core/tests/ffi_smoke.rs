//! Integration-level FFI smoke test: links the core as an rlib and drives a full
//! Mock session through the public C ABI, exactly as the C++/Qt layer would.

use research_ssh_core::event::{RsEvent, RsEventKind, RsSessionState};
use research_ssh_core::ffi::{
    rscore_create, rscore_destroy, rscore_session_connect, rscore_session_create,
    rscore_session_destroy, rscore_session_disconnect, rscore_session_send, RsSessionConfig,
};
use research_ssh_core::provider::RsProviderKind;
use research_ssh_core::RsErrorCode;
use std::ffi::{c_void, CString};
use std::sync::Mutex;
use std::time::Duration;

#[derive(Default)]
struct Sink {
    states: Mutex<Vec<RsSessionState>>,
    text: Mutex<String>,
    errors: Mutex<Vec<RsErrorCode>>,
}

extern "C" fn on_events(user_data: *mut c_void, events: *const RsEvent, count: usize) {
    // SAFETY: user_data is the &Sink we registered; the slice is valid for the call.
    let sink = unsafe { &*(user_data as *const Sink) };
    let evs = unsafe { std::slice::from_raw_parts(events, count) };
    for ev in evs {
        match ev.kind {
            RsEventKind::StateChanged => sink.states.lock().unwrap().push(ev.state),
            RsEventKind::Data => {
                let bytes = unsafe { std::slice::from_raw_parts(ev.data, ev.data_len) };
                sink.text
                    .lock()
                    .unwrap()
                    .push_str(&String::from_utf8_lossy(bytes));
            }
            RsEventKind::Error => sink.errors.lock().unwrap().push(ev.error_code),
            RsEventKind::HostKeyPrompt => {}
        }
    }
}

#[test]
fn full_mock_session_over_c_abi() {
    let mut err = RsErrorCode::Internal;
    let core = rscore_create(&mut err);
    assert!(!core.is_null());
    assert_eq!(err, RsErrorCode::Ok);

    let sink = Sink::default();
    let host = CString::new("gpu.cluster.edu").unwrap();
    let user = CString::new("alice").unwrap();
    let config = RsSessionConfig {
        host: host.as_ptr(),
        port: 22,
        username: user.as_ptr(),
        provider: RsProviderKind::Mock,
    };

    let session = rscore_session_create(
        core,
        &config,
        Some(on_events),
        &sink as *const Sink as *mut c_void,
        &mut err,
    );
    assert!(!session.is_null());

    assert_eq!(rscore_session_connect(session), RsErrorCode::Ok);
    std::thread::sleep(Duration::from_millis(450));

    let cmd = b"nvidia-smi\n";
    assert_eq!(
        rscore_session_send(session, cmd.as_ptr(), cmd.len()),
        RsErrorCode::Ok
    );
    std::thread::sleep(Duration::from_millis(300));

    assert_eq!(rscore_session_disconnect(session), RsErrorCode::Ok);
    std::thread::sleep(Duration::from_millis(250));

    rscore_session_destroy(session);
    rscore_destroy(core);

    assert!(sink
        .states
        .lock()
        .unwrap()
        .contains(&RsSessionState::Connected));
    assert!(sink.errors.lock().unwrap().is_empty());
    let text = sink.text.lock().unwrap();
    assert!(text.contains("gpu.cluster.edu"));
    assert!(text.contains("NVIDIA"));
}
