// Copyright 2026 The Umbra Authors.
//! The phone link over UDP: remote/net/umbranet.c, in the style of Parsec
//! (video and sound never resent, control messages reliable, bit rate
//! follows the network). A UDP phone looks to the hub exactly like a
//! WebSocket one (server.rs), so the hub doesn't care which one it uses;
//! phones fall back to the WebSocket when UDP is blocked.

use std::collections::HashMap;
use std::ffi::{c_char, c_int};
use std::sync::Arc;
use std::time::{Duration, Instant};

use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

use crate::hub::ClientMsg;
use crate::log;
use crate::server;

pub const RELIABLE: i32 = 0;
pub const FRAMES: i32 = 1;
pub const SOUND: i32 = 2;

#[repr(C)]
struct RawEp {
  _private: [u8; 0],
}

#[repr(C)]
#[derive(Default)]
struct RawEvent {
  kind: c_int,
  peer: u32,
  channel: c_int,
  len: u32,
  value: u32,
}

unsafe extern "C" {
  fn un_listen(port: u16, err: *mut c_char, err_len: usize) -> *mut RawEp;
  #[allow(dead_code)]
  fn un_connect(host: *const c_char, port: u16, err: *mut c_char, err_len: usize) -> *mut RawEp;
  fn un_poll(ep: *mut RawEp, ev: *mut RawEvent, buf: *mut u8, cap: u32, timeout_ms: c_int) -> c_int;
  fn un_send(ep: *mut RawEp, peer: u32, channel: c_int, data: *const u8, len: u32) -> c_int;
  fn un_close_peer(ep: *mut RawEp, peer: u32);
  fn un_rtt_us(ep: *mut RawEp, peer: u32) -> u32;
  #[allow(dead_code)]
  fn un_set_drop(ep: *mut RawEp, per_mille: u32);
  fn un_free(ep: *mut RawEp);
}

#[derive(Debug)]
pub enum Event {
  Open { peer: u32, addr: String },
  Closed { peer: u32, why: u32 },
  Message { peer: u32, channel: i32, data: Vec<u8> },
  #[allow(dead_code)] // phones don't send pictures
  Lost { peer: u32, channel: i32, msg: u32 },
  Rate { peer: u32, bps: u32 },
}

/// One endpoint. The C side does its own locking: any thread may send, one
/// thread polls.
pub struct Link(*mut RawEp);

unsafe impl Send for Link {}
unsafe impl Sync for Link {}

fn made(ep: *mut RawEp, err: &[u8]) -> Result<Link, String> {
  if ep.is_null() {
    let end = err.iter().position(|&b| b == 0).unwrap_or(err.len());
    Err(String::from_utf8_lossy(&err[..end]).into_owned())
  } else {
    Ok(Link(ep))
  }
}

impl Link {
  pub fn listen(port: u16) -> Result<Link, String> {
    let mut err = [0u8; 128];
    made(unsafe { un_listen(port, err.as_mut_ptr() as *mut c_char, err.len()) }, &err)
  }

  #[allow(dead_code)]
  pub fn connect(host: &str, port: u16) -> Result<Link, String> {
    let host = std::ffi::CString::new(host).map_err(|e| e.to_string())?;
    let mut err = [0u8; 128];
    made(unsafe { un_connect(host.as_ptr(), port, err.as_mut_ptr() as *mut c_char, err.len()) }, &err)
  }

  /// The next event, waiting up to `timeout_ms`. `buf` must fit the
  /// biggest message expected (bigger ones are dropped).
  pub fn poll(&self, buf: &mut [u8], timeout_ms: i32) -> Option<Event> {
    loop {
      let mut ev = RawEvent::default();
      if unsafe { un_poll(self.0, &mut ev, buf.as_mut_ptr(), buf.len() as u32, timeout_ms) } == 0 {
        return None;
      }
      let data = || buf[..ev.len as usize].to_vec();
      return Some(match ev.kind {
        1 => Event::Open { peer: ev.peer, addr: String::from_utf8_lossy(&data()).into_owned() },
        2 => Event::Closed { peer: ev.peer, why: ev.value },
        3 => Event::Message { peer: ev.peer, channel: ev.channel, data: data() },
        4 => Event::Lost { peer: ev.peer, channel: ev.channel, msg: ev.value },
        5 => Event::Rate { peer: ev.peer, bps: ev.value },
        _ => continue, // too big for buf
      });
    }
  }

