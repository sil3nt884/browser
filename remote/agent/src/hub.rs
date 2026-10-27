// Copyright 2026 The Umbra Authors.
//! The bridge: one phone at a time, one page on screen. The picture is
//! H.264 video of Umbra's window (video.rs), or DevTools' JPEG screencast
//! where there's no hardware encoder; taps, scrolls and typing go back as
//! `Input.*`.
//!
//! Every target Umbra creates (tabs, pop-ups, cross-site iframes, workers,
//! service workers) is auto-attached while paused, given the phone's
//! identity (Android UA, client hints, platform, touch, screen) and only
//! then resumed, so even a page's first request and its embedded
//! challenge frames look like the same phone.
//!
//! Phone -> agent (JSON text):
//!   hello {w, h, sw, sh, dpr, mobile, quality}   viewport and whole screen
//!                                         in CSS px; resend on resize/rotate
//!   ack                                   frame shown, send the next one
//!   idr                                   video decoder needs a keyframe
//!   touch {k: start|move|end|cancel, p: [[x, y, id], ...], ts}  active
//!                                         points; ts = phone's event time, ms
//!   wheel {x, y, dx, dy}   text {s}   key {key}   nav {url}
//!   back | fwd | reload    dialog {accept, text}    exit_fullscreen
//!   tabs                   send the tab list   new_tab {url?}
//!   switch_tab {id}        close_tab {id}
//!   new_identity           restart Umbra (new Tor circuits), reopen the page
//!   restart_agent          restart this agent (Umbra keeps running)
//! Agent -> phone: binary, first byte = kind: 1 = one JPEG frame,
//!   2 = 20 ms of sound (48 kHz stereo s16le), 3 = one H.264 access unit
//!   (Annex B; keyframes carry SPS/PPS). JSON text =
//!   audio {rate, ch, state}  meta {url, title}  load {on}  vp {w, h}  kbd {on}
//!   video {w, h}   a new video stream of this size (px) follows
//!   fullscreen {on}  the page entered or left full screen
//!   tabs {current, list: [{id, title, url}, ...]}  oldest first; sent when
//!                    tabs come, go or change, and when asked
//!   dialog {type, message, value}
//!   bye (another device took over; don't reconnect on your own)

use std::collections::HashMap;
use std::process::Child;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use base64::Engine;
use serde_json::{Value, json};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

use crate::audio;
use crate::browser;
use crate::cdp::Cdp;
use crate::log;
use crate::video;
use crate::window;

pub enum Cmd {
  ShowWindow(bool),
  Restart,
  Quit,
}

pub enum ClientMsg {
  Open(u64, String, mpsc::UnboundedSender<Message>),
  Text(u64, String),
  Closed(u64),
}

#[derive(Default, Clone)]
pub struct Status {
  pub browser: String,
  pub phone: Option<String>,
  pub title: String,
  pub sound: audio::SoundState,
}

pub type SharedStatus = Arc<Mutex<Status>>;

/// What `run` ended for.
pub enum Ended {
  Quit,
  /// The phone asked for a fresh agent; Umbra is left running for it.
  RestartAgent,
}

struct Client {
  id: u64,
  tx: mpsc::UnboundedSender<Message>,
}

#[derive(Clone, Copy, PartialEq)]
struct View {
  w: f64,
  h: f64,
  /// The phone's whole screen (>= the page's viewport), for `screen.*`.
  sw: f64,
  sh: f64,
  dpr: f64,
  mobile: bool,
  quality: i64,
}

/// Until a phone says otherwise: a typical 6.3" Android phone (Pixel 8).
const DEFAULT_VIEW: View = View { w: 412.0, h: 780.0, sw: 412.0, sh: 915.0, dpr: 2.625, mobile: true, quality: 70 };

enum Exit {
  Quit,
  Restart,
  RestartAgent,
  Gone,
}

struct Hub {
  cdp: Option<Arc<Cdp>>,
  /// The tab on screen, and its session.
  target: Option<String>,
  session: Option<String>,
  /// Every attached target: target id -> (session id, target type).
  sessions: HashMap<String, (String, String)>,
  /// Open tabs, oldest first, with the tab that opened each one, and each
  /// one's (title, url).
  pages: Vec<(String, Option<String>)>,
  tab_info: HashMap<String, (String, String)>,
  /// Show the next tab that opens (the phone asked for a new tab).
  open_next: bool,
  home: String,
  client: Option<Client>,
  /// The phone's screen (or DEFAULT_VIEW), applied to every tab.
  view: View,
  /// Whether `view` came from a connected phone (only then do we stream).
  phone_view: bool,
  chrome_version: String,
  /// Frames sent to the phone and not yet acked by it; and the Chrome ack
  /// held back while that is at FRAMES_IN_FLIGHT.
  in_flight: u32,
  pending_ack: Option<(String, i64)>,
  last_vp: (i64, i64),
  last_url: String,
  shown: bool,
  exit: Option<Exit>,
  status: SharedStatus,
  audio_device: Option<String>,
  /// Umbra's browser process, and the physical pixels per CSS pixel it
  /// was started with (`browser::launch`).
  pid: Option<u32>,
  scale: f64,
  /// The window of the tab on screen, and where it belongs (x, y, w, h).
  hwnd: Option<isize>,
  place: (i32, i32, i32, i32),
  /// Whether the page on screen is full screen, as last told to the phone
  /// (shared with the check that runs after taps).
  fullscreen: Arc<std::sync::atomic::AtomicBool>,
  /// The video stream, while a phone watches (None: JPEG screencast).
  video: Option<video::Video>,
  /// Each stream's frames carry its number; frames of a stopped one that
  /// are still queued are dropped.
  video_gen: u64,
  video_tx: Option<mpsc::UnboundedSender<(u64, Vec<u8>)>>,
  video_in_flight: Arc<AtomicU32>,
  /// No hardware encoder or capture: don't try again until restarted.
  video_broken: bool,
  /// Restarted Umbra once to match the phone's density (don't loop).
  rescaled: bool,
  /// Phone event time -> epoch seconds (smallest delay seen), and the last
  /// timestamp given to Chrome.
  touch_clock: Option<f64>,
  last_touch_ts: f64,
}

