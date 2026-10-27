// Copyright 2026 The Umbra Authors.
//! Umbra Remote: a tray app that runs Umbra off-screen and streams it to a
//! phone on the same Wi-Fi (see hub.rs for the protocol).
//!
//!   umbra-remote.exe [--port 7777] [--chrome PATH] [--home URL]
//!                    [--audio-device NAME]   (default: VB-Audio Virtual Cable)

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod audio;
mod browser;
mod cdp;
mod hub;
mod server;
mod tray;
mod video;
mod window;

use std::io::Write;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

pub fn log_line(msg: &str) {
  let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
  let line = format!("[{secs}] {msg}\n");
  if cfg!(debug_assertions) {
    eprint!("{line}");
  }
  static FILE: Mutex<Option<std::fs::File>> = Mutex::new(None);
  let mut f = FILE.lock().unwrap();
  if f.is_none() {
    let dir = browser::data_dir();
    let _ = std::fs::create_dir_all(&dir);
    *f = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("agent.log")).ok();
  }
  if let Some(f) = f.as_mut() {
    let _ = f.write_all(line.as_bytes());
  }
}

#[macro_export]
macro_rules! log {
  ($($t:tt)*) => { $crate::log_line(&format!($($t)*)) };
}

struct Args {
  port: u16,
  chrome: PathBuf,
  home: String,
  audio_device: Option<String>,
}

fn parse_args() -> Args {
  let mut a = Args { port: 7777, chrome: default_chrome(), home: "https://duckduckgo.com/".into(), audio_device: None };
  let mut it = std::env::args().skip(1);
  while let Some(k) = it.next() {
    match (k.as_str(), it.next()) {
      ("--port", Some(v)) => a.port = v.parse().unwrap_or(a.port),
      ("--chrome", Some(v)) => a.chrome = v.into(),
      ("--home", Some(v)) => a.home = v,
      ("--audio-device", Some(v)) => a.audio_device = Some(v),
      _ => {}
    }
  }
  a
}

/// chrome.exe next to this exe (the packaged layout), else C:\UmbraBrowser.
fn default_chrome() -> PathBuf {
  if let Some(p) = std::env::current_exe().ok().and_then(|e| Some(e.parent()?.join("chrome.exe"))) {
    if p.exists() {
      return p;
    }
  }
  PathBuf::from(r"C:\UmbraBrowser\chrome.exe")
}

/// The address phones should use. Connecting a UDP socket sends nothing; it
/// only asks Windows which local interface would route outward.
fn lan_ip() -> String {
  std::net::UdpSocket::bind("0.0.0.0:0")
    .and_then(|s| {
      s.connect("192.0.2.1:9")?; // TEST-NET-1, never actually contacted
      s.local_addr()
    })
    .map(|a| a.ip().to_string())
    .unwrap_or_else(|_| "this-pc".into())
}

fn main() {
  let raw: Vec<String> = std::env::args().collect();
  if raw.get(1).map(String::as_str) == Some("--route-test") && raw.len() == 4 {
    return audio::route_test(raw[2].parse().unwrap_or(0), &raw[3]);
  }
  if raw.get(1).map(String::as_str) == Some("--video-test") && raw.len() == 5 {
    return video_test(&raw[2], raw[3].parse().unwrap_or(5), &raw[4]);
  }
  let args = parse_args();
  log!("starting; chrome = {}", args.chrome.display());
  // After a restart from the phone the old agent may still hold the port
  // for a moment.
  let restarted = raw.iter().any(|a| a == "--restarted");
  let mut bound = std::net::TcpListener::bind(("0.0.0.0", args.port));
  for _ in 0..(if restarted { 50 } else { 0 }) {
    if bound.is_ok() {
      break;
    }
    std::thread::sleep(std::time::Duration::from_millis(200));
    bound = std::net::TcpListener::bind(("0.0.0.0", args.port));
  }
  let listener = match bound {
    Ok(l) => l,
    Err(e) => {
      tray::message_box(&format!(
        "Umbra Remote can't use port {}: {e}\n\nIs it already running?",
        args.port
      ));
      return;
    }
  };
  listener.set_nonblocking(true).expect("nonblocking");
  let address = format!("{}:{}", lan_ip(), args.port);
  log!("phones connect to {address}");

  let status: hub::SharedStatus = Arc::new(Mutex::new(hub::Status::default()));
  let (client_tx, client_rx) = tokio::sync::mpsc::unbounded_channel();
  let (cmd_tx, cmd_rx) = tokio::sync::mpsc::unbounded_channel();
  let (done_tx, done_rx) = std::sync::mpsc::channel::<bool>();

  let st = status.clone();
  let main_thread = tray::current_thread();
  std::thread::spawn(move || {
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().expect("runtime");
    let ended = rt.block_on(async move {
      let l = tokio::net::TcpListener::from_std(listener).expect("listener");
      tokio::spawn(server::serve(l, client_tx));
      hub::run(args.chrome, args.home, args.audio_device, client_rx, cmd_rx, st).await
    });
    let again = matches!(ended, hub::Ended::RestartAgent);
    let _ = done_tx.send(again);
    if again {
      tray::quit(main_thread); // the tray loop ends; main() relaunches us
    }
  });

  tray::run(status, cmd_tx, &address);
  // Give the browser a moment to close cleanly.
  let again = done_rx.recv_timeout(std::time::Duration::from_secs(8)).unwrap_or(false);
  if again {
    let mut cmd = std::process::Command::new(std::env::current_exe().expect("exe"));
    cmd.args(raw.iter().skip(1).filter(|a| *a != "--restarted")).arg("--restarted");
    match cmd.spawn() {
      Ok(_) => log!("relaunched"),
      Err(e) => log!("relaunch failed: {e}"),
    }
  }
  log!("bye");
}

