// Copyright 2026 The Umbra Authors.
//! The tray icon and its menu, on the main thread's Win32 message loop.

use tokio::sync::mpsc::UnboundedSender;
use tray_icon::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIconBuilder};
use windows_sys::Win32::UI::WindowsAndMessaging::{
  DispatchMessageW, GetMessageW, MB_ICONWARNING, MB_OK, MSG, MessageBoxW, PostQuitMessage, SetTimer,
  TranslateMessage, WM_QUIT, WM_TIMER,
};

use crate::hub::{Cmd, SharedStatus};

fn wide(s: &str) -> Vec<u16> {
  s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn current_thread() -> u32 {
  unsafe { windows_sys::Win32::System::Threading::GetCurrentThreadId() }
}

/// Ends `run` on the tray's thread from any other thread.
pub fn quit(thread: u32) {
  unsafe { windows_sys::Win32::UI::WindowsAndMessaging::PostThreadMessageW(thread, WM_QUIT, 0, 0) };
}

pub fn message_box(text: &str) {
  let (t, c) = (wide(text), wide("Umbra Remote"));
  unsafe { MessageBoxW(std::ptr::null_mut(), t.as_ptr(), c.as_ptr(), MB_OK | MB_ICONWARNING) };
}

/// A 32x32 ring: Umbra's "shadow" mark, drawn rather than shipped as a file.
fn icon() -> Icon {
  const N: usize = 32;
  let mut px = vec![0u8; N * N * 4];
  for y in 0..N {
    for x in 0..N {
      let (dx, dy) = (x as f32 - 15.5, y as f32 - 15.5);
      let r = (dx * dx + dy * dy).sqrt();
      let i = (y * N + x) * 4;
      let (c, a) = if r < 9.0 {
        ([24, 16, 40], 255)
      } else if r < 15.0 {
        ([138, 92, 246], 255)
      } else if r < 16.0 {
        ([138, 92, 246], ((16.0 - r) * 255.0) as u8)
      } else {
        ([0, 0, 0], 0)
      };
      px[i..i + 4].copy_from_slice(&[c[0], c[1], c[2], a]);
    }
  }
  Icon::from_rgba(px, N as u32, N as u32).expect("icon")
}

pub fn run(status: SharedStatus, cmds: UnboundedSender<Cmd>, address: &str) {
  let browser = MenuItem::new("Umbra: starting", false, None);
  let phone = MenuItem::new("Phone: not connected", false, None);
  let sound = MenuItem::new("Sound: starting", false, None);
  let addr = MenuItem::new(format!("Phone app address: {address}"), false, None);
  let open = MenuItem::new("Open the viewer on this PC", true, None);
  let show = CheckMenuItem::new("Show browser window", true, false, None);
  let restart = MenuItem::new("Restart browser", true, None);
  let quit = MenuItem::new("Quit", true, None);
  let menu = Menu::new();
  let _ = menu.append_items(&[
    &browser,
    &phone,
    &sound,
    &addr,
    &PredefinedMenuItem::separator(),
    &open,
    &show,
    &restart,
    &PredefinedMenuItem::separator(),
    &quit,
  ]);
  let tray = TrayIconBuilder::new()
    .with_menu(Box::new(menu))
    .with_tooltip(format!("Umbra Remote ({address})"))
    .with_icon(icon())
    .build()
    .expect("tray icon");

  let local_url = format!("http://127.0.0.1:{}/", address.rsplit(':').next().unwrap_or("7777"));
  let mut last = None;
  unsafe { SetTimer(std::ptr::null_mut(), 0, 500, None) };
  let mut msg: MSG = unsafe { std::mem::zeroed() };
  loop {
    if unsafe { GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) } <= 0 {
      break;
    }
    unsafe {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    while let Ok(ev) = MenuEvent::receiver().try_recv() {
      if ev.id == quit.id() {
        let _ = cmds.send(Cmd::Quit);
        unsafe { PostQuitMessage(0) };
      } else if ev.id == restart.id() {
        let _ = cmds.send(Cmd::Restart);
      } else if ev.id == show.id() {
        let _ = cmds.send(Cmd::ShowWindow(show.is_checked()));
      } else if ev.id == open.id() {
        let _ = std::process::Command::new("explorer").arg(&local_url).spawn();
      }
    }
    if msg.message == WM_TIMER {
      let s = status.lock().unwrap().clone();
      let snd = s.sound.lock().unwrap().clone();
      let key = (s.browser.clone(), s.phone.clone(), s.title.clone(), snd.clone());
      if last.as_ref() != Some(&key) {
        browser.set_text(format!("Umbra: {}", s.browser));
        sound.set_text(format!("Sound: {}", if snd.is_empty() { "starting" } else { &snd }));
        phone.set_text(match &s.phone {
          Some(p) => format!("Phone: connected ({p})"),
          None => "Phone: not connected".into(),
        });
        let tip = match &s.phone {
          Some(_) if !s.title.is_empty() => format!("Umbra Remote: {}", s.title),
          Some(_) => "Umbra Remote: phone connected".into(),
          None => format!("Umbra Remote ({address})"),
        };
        let _ = tray.set_tooltip(Some(tip.chars().take(120).collect::<String>()));
        last = Some(key);
      }
    }
  }
  let _ = cmds.send(Cmd::Quit);
}