pub async fn run(
  chrome: std::path::PathBuf,
  home: String,
  audio_device: Option<String>,
  mut clients: mpsc::UnboundedReceiver<ClientMsg>,
  mut cmds: mpsc::UnboundedReceiver<Cmd>,
  status: SharedStatus,
) -> Ended {
  let mut hub = Hub {
    cdp: None,
    target: None,
    session: None,
    sessions: HashMap::new(),
    pages: vec![],
    tab_info: HashMap::new(),
    open_next: false,
    home: home.clone(),
    client: None,
    view: DEFAULT_VIEW,
    phone_view: false,
    chrome_version: String::new(),
    in_flight: 0,
    pending_ack: None,
    last_vp: (0, 0),
    last_url: String::new(),
    shown: false,
    exit: None,
    status,
    audio_device,
    pid: None,
    scale: 1.0,
    hwnd: None,
    place: (0, 0, 0, 0),
    fullscreen: Arc::new(std::sync::atomic::AtomicBool::new(false)),
    video: None,
    video_gen: 0,
    video_tx: None,
    video_in_flight: Arc::new(AtomicU32::new(0)),
    video_broken: false,
    rescaled: false,
    touch_clock: None,
    last_touch_ts: 0.0,
  };
  let mut start_url = home.clone();
  loop {
    hub.set_browser("starting Umbra");
    let mut child: Option<Child> = None;
    let mut conn = None;
    if let Some(url) = browser::existing() {
      conn = Cdp::connect(&url).await.ok();
      if conn.is_some() {
        log!("reusing the running Umbra");
        hub.scale = browser::running_scale();
      }
    }
    if conn.is_none() {
      let scale = browser::phone_dpr().unwrap_or(DEFAULT_VIEW.dpr);
      match browser::launch(&chrome, &start_url, scale).await {
        Ok((c, url)) => {
          hub.scale = scale;
          child = Some(c);
          conn = Cdp::connect(&url).await.map_err(|e| log!("devtools: {e}")).ok();
        }
        Err(e) => log!("{e}"),
      }
    }
    let Some((cdp, events)) = conn else {
      hub.set_browser("can't start Umbra (see agent.log)");
      if wait_or_quit(&mut cmds, Duration::from_secs(10)).await {
        return Ended::Quit;
      }
      continue;
    };
    hub.set_browser("running");
    let exit = hub.serve(cdp.clone(), events, &mut clients, &mut cmds).await;
    hub.cdp = None;
    hub.video = None;
    hub.hwnd = None;
    match exit {
      Exit::Gone => {
        log!("Umbra exited; restarting it");
        start_url = home.clone();
        tokio::time::sleep(Duration::from_secs(1)).await;
      }
      Exit::RestartAgent => {
        log!("restarting the agent (phone asked)");
        return Ended::RestartAgent;
      }
      Exit::Restart | Exit::Quit => {
        let _ = cdp.call("Browser.close", json!({}), None).await;
        wait_for_exit(child).await;
        if matches!(exit, Exit::Quit) {
          return Ended::Quit;
        }
        // New Tor circuits come with the new browser; carry on where we were.
        log!("restarting Umbra");
        start_url = if hub.last_url.starts_with("http") { hub.last_url.clone() } else { home.clone() };
      }
    }
  }
}

async fn wait_for_exit(child: Option<Child>) {
  if let Some(mut c) = child {
    for _ in 0..50 {
      if matches!(c.try_wait(), Ok(Some(_))) {
        return;
      }
      tokio::time::sleep(Duration::from_millis(100)).await;
    }
    let _ = c.kill();
  } else {
    // Reused browser (not our child): give it a moment to let go of the
    // profile before a new one starts.
    tokio::time::sleep(Duration::from_secs(2)).await;
  }
}

/// Sleeps, but returns true early if Quit arrives.
async fn wait_or_quit(cmds: &mut mpsc::UnboundedReceiver<Cmd>, d: Duration) -> bool {
  let deadline = tokio::time::sleep(d);
  tokio::pin!(deadline);
  loop {
    tokio::select! {
      _ = &mut deadline => return false,
      c = cmds.recv() => if matches!(c, None | Some(Cmd::Quit)) { return true },
    }
  }
}

impl Hub {
  fn cdp(&self) -> &Arc<Cdp> {
    self.cdp.as_ref().expect("connected")
  }

  fn set_browser(&self, s: &str) {
    self.status.lock().unwrap().browser = s.to_string();
  }

  fn to_client(&mut self, msg: Message) {
    if let Some(c) = &self.client {
      if c.tx.send(msg).is_err() {
        self.client = None;
      }
    }
  }

  fn to_client_json(&mut self, v: Value) {
    self.to_client(Message::text(v.to_string()));
  }

