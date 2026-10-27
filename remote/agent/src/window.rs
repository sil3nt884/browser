// Copyright 2026 The Umbra Authors.
//! Umbra's browser windows, as Win32 sees them: finding them, sizing them
//! to the phone (larger than the monitor if need be) and locating the page
//! inside them for the video crop.

use windows::Win32::Foundation::{HWND, LPARAM, RECT};
use windows::Win32::Graphics::Dwm::{DWMWA_EXTENDED_FRAME_BOUNDS, DwmGetWindowAttribute};
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::BOOL;

fn class_of(h: HWND) -> String {
  let mut buf = [0u16; 64];
  let n = unsafe { GetClassNameW(h, &mut buf) } as usize;
  String::from_utf16_lossy(&buf[..n])
}

pub fn title(h: isize) -> String {
  let mut buf = [0u16; 512];
  let n = unsafe { GetWindowTextW(HWND(h as _), &mut buf) } as usize;
  String::from_utf16_lossy(&buf[..n])
}

/// Visible pages of the window (one per tab that is showing), largest first.
fn page_rects(h: HWND) -> Vec<RECT> {
  unsafe extern "system" fn cb(c: HWND, l: LPARAM) -> BOOL {
    let v = unsafe { &mut *(l.0 as *mut Vec<RECT>) };
    if unsafe { IsWindowVisible(c) }.as_bool() && class_of(c) == "Chrome_RenderWidgetHostHWND" {
      let mut r = RECT::default();
      if unsafe { GetWindowRect(c, &mut r) }.is_ok() {
        v.push(r);
      }
    }
    true.into()
  }
  let mut v: Vec<RECT> = vec![];
  unsafe {
    let _ = EnumChildWindows(Some(h), Some(cb), LPARAM(&mut v as *mut _ as isize));
  }
  v.sort_by_key(|r| -((r.right - r.left) as i64 * (r.bottom - r.top) as i64));
  v
}

/// Umbra's browser windows (not menus, bubbles or pop-ups without a page).
pub fn browser_windows(pid: u32) -> Vec<isize> {
  struct S(u32, Vec<isize>);
  unsafe extern "system" fn cb(h: HWND, l: LPARAM) -> BOOL {
    let s = unsafe { &mut *(l.0 as *mut S) };
    let mut p = 0;
    unsafe { GetWindowThreadProcessId(h, Some(&mut p)) };
    if p == s.0 && unsafe { IsWindowVisible(h) }.as_bool() && class_of(h) == "Chrome_WidgetWin_1" && !page_rects(h).is_empty() {
      s.1.push(h.0 as isize);
    }
    true.into()
  }
  let mut s = S(pid, vec![]);
  unsafe {
    let _ = EnumWindows(Some(cb), LPARAM(&mut s as *mut S as isize));
  }
  s.1
}

/// Moves and sizes a window in physical pixels. Windows normally caps a
/// window at the monitor's size (via WM_GETMINMAXINFO, applied while
/// handling WM_WINDOWPOSCHANGING); skipping that message lets the window be
/// as tall as a phone screen at the phone's density.
pub fn place(h: isize, x: i32, y: i32, w: i32, hgt: i32) {
  unsafe {
    let _ = SetWindowPos(HWND(h as _), None, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
  }
}

pub fn rect(h: isize) -> Option<(i32, i32, i32, i32)> {
  let mut r = RECT::default();
  unsafe { GetWindowRect(HWND(h as _), &mut r) }.ok()?;
  Some((r.left, r.top, r.right - r.left, r.bottom - r.top))
}

/// Where the page starts within the captured image of the window (which
/// Windows Graphics Capture takes at the window's visible frame bounds).
pub fn page_origin(h: isize) -> Option<(i32, i32)> {
  let hwnd = HWND(h as _);
  let mut frame = RECT::default();
  let ok = unsafe {
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &mut frame as *mut RECT as *mut _, size_of::<RECT>() as u32)
  };
  if ok.is_err() {
    unsafe { GetWindowRect(hwnd, &mut frame) }.ok()?;
  }
  let page = *page_rects(hwnd).first()?;
  Some((page.left - frame.left, page.top - frame.top))
}

/// Makes a window invisible on the PC's screen and click-through (it is
/// still drawn, and Windows Graphics Capture still sees every frame), or
/// normal again. If a page takes Umbra full screen, Chrome stretches the
/// window over the monitor; this way nothing shows there in the moment
/// before the agent puts it back.
pub fn set_invisible(h: isize, on: bool) {
  let hwnd = HWND(h as _);
  let bits = (WS_EX_LAYERED.0 | WS_EX_TRANSPARENT.0) as isize;
  unsafe {
    let ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if on {
      if ex & bits != bits {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | bits);
      }
      let _ = SetLayeredWindowAttributes(hwnd, windows::Win32::Foundation::COLORREF(0), 0, LWA_ALPHA);
    } else if ex & bits != 0 {
      SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & !bits);
    }
  }
}
