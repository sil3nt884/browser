// Copyright 2026 The Umbra Authors.
//! Sound goes to the phone, not the PC's speakers.
//!
//! Windows applies an app's mixer volume and mute *before* process loopback
//! capture sees its audio, so muting Umbra would silence the phone too.
//! Instead Umbra's processes are pointed at a virtual audio cable
//! (VB-Audio Virtual Cable) that nobody listens to, the same per-app
//! "Output device" setting the Windows volume mixer offers, and their
//! audio is recorded there with process loopback. The system volume, the
//! default device and every other app are left alone.
//!
//! Without a cable installed, Umbra is muted on this PC instead (no sound
//! anywhere) and the tray says so.

use std::collections::{HashSet, VecDeque};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::sync::mpsc::UnboundedSender;
use wasapi::{AudioClient, DeviceEnumerator, Direction, SampleType, StreamMode, WaveFormat};
use windows::Win32::Foundation::CloseHandle;
use windows::Win32::Media::Audio::{
  DEVICE_STATE_ACTIVE, EDataFlow, ERole, IAudioSessionControl2, IAudioSessionManager2, IMMDeviceEnumerator,
  ISimpleAudioVolume, MMDeviceEnumerator, eConsole, eMultimedia, eRender,
};
use windows::Win32::System::Com::{CLSCTX_ALL, CoCreateInstance};
use windows::Win32::System::Diagnostics::ToolHelp::{
  CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPPROCESS,
};
use windows::Win32::System::WinRT::RoGetActivationFactory;
use windows::core::{HRESULT, HSTRING, IUnknown, IUnknown_Vtbl, Interface, interface};

use crate::log;

pub const RATE: u32 = 48_000;
pub const CHANNELS: u16 = 2;
/// 20 ms of 16-bit stereo per message: small enough for low latency, big
/// enough not to flood the socket with tiny frames.
const CHUNK_FRAMES: usize = (RATE / 50) as usize;

/// What the tray and the phone show about sound.
pub type SoundState = Arc<Mutex<String>>;