  async fn serve(
    &mut self,
    cdp: Arc<Cdp>,
    mut events: mpsc::UnboundedReceiver<Value>,
    clients: &mut mpsc::UnboundedReceiver<ClientMsg>,
    cmds: &mut mpsc::UnboundedReceiver<Cmd>,
  ) -> Exit {
    self.cdp = Some(cdp.clone());
    self.target = None;
    self.session = None;
    self.sessions.clear();
    self.pages.clear();
    self.tab_info.clear();
    self.pending_ack = None;
    self.in_flight = 0;
    self.exit = None;

    if let Ok(v) = cdp.call("Browser.getVersion", json!({}), None).await {
      // "Chrome/157.0.8103.15" (the UA may already be overridden; this isn't).
      self.chrome_version = v["product"].as_str().unwrap_or_default().trim_start_matches("Chrome/").to_string();
    }
    // Sound: capture Umbra's process tree for the phone, keep it off the PC.
    let (pcm_tx, mut pcm_rx) = mpsc::unbounded_channel::<Vec<u8>>();
    self.pid = match cdp.call("SystemInfo.getProcessInfo", json!({}), None).await {
      Ok(info) => info["processInfo"]
        .as_array()
        .and_then(|a| a.iter().find(|p| p["type"] == "browser"))
        .and_then(|p| p["id"].as_u64())
        .map(|pid| pid as u32),
      Err(e) => {
        log!("no browser pid, so no sound or video: {e}");
        None
      }
    };
    let _capture = self
      .pid
      .map(|pid| audio::start(pid, pcm_tx, self.status.lock().unwrap().sound.clone(), self.audio_device.clone()));
    let (video_tx, mut video_rx) = mpsc::unbounded_channel();
    self.video_tx = Some(video_tx);

    // Discovery keeps the tab list (with openers) up to date; auto-attach
    // pauses each new target until it has the phone identity.
    if let Err(e) = cdp.call("Target.setDiscoverTargets", json!({ "discover": true }), None).await {
      log!("{e}");
      return Exit::Gone;
    }
    if let Err(e) = cdp
      .call("Target.setAutoAttach", json!({ "autoAttach": true, "waitForDebuggerOnStart": true, "flatten": true }), None)
      .await
    {
      log!("{e}");
      return Exit::Gone;
    }

    loop {
      tokio::select! {
        ev = events.recv() => match ev {
          None => return Exit::Gone,
          Some(v) => self.on_event(v).await,
        },
        m = clients.recv() => match m {
          None => return Exit::Quit,
          Some(m) => self.on_client(m).await,
        },
        Some(pcm) = pcm_rx.recv() => {
          if self.client.is_some() {
            let mut msg = Vec::with_capacity(pcm.len() + 1);
            msg.push(2);
            msg.extend_from_slice(&pcm);
            self.to_client(Message::binary(msg));
          }
        },
        Some((generation, au)) = video_rx.recv() => {
          if generation == self.video_gen && self.video.is_some() {
            self.to_client(Message::binary(au));
          }
        },
        c = cmds.recv() => match c {
          None | Some(Cmd::Quit) => return Exit::Quit,
          Some(Cmd::Restart) => return Exit::Restart,
          Some(Cmd::ShowWindow(b)) => { self.shown = b; self.place_window().await; }
        },
      }
      if let Some(e) = self.exit.take() {
        return e;
      }
    }
  }

  // ---- identity -----------------------------------------------------------

  fn major(&self) -> &str {
    self.chrome_version.split('.').next().filter(|m| !m.is_empty()).unwrap_or("157")
  }

  /// UA string, navigator.platform and client hints for every target.
  /// Mobile = what Chrome on a Pixel 8 sends (the "reduced" UA string, with
  /// matching high-entropy hints); desktop = Chrome on Windows.
  fn ua_override(&self) -> Value {
    let major = self.major();
    let full = if self.chrome_version.is_empty() { format!("{major}.0.0.0") } else { self.chrome_version.clone() };
    let brands = json!([{ "brand": "Chromium", "version": major }, { "brand": "Not.A/Brand", "version": "99" }]);
    let full_list =
      json!([{ "brand": "Chromium", "version": full }, { "brand": "Not.A/Brand", "version": "99.0.0.0" }]);
    if self.view.mobile {
      json!({
        "userAgent": browser::mobile_ua(major),
        "platform": "Linux armv8l",
        "userAgentMetadata": {
          "brands": brands, "fullVersionList": full_list, "fullVersion": full,
          "platform": "Android", "platformVersion": "14.0.0", "architecture": "", "bitness": "",
          "model": "Pixel 8", "mobile": true, "wow64": false
        }
      })
    } else {
      json!({
        "userAgent": browser::desktop_ua(major),
        "platform": "Win32",
        "userAgentMetadata": {
          "brands": brands, "fullVersionList": full_list, "fullVersion": full,
          "platform": "Windows", "platformVersion": "19.0.0", "architecture": "x86", "bitness": "64",
          "model": "", "mobile": false, "wow64": false
        }
      })
    }
  }

  /// The phone's screen for a tab (also its orientation, so
  /// `screen.orientation` and orientation media queries follow the phone).
  async fn apply_metrics(&self, session: &str) {
    let v = self.view;
    let (w, h) = (v.w.round() as i64, v.h.round() as i64);
    let (sw, sh) = (v.sw.round() as i64, v.sh.round() as i64);
    let orientation = if sw > sh {
      json!({ "type": "landscapePrimary", "angle": 90 })
    } else {
      json!({ "type": "portraitPrimary", "angle": 0 })
    };
    // The density Umbra really renders at (normally the phone's own, see
    // on_hello): emulating another one would make Chrome scale the page
    // inside the window, and the video would be resampled.
    self.cdp().send(
      "Emulation.setDeviceMetricsOverride",
      json!({ "width": w, "height": h, "deviceScaleFactor": self.scale, "mobile": v.mobile,
              "screenWidth": sw, "screenHeight": sh, "screenOrientation": orientation }),
      Some(session),
    );
  }

  /// The page's size in the window, in physical pixels (even, for H.264).
  fn page_px(&self) -> (u32, u32) {
    let even = |css: f64| ((css * self.scale).round() as u32 + 1) & !1;
    (even(self.view.w), even(self.view.h))
  }

