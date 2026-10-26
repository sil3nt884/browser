//! Starts the real engine through its C ABI and checks the browser-facing
//! contract: the port is bound immediately, start is idempotent, the SOCKS
//! front door answers, and a CONNECT made before bootstrap waits instead of
//! failing. Does not need (or use) a working Tor network.

use std::ffi::CString;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

#[test]
fn start_binds_port_and_socks_answers() {
    let dir = std::env::temp_dir().join(format!("umbra_tor_test_{}", std::process::id()));
    let state = CString::new(dir.join("state").to_str().unwrap()).unwrap();
    let cache = CString::new(dir.join("cache").to_str().unwrap()).unwrap();
    std::fs::create_dir_all(&dir).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    }

    let t0 = Instant::now();
    let mut port = 0u16;
    let rc = unsafe { umbra_tor::umbra_tor_start(state.as_ptr(), cache.as_ptr(), &mut port) };
    assert_eq!(rc, umbra_tor::OK);
    assert_ne!(port, 0);
    assert!(t0.elapsed() < Duration::from_secs(1), "start must not block on bootstrap");

    let mut port2 = 0u16;
    let rc2 = unsafe { umbra_tor::umbra_tor_start(state.as_ptr(), cache.as_ptr(), &mut port2) };
    assert_eq!(rc2, umbra_tor::OK);
    assert_eq!(port, port2, "second start returns the same port");

    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    s.write_all(&[5, 1, 0]).unwrap();
    let mut greet = [0u8; 2];
    s.read_exact(&mut greet).unwrap();
    assert_eq!(greet, [5, 0]);

    let host = b"example.com";
    let mut req = vec![5, 1, 0, 3, host.len() as u8];
    req.extend_from_slice(host);
    req.extend_from_slice(&443u16.to_be_bytes());
    s.write_all(&req).unwrap();

    // Before bootstrap completes the request must be held (read times out),
    // not answered with an immediate failure. On a fast network it may even
    // succeed; that's fine too.
    let mut reply = [0u8; 10];
    match s.read_exact(&mut reply) {
        Ok(()) => assert_eq!(reply[1], 0, "early reply must be success, got {reply:?}"),
        Err(e) => assert!(
            matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut),
            "unexpected: {e:?}"
        ),
    }

    let mut pct = -1;
    let phase = unsafe { umbra_tor::umbra_tor_status(&mut pct) };
    assert!(phase >= 0, "engine should be alive, phase={phase}");
    assert!((0..=100).contains(&pct));
}
