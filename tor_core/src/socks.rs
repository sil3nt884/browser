//! Minimal SOCKS5 server (RFC 1928 + RFC 1929 username/password).
//!
//! Chromium talks to this front door with `--proxy-server=socks5://127.0.0.1:PORT`.
//! Chromium's SOCKS5 client always sends the *hostname* (remote DNS), so no DNS
//! lookup ever happens on the local machine.
//!
//! The protocol code is generic over [`Connector`] so it can be unit-tested
//! without a Tor network.

use std::future::Future;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::time::Duration;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

/// Where the browser wants to go.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Target {
    /// Hostname + port. Resolved by the Tor exit (or an onion service lookup).
    Domain(String, u16),
    /// Literal IP address + port.
    Ip(SocketAddr),
}

/// Why an outbound connection failed, mapped to a SOCKS5 reply code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectError {
    General,
    NotAllowed,
    NetworkUnreachable,
    HostUnreachable,
    Refused,
    TimedOut,
}

impl ConnectError {
    fn reply_code(self) -> u8 {
        match self {
            ConnectError::General => 0x01,
            ConnectError::NotAllowed => 0x02,
            ConnectError::NetworkUnreachable => 0x03,
            ConnectError::HostUnreachable => 0x04,
            ConnectError::Refused => 0x05,
            ConnectError::TimedOut => 0x06,
        }
    }
}

/// Something that can open a byte stream to a [`Target`].
///
/// `isolation_key` is the raw SOCKS username/password pair (if the client sent
/// one). Streams with different keys must never share a Tor circuit; that is
/// how per-site circuit isolation is expressed.
pub trait Connector: Send + Sync + 'static {
    type Stream: AsyncRead + AsyncWrite + Unpin + Send + 'static;

    fn connect(
        &self,
        target: Target,
        isolation_key: Option<Vec<u8>>,
    ) -> impl Future<Output = Result<Self::Stream, ConnectError>> + Send;
}

/// Protocol-level failure while talking to the browser (not the destination).
#[derive(Debug)]
pub enum SocksError {
    Io(std::io::Error),
    Protocol(&'static str),
}

impl From<std::io::Error> for SocksError {
    fn from(e: std::io::Error) -> Self {
        SocksError::Io(e)
    }
}

impl std::fmt::Display for SocksError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SocksError::Io(e) => write!(f, "io: {e}"),
            SocksError::Protocol(m) => write!(f, "socks protocol: {m}"),
        }
    }
}

const VER5: u8 = 0x05;
const METHOD_NO_AUTH: u8 = 0x00;
const METHOD_USERPASS: u8 = 0x02;
const METHOD_NONE_ACCEPTABLE: u8 = 0xFF;
const CMD_CONNECT: u8 = 0x01;
const ATYP_V4: u8 = 0x01;
const ATYP_DOMAIN: u8 = 0x03;
const ATYP_V6: u8 = 0x04;
const REPLY_CMD_NOT_SUPPORTED: u8 = 0x07;
const REPLY_ATYP_NOT_SUPPORTED: u8 = 0x08;

/// How long the browser gets to finish the SOCKS handshake.
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(30);

/// Serve one SOCKS5 client connection to completion.
///
/// `connect_timeout` bounds how long we wait for Tor to open the stream. It
/// includes any time spent waiting for the first bootstrap, so it is generous.
pub async fn serve<S, C>(mut client: S, connector: &C, connect_timeout: Duration) -> Result<(), SocksError>
where
    S: AsyncRead + AsyncWrite + Unpin,
    C: Connector,
{
    let (target, isolation_key) =
        match tokio::time::timeout(HANDSHAKE_TIMEOUT, handshake(&mut client)).await {
            Ok(Ok(Some(req))) => req,
            Ok(Ok(None)) => return Ok(()), // replied with an error already
            Ok(Err(e)) => return Err(e),
            Err(_) => return Err(SocksError::Protocol("handshake timed out")),
        };

    let result = match tokio::time::timeout(connect_timeout, connector.connect(target, isolation_key)).await {
        Ok(r) => r,
        Err(_) => Err(ConnectError::TimedOut),
    };

    match result {
        Ok(mut upstream) => {
            write_reply(&mut client, 0x00).await?;
            // Pump bytes both ways until either side closes.
            let _ = tokio::io::copy_bidirectional(&mut client, &mut upstream).await;
            Ok(())
        }
        Err(e) => {
            write_reply(&mut client, e.reply_code()).await?;
            Ok(())
        }
    }
}