  /// Gives a newly attached target the phone identity, then lets it run.
  ///
  /// Everything is sent without waiting for replies: Chrome runs a session's
  /// commands in order, so the overrides still land before
  /// runIfWaitingForDebugger, and a page that spawns dozens of frames and
  /// workers no longer stalls taps and frames while each is set up.
  async fn adopt(&mut self, session: &str, info: &Value, waiting: bool) {
    let cdp = self.cdp().clone();
    let kind = info["type"].as_str().unwrap_or_default().to_string();
    let target = info["targetId"].as_str().unwrap_or_default().to_string();
    let s = Some(session);
    match kind.as_str() {
      "page" | "iframe" => {
        cdp.send("Emulation.setUserAgentOverride", self.ua_override(), s);
        if self.view.mobile {
          cdp.send("Emulation.setHardwareConcurrencyOverride", json!({ "hardwareConcurrency": 8 }), s);
        }
        cdp.send("Emulation.setTouchEmulationEnabled", json!({ "enabled": self.view.mobile, "maxTouchPoints": 5 }), s);
        // Reach this target's own cross-site frames and workers too.
        cdp.send("Target.setAutoAttach", json!({ "autoAttach": true, "waitForDebuggerOnStart": true, "flatten": true }), s);
        if kind == "page" {
          self.apply_metrics(session).await;
          cdp.send("Page.enable", json!({}), s);
        }
      }
      "worker" | "shared_worker" | "service_worker" => {
        cdp.send("Network.setUserAgentOverride", self.ua_override(), s);
        if self.view.mobile {
          cdp.send("Emulation.setHardwareConcurrencyOverride", json!({ "hardwareConcurrency": 8 }), s);
        }
      }
      _ => {}
    }
    self.sessions.insert(target.clone(), (session.to_string(), kind.clone()));
    if waiting {
      cdp.send("Runtime.runIfWaitingForDebugger", json!({}), s);
    }
    if kind != "page" || info["subtype"] == "prerender" {
      return;
    }
    let url = info["url"].as_str().unwrap_or_default();
    if !waiting && url.starts_with("http") {
      // Loaded before we got to it (the agent just started): load it again
      // as the phone.
      cdp.send("Page.reload", json!({}), s);
    }
    let opened_here = self.target.is_some() && info["openerId"].as_str() == self.target.as_deref();
    if self.target.is_none() || opened_here || self.open_next {
      self.open_next = false;
      self.show_tab(target).await;
    }
  }

  /// Puts a tab on screen (and on the phone).
  async fn show_tab(&mut self, target: String) {
    let Some((session, _)) = self.sessions.get(&target).cloned() else { return };
    if let Some(old) = self.session.take() {
      self.cdp().send("Page.stopScreencast", json!({}), Some(&old));
    }
    self.pending_ack = None;
    self.in_flight = 0;
    self.target = Some(target.clone());
    self.session = Some(session);
    // Another tab may live in another window.
    self.video = None;
    self.start_stream().await;
    if let Ok(r) = self.cdp().call("Target.getTargetInfo", json!({ "targetId": target }), None).await {
      self.on_info(&r["targetInfo"]);
    }
    self.send_tabs();
  }

  /// Keeps a tab's title and address for the phone's tab list.
  fn note_tab(&mut self, info: &Value) {
    let id = info["targetId"].as_str().unwrap_or_default().to_string();
    let entry = (info["title"].as_str().unwrap_or_default().to_string(), info["url"].as_str().unwrap_or_default().to_string());
    if self.tab_info.get(&id) != Some(&entry) {
      self.tab_info.insert(id, entry);
      self.send_tabs();
    }
  }

  fn send_tabs(&mut self) {
    let list: Vec<Value> = self
      .pages
      .iter()
      .filter_map(|(id, _)| self.tab_info.get(id).map(|(title, url)| json!({ "id": id, "title": title, "url": url })))
      .collect();
    self.to_client_json(json!({ "t": "tabs", "current": self.target, "list": list }));
  }

  /// Opens a tab and puts it on screen. DevTools can't open tabs in
  /// Umbra's always-private profile (Target.createTarget fails there), so
  /// the tab on screen opens it: window.open from an isolated world (page
  /// scripts can't see it, or replace window.open), as a user gesture so
  /// the pop-up blocker lets it through. "noopener" makes it a tab of its
  /// own rather than a pop-up tied to this page.
  async fn new_tab(&mut self, url: &str) {
    let (Some(cdp), Some(s), Some(t)) = (self.cdp.clone(), self.session.clone(), self.target.clone()) else { return };
    let world = cdp
      .call("Page.createIsolatedWorld", json!({ "frameId": t, "worldName": "umbra" }), Some(&s))
      .await
      .map(|w| w["executionContextId"].clone());
    let Ok(context) = world else { return log!("can't open a tab from this page") };
    self.open_next = true;
    let expr = format!("window.open({}, '_blank', 'noopener')", json!(to_url(url)));
    if let Err(e) = cdp
      .call("Runtime.evaluate", json!({ "expression": expr, "contextId": context, "userGesture": true }), Some(&s))
      .await
    {
      self.open_next = false;
      log!("new tab: {e}");
    }
  }

  /// (Re)starts the picture of the tab on screen at the phone's size:
  /// video if the PC can encode it, else the JPEG screencast.
  async fn start_stream(&mut self) {
    let Some(s) = self.session.clone() else { return };
    self.apply_metrics(&s).await;
    self.place_window().await;
    let cdp = self.cdp().clone();
    cdp.send("Page.stopScreencast", json!({}), Some(&s));
    self.pending_ack = None;
    self.in_flight = 0;
    if self.client.is_none() || !self.phone_view {
      self.video = None;
      return;
    }
    if self.start_video() {
      return;
    }
    let v = self.view;
    let (scale, jpeg) = stream_quality(v.quality, v.dpr);
    let _ = cdp
      .call(
        "Page.startScreencast",
        json!({ "format": "jpeg", "quality": jpeg,
                "maxWidth": (v.w * scale).round() as i64, "maxHeight": (v.h * scale).round() as i64,
                "everyNthFrame": 1 }),
        Some(&s),
      )
      .await;
  }

