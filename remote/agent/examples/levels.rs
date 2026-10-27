// Copyright 2026 The Umbra Authors.
//! Test helper: what can the PC's speakers hear right now?
//!
//!   cargo run --example levels
//!
//! Lists the mixer sessions (process, muted, level) and records 2 s of the
//! default output device's loopback, i.e. the final mix the speakers get,
//! reporting its peak. Used to check that Umbra is silent on the PC while
//! other apps are not.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use wasapi::{DeviceEnumerator, Direction, SampleType, StreamMode, WaveFormat};

fn main() {
  let _ = wasapi::initialize_mta();
  let dev = DeviceEnumerator::new().unwrap().get_default_device(&Direction::Render).unwrap();
  println!("output device: {}", dev.get_friendlyname().unwrap_or_default());

  let sessions = dev.get_iaudiosessionmanager().unwrap().get_audiosessionenumerator().unwrap();
  for i in 0..sessions.get_count().unwrap() {
    let s = sessions.get_session(i).unwrap();
    let pid = s.get_process_id().unwrap_or(0);
    let peak = s.get_audiometerinformation().and_then(|m| m.get_peak_value()).unwrap_or(-1.0);
    println!("  session pid {pid:>6}  state {:?}  meter {peak:.3}", s.get_state().ok());
  }

  mixer();

  let mut client = dev.get_iaudioclient().unwrap();
  let fmt = WaveFormat::new(32, 32, &SampleType::Float, 48000, 2, None);
  let mode = StreamMode::EventsShared { autoconvert: true, buffer_duration_hns: 0 };
  client.initialize_client(&fmt, &Direction::Capture, &mode).unwrap();
  let ev = client.set_get_eventhandle().unwrap();
  let cap = client.get_audiocaptureclient().unwrap();
  client.start_stream().unwrap();
  let mut q = VecDeque::new();
  let t = Instant::now();
  while t.elapsed() < Duration::from_secs(2) {
    let _ = ev.wait_for_event(200);
    while cap.get_next_packet_size().unwrap().unwrap_or(0) > 0 {
      cap.read_from_device_to_deque(&mut q).unwrap();
    }
  }
  let bytes: Vec<u8> = q.into_iter().collect();
  let peak = bytes
    .chunks_exact(4)
    .map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]).abs())
    .fold(0.0f32, f32::max);
  println!("speakers (loopback) peak over 2 s: {peak:.4}  ({} samples)", bytes.len() / 4);
}

/// Per-app volume and mute, as the Windows volume mixer shows them.
fn mixer() {
  use windows::Win32::Media::Audio::*;
  use windows::Win32::System::Com::{CLSCTX_ALL, CoCreateInstance};
  use windows::core::Interface;
  unsafe {
    let e: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL).unwrap();
    let dev = e.GetDefaultAudioEndpoint(eRender, eConsole).unwrap();
    let m: IAudioSessionManager2 = dev.Activate(CLSCTX_ALL, None).unwrap();
    let list = m.GetSessionEnumerator().unwrap();
    for i in 0..list.GetCount().unwrap() {
      let c = list.GetSession(i).unwrap();
      let pid = c.cast::<IAudioSessionControl2>().and_then(|c| c.GetProcessId()).unwrap_or(0);
      let v = c.cast::<ISimpleAudioVolume>().unwrap();
      println!(
        "  mixer pid {pid:>6}  volume {:.2}  muted {}",
        v.GetMasterVolume().unwrap_or(-1.0),
        v.GetMute().map(|b| b.as_bool()).unwrap_or(false)
      );
    }
  }
}
