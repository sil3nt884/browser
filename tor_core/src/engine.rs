//! The Tor engine: an Arti client plus the SOCKS5 accept loop that fronts it.

use std::collections::HashMap;
use std::future::Future;
use std::path::PathBuf;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use arti_client::config::{BoolOrAuto, TorClientConfigBuilder};
use arti_client::{
    DataStream, ErrorKind, HasKind, IntoTorAddr, IsolationToken, StreamPrefs, TorAddr, TorClient,
};
use futures::StreamExt;
use tokio::net::TcpListener;
use tokio::sync::Semaphore;
use tor_rtcompat::PreferredRuntime;

use crate::socks::{self, ConnectError, Connector, Target};

/// Lifecycle as seen from the browser.
pub const PHASE_STARTING: i32 = 0;
pub const PHASE_BOOTSTRAPPING: i32 = 1;
pub const PHASE_READY: i32 = 2;
pub const PHASE_FAILED: i32 = -1;

pub static PHASE: AtomicI32 = AtomicI32::new(PHASE_STARTING);
pub static PERCENT: AtomicI32 = AtomicI32::new(0);
pub static LAST_ERROR: Mutex<String> = Mutex::new(String::new());

/// Upper bound on simultaneous browser connections we service.
const MAX_CONCURRENT_STREAMS: usize = 1024;
/// How long a single stream may wait to open, including first bootstrap.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(120);
/// Cap on remembered isolation groups before we start a fresh table.
const MAX_ISOLATION_GROUPS: usize = 4096;

pub fn set_error(msg: impl Into<String>) {
    let msg = msg.into();
    tracing::error!("{msg}");
    if let Ok(mut e) = LAST_ERROR.lock() {
        *e = msg;
    }
}

pub struct Dirs {
    pub state: PathBuf,
    pub cache: PathBuf,
}

/// Arti-backed [`Connector`].
pub struct ArtiConnector {
    client: Arc<TorClient<PreferredRuntime>>,
    groups: Mutex<HashMap<Vec<u8>, IsolationToken>>,
}

impl ArtiConnector {
    fn token_for(&self, key: &[u8]) -> IsolationToken {
        let mut groups = self.groups.lock().expect("isolation map poisoned");
        if groups.len() >= MAX_ISOLATION_GROUPS && !groups.contains_key(key) {
            // Dropping old groups only means a later stream for that group
            // gets a *new* circuit, which is the safe direction.
            groups.clear();
        }
        *groups.entry(key.to_vec()).or_insert_with(IsolationToken::new)
    }
}

impl Connector for ArtiConnector {
    type Stream = DataStream;

    fn connect(
        &self,
        target: Target,
        isolation_key: Option<Vec<u8>>,
    ) -> impl Future<Output = Result<DataStream, ConnectError>> + Send {
        let mut prefs = StreamPrefs::new();
        prefs.connect_to_onion_services(BoolOrAuto::Explicit(true));
        if let Some(key) = isolation_key {
            prefs.set_isolation(self.token_for(&key));
        }
        let client = self.client.clone();

        async move {
            let addr: TorAddr = match target {
                Target::Domain(host, port) => (host.as_str(), port)
                    .into_tor_addr()
                    .map_err(|_| ConnectError::HostUnreachable)?,
                // An IP literal typed by the user or embedded in a page. No DNS is
                // involved, so this is safe; Arti still refuses private ranges.
                Target::Ip(sa) => {
                    TorAddr::dangerously_from(sa).map_err(|_| ConnectError::NotAllowed)?
                }
            };
            client
                .connect_with_prefs(addr, &prefs)
                .await
                .map_err(|e| map_error(e.kind()))
        }
    }
}

