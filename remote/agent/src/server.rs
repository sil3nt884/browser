// Copyright 2026 The Umbra Authors.
//! The phone-facing server: `GET /` is the viewer page, `GET /ws` the
//! WebSocket. Only devices on the local network (and this PC) may connect;
//! DevTools itself never leaves 127.0.0.1.

use std::net::{IpAddr, SocketAddr};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

use crate::hub::ClientMsg;
use crate::log;

const VIEWER: &str = include_str!("../viewer/index.html");

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

pub async fn serve(listener: TcpListener, hub: mpsc::UnboundedSender<ClientMsg>) {
  loop {
    let Ok((sock, peer)) = listener.accept().await else { continue };
    if !is_local(peer.ip()) {
      log!("refused {peer}: not on the local network");
      continue;
    }
    let _ = sock.set_nodelay(true);
    tokio::spawn(handle(sock, peer, hub.clone()));
  }
}

/// Loopback, private (RFC 1918 / IPv6 ULA) and link-local addresses.
pub fn is_local(ip: IpAddr) -> bool {
  match ip.to_canonical() {
    IpAddr::V4(v4) => v4.is_loopback() || v4.is_private() || v4.is_link_local(),
    IpAddr::V6(v6) => {
      v6.is_loopback() || (v6.segments()[0] & 0xfe00) == 0xfc00 || (v6.segments()[0] & 0xffc0) == 0xfe80
    }
  }
}

async fn handle(mut sock: TcpStream, peer: SocketAddr, hub: mpsc::UnboundedSender<ClientMsg>) {
  // Peek at the request head without consuming it, so a WebSocket upgrade
  // can still be handed to tungstenite intact.
  let mut buf = vec![0u8; 8192];
  let mut head = String::new();
  for _ in 0..50 {
    let Ok(n) = sock.peek(&mut buf).await else { return };
    if n == 0 {
      return;
    }
    head = String::from_utf8_lossy(&buf[..n]).into_owned();
    if head.contains("\r\n\r\n") || n == buf.len() {
      break;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  let path = head.split_whitespace().nth(1).unwrap_or("/").to_string();
  let upgrade = head.to_ascii_lowercase().contains("upgrade: websocket");

  if path.starts_with("/ws") && upgrade {
    let Ok(ws) = tokio_tungstenite::accept_async(sock).await else { return };
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let (mut sink, mut stream) = ws.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<Message>();
    let _ = hub.send(ClientMsg::Open(id, peer.ip().to_string(), tx));
    let writer = tokio::spawn(async move {
      while let Some(m) = rx.recv().await {
        if sink.send(m).await.is_err() {
          break;
        }
      }
      let _ = sink.close().await;
    });
    while let Some(Ok(m)) = stream.next().await {
      match m {
        Message::Text(t) => {
          let _ = hub.send(ClientMsg::Text(id, t.as_str().to_string()));
        }
        Message::Close(_) => break,
        _ => {}
      }
      if writer.is_finished() {
        break; // replaced by a newer phone
      }
    }
    writer.abort();
    let _ = hub.send(ClientMsg::Closed(id));
    return;
  }

  // Plain HTTP: drain the request head, answer, close.
  let _ = sock.read(&mut buf).await;
  let (status, ctype, extra, body): (&str, &str, &str, Vec<u8>) = match path.split('?').next().unwrap_or("/") {
    "/" | "/index.html" => ("200 OK", "text/html; charset=utf-8", "", VIEWER.as_bytes().to_vec()),
    // The phone app, for installing straight from the phone's browser.
    "/UmbraRemote.apk" => match apk() {
      Some(b) => (
        "200 OK",
        "application/vnd.android.package-archive",
        "Content-Disposition: attachment; filename=\"UmbraRemote.apk\"\r\n",
        b,
      ),
      None => ("404 Not Found", "text/plain", "", b"no UmbraRemote.apk next to umbra-remote.exe".to_vec()),
    },
    _ => ("404 Not Found", "text/plain", "", b"not found".to_vec()),
  };
  let head = format!(
    "HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nCache-Control: no-store\r\n\
     {extra}X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
    body.len()
  );
  let _ = sock.write_all(head.as_bytes()).await;
  let _ = sock.write_all(&body).await;
  let _ = sock.shutdown().await;
}

/// `UmbraRemote.apk` from next to this exe (deploy.ps1 puts it there).
fn apk() -> Option<Vec<u8>> {
  let dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
  std::fs::read(dir.join("UmbraRemote.apk")).ok()
}

#[cfg(test)]
mod tests {
  use super::is_local;

  #[test]
  fn local_only() {
    for ok in ["127.0.0.1", "192.168.1.20", "10.0.0.5", "172.20.1.1", "169.254.3.3", "::1", "fe80::1", "fd00::5"] {
      assert!(is_local(ok.parse().unwrap()), "{ok}");
    }
    for bad in ["8.8.8.8", "100.64.0.1", "172.32.0.1", "2001:db8::1", "::ffff:8.8.8.8"] {
      assert!(!is_local(bad.parse().unwrap()), "{bad}");
    }
  }
}