  /// Starts (or keeps) the video stream; false if the PC can't do video.
  fn start_video(&mut self) -> bool {
    let (Some(hwnd), Some(tx)) = (self.hwnd, self.video_tx.clone()) else { return false };
    if self.video_broken {
      return false;
    }
    let (width, height) = self.page_px();
    // Megabits per second for the phone's quality preset, at 60 fps.
    let bitrate = match self.view.quality {
      ..=50 => 5_000_000,
      51..=75 => 10_000_000,
      _ => 18_000_000,
    };
    let settings = video::Settings { width, height, bitrate, fps: 60, place: self.place };
    self.video_in_flight.store(0, Ordering::Relaxed);
    if let Some(v) = &self.video {
      if v.hwnd == hwnd && v.settings == settings {
        // Same stream: the phone restarts its decoder on a fresh keyframe.
        v.request_key();
        self.video_started(width, height);
        return true;
      }
    }
    self.video = None;
    self.video_gen += 1;
    let generation = self.video_gen;
    let started = tokio::task::block_in_place(|| {
      video::start(hwnd, settings, move |au| drop(tx.send((generation, au))), self.video_in_flight.clone())
    });
    match started {
      Ok(v) => {
        self.video = Some(v);
        self.video_started(width, height);
        true
      }
      Err(e) => {
        log!("no video, using the JPEG screencast: {e}");
        self.video_broken = true;
        false
      }
    }
  }

  /// Tells the phone a stream of this size starts (and the page's size,
  /// for mapping touches).
  fn video_started(&mut self, width: u32, height: u32) {
    self.to_client_json(json!({ "t": "video", "w": width, "h": height }));
    let vp = (self.view.w.round() as i64, self.view.h.round() as i64);
    self.last_vp = vp;
    self.to_client_json(json!({ "t": "vp", "w": vp.0, "h": vp.1 }));
  }

  /// Parks the browser window off-screen (or brings it back), with the tab
  /// on screen in front and room for the page at the phone's resolution.
  async fn place_window(&mut self) {
    let Some(t) = self.target.clone() else { return };
    let cdp = self.cdp().clone();
    let Ok(w) = cdp.call("Browser.getWindowForTarget", json!({ "targetId": t }), None).await else {
      return;
    };
    // Not when full screen: that would take the page out of it (the window
    // itself stays parked either way, see video.rs).
    if w["bounds"]["windowState"] == "minimized" || w["bounds"]["windowState"] == "maximized" {
      let _ = cdp
        .call("Browser.setWindowBounds", json!({ "windowId": w["windowId"], "bounds": { "windowState": "normal" } }), None)
        .await;
    }
    // Only the active tab of a window is drawn into it.
    let _ = cdp.call("Target.activateTarget", json!({ "targetId": t }), None).await;
    let Some(pid) = self.pid else { return };
    let windows = window::browser_windows(pid);
    let title = cdp
      .call("Target.getTargetInfo", json!({ "targetId": t }), None)
      .await
      .ok()
      .and_then(|r| r["targetInfo"]["title"].as_str().map(String::from))
      .unwrap_or_default();
    // Window titles are "<tab title> - Umbra".
    let hwnd = match windows.as_slice() {
      [] => None,
      [only] => Some(*only),
      many => many.iter().copied().find(|h| !title.is_empty() && window::title(*h).starts_with(&title)).or(Some(many[0])),
    };
    let Some(hwnd) = hwnd else { return };
    let (pw, ph) = self.page_px();
    // Above the page: tab strip, toolbar and the odd infobar.
    let chrome_ui = (240.0 * self.scale) as i32;
    let (left, top) = if self.shown { (80, 40) } else { (-32000, -32000) };
    self.place = (left, top, (pw as i32 + 64).max(800), ph as i32 + chrome_ui);
    window::place(hwnd, self.place.0, self.place.1, self.place.2, self.place.3);
    window::set_invisible(hwnd, !self.shown);
    if self.hwnd != Some(hwnd) {
      self.video = None;
    }
    self.hwnd = Some(hwnd);
  }

  fn on_info(&mut self, info: &Value) {
    let title = info["title"].as_str().unwrap_or_default().to_string();
    let url = info["url"].as_str().unwrap_or_default().to_string();
    self.status.lock().unwrap().title = title.clone();
    self.last_url = url.clone();
    self.to_client_json(json!({ "t": "meta", "url": url, "title": title }));
  }

  // ---- events -------------------------------------------------------------

