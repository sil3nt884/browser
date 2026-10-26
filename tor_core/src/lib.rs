//! umbra_tor: Tor (via Arti) running inside the browser process, reachable
//! through a loopback-only SOCKS5 port.
//!
//! C ABI (see `include/umbra_tor.h`):
//!   umbra_tor_abi_version() -> u32
//!   umbra_tor_start(state_dir, cache_dir, *out_port) -> i32   (0 = ok)
//!   umbra_tor_status(*out_percent) -> i32                     (phase)
//!   umbra_tor_last_error(buf, len) -> usize
//!
//! `umbra_tor_start` binds the SOCKS port synchronously and returns at once,
//! so the browser can point its proxy settings at the port before any network
//! code runs. Tor bootstraps on a background thread; streams opened before it
//! finishes wait instead of failing.

pub mod engine;
pub mod socks;

use std::ffi::{c_char, CStr};
use std::net::{Ipv4Addr, TcpListener};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::OnceLock;

/// Bumped whenever the C ABI changes shape.
pub const ABI_VERSION: u32 = 1;

pub const OK: i32 = 0;
pub const ERR_BAD_ARGS: i32 = -1;
pub const ERR_BIND: i32 = -2;
pub const ERR_THREAD: i32 = -3;
pub const ERR_PANIC: i32 = -4;

static STARTED_PORT: OnceLock<u16> = OnceLock::new();

#[no_mangle]
pub extern "C" fn umbra_tor_abi_version() -> u32 {
    ABI_VERSION
}

/// Start the engine (idempotent). On success writes the SOCKS port to
/// `out_port` and returns 0.
///
/// # Safety
/// `state_dir` and `cache_dir` must be valid NUL-terminated UTF-8 strings;
/// `out_port` must be a valid pointer to a `u16`.
#[no_mangle]
pub unsafe extern "C" fn umbra_tor_start(
    state_dir: *const c_char,
    cache_dir: *const c_char,
    out_port: *mut u16,
) -> i32 {
    if state_dir.is_null() || cache_dir.is_null() || out_port.is_null() {
        return ERR_BAD_ARGS;
    }
    let state = CStr::from_ptr(state_dir).to_str().map(PathBuf::from);
    let cache = CStr::from_ptr(cache_dir).to_str().map(PathBuf::from);
    let (Ok(state), Ok(cache)) = (state, cache) else {
        return ERR_BAD_ARGS;
    };

    let res = catch_unwind(AssertUnwindSafe(|| start_inner(state, cache)));
    match res {
        Ok(Ok(port)) => {
            *out_port = port;
            OK
        }
        Ok(Err(code)) => code,
        Err(_) => {
            engine::set_error("panic during start");
            ERR_PANIC
        }
    }
}

fn start_inner(state: PathBuf, cache: PathBuf) -> Result<u16, i32> {
    if let Some(port) = STARTED_PORT.get() {
        return Ok(*port);
    }
    init_logging();
    // Must happen before Arti opens its first TLS channel. Err just means a
    // provider is already installed (e.g. start called after a host did it).
    let _ = rustls::crypto::ring::default_provider().install_default();

    // Port 0: let the OS pick, so two installs (or a dev build next to a
    // release build) never fight over a fixed port.
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).map_err(|e| {
        engine::set_error(format!("bind failed: {e}"));
        ERR_BIND
    })?;
    listener.set_nonblocking(true).map_err(|_| ERR_BIND)?;
    let port = listener.local_addr().map_err(|_| ERR_BIND)?.port();

    let dirs = engine::Dirs { state, cache };
    std::thread::Builder::new()
        .name("umbra-tor".into())
        .spawn(move || {
            let rt = match tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .thread_name("umbra-tor-worker")
                .enable_all()
                .build()
            {
                Ok(rt) => rt,
                Err(e) => {
                    engine::PHASE.store(engine::PHASE_FAILED, Ordering::SeqCst);
                    engine::set_error(format!("tokio runtime: {e}"));
                    return;
                }
            };
            let outcome = catch_unwind(AssertUnwindSafe(|| rt.block_on(engine::run(listener, dirs))));
            if outcome.is_err() {
                engine::PHASE.store(engine::PHASE_FAILED, Ordering::SeqCst);
                engine::set_error("tor engine panicked");
            }
        })
        .map_err(|_| ERR_THREAD)?;

    let _ = STARTED_PORT.set(port);
    Ok(port)
}

/// Returns the lifecycle phase (0 starting, 1 bootstrapping, 2 ready,
/// -1 failed) and writes bootstrap progress (0–100) to `out_percent` if non-null.
///
/// # Safety
/// `out_percent` must be null or a valid pointer to an `i32`.
#[no_mangle]
pub unsafe extern "C" fn umbra_tor_status(out_percent: *mut i32) -> i32 {
    if !out_percent.is_null() {
        *out_percent = engine::PERCENT.load(Ordering::SeqCst);
    }
    engine::PHASE.load(Ordering::SeqCst)
}

/// Copies the last error (UTF-8, NUL-terminated, truncated to fit) into `buf`.
/// Returns the full length of the message excluding the NUL.
///
/// # Safety
/// `buf` must be null or valid for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn umbra_tor_last_error(buf: *mut c_char, len: usize) -> usize {
    let msg = engine::LAST_ERROR.lock().map(|m| m.clone()).unwrap_or_default();
    if !buf.is_null() && len > 0 {
        let n = msg.len().min(len - 1);
        std::ptr::copy_nonoverlapping(msg.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
    msg.len()
}

fn init_logging() {
    use tracing_subscriber::EnvFilter;
    let filter = EnvFilter::try_from_env("UMBRA_TOR_LOG").unwrap_or_else(|_| EnvFilter::new("warn"));
    let _ = tracing_subscriber::fmt().with_env_filter(filter).with_writer(std::io::stderr).try_init();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn last_error_truncates_and_terminates() {
        engine::set_error("abcdef");
        let mut buf = [1 as c_char; 4];
        let full = unsafe { umbra_tor_last_error(buf.as_mut_ptr(), buf.len()) };
        assert_eq!(full, 6);
        let s = unsafe { CStr::from_ptr(buf.as_ptr()) };
        assert_eq!(s.to_str().unwrap(), "abc");
    }

    #[test]
    fn start_rejects_null_args() {
        let mut port = 0u16;
        let rc = unsafe { umbra_tor_start(std::ptr::null(), std::ptr::null(), &mut port) };
        assert_eq!(rc, ERR_BAD_ARGS);
    }
}