pub struct Capture {
  stop: Arc<AtomicBool>,
  thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for Capture {
  fn drop(&mut self) {
    self.stop.store(true, Ordering::Relaxed);
    if let Some(t) = self.thread.take() {
      let _ = t.join();
    }
  }
}

/// Keeps Umbra (browser process `pid` and its children) off the PC's
/// speakers and streams its audio as 48 kHz stereo s16le chunks. `device`
/// picks the silent output by name (default: a VB-Audio cable). Dropping
/// the handle stops the capture.
pub fn start(pid: u32, out: UnboundedSender<Vec<u8>>, state: SoundState, device: Option<String>) -> Capture {
  let stop = Arc::new(AtomicBool::new(false));
  let s = stop.clone();
  let thread = std::thread::Builder::new()
    .name("audio".into())
    .spawn(move || {
      let _ = wasapi::initialize_mta();
      let mut warned = false;
      while !s.load(Ordering::Relaxed) {
        if let Some((id, name)) = find_output(device.as_deref()) {
          log!("sending Umbra's sound to {name}");
          *state.lock().unwrap() = "phone only".into();
          if let Err(e) = capture(pid, &id, &out, &s) {
            log!("audio capture stopped: {e}");
            *state.lock().unwrap() = format!("error: {e}");
          }
          break;
        }
        // No cable (yet): keep Umbra quiet here, and look again every couple
        // of seconds so installing the cable starts phone sound without a
        // restart.
        if !warned {
          log!("no virtual audio cable found; Umbra stays muted on this PC");
          *state.lock().unwrap() = "off (install VB-Cable for phone sound)".into();
          warned = true;
        }
        for _ in 0..4 {
          if s.load(Ordering::Relaxed) {
            break;
          }
          set_mute(&tree(pid), true);
          std::thread::sleep(Duration::from_millis(500));
        }
      }
      if warned && s.load(Ordering::Relaxed) {
        set_mute(&tree(pid), false);
      }
      wasapi::deinitialize();
    })
    .ok();
  Capture { stop, thread }
}

/// The output Umbra's sound is parked on, as (MMDevice id, friendly name):
/// the one named `wanted`, else an installed VB-Audio Virtual Cable.
/// Depending on the version, the cable's stereo input is called "CABLE
/// Input" or "Speakers (VB-Audio Virtual Cable)", next to a 16-channel
/// "CABLE In 16 Ch"; the stereo one is preferred.
fn find_output(wanted: Option<&str>) -> Option<(String, String)> {
  let devices = DeviceEnumerator::new().ok()?.get_device_collection(&Direction::Render).ok()?;
  let mut best: Option<(u8, String, String)> = None;
  for dev in &devices {
    let Ok(dev) = dev else { continue };
    let name = dev.get_friendlyname().unwrap_or_default();
    let rank = match wanted {
      Some(w) if name.contains(w) => 3,
      Some(_) => 0,
      None if name.contains("CABLE Input") || name.starts_with("Speakers (VB-Audio Virtual Cable") => 3,
      None if name.contains("VB-Audio Virtual Cable") && !name.contains("16 Ch") => 2,
      None if name.contains("VB-Audio Virtual Cable") => 1,
      None => 0,
    };
    if rank > best.as_ref().map_or(0, |b| b.0) {
      if let Ok(id) = dev.get_id() {
        best = Some((rank, id, name));
      }
    }
  }
  best.map(|(_, id, name)| (id, name))
}

fn capture(pid: u32, cable: &str, out: &UnboundedSender<Vec<u8>>, stop: &AtomicBool) -> Result<(), String> {
  let policy = AudioPolicy::new().map_err(|e| format!("per-app output device unavailable: {e}"))?;
  let mut failed = HashSet::new();
  route_audio(&policy, pid, cable, &mut failed);

  let e = |e: wasapi::WasapiError| e.to_string();
  let format = WaveFormat::new(16, 16, &SampleType::Int, RATE as usize, CHANNELS as usize, None);
  let block = format.get_blockalign() as usize;
  let mut client = AudioClient::new_application_loopback_client(pid, true).map_err(e)?;
  let mode = StreamMode::EventsShared { autoconvert: true, buffer_duration_hns: 0 };
  client.initialize_client(&format, &Direction::Capture, &mode).map_err(e)?;
  let event = client.set_get_eventhandle().map_err(e)?;
  let reader = client.get_audiocaptureclient().map_err(e)?;
  client.start_stream().map_err(e)?;
  log!("capturing Umbra's audio (pid {pid} and children)");

  let mut queue: VecDeque<u8> = VecDeque::new();
  while !stop.load(Ordering::Relaxed) {
    // Chromium's audio service opens a mixer session when a page starts
    // playing (and may be restarted); catch it fast. Windows also remembers
    // the choice for this chrome.exe, so later sessions start on the cable.
    route_audio(&policy, pid, cable, &mut failed);
    // No events arrive while nothing plays; a timeout just means silence.
    if event.wait_for_event(100).is_err() {
      continue;
    }
    while reader.get_next_packet_size().map_err(e)?.unwrap_or(0) > 0 {
      reader.read_from_device_to_deque(&mut queue).map_err(e)?;
    }
    let chunk = CHUNK_FRAMES * block;
    while queue.len() >= chunk {
      let pcm: Vec<u8> = queue.drain(..chunk).collect();
      if out.send(pcm).is_err() {
        return Ok(());
      }
    }
  }
  let _ = client.stop_stream();
  Ok(())
}

/// Moves every Umbra process that plays sound onto the cable. Windows only
/// accepts this for processes that own a mixer session.
fn route_audio(policy: &AudioPolicy, pid: u32, cable: &str, failed: &mut HashSet<u32>) {
  let mut playing = HashSet::new();
  for_sessions(&tree(pid), |p, volume| {
    playing.insert(p);
    // A mute left over from running without a cable would silence the phone.
    unsafe {
      if volume.GetMute().is_ok_and(|m| m.as_bool()) {
        let _ = volume.SetMute(false, std::ptr::null());
      }
    }
  });
  for p in playing {
    if let Err(e) = policy.route(p, Some(cable)) {
      if failed.insert(p) {
        log!("routing pid {p}: {e}");
      }
    }
  }
}

// ---- per-app output device ----------------------------------------------
//
// Windows.Media.Internal.AudioPolicyConfig is what Settings > Sound > Volume
// mixer > "Output device" uses. It isn't in the SDK; this layout (Windows 10
// 21H2 and later, including Windows 11) matches what EarTrumpet and
// SoundSwitch use. The first 22 slots are IInspectable's 3 methods and 19
// we never call.

#[interface("ab3d4648-e242-459f-b02f-541c70306324")]
unsafe trait IAudioPolicyConfig: IUnknown {
  fn _inspectable0(&self) -> HRESULT;
  fn _inspectable1(&self) -> HRESULT;
  fn _inspectable2(&self) -> HRESULT;
  fn _unused0(&self) -> HRESULT;
  fn _unused1(&self) -> HRESULT;
  fn _unused2(&self) -> HRESULT;
  fn _unused3(&self) -> HRESULT;
  fn _unused4(&self) -> HRESULT;
  fn _unused5(&self) -> HRESULT;
  fn _unused6(&self) -> HRESULT;
  fn _unused7(&self) -> HRESULT;
  fn _unused8(&self) -> HRESULT;
  fn _unused9(&self) -> HRESULT;
  fn _unused10(&self) -> HRESULT;
  fn _unused11(&self) -> HRESULT;
  fn _unused12(&self) -> HRESULT;
  fn _unused13(&self) -> HRESULT;
  fn _unused14(&self) -> HRESULT;
  fn _unused15(&self) -> HRESULT;
  fn _unused16(&self) -> HRESULT;
  fn _unused17(&self) -> HRESULT;
  fn _unused18(&self) -> HRESULT;
  fn set_persisted_default_audio_endpoint(&self, pid: u32, flow: EDataFlow, role: ERole, device: *mut c_void) -> HRESULT;
  fn get_persisted_default_audio_endpoint(
    &self,
    pid: u32,
    flow: EDataFlow,
    role: ERole,
    device: *mut *mut c_void,
  ) -> HRESULT;
}

struct AudioPolicy(IAudioPolicyConfig);

impl AudioPolicy {
  fn new() -> windows::core::Result<Self> {
    let class = HSTRING::from("Windows.Media.Internal.AudioPolicyConfig");
    unsafe { RoGetActivationFactory(&class).map(Self) }
  }