/// Run the greeting, optional auth and request phases.
/// Returns `Ok(None)` when we already sent an error reply and should hang up.
async fn handshake<S>(client: &mut S) -> Result<Option<(Target, Option<Vec<u8>>)>, SocksError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    // Greeting: VER NMETHODS METHODS...
    let ver = client.read_u8().await?;
    if ver != VER5 {
        return Err(SocksError::Protocol("not SOCKS5"));
    }
    let n = client.read_u8().await? as usize;
    let mut methods = vec![0u8; n];
    client.read_exact(&mut methods).await?;

    // Prefer username/password when offered: it carries the isolation key.
    let method = if methods.contains(&METHOD_USERPASS) {
        METHOD_USERPASS
    } else if methods.contains(&METHOD_NO_AUTH) {
        METHOD_NO_AUTH
    } else {
        METHOD_NONE_ACCEPTABLE
    };
    client.write_all(&[VER5, method]).await?;
    if method == METHOD_NONE_ACCEPTABLE {
        return Ok(None);
    }

    let mut isolation_key = None;
    if method == METHOD_USERPASS {
        // RFC 1929: VER(1) ULEN UNAME PLEN PASSWD. Any credentials are accepted;
        // they only select a circuit-isolation group.
        let sub_ver = client.read_u8().await?;
        if sub_ver != 0x01 {
            return Err(SocksError::Protocol("bad auth sub-negotiation version"));
        }
        let ulen = client.read_u8().await? as usize;
        let mut uname = vec![0u8; ulen];
        client.read_exact(&mut uname).await?;
        let plen = client.read_u8().await? as usize;
        let mut passwd = vec![0u8; plen];
        client.read_exact(&mut passwd).await?;
        client.write_all(&[0x01, 0x00]).await?;

        let mut key = uname;
        key.push(0);
        key.extend_from_slice(&passwd);
        isolation_key = Some(key);
    }

    // Request: VER CMD RSV ATYP DST.ADDR DST.PORT
    let mut head = [0u8; 4];
    client.read_exact(&mut head).await?;
    if head[0] != VER5 {
        return Err(SocksError::Protocol("bad request version"));
    }
    let cmd = head[1];
    let atyp = head[3];

    let target = match atyp {
        ATYP_V4 => {
            let mut a = [0u8; 4];
            client.read_exact(&mut a).await?;
            let port = client.read_u16().await?;
            Target::Ip(SocketAddr::new(IpAddr::V4(Ipv4Addr::from(a)), port))
        }
        ATYP_V6 => {
            let mut a = [0u8; 16];
            client.read_exact(&mut a).await?;
            let port = client.read_u16().await?;
            Target::Ip(SocketAddr::new(IpAddr::V6(Ipv6Addr::from(a)), port))
        }
        ATYP_DOMAIN => {
            let len = client.read_u8().await? as usize;
            let mut name = vec![0u8; len];
            client.read_exact(&mut name).await?;
            let port = client.read_u16().await?;
            match String::from_utf8(name) {
                Ok(host) if !host.is_empty() => Target::Domain(host, port),
                _ => {
                    write_reply(client, ConnectError::HostUnreachable.reply_code()).await?;
                    return Ok(None);
                }
            }
        }
        _ => {
            write_reply(client, REPLY_ATYP_NOT_SUPPORTED).await?;
            return Ok(None);
        }
    };

    if cmd != CMD_CONNECT {
        // BIND and UDP ASSOCIATE are refused: UDP cannot go over Tor.
        write_reply(client, REPLY_CMD_NOT_SUPPORTED).await?;
        return Ok(None);
    }

    Ok(Some((target, isolation_key)))
}