  async fn on_event(&mut self, v: Value) {
    let method = v["method"].as_str().unwrap_or_default();
    let p = &v["params"];
    let session = v["sessionId"].as_str();
    let current = session.is_some() && session == self.session.as_deref();
    match method {
      "Target.attachedToTarget" => {
        let s = p["sessionId"].as_str().unwrap_or_default().to_string();
        let waiting = p["waitingForDebugger"].as_bool().unwrap_or(false);
        self.adopt(&s, &p["targetInfo"], waiting).await;
      }
      "Target.detachedFromTarget" => {
        let s = p["sessionId"].as_str().unwrap_or_default();
        self.sessions.retain(|_, (sess, _)| sess != s);
      }
      "Page.screencastFrame" if current => {
        let ack = (session.unwrap().to_string(), p["sessionId"].as_i64().unwrap_or(0));
        if self.client.is_none() {
          self.cdp().send("Page.screencastFrameAck", json!({ "sessionId": ack.1 }), Some(&ack.0));
          return;
        }
        let vp = (
          p["metadata"]["deviceWidth"].as_f64().unwrap_or(0.0).round() as i64,
          p["metadata"]["deviceHeight"].as_f64().unwrap_or(0.0).round() as i64,
        );
        if vp != self.last_vp {
          self.last_vp = vp;
          self.to_client_json(json!({ "t": "vp", "w": vp.0, "h": vp.1 }));
        }
        let Ok(jpeg) = base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap_or_default())
        else {
          return;
        };
        let mut msg = Vec::with_capacity(jpeg.len() + 1);
        msg.push(1);
        msg.extend_from_slice(&jpeg);
        self.to_client(Message::binary(msg));
        // Let Chrome render the next frame while this one is on its way,
        // instead of waiting a full Wi-Fi round trip per frame.
        self.in_flight += 1;
        if self.in_flight < FRAMES_IN_FLIGHT {
          self.cdp().send("Page.screencastFrameAck", json!({ "sessionId": ack.1 }), Some(&ack.0));
        } else {
          self.pending_ack = Some(ack);
        }
      }
      "Target.targetCreated" => {
        let info = &p["targetInfo"];
        if info["type"] == "page" && info["subtype"] != "prerender" {
          let id = info["targetId"].as_str().unwrap_or_default().to_string();
          if !self.pages.iter().any(|(t, _)| *t == id) {
            self.pages.push((id, info["openerId"].as_str().map(String::from)));
          }
          self.note_tab(info);
        }
      }
      "Target.targetDestroyed" => {
        let id = p["targetId"].as_str().unwrap_or_default();
        let opener = self.pages.iter().find(|(t, _)| t == id).and_then(|(_, o)| o.clone());
        self.pages.retain(|(t, _)| t != id);
        self.sessions.remove(id);
        if self.tab_info.remove(id).is_some() {
          self.send_tabs();
        }
        if self.target.as_deref() == Some(id) {
          self.target = None;
          self.session = None;
          // Back to the tab that opened this one, like Chrome on a phone.
          let next = opener
            .filter(|o| self.pages.iter().any(|(t, _)| t == o))
            .or_else(|| self.pages.last().map(|(t, _)| t.clone()));
          if let Some(next) = next {
            self.show_tab(next).await;
          }
        }
      }
      "Target.targetInfoChanged" => {
        let info = &p["targetInfo"];
        let id = info["targetId"].as_str().unwrap_or_default().to_string();
        if info["type"] == "page" && info["subtype"] != "prerender" && !self.pages.iter().any(|(t, _)| *t == id) {
          // A prerendered page that was just shown becomes an ordinary tab.
          self.pages.push((id.clone(), None));
        }
        if self.pages.iter().any(|(t, _)| *t == id) {
          self.note_tab(info);
        }
        if Some(id.as_str()) == self.target.as_deref() {
          self.on_info(info);
        }
      }
      "Page.frameStartedLoading" | "Page.frameStoppedLoading" if current => {
        // A page's main frame id equals its target id.
        if p["frameId"].as_str() == self.target.as_deref() {
          let on = method == "Page.frameStartedLoading";
          self.to_client_json(json!({ "t": "load", "on": on }));
        }
      }
      "Page.javascriptDialogOpening" if current => {
        if self.client.is_some() {
          self.to_client_json(json!({ "t": "dialog", "type": p["type"], "message": p["message"],
                                      "value": p["defaultPrompt"] }));
        } else {
          self.cdp().send("Page.handleJavaScriptDialog", json!({ "accept": false }), session);
        }
      }
      _ => {}
    }
  }

  async fn on_client(&mut self, m: ClientMsg) {
    match m {
      ClientMsg::Open(id, peer, tx) => {
        log!("phone connected from {peer}");
        // One viewer at a time. Tell the older one, so it stops reconnecting
        // instead of two devices taking turns kicking each other off.
        if let Some(old) = self.client.take() {
          let _ = old.tx.send(Message::text(json!({ "t": "bye" }).to_string()));
        }
        self.client = Some(Client { id, tx });
        self.last_vp = (0, 0);
        self.touch_clock = None;
        self.fullscreen.store(false, Ordering::Relaxed);
        self.status.lock().unwrap().phone = Some(peer);
      }
      ClientMsg::Closed(id) => {
        if self.client.as_ref().is_some_and(|c| c.id == id) {
          log!("phone disconnected");
          self.client = None;
          self.status.lock().unwrap().phone = None;
          if let (Some(cdp), Some(s)) = (&self.cdp, &self.session) {
            cdp.send("Page.stopScreencast", json!({}), Some(s));
          }
          self.pending_ack = None;
          self.in_flight = 0;
          // Nobody watching: stop capturing and encoding.
          self.video = None;
        }
      }
      ClientMsg::Text(id, text) => {
        if !self.client.as_ref().is_some_and(|c| c.id == id) {
          return;
        }
        let Ok(v) = serde_json::from_str::<Value>(&text) else { return };
        self.on_phone(v).await;
      }
    }
  }

  async fn on_phone(&mut self, v: Value) {
    match v["t"].as_str().unwrap_or_default() {
      "new_identity" => {
        log!("new Tor identity (phone asked)");
        self.exit = Some(Exit::Restart);
        return;
      }
      "restart_agent" => {
        self.exit = Some(Exit::RestartAgent);
        return;
      }
      "hello" => return self.on_hello(&v).await,
      _ => {}
    }
    let (Some(cdp), Some(s)) = (self.cdp.clone(), self.session.clone()) else { return };
    let s = Some(s.as_str());
    match v["t"].as_str().unwrap_or_default() {
      "ack" if self.video.is_some() => {
        let _ = self.video_in_flight.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_sub(1));
      }
      "ack" => {
        self.in_flight = self.in_flight.saturating_sub(1);
        if let Some((sess, fid)) = self.pending_ack.take() {
          cdp.send("Page.screencastFrameAck", json!({ "sessionId": fid }), Some(&sess));
        }
      }
      "idr" => {
        if let Some(video) = &self.video {
          video.request_key();
        }
      }
      "exit_fullscreen" => cdp.send(
        "Runtime.evaluate",
        json!({ "expression": "document.fullscreenElement && document.exitFullscreen()" }),
        s,
      ),
      "touch" => {
        let ts = self.touch_time(v["ts"].as_f64());
        let pts: Vec<Value> = v["p"]
          .as_array()
          .map(|a| {
            a.iter()
              .map(|p| json!({ "x": p[0], "y": p[1], "id": p[2], "radiusX": 8, "radiusY": 8, "force": 1 }))
              .collect()
          })
          .unwrap_or_default();
        let k = v["k"].as_str().unwrap_or_default();
        let touch = |ty: &str, pts: &[Value]| {
          cdp.send("Input.dispatchTouchEvent", json!({ "type": ty, "touchPoints": pts, "timestamp": ts }), s)
        };
        match (k, pts.is_empty()) {
          ("start", false) => touch("touchStart", &pts),
          ("move", false) => touch("touchMove", &pts),
          ("cancel", _) => touch("touchCancel", &[]),
          ("end", _) => {
            // CDP's touchEnd releases every point; restart any that remain.
            touch("touchEnd", &[]);
            if !pts.is_empty() {
              touch("touchStart", &pts);
            } else {
              self.check_page(true);
            }
          }
          _ => {}
        }
      }
      "wheel" => cdp.send(
        "Input.dispatchMouseEvent",
        json!({ "type": "mouseWheel", "x": v["x"], "y": v["y"], "deltaX": v["dx"], "deltaY": v["dy"] }),
        s,
      ),
      "text" => cdp.send("Input.insertText", json!({ "text": v["s"] }), s),
      "key" => {
        press_key(&cdp, s, v["key"].as_str().unwrap_or_default());
        self.check_page(false);
      }
      "nav" => {
        let url = to_url(v["url"].as_str().unwrap_or_default());
        cdp.send("Page.navigate", json!({ "url": url }), s);
      }
      "back" | "fwd" => {
        let step = if v["t"] == "back" { -1 } else { 1 };
        if let Ok(h) = cdp.call("Page.getNavigationHistory", json!({}), s).await {
          let i = h["currentIndex"].as_i64().unwrap_or(0) + step;
          if let Some(e) = h["entries"].as_array().and_then(|a| a.get(i.max(0) as usize)).filter(|_| i >= 0) {
            cdp.send("Page.navigateToHistoryEntry", json!({ "entryId": e["id"] }), s);
          } else if step < 0 && self.pages.len() > 1 {
            // Nothing left to go back to in a tab a link opened: close it
            // (targetDestroyed then switches to its opener).
            if let Some(t) = &self.target {
              cdp.send("Target.closeTarget", json!({ "targetId": t }), None);
            }
          }
        }
      }
      "reload" => cdp.send("Page.reload", json!({}), s),
      "tabs" => self.send_tabs(),
      "new_tab" => {
        let url = v["url"].as_str().filter(|u| !u.trim().is_empty()).map(String::from).unwrap_or_else(|| self.home.clone());
        self.new_tab(&url).await;
      }
      "switch_tab" => {
        if let Some(id) = v["id"].as_str().filter(|id| self.pages.iter().any(|(t, _)| t == id)) {
          if Some(id) != self.target.as_deref() {
            self.show_tab(id.to_string()).await;
          }
        }
      }
      "close_tab" => {
        if let Some(id) = v["id"].as_str().filter(|id| self.pages.iter().any(|(t, _)| t == id)).map(String::from) {
          // Closing Umbra's last tab would close Umbra: open a fresh one first.
          if self.pages.len() <= 1 {
            let home = self.home.clone();
            self.new_tab(&home).await;
          }
          cdp.send("Target.closeTarget", json!({ "targetId": id }), None);
        }
      }
      "dialog" => cdp.send(
        "Page.handleJavaScriptDialog",
        json!({ "accept": v["accept"].as_bool().unwrap_or(false), "promptText": v["text"].as_str().unwrap_or("") }),
        s,
      ),
      _ => {}
    }
  }

  async fn on_hello(&mut self, v: &Value) {
    let old = self.view;
    let w = v["w"].as_f64().unwrap_or(DEFAULT_VIEW.w).clamp(200.0, 4000.0);
    let h = v["h"].as_f64().unwrap_or(DEFAULT_VIEW.h).clamp(200.0, 4000.0);
    self.view = View {
      w,
      h,
      sw: v["sw"].as_f64().unwrap_or(w).clamp(w, 4000.0),
      sh: v["sh"].as_f64().unwrap_or(h).clamp(h, 4000.0),
      // Above 3x the picture gets heavy on Wi-Fi for little visible gain.
      dpr: v["dpr"].as_f64().unwrap_or(DEFAULT_VIEW.dpr).clamp(1.0, 3.0),
      mobile: v["mobile"].as_bool().unwrap_or(true),
      quality: v["quality"].as_i64().unwrap_or(70).clamp(20, 95),
    };
    self.phone_view = true;
    if (self.view.dpr - browser::phone_dpr().unwrap_or(0.0)).abs() > 0.001 {
      browser::save_phone_dpr(self.view.dpr);
    }
    let sound = self.status.lock().unwrap().sound.lock().unwrap().clone();
    self.to_client_json(json!({ "t": "audio", "rate": audio::RATE, "ch": audio::CHANNELS, "state": sound }));
    if self.cdp.is_none() {
      return;
    }
    if (self.view.dpr - self.scale).abs() > 0.01 && !self.video_broken && !self.rescaled {
      // Umbra's density is fixed at start: restart it once at this
      // phone's, so the video is pixel for pixel (it reopens the page).
      log!("restarting Umbra at the phone's density {} (was {})", self.view.dpr, self.scale);
      self.rescaled = true;
      self.exit = Some(Exit::Restart);
      return;
    }
    if old.mobile != self.view.mobile {
      // "Desktop site" toggled: every target changes identity, and the page
      // on screen reloads as the new one.
      let ua = self.ua_override();
      let cdp = self.cdp().clone();
      for (sess, kind) in self.sessions.values() {
        match kind.as_str() {
          "page" | "iframe" => {
            cdp.send("Emulation.setUserAgentOverride", ua.clone(), Some(sess));
            cdp.send("Emulation.setTouchEmulationEnabled", json!({ "enabled": self.view.mobile, "maxTouchPoints": 5 }), Some(sess));
          }
          "worker" | "shared_worker" | "service_worker" => cdp.send("Network.setUserAgentOverride", ua.clone(), Some(sess)),
          _ => {}
        }
      }
      for (sess, kind) in self.sessions.values() {
        if kind == "page" {
          self.apply_metrics(sess).await;
        }
      }
      if let Some(s) = &self.session {
        cdp.send("Page.reload", json!({}), Some(s));
      }
    }
    self.start_stream().await;
    if let Some(t) = self.target.clone() {
      if let Ok(r) = self.cdp().call("Target.getTargetInfo", json!({ "targetId": t }), None).await {
        self.on_info(&r["targetInfo"]);
      }
    }
  }

  /// When a touch happened, in Chrome's clock (epoch seconds). Chrome
  /// works out fling speed from these, so they must follow the finger, not
  /// the Wi-Fi: phone event times are shifted by the smallest delay seen
  /// so far (the clocks differ by an offset, the network adds jitter).
  fn touch_time(&mut self, phone_ms: Option<f64>) -> f64 {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0);
    let t = match phone_ms {
      Some(ms) => {
        let offset = now - ms / 1000.0;
        let clock = self.touch_clock.map_or(offset, |c| c.min(offset));
        self.touch_clock = Some(clock);
        ms / 1000.0 + clock
      }
      None => now,
    };
    // Never backwards (the offset can shrink mid-swipe).
    self.last_touch_ts = t.max(self.last_touch_ts + 0.000_1);
    self.last_touch_ts
  }

  /// After a tap or key: tell the phone whether a text field now has
  /// focus (so it can raise its keyboard; taps only) and whether the page
  /// went in or out of full screen (so it can hide its toolbar). While full
  /// screen, keeps checking, to notice the page leaving it on its own.
  fn check_page(&self, keyboard: bool) {
    let (Some(cdp), Some(s), Some(c)) = (self.cdp.clone(), self.session.clone(), self.client.as_ref()) else {
      return;
    };
    let tx = c.tx.clone();
    let fullscreen = self.fullscreen.clone();
    tokio::spawn(async move {
      let expr = r#"(() => { let e = document.activeElement;
        while (e && e.shadowRoot && e.shadowRoot.activeElement) e = e.shadowRoot.activeElement;
        const fs = !!document.fullscreenElement;
        if (!e) return [false, fs]; if (e.isContentEditable || e.tagName === 'TEXTAREA') return [true, fs];
        if (e.tagName !== 'INPUT') return [false, fs];
        return [!['button','checkbox','radio','submit','reset','file','image','range','color','hidden']
          .includes((e.type || 'text').toLowerCase()), fs]; })()"#;
      let mut first = true;
      loop {
        tokio::time::sleep(Duration::from_millis(if first { 150 } else { 1000 })).await;
        let Ok(r) = cdp.call("Runtime.evaluate", json!({ "expression": expr, "returnByValue": true }), Some(&s)).await
        else {
          return;
        };
        let v = &r["result"]["value"];
        if first && keyboard {
          let _ = tx.send(Message::text(json!({ "t": "kbd", "on": v[0].as_bool().unwrap_or(false) }).to_string()));
        }
        let fs = v[1].as_bool().unwrap_or(false);
        let was = fullscreen.swap(fs, Ordering::Relaxed);
        if fs != was {
          let _ = tx.send(Message::text(json!({ "t": "fullscreen", "on": fs }).to_string()));
        }
        // One check keeps watch while full screen (the one that saw it
        // start); the others only report.
        if !fs || (was && first) || tx.is_closed() {
          return;
        }
        first = false;
      }
    });
  }
}