/// Encodes a test page in a throwaway Umbra window for a few seconds and
/// writes the raw H.264 stream (kind bytes stripped) to `out`.
fn video_test(chrome: &str, secs: u64, out: &str) {
  use std::time::{Duration, Instant};
  let profile = std::env::temp_dir().join("umbra-video-test");
  let page = "data:text/html,<body style='margin:0;font:28px sans-serif'><div id=d></div>    <div style='font:14px serif'>The quick brown fox jumps over the lazy dog 0123456789</div>    <div id=b style='position:absolute;top:300px;width:80px;height:80px;background:%23e33'></div>    <script>let n=0;(function f(){n++;d.textContent=n+' dpr='+devicePixelRatio+' '+innerWidth+'x'+innerHeight;    b.style.left=(n*4%300)+'px';requestAnimationFrame(f)})()</script>";
  let mut child = std::process::Command::new(chrome)
    .arg(format!("--user-data-dir={}", profile.display()))
    .args(["--no-first-run", "--disable-features=CalculateNativeWinOcclusion", "--disable-backgrounding-occluded-windows",
      "--disable-renderer-backgrounding", "--window-position=-32000,-32000", "--force-device-scale-factor=2.625"])
    .arg(page)
    .spawn()
    .expect("chrome");
  let mut hwnd = None;
  for _ in 0..100 {
    std::thread::sleep(Duration::from_millis(100));
    hwnd = window::browser_windows(child.id()).first().copied();
    if hwnd.is_some() {
      break;
    }
  }
  let Some(hwnd) = hwnd else { return log!("video-test: no window") };
  std::thread::sleep(Duration::from_secs(2));
  window::place(hwnd, -32000, -32000, 1200, 2700);
  std::thread::sleep(Duration::from_millis(500));
  log!("video-test: window {:?} page at {:?}", window::rect(hwnd), window::page_origin(hwnd));
  let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
  let in_flight = Arc::new(std::sync::atomic::AtomicU32::new(0));
  let s = video::Settings { width: 1082, height: 2048, bitrate: 12_000_000, fps: 60, place: (-32000, -32000, 1200, 2700) };
  let v = match video::start(hwnd, s, move |au| { let _ = tx.send(au); }, in_flight.clone()) {
    Ok(v) => v,
    Err(e) => {
      let _ = child.kill();
      return log!("video-test: {e}");
    }
  };
  let t = Instant::now();
  let (mut n, mut bytes, mut keys, mut stream) = (0, 0, 0, Vec::new());
  while t.elapsed() < Duration::from_secs(secs) {
    while let Ok(au) = rx.try_recv() {
      n += 1;
      bytes += au.len();
      if au.windows(4).any(|w| w[..3] == [0, 0, 1] && w[3] & 0x1f == 5) {
        keys += 1;
      }
      stream.extend_from_slice(&au[1..]);
      in_flight.fetch_sub(1, std::sync::atomic::Ordering::Relaxed); // the "phone" acks at once
    }
    if n == 120 {
      v.request_key();
    }
    std::thread::sleep(Duration::from_millis(2));
  }
  drop(v);
  let _ = child.kill();
  let _ = std::fs::write(out, &stream);
  log!("video-test: {n} frames ({:.1} fps), {:.2} Mbit/s, {keys} keyframes", n as f64 / secs as f64, bytes as f64 * 8.0 / secs as f64 / 1e6);
}
