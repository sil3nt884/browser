// Copyright 2026 The Umbra Authors.
//! Starts Umbra with DevTools on a loopback-only port and finds that port.

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Duration;

pub fn data_dir() -> PathBuf {
  let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
  base.join("UmbraRemote")
}

fn profile_dir() -> PathBuf {
  data_dir().join("profile")
}

/// `DevToolsActivePort` holds the port on line 1 and the browser target path
/// on line 2. Chrome only ever binds it to 127.0.0.1.
fn read_port_file() -> Option<String> {
  let text = std::fs::read_to_string(profile_dir().join("DevToolsActivePort")).ok()?;
  let mut lines = text.lines();
  let port: u16 = lines.next()?.trim().parse().ok()?;
  let path = lines.next()?.trim();
  Some(format!("ws://127.0.0.1:{port}{path}"))
}

/// The DevTools URL of an Umbra this agent started earlier, if one is
/// still running (e.g. the agent was restarted but the browser wasn't).
pub fn existing() -> Option<String> {
  read_port_file()
}

/// Chrome for Android's "reduced" UA string (what every Chrome phone sends).
pub fn mobile_ua(major: &str) -> String {
  format!("Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/{major}.0.0.0 Mobile Safari/537.36")
}

pub fn desktop_ua(major: &str) -> String {
  format!("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/{major}.0.0.0 Safari/537.36")
}

/// Umbra's major version, from the `<version>.manifest` next to chrome.exe.
fn chrome_major(chrome: &Path) -> String {
  let dir = chrome.parent().map(Path::to_path_buf).unwrap_or_default();
  std::fs::read_dir(dir)
    .into_iter()
    .flatten()
    .flatten()
    .filter_map(|e| e.file_name().into_string().ok())
    .find_map(|n| {
      let v = n.strip_suffix(".manifest")?;
      (v.split('.').count() == 4 && v.split('.').all(|p| p.parse::<u32>().is_ok()))
        .then(|| v.split('.').next().unwrap().to_string())
    })
    .unwrap_or_else(|| "157".into())
}

/// The pixel density Umbra was started with (see `launch`); 1 for a
/// browser that predates it.
pub fn running_scale() -> f64 {
  read_number("scale").unwrap_or(1.0)
}

/// The last phone's pixel density, to start the next browser with.
pub fn phone_dpr() -> Option<f64> {
  read_number("phone-dpr")
}

pub fn save_phone_dpr(dpr: f64) {
  let _ = std::fs::write(data_dir().join("phone-dpr"), dpr.to_string());
}

fn read_number(name: &str) -> Option<f64> {
  std::fs::read_to_string(data_dir().join(name)).ok()?.trim().parse().ok().filter(|v: &f64| *v >= 1.0 && *v <= 4.0)
}

/// Starts Umbra at `scale` physical pixels per CSS pixel (the phone's
/// density), so the window the agent captures already holds the page at
/// the phone's own resolution.
pub async fn launch(chrome: &Path, home: &str, scale: f64) -> Result<(Child, String), String> {
  let profile = profile_dir();
  std::fs::create_dir_all(&profile).map_err(|e| e.to_string())?;
  let _ = std::fs::remove_file(profile.join("DevToolsActivePort"));
  let _ = std::fs::write(data_dir().join("scale"), scale.to_string());
  let child = Command::new(chrome)
    .arg(format!("--user-data-dir={}", profile.display()))
    .arg("--remote-debugging-port=0")
    .arg("--no-first-run")
    .arg("--no-default-browser-check")
    // The window is parked off-screen (see Hub::place_window). Keep it
    // rendering anyway: by default Chromium stops painting windows it
    // considers hidden, which would freeze the phone's picture.
    .arg("--disable-features=CalculateNativeWinOcclusion")
    .arg("--disable-backgrounding-occluded-windows")
    .arg("--disable-renderer-backgrounding")
    .arg("--disable-background-timer-throttling")
    // Off-screen from the first frame; the agent sizes it to the phone.
    .arg("--window-position=-32000,-32000")
    .arg(format!("--force-device-scale-factor={scale}"))
    // Base layer for "every request looks like a phone": the agent also
    // overrides each target (with matching client hints) as it attaches,
    // but this covers anything that runs before or outside of that.
    .arg(format!("--user-agent={}", mobile_ua(&chrome_major(chrome))))
    // DevTools being open makes Chromium report navigator.webdriver = true
    // ("controlled by automation software"). A person is driving this
    // browser from their phone, so report what a normal browser does.
    .arg("--disable-blink-features=AutomationControlled")
    .arg("--pull-to-refresh=1")
    .arg(home)
    .stdin(Stdio::null())
    .stdout(Stdio::null())
    .stderr(Stdio::null())
    .spawn()
    .map_err(|e| format!("can't start {}: {e}", chrome.display()))?;
  for _ in 0..600 {
    if let Some(url) = read_port_file() {
      return Ok((child, url));
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  Err("browser started but DevTools never came up".into())
}