/// Frames that may be between Chrome and the phone's screen at once, so a
/// slow Wi-Fi round trip doesn't cap the frame rate.
const FRAMES_IN_FLIGHT: u32 = 3;

/// Picture quality preset (the phone's Fastest / Balanced / Sharpest) ->
/// (max stream scale, JPEG quality). In practice Chrome captures a headed
/// window at that window's own density (1x on a 1080p monitor), whatever
/// density is emulated, so frames arrive at the page's CSS size and the
/// scale only caps that; the JPEG quality is what the presets change.
fn stream_quality(preset: i64, dpr: f64) -> (f64, i64) {
  let (scale, jpeg) = match preset {
    ..=50 => (1.0, 50),
    51..=75 => (1.5, 70),
    _ => (2.0, 85),
  };
  (f64::min(scale, dpr), jpeg)
}

fn press_key(cdp: &Cdp, s: Option<&str>, key: &str) {
  let (vk, text) = match key {
    "Enter" => (13, "\r"),
    "Backspace" => (8, ""),
    "Tab" => (9, ""),
    "Escape" => (27, ""),
    "Delete" => (46, ""),
    "ArrowLeft" => (37, ""),
    "ArrowUp" => (38, ""),
    "ArrowRight" => (39, ""),
    "ArrowDown" => (40, ""),
    _ => return,
  };
  let mut down = json!({ "type": if text.is_empty() { "rawKeyDown" } else { "keyDown" },
                         "key": key, "code": key, "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk });
  if !text.is_empty() {
    down["text"] = json!(text);
    down["unmodifiedText"] = json!(text);
  }
  cdp.send("Input.dispatchKeyEvent", down, s);
  cdp.send(
    "Input.dispatchKeyEvent",
    json!({ "type": "keyUp", "key": key, "code": key, "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk }),
    s,
  );
}

/// What a browser's address bar does: URLs go straight through, bare
/// domains get https://, anything else is a search.
fn to_url(input: &str) -> String {
  let s = input.trim();
  if s.contains("://") || ["about:", "chrome:", "data:"].iter().any(|p| s.starts_with(p)) {
    return s.to_string();
  }
  if !s.contains(' ') && s.contains('.') {
    return format!("https://{s}");
  }
  let q: String = s
    .bytes()
    .map(|b| match b {
      b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (b as char).to_string(),
      b' ' => "+".to_string(),
      _ => format!("%{b:02X}"),
    })
    .collect();
  format!("https://duckduckgo.com/?q={q}")
}

#[cfg(test)]
mod tests {
  use super::to_url;

  #[test]
  fn address_bar() {
    assert_eq!(to_url("example.com"), "https://example.com");
    assert_eq!(to_url("http://x.org/a"), "http://x.org/a");
    assert_eq!(to_url("tor browser"), "https://duckduckgo.com/?q=tor+browser");
    assert_eq!(to_url("c++"), "https://duckduckgo.com/?q=c%2B%2B");
  }
}
