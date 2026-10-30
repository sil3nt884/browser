//! umbra_tor_stop closes the SOCKS port and reports the stopped phase, and
//! umbra_tor_start afterwards starts Tor again on a working port. Its own
//! test binary: the engine is process-wide. Does not need a Tor network.

use std::ffi::CString;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

fn socks_answers(port: u16) -> bool {
    let Ok(mut s) = TcpStream::connect(("127.0.0.1", port)) else {
        return false;
    };
    s.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    let mut greet = [0u8; 2];
    s.write_all(&[5, 1, 0]).is_ok() && s.read_exact(&mut greet).is_ok() && greet == [5, 0]
}

#[test]
fn stop_closes_the_port_and_start_works_again() {
    let dir = std::env::temp_dir().join(format!("umbra_tor_stop_{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    }
    let state = CString::new(dir.join("state").to_str().unwrap()).unwrap();
    let cache = CString::new(dir.join("cache").to_str().unwrap()).unwrap();

    let mut port = 0u16;
    let rc = unsafe { umbra_tor::umbra_tor_start(state.as_ptr(), cache.as_ptr(), &mut port) };
    assert_eq!(rc, umbra_tor::OK);
    assert!(socks_answers(port));

    assert_eq!(umbra_tor::umbra_tor_stop(), umbra_tor::OK);
    let mut pct = -1;
    let phase = unsafe { umbra_tor::umbra_tor_status(&mut pct) };
    assert_eq!(phase, umbra_tor::engine::PHASE_STOPPED);
    assert_eq!(pct, 0);
    assert!(!socks_answers(port), "the old port must be closed after stop");

    // Stopping twice is harmless.
    assert_eq!(umbra_tor::umbra_tor_stop(), umbra_tor::OK);

    let mut port2 = 0u16;
    let rc2 = unsafe { umbra_tor::umbra_tor_start(state.as_ptr(), cache.as_ptr(), &mut port2) };
    assert_eq!(rc2, umbra_tor::OK);
    assert_ne!(port2, 0);
    assert!(socks_answers(port2), "a restarted engine must serve SOCKS");
    let phase = unsafe { umbra_tor::umbra_tor_status(&mut pct) };
    assert!(phase >= 0 && phase != umbra_tor::engine::PHASE_STOPPED, "phase={phase}");

    assert_eq!(umbra_tor::umbra_tor_stop(), umbra_tor::OK);
}