  /// 0 = sent; -1 = peer gone; -2 = reliable window full; -3 = too big.
  pub fn send(&self, peer: u32, channel: i32, data: &[u8]) -> i32 {
    unsafe { un_send(self.0, peer, channel, data.as_ptr(), data.len() as u32) }
  }

  pub fn close_peer(&self, peer: u32) {
    unsafe { un_close_peer(self.0, peer) }
  }

  pub fn rtt_us(&self, peer: u32) -> u32 {
    unsafe { un_rtt_us(self.0, peer) }
  }

  #[allow(dead_code)]
  pub fn set_drop(&self, per_mille: u32) {
    unsafe { un_set_drop(self.0, per_mille) }
  }
}

impl Drop for Link {
  fn drop(&mut self) {
    unsafe { un_free(self.0) }
  }
}

/// Serves phones on UDP `port` next to the WebSocket. If the port can't be
/// had, phones simply use the WebSocket.
pub fn serve(port: u16, hub: mpsc::UnboundedSender<ClientMsg>) {
  let link = match Link::listen(port) {
    Ok(l) => Arc::new(l),
    Err(e) => return log!("no UDP on port {port} ({e}); phones will use TCP"),
  };
  let spawned = std::thread::Builder::new().name("udp".into()).spawn(move || {
    let mut ids: HashMap<u32, u64> = HashMap::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
      let Some(ev) = link.poll(&mut buf, 1000) else { continue };
      match ev {
        Event::Open { peer, addr } => {
          if !addr.parse().is_ok_and(server::is_local) {
            log!("refused {addr} (UDP): not on the local network");
            link.close_peer(peer);
            continue;
          }
          let id = server::next_id();
          ids.insert(peer, id);
          let (tx, rx) = mpsc::unbounded_channel::<Message>();
          let _ = hub.send(ClientMsg::Open(id, format!("{addr} (UDP)"), tx));
          let l = link.clone();
          let _ = std::thread::Builder::new().name("udp-send".into()).spawn(move || writer(&l, peer, rx));
        }
        Event::Closed { peer, why } => {
          if let Some(id) = ids.remove(&peer) {
            if why == 0 {
              log!("phone (UDP) went quiet");
            }
            let _ = hub.send(ClientMsg::Closed(id));
          }
        }
        Event::Message { peer, channel: RELIABLE, data } => {
          if let Some(&id) = ids.get(&peer) {
            let _ = hub.send(ClientMsg::Text(id, String::from_utf8_lossy(&data).into_owned()));
          }
        }
        Event::Rate { peer, bps } => {
          if let Some(&id) = ids.get(&peer) {
            log!("network: {:.1} Mbit/s suggested, round trip {:.1} ms", bps as f64 / 1e6, link.rtt_us(peer) as f64 / 1e3);
            let _ = hub.send(ClientMsg::Rate(id, bps));
          }
        }
        _ => {}
      }
    }
  });
  if let Err(e) = spawned {
    log!("no UDP thread: {e}");
  }
}