  /// The endpoint string this API uses for an MMDevice id.
  fn endpoint(mmdevice_id: &str) -> String {
    format!(r"\\?\SWD#MMDEVAPI#{mmdevice_id}#{{e6327cad-dcec-4949-ae8a-991e976a79d2}}")
  }

  fn current(&self, pid: u32, role: ERole) -> String {
    let mut raw: *mut c_void = std::ptr::null_mut();
    unsafe {
      if self.0.get_persisted_default_audio_endpoint(pid, eRender, role, &mut raw).is_err() || raw.is_null() {
        return String::new();
      }
      // Takes ownership, so the string is freed.
      std::mem::transmute::<*mut c_void, HSTRING>(raw).to_string()
    }
  }

  /// Points `pid`'s default output at `device` (None = back to the
  /// system default). Only writes when something changes.
  fn route(&self, pid: u32, device: Option<&str>) -> windows::core::Result<()> {
    let want = device.map(Self::endpoint).unwrap_or_default();
    for role in [eConsole, eMultimedia] {
      if self.current(pid, role).eq_ignore_ascii_case(&want) {
        continue;
      }
      let h = HSTRING::from(want.as_str());
      let raw = if want.is_empty() { std::ptr::null_mut() } else { unsafe { std::mem::transmute_copy(&h) } };
      unsafe { self.0.set_persisted_default_audio_endpoint(pid, eRender, role, raw).ok()? };
    }
    Ok(())
  }
}

// ---- processes and mixer ------------------------------------------------

/// `root` and all of its descendants.
fn tree(root: u32) -> HashSet<u32> {
  let mut parents = vec![];
  unsafe {
    let Ok(snap) = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) else {
      return HashSet::from([root]);
    };
    let mut e = PROCESSENTRY32W { dwSize: size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
    if Process32FirstW(snap, &mut e).is_ok() {
      loop {
        parents.push((e.th32ProcessID, e.th32ParentProcessID));
        if Process32NextW(snap, &mut e).is_err() {
          break;
        }
      }
    }
    let _ = CloseHandle(snap);
  }
  let mut set = HashSet::from([root]);
  loop {
    let before = set.len();
    for (pid, parent) in &parents {
      if set.contains(parent) {
        set.insert(*pid);
      }
    }
    if set.len() == before {
      return set;
    }
  }
}

/// Mutes or unmutes the mixer sessions owned by `pids`. Nothing else in the
/// mixer is touched.
fn set_mute(pids: &HashSet<u32>, mute: bool) {
  for_sessions(pids, |_, volume| unsafe {
    if volume.GetMute().map(|m| m.as_bool()) != Ok(mute) {
      let _ = volume.SetMute(mute, std::ptr::null());
    }
  });
}

/// Calls `f` for each mixer session owned by `pids`, on every active output.
fn for_sessions(pids: &HashSet<u32>, mut f: impl FnMut(u32, &ISimpleAudioVolume)) {
  let r: windows::core::Result<()> = unsafe {
    (|| {
      let devices: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
      let list = devices.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)?;
      for i in 0..list.GetCount()? {
        let manager: IAudioSessionManager2 = list.Item(i)?.Activate(CLSCTX_ALL, None)?;
        let sessions = manager.GetSessionEnumerator()?;
        for j in 0..sessions.GetCount()? {
          let control = sessions.GetSession(j)?;
          let Ok(c2) = control.cast::<IAudioSessionControl2>() else { continue };
          let p = c2.GetProcessId().unwrap_or(0);
          if !pids.contains(&p) {
            continue;
          }
          if let Ok(volume) = control.cast::<ISimpleAudioVolume>() {
            f(p, &volume);
          }
        }
      }
      Ok(())
    })()
  };
  if let Err(err) = r {
    log!("mixer: {err}");
  }
}

/// Test hook (`--route-test <pid> <device-name-part>`): routes one process
/// tree to the named output and prints what Windows reports back.
pub fn route_test(pid: u32, name_part: &str) {
  let _ = wasapi::initialize_mta();
  let devices = DeviceEnumerator::new().unwrap().get_device_collection(&Direction::Render).unwrap();
  let target = (&devices)
    .into_iter()
    .flatten()
    .find(|d| d.get_friendlyname().unwrap_or_default().contains(name_part))
    .map(|d| d.get_id().unwrap());
  let policy = match AudioPolicy::new() {
    Ok(p) => p,
    Err(e) => return println!("AudioPolicyConfig: {e}"),
  };
  for p in tree(pid) {
    let r = policy.route(p, target.as_deref());
    println!("pid {p}: {:?} -> now {:?}", r.map_err(|e| e.to_string()), policy.current(p, eConsole));
  }
}