async fn write_reply<S: AsyncWrite + Unpin>(client: &mut S, code: u8) -> std::io::Result<()> {
    // BND.ADDR/BND.PORT are meaningless here; report 0.0.0.0:0.
    client.write_all(&[VER5, code, 0x00, ATYP_V4, 0, 0, 0, 0, 0, 0]).await?;
    client.flush().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use tokio::io::{duplex, DuplexStream};

    /// Connector that records what it was asked for and hands back one end of
    /// an in-memory pipe whose other end echoes everything back.
    #[derive(Default)]
    struct EchoConnector {
        seen: Arc<Mutex<Vec<(Target, Option<Vec<u8>>)>>>,
        fail_with: Option<ConnectError>,
    }

    impl Connector for EchoConnector {
        type Stream = DuplexStream;
        fn connect(
            &self,
            target: Target,
            key: Option<Vec<u8>>,
        ) -> impl Future<Output = Result<DuplexStream, ConnectError>> + Send {
            self.seen.lock().unwrap().push((target, key));
            let fail = self.fail_with;
            async move {
                if let Some(e) = fail {
                    return Err(e);
                }
                let (a, mut b) = duplex(4096);
                tokio::spawn(async move {
                    let mut buf = [0u8; 1024];
                    loop {
                        match b.read(&mut buf).await {
                            Ok(0) | Err(_) => break,
                            Ok(n) => {
                                if b.write_all(&buf[..n]).await.is_err() {
                                    break;
                                }
                            }
                        }
                    }
                });
                Ok(a)
            }
        }
    }

    fn spawn_server(conn: EchoConnector) -> DuplexStream {
        let (client, server) = duplex(4096);
        tokio::spawn(async move {
            let _ = serve(server, &conn, Duration::from_secs(5)).await;
        });
        client
    }

    #[tokio::test]
    async fn connect_domain_no_auth_and_echo() {
        let conn = EchoConnector::default();
        let seen = conn.seen.clone();
        let mut c = spawn_server(conn);

        c.write_all(&[5, 1, 0]).await.unwrap();
        let mut r = [0u8; 2];
        c.read_exact(&mut r).await.unwrap();
        assert_eq!(r, [5, 0]);

        let host = b"example.onion";
        let mut req = vec![5, 1, 0, 3, host.len() as u8];
        req.extend_from_slice(host);
        req.extend_from_slice(&443u16.to_be_bytes());
        c.write_all(&req).await.unwrap();

        let mut reply = [0u8; 10];
        c.read_exact(&mut reply).await.unwrap();
        assert_eq!(reply[1], 0x00, "success reply");

        c.write_all(b"ping").await.unwrap();
        let mut back = [0u8; 4];
        c.read_exact(&mut back).await.unwrap();
        assert_eq!(&back, b"ping");

        let seen = seen.lock().unwrap();
        assert_eq!(seen[0].0, Target::Domain("example.onion".into(), 443));
        assert_eq!(seen[0].1, None);
    }

    #[tokio::test]
    async fn userpass_becomes_isolation_key() {
        let conn = EchoConnector::default();
        let seen = conn.seen.clone();
        let mut c = spawn_server(conn);

        c.write_all(&[5, 2, 0, 2]).await.unwrap();
        let mut r = [0u8; 2];
        c.read_exact(&mut r).await.unwrap();
        assert_eq!(r, [5, 2], "server picks username/password");

        c.write_all(&[1, 4]).await.unwrap();
        c.write_all(b"site").await.unwrap();
        c.write_all(&[1]).await.unwrap();
        c.write_all(b"x").await.unwrap();
        let mut a = [0u8; 2];
        c.read_exact(&mut a).await.unwrap();
        assert_eq!(a, [1, 0]);

        c.write_all(&[5, 1, 0, 1, 93, 184, 216, 34, 0, 80]).await.unwrap();
        let mut reply = [0u8; 10];
        c.read_exact(&mut reply).await.unwrap();
        assert_eq!(reply[1], 0);

        let seen = seen.lock().unwrap();
        assert_eq!(seen[0].0, Target::Ip("93.184.216.34:80".parse().unwrap()));
        assert_eq!(seen[0].1.as_deref(), Some(&b"site\0x"[..]));
    }

    #[tokio::test]
    async fn udp_associate_is_refused() {
        let mut c = spawn_server(EchoConnector::default());
        c.write_all(&[5, 1, 0]).await.unwrap();
        let mut r = [0u8; 2];
        c.read_exact(&mut r).await.unwrap();
        c.write_all(&[5, 3, 0, 1, 0, 0, 0, 0, 0, 0]).await.unwrap();
        let mut reply = [0u8; 10];
        c.read_exact(&mut reply).await.unwrap();
        assert_eq!(reply[1], REPLY_CMD_NOT_SUPPORTED);
    }

    #[tokio::test]
    async fn connect_failure_maps_to_reply_code() {
        let conn = EchoConnector { fail_with: Some(ConnectError::Refused), ..Default::default() };
        let mut c = spawn_server(conn);
        c.write_all(&[5, 1, 0]).await.unwrap();
        let mut r = [0u8; 2];
        c.read_exact(&mut r).await.unwrap();
        let mut req = vec![5, 1, 0, 3, 1, b'a'];
        req.extend_from_slice(&80u16.to_be_bytes());
        c.write_all(&req).await.unwrap();
        let mut reply = [0u8; 10];
        c.read_exact(&mut reply).await.unwrap();
        assert_eq!(reply[1], 0x05);
    }

    #[tokio::test]
    async fn no_acceptable_method() {
        let mut c = spawn_server(EchoConnector::default());
        c.write_all(&[5, 1, 0x80]).await.unwrap();
        let mut r = [0u8; 2];
        c.read_exact(&mut r).await.unwrap();
        assert_eq!(r, [5, 0xFF]);
    }
}