/// Sends what the hub has for one phone, until the hub lets go of it (then
/// the phone is told goodbye once its control messages are through).
fn writer(link: &Link, peer: u32, mut rx: mpsc::UnboundedReceiver<Message>) {
  while let Some(m) = rx.blocking_recv() {
    let (channel, data): (i32, &[u8]) = match &m {
      Message::Text(t) => (RELIABLE, t.as_bytes()),
      // Kind byte 2 = sound; pictures are 1 (JPEG) and 3 (H.264).
      Message::Binary(b) if b.first() == Some(&2) => (SOUND, b),
      Message::Binary(b) => (FRAMES, b),
      Message::Close(_) => break,
      _ => continue,
    };
    let mut r = link.send(peer, channel, data);
    let since = Instant::now();
    while r == -2 && since.elapsed() < Duration::from_millis(500) {
      std::thread::sleep(Duration::from_millis(1));
      r = link.send(peer, channel, data);
    }
    if r == -1 {
      return; // gone; the poll thread tells the hub
    }
  }
  link.close_peer(peer);
}

/// `umbra-remote.exe --udp-test HOST:PORT SECS [DROP_PER_MILLE]`: plays a
/// phone against a running agent over UDP. It opens its own tab (and closes
/// it after), taps it to time tap -> picture, then runs an animation for
/// frame rate, bit rate and losses. DROP throws away that many packets per
/// thousand each way, to see recovery.
pub fn phone_test(addr: &str, secs: u64, drop: u32, hz: u32) {
  use serde_json::{Value, json};
  // "ws://HOST:PORT" runs the same test over the WebSocket, for comparison.
  let link = if let Some(a) = addr.strip_prefix("ws://") {
    match TestLink::websocket(a) {
      Ok(l) => l,
      Err(e) => return log!("udp-test: {e}"),
    }
  } else {
    let (host, port) = addr.rsplit_once(':').unwrap_or((addr, "7777"));
    match Link::connect(host, port.parse().unwrap_or(7777)) {
      Ok(l) => {
        l.set_drop(drop);
        TestLink::Udp(l)
      }
      Err(e) => return log!("udp-test: {e}"),
    }
  };
  let over = if matches!(link, TestLink::Udp(_)) { "UDP" } else { "WebSocket" };
  let start = Instant::now();
  let send = |v: Value| link.send(&v.to_string());
  let mut buf = vec![0u8; 8 << 20];
  let mut open = matches!(link, TestLink::Ws(_));
  while !open && start.elapsed() < Duration::from_secs(3) {
    open = matches!(link.poll(&mut buf, 10), Some(Event::Open { .. }));
  }
  if !open {
    return log!("udp-test: no answer from {addr}");
  }
  send(json!({ "t": "hello", "w": 412, "h": 780, "sw": 412, "sh": 915, "dpr": 2.8125, "mobile": true, "quality": 70,
               "hz": hz }));
  let tap_page = "data:text/html,<body style='margin:0;height:100vh;background:%23000' \
    ontouchstart=\"this.style.background=this.style.background=='red'?'blue':'red'\">";

  struct Count {
    frames: u32,
    bytes: usize,
    keys: u32,
    lost: u32,
    idr: u32,
  }
  let mut c = Count { frames: 0, bytes: 0, keys: 0, lost: 0, idr: 0 };
  let mut tab = String::new();
  let mut asked_key = Instant::now() - Duration::from_secs(1);
  // Handles what arrives for `d`; returns how many pictures came.
  let mut pump = |d: Duration, c: &mut Count, tab: &mut String| -> u32 {
    let until = Instant::now() + d;
    let mut pictures = 0;
    while Instant::now() < until {
      match link.poll(&mut buf, 1) {
        Some(Event::Message { channel: FRAMES, data, .. }) => {
          c.frames += 1;
          c.bytes += data.len();
          if data.windows(4).any(|w| w[..3] == [0, 0, 1] && w[3] & 0x1f == 5) {
            c.keys += 1;
          }
          pictures += 1;
          send(json!({ "t": "ack" }));
        }
        Some(Event::Lost { channel: FRAMES, .. }) => {
          c.lost += 1;
          send(json!({ "t": "ack" }));
          if asked_key.elapsed() > Duration::from_millis(150) {
            asked_key = Instant::now();
            c.idr += 1;
            send(json!({ "t": "idr" }));
          }
        }
        Some(Event::Message { channel: RELIABLE, data, .. }) => {
          let v: Value = serde_json::from_slice(&data).unwrap_or_default();
          if v["t"] == "tabs" {
            *tab = v["current"].as_str().unwrap_or_default().to_string();
          }
        }
        Some(Event::Closed { why, .. }) => {
          log!("udp-test: closed ({why})");
          return pictures;
        }
        _ => {}
      }
    }
    pictures
  };
  // A tab of our own: pages can't open data: URLs in a new tab, so open a
  // blank one and navigate it (and never touch a tab that was already open).
  send(json!({ "t": "tabs" }));
  pump(Duration::from_secs(2), &mut c, &mut tab);
  let theirs = tab.clone();
  send(json!({ "t": "new_tab" }));
  pump(Duration::from_secs(2), &mut c, &mut tab);
  send(json!({ "t": "tabs" }));
  pump(Duration::from_millis(500), &mut c, &mut tab);
  if tab.is_empty() || tab == theirs {
    return log!("udp-test: no new tab of our own; not touching yours");
  }
  send(json!({ "t": "nav", "url": tap_page }));
  pump(Duration::from_secs(2), &mut c, &mut tab);

  // Request -> reply through the agent, without Chrome: the link alone.
  let mut echo = vec![];
  let mut buf = vec![0u8; 8 << 20];
  for _ in 0..20 {
    pump(Duration::from_millis(50), &mut c, &mut tab);
    let t = Instant::now();
    send(json!({ "t": "tabs" }));
    while t.elapsed() < Duration::from_millis(500) {
      if let Some(Event::Message { channel: RELIABLE, data, .. }) = link.poll(&mut buf, 1) {
        if data.starts_with(br#"{"current""#) || data.windows(8).any(|w| w == br#""tabs""#) {
          echo.push(t.elapsed().as_secs_f64() * 1000.0);
          break;
        }
      }
    }
  }
  echo.sort_by(f64::total_cmp);
  if !echo.is_empty() {
    log!("udp-test: request -> reply over {over}: median {:.2} ms, worst {:.2}", echo[echo.len() / 2], echo[echo.len() - 1]);
  }

  // Taps: the page is still, so the first picture after a tap is the answer.
  let mut taps = vec![];
  let mut tap_bytes = vec![];
  for _ in 0..12 {
    pump(Duration::from_millis(400), &mut c, &mut tab);
    let ts = start.elapsed().as_secs_f64() * 1000.0;
    let t = Instant::now();
    send(json!({ "t": "touch", "k": "start", "p": [[206, 390, 0]], "ts": ts }));
    send(json!({ "t": "touch", "k": "end", "p": [], "ts": ts + 30.0 }));
    let until = t + Duration::from_millis(500);
    while Instant::now() < until {
      let bytes = c.bytes;
      if pump(Duration::from_millis(1), &mut c, &mut tab) > 0 {
        taps.push(t.elapsed().as_secs_f64() * 1000.0);
        tap_bytes.push(c.bytes - bytes);
        break;
      }
    }
  }
  taps.sort_by(f64::total_cmp);
  if !taps.is_empty() {
    log!(
      "udp-test: tap -> picture over {over}, {} taps: median {:.1} ms, best {:.1}, worst {:.1}; answer frames ~{} KB",
      taps.len(),
      taps[taps.len() / 2],
      taps[0],
      taps[taps.len() - 1],
      tap_bytes.iter().sum::<usize>() / tap_bytes.len().max(1) / 1000,
    );
  }

  // Animation: frame rate and bit rate.
  let anim = "data:text/html,<body style='margin:0'><div id=b style='position:absolute;top:200px;width:120px;\
    height:120px;background:%23e33'></div><p id=d style='font:30px sans-serif'></p><script>let n=0;\
    (function f(){n++;d.textContent=n;b.style.left=(n*5%300)+'px';requestAnimationFrame(f)})()</script>";
  send(json!({ "t": "nav", "url": anim }));
  pump(Duration::from_secs(2), &mut c, &mut tab);
  let before = Count { ..c };
  pump(Duration::from_secs(secs), &mut c, &mut tab);
  let s = secs as f64;
  log!(
    "udp-test: animation over {over}: {:.1} fps, {:.2} Mbit/s, {} lost, {} keyframes, {} keyframe requests (drop {drop}/1000 each way)",
    (c.frames - before.frames) as f64 / s,
    (c.bytes - before.bytes) as f64 * 8.0 / s / 1e6,
    c.lost - before.lost,
    c.keys - before.keys,
    c.idr - before.idr
  );
  if !tab.is_empty() {
    send(json!({ "t": "close_tab", "id": tab }));
  }
  pump(Duration::from_millis(500), &mut c, &mut tab);
}

