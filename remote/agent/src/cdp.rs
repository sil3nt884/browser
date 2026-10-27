// Copyright 2026 The Umbra Authors.
//! Minimal Chrome DevTools Protocol client over one browser-level WebSocket.
//! Sessions are flat (`Target.attachToTarget {flatten: true}`), so every page
//! is reached through this one connection by `sessionId`.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use serde_json::{Value, json};
use tokio::sync::{mpsc, oneshot};
use tokio_tungstenite::tungstenite::Message;

type Waiters = Arc<Mutex<HashMap<u64, oneshot::Sender<Result<Value, String>>>>>;

pub struct Cdp {
  out: mpsc::UnboundedSender<String>,
  next: AtomicU64,
  waiting: Waiters,
}

impl Cdp {
  /// Connects and returns the client plus its event stream. The stream ends
  /// when the browser goes away.
  pub async fn connect(url: &str) -> Result<(Arc<Cdp>, mpsc::UnboundedReceiver<Value>), String> {
    let (ws, _) = tokio::time::timeout(Duration::from_secs(5), tokio_tungstenite::connect_async(url))
      .await
      .map_err(|_| "connect timeout".to_string())?
      .map_err(|e| e.to_string())?;
    let (mut sink, mut stream) = ws.split();
    let (out_tx, mut out_rx) = mpsc::unbounded_channel::<String>();
    let (ev_tx, ev_rx) = mpsc::unbounded_channel();
    let waiting: Waiters = Arc::default();

    tokio::spawn(async move {
      while let Some(s) = out_rx.recv().await {
        if sink.send(Message::text(s)).await.is_err() {
          break;
        }
      }
    });
    let w = waiting.clone();
    tokio::spawn(async move {
      while let Some(Ok(msg)) = stream.next().await {
        let Message::Text(t) = msg else { continue };
        let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
        if let Some(id) = v.get("id").and_then(Value::as_u64) {
          if let Some(tx) = w.lock().unwrap().remove(&id) {
            let r = match v.get("error") {
              Some(e) => Err(e.to_string()),
              None => Ok(v.get("result").cloned().unwrap_or(Value::Null)),
            };
            let _ = tx.send(r);
          }
        } else if ev_tx.send(v).is_err() {
          break;
        }
      }
      w.lock().unwrap().clear(); // fails every pending call
    });
    Ok((Arc::new(Cdp { out: out_tx, next: AtomicU64::new(1), waiting }), ev_rx))
  }

  fn encode(&self, method: &str, params: Value, session: Option<&str>) -> (u64, String) {
    let id = self.next.fetch_add(1, Ordering::Relaxed);
    let mut msg = json!({ "id": id, "method": method, "params": params });
    if let Some(s) = session {
      msg["sessionId"] = json!(s);
    }
    (id, msg.to_string())
  }

  /// Fire and forget (input events, acks): the reply is dropped.
  pub fn send(&self, method: &str, params: Value, session: Option<&str>) {
    let (_, s) = self.encode(method, params, session);
    let _ = self.out.send(s);
  }

  pub async fn call(&self, method: &str, params: Value, session: Option<&str>) -> Result<Value, String> {
    let (id, s) = self.encode(method, params, session);
    let (tx, rx) = oneshot::channel();
    self.waiting.lock().unwrap().insert(id, tx);
    self.out.send(s).map_err(|_| "cdp closed".to_string())?;
    match tokio::time::timeout(Duration::from_secs(15), rx).await {
      Ok(Ok(r)) => r.map_err(|e| format!("{method}: {e}")),
      Ok(Err(_)) => Err("cdp closed".into()),
      Err(_) => {
        self.waiting.lock().unwrap().remove(&id);
        Err(format!("{method}: timeout"))
      }
    }
  }
}