fn map_error(kind: ErrorKind) -> ConnectError {
    match kind {
        ErrorKind::RemoteConnectionRefused => ConnectError::Refused,
        ErrorKind::RemoteHostNotFound | ErrorKind::RemoteHostResolutionFailed => {
            ConnectError::HostUnreachable
        }
        ErrorKind::RemoteNetworkTimeout => ConnectError::TimedOut,
        ErrorKind::ForbiddenStreamTarget | ErrorKind::InvalidStreamTarget => {
            ConnectError::NotAllowed
        }
        ErrorKind::TorAccessFailed | ErrorKind::BootstrapRequired => {
            ConnectError::NetworkUnreachable
        }
        _ => ConnectError::General,
    }
}

/// Runs forever on the engine thread's tokio runtime.
pub async fn run(listener: std::net::TcpListener, dirs: Dirs) {
    let listener = match TcpListener::from_std(listener) {
        Ok(l) => l,
        Err(e) => {
            PHASE.store(PHASE_FAILED, Ordering::SeqCst);
            set_error(format!("could not adopt SOCKS listener: {e}"));
            return;
        }
    };

    let config = match TorClientConfigBuilder::from_directories(&dirs.state, &dirs.cache).build() {
        Ok(c) => c,
        Err(e) => {
            PHASE.store(PHASE_FAILED, Ordering::SeqCst);
            set_error(format!("bad Tor config: {e}"));
            return;
        }
    };

    // Unbootstrapped + on-demand: streams that arrive before bootstrap simply
    // wait for it instead of failing. The browser sees a slow first load, not
    // an error page.
    let client = match TorClient::builder().config(config).create_unbootstrapped() {
        Ok(c) => c,
        Err(e) => {
            PHASE.store(PHASE_FAILED, Ordering::SeqCst);
            set_error(format!("could not create Tor client: {e}"));
            return;
        }
    };

    PHASE.store(PHASE_BOOTSTRAPPING, Ordering::SeqCst);

    // Progress reporting for the browser UI.
    let mut events = client.bootstrap_events();
    tokio::spawn(async move {
        while let Some(status) = events.next().await {
            let pct = (status.as_frac() * 100.0).round() as i32;
            PERCENT.store(pct.clamp(0, 100), Ordering::SeqCst);
            if status.ready_for_traffic() {
                PHASE.store(PHASE_READY, Ordering::SeqCst);
            }
        }
    });

    // Bootstrap eagerly so circuits are warm before the first click.
    {
        let client = client.clone();
        tokio::spawn(async move {
            let mut backoff = Duration::from_secs(2);
            loop {
                match client.bootstrap().await {
                    Ok(()) => {
                        PHASE.store(PHASE_READY, Ordering::SeqCst);
                        PERCENT.store(100, Ordering::SeqCst);
                        tracing::info!("tor bootstrapped");
                        break;
                    }
                    Err(e) => {
                        set_error(format!("bootstrap failed, retrying: {e}"));
                        tokio::time::sleep(backoff).await;
                        backoff = (backoff * 2).min(Duration::from_secs(60));
                    }
                }
            }
        });
    }

    let connector = Arc::new(ArtiConnector { client, groups: Mutex::new(HashMap::new()) });
    let permits = Arc::new(Semaphore::new(MAX_CONCURRENT_STREAMS));

    loop {
        let (sock, peer) = match listener.accept().await {
            Ok(x) => x,
            Err(e) => {
                tracing::warn!("accept failed: {e}");
                tokio::time::sleep(Duration::from_millis(50)).await;
                continue;
            }
        };
        // Defence in depth: the listener is bound to loopback, but never serve
        // anything that isn't.
        if !peer.ip().is_loopback() {
            continue;
        }
        let _ = sock.set_nodelay(true);
        let connector = connector.clone();
        let permit = match permits.clone().acquire_owned().await {
            Ok(p) => p,
            Err(_) => break,
        };
        tokio::spawn(async move {
            if let Err(e) = socks::serve(sock, &*connector, CONNECT_TIMEOUT).await {
                tracing::debug!("socks session ended: {e}");
            }
            drop(permit);
        });
    }
}