/// The test phone's connection: UDP, or the WebSocket for comparison.
enum TestLink {
  Udp(Link),
  Ws(std::cell::RefCell<tokio_tungstenite::tungstenite::WebSocket<std::net::TcpStream>>),
}

impl TestLink {
  fn websocket(addr: &str) -> Result<TestLink, String> {
    let tcp = std::net::TcpStream::connect(addr).map_err(|e| e.to_string())?;
    tcp.set_nodelay(true).map_err(|e| e.to_string())?;
    let (ws, _) = tokio_tungstenite::tungstenite::client(format!("ws://{addr}/ws"), tcp).map_err(|e| e.to_string())?;
    ws.get_ref().set_read_timeout(Some(Duration::from_millis(1))).map_err(|e| e.to_string())?;
    Ok(TestLink::Ws(std::cell::RefCell::new(ws)))
  }

  fn send(&self, text: &str) {
    match self {
      TestLink::Udp(l) => drop(l.send(1, RELIABLE, text.as_bytes())),
      TestLink::Ws(ws) => drop(ws.borrow_mut().send(Message::text(text))),
    }
  }

  fn poll(&self, buf: &mut [u8], timeout_ms: i32) -> Option<Event> {
    match self {
      TestLink::Udp(l) => l.poll(buf, timeout_ms),
      TestLink::Ws(ws) => match ws.borrow_mut().read() {
        Ok(Message::Text(t)) => Some(Event::Message { peer: 1, channel: RELIABLE, data: t.as_bytes().to_vec() }),
        Ok(Message::Binary(b)) => {
          let channel = if b.first() == Some(&2) { SOUND } else { FRAMES };
          Some(Event::Message { peer: 1, channel, data: b.to_vec() })
        }
        _ => None,
      },
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn pair(port: u16) -> (Link, Link, u32) {
    let server = Link::listen(port).expect("listen");
    let client = Link::connect("127.0.0.1", port).expect("connect");
    let mut buf = vec![0u8; 1 << 20];
    let mut peer = None;
    let start = Instant::now();
    let mut client_open = false;
    while (peer.is_none() || !client_open) && start.elapsed() < Duration::from_secs(3) {
      if let Some(Event::Open { peer: p, .. }) = server.poll(&mut buf, 5) {
        peer = Some(p);
      }
      if let Some(Event::Open { .. }) = client.poll(&mut buf, 5) {
        client_open = true;
      }
    }
    (server, client, peer.expect("server saw the client"))
  }

  #[test]
  fn reliable_survives_loss() {
    let (server, client, _) = pair(47811);
    client.set_drop(100);
    server.set_drop(100);
    for i in 0..300 {
      assert_eq!(client.send(1, RELIABLE, format!("msg {i} {}", "x".repeat(i * 7)).as_bytes()), 0);
    }
    let mut buf = vec![0u8; 1 << 20];
    let mut next = 0;
    let start = Instant::now();
    while next < 300 && start.elapsed() < Duration::from_secs(10) {
      let _ = client.poll(&mut buf, 1);
      if let Some(Event::Message { channel: RELIABLE, data, .. }) = server.poll(&mut buf, 1) {
        assert_eq!(data, format!("msg {next} {}", "x".repeat(next * 7)).into_bytes(), "in order");
        next += 1;
      }
    }
    assert_eq!(next, 300, "all delivered despite 10% loss each way");
  }

  #[test]
  fn frames_arrive_whole_or_are_reported_lost() {
    let (server, client, peer) = pair(47812);
    server.set_drop(10);
    let sent = 200u32;
    let mut buf = vec![0u8; 1 << 20];
    let (mut whole, mut lost) = (0u32, 0u32);
    for i in 0..sent {
      let frame: Vec<u8> = (0..30_000u32).map(|b| (b ^ i) as u8).collect();
      assert_eq!(server.send(peer, FRAMES, &frame), 0);
      let _ = server.poll(&mut buf, 0);
      while let Some(ev) = client.poll(&mut buf, 0) {
        match ev {
          Event::Message { channel: FRAMES, data, .. } => {
            assert_eq!(data.len(), 30_000);
            let i = data[0] as u32; // b = 0, so the first byte is the frame number (mod 256)
            assert!(data.iter().enumerate().all(|(b, &v)| v == (b as u32 ^ i) as u8), "intact");
            whole += 1;
          }
          Event::Lost { channel: FRAMES, .. } => lost += 1,
          _ => {}
        }
      }
      std::thread::sleep(Duration::from_millis(2));
    }
    let start = Instant::now();
    while whole + lost < sent && start.elapsed() < Duration::from_secs(2) {
      let _ = server.poll(&mut buf, 1);
      match client.poll(&mut buf, 1) {
        Some(Event::Message { channel: FRAMES, .. }) => whole += 1,
        Some(Event::Lost { channel: FRAMES, .. }) => lost += 1,
        _ => {}
      }
    }
    eprintln!("{whole} whole, {lost} lost of {sent} (1% packet loss, 26 packets a frame)");
    assert_eq!(whole + lost, sent, "every frame accounted for");
    assert!(whole > sent / 2);
  }

  /// How long a tap takes there and back when packets go missing: a lost
  /// one should cost a few ms (fast resend), not TCP's 200+ ms.
  #[test]
  fn resends_are_quick() {
    let (server, client, peer) = pair(47814);
    client.set_drop(50);
    server.set_drop(50);
    let mut buf = vec![0u8; 1 << 20];
    let mut times = vec![];
    for i in 0..200u32 {
      let t = Instant::now();
      assert_eq!(client.send(1, RELIABLE, &i.to_le_bytes()), 0);
      // Keep the link busy, like a stream of touch moves does.
      let mut back = false;
      while !back && t.elapsed() < Duration::from_secs(2) {
        if let Some(Event::Message { channel: RELIABLE, data, .. }) = server.poll(&mut buf, 0) {
          server.send(peer, RELIABLE, &data);
        }
        if let Some(Event::Message { channel: RELIABLE, .. }) = client.poll(&mut buf, 0) {
          back = true;
        }
      }
      assert!(back);
      times.push(t.elapsed().as_micros() as u64);
    }
    times.sort();
    let (med, p95, worst) = (times[100], times[190], times[199]);
    eprintln!("round trip at 5% loss each way: median {med} us, 95% {p95} us, worst {worst} us");
    assert!(p95 < 50_000, "lost packets are resent quickly");
  }

  #[test]
  fn heavy_loss_lowers_the_rate() {
    let (server, client, peer) = pair(47813);
    server.set_drop(200);
    let mut buf = vec![0u8; 1 << 20];
    let mut rate = None;
    let start = Instant::now();
    while rate.is_none() && start.elapsed() < Duration::from_secs(5) {
      let _ = server.send(peer, FRAMES, &[7u8; 20_000]);
      for _ in 0..4 {
        if let Some(Event::Rate { bps, .. }) = server.poll(&mut buf, 1) {
          rate = Some(bps);
        }
        let _ = client.poll(&mut buf, 1);
      }
    }
    assert!(rate.is_some_and(|r| r < 100_000_000), "rate came down: {rate:?}");
  }
}
