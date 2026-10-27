# Architecture

```
┌──────────────────────── browser process ────────────────────────┐
│ ChromeMainDelegate::BasicStartupComplete()                      │
│   └─ umbra::OnBasicStartupComplete()          [hook: startup]   │
│        ├─ TorLibrary::Start()  ── loads umbra_tor.dll / .so     │
│        │     └─ umbra_tor_start(): bind 127.0.0.1:0 (sync),     │
│        │        spawn thread → tokio → Arti bootstrap           │
│        └─ append switches: --proxy-server=socks5://127.0.0.1:N  │
│             --proxy-bypass-list=<-loopback>                     │
│             --host-resolver-rules=MAP * ~NOTFOUND ...           │
│             --disable-quic  --force-webrtc-ip-handling-policy   │
│                                                                 │
│ RegisterProfilePrefs()  (pref_service_builder_utils.cc)         │
│   └─ umbra::OverrideProfilePrefDefaults()     [hook: prefs]     │
│        forced incognito (RAM-only cache), no search suggest,    │
│        PDFs download instead of render, printing off ...        │
└─────────────────────────────────────────────────────────────────┘
          │ network service process: plain Chromium net stack,
          │ every socket → SOCKS5 → umbra_tor → Tor network
┌──────────────────────── renderer process ───────────────────────┐
│ ChromeContentRendererClient::RenderFrameCreated()               │
│   └─ umbra::PreloadInjector::MaybeAttach()    [hook: renderer]  │
│        on DOMContentLoaded (main frame, http/s):                │
│        run preload_script.h in isolated world                   │
│        ISOLATED_WORLD_ID_UMBRA_PRELOAD  [hook: isolated-world-id]│
│          ├─ <script type=speculationrules> same-origin,         │
│          │   prerender+prefetch, eagerness "moderate"           │
│          └─ <video preload=auto>                                │
│ Blink media: buffer 60 s ahead, 200 MB cap [hook: media-buffer] │
└─────────────────────────────────────────────────────────────────┘
```

## Why these choices

**Tor in-process, via Arti, as a shared library.** Arti is the Tor Project's
Rust implementation of Tor. It cross-compiles to Windows and Android with plain
`cargo`, which keeps "one repo, two platforms" simple. C tor would need its own
OpenSSL, and that collides with Chromium's BoringSSL symbols if linked
statically. The library is loaded at runtime rather than linked, because
statically linking a second Rust std into Chromium (which has its own Rust
toolchain) causes duplicate-symbol problems. Only four `extern "C"` functions
cross the boundary (`tor_core/include/umbra_tor.h`).

**Bind first, bootstrap later.** `umbra_tor_start` binds the SOCKS port
synchronously and returns within milliseconds. The port number goes onto the
command line before Chromium creates any network context. Arti bootstraps in
the background. Its client is created "on-demand", so a stream opened before
bootstrap finishes waits for it rather than failing. The user sees a slow first
page, not an error page.

**Fail closed.** If the library is missing, the ABI version doesn't match, or
the port can't be bound, the proxy is still set, pointing at a dead loopback
port. Nothing ever goes out directly.

**Persistent guard state, RAM-only browsing state.** Tor's `state/` (guards)
and `cache/` (consensus) live in the app data dir and persist on purpose.
Rotating guards on every launch increases exposure to malicious guards, and a
cached consensus makes warm starts take seconds. Browsing state (cache, cookies,
history) lives only in memory because every window is incognito.

**Preloading via speculation rules.** Chromium already has a mature preloading
engine: prefetch, prerender, eagerness heuristics and memory limits. Rather
than patching the network stack, Umbra feeds it rules from an isolated world,
which keeps the change in `//umbra` and leaves the page's own CSP alone. Rules
are same-origin only, because a cross-site preload would contact a site the
user never chose. Links that look state-changing (logout, delete, cart…) are
excluded.

To verify on a device, open DevTools → Application → Speculative loads, or
`chrome://process-internals` (desktop).

**Video.** For plain `<video src=…>` the engine decides how far to read ahead,
so the `media-buffer` hook raises that target. Sites using Media Source
Extensions (YouTube and most large streaming sites) run their own JS player
and choose their own buffer size; the engine can't force those to buffer more.

## Roadmap (in priority order)

1. **First compile.** Expect small API drift in `tor_library.cc`
   (`OpenApkAsset` signature), `preload_injector.cc` (Blink isolated-world
   API) and `umbra_prefs.cc` (`PrefStore::GetValue`).
2. **Tor status UI.** `TorLibrary::GetStatus()` already exposes bootstrap
   progress. Surface it on the new-tab page and the toolbar.
3. **Per-site circuit isolation.** Add a hook in
   `net/socket/socks5_client_socket.cc` so Chromium sends the top-level site
   as SOCKS username/password. `tor_core` already maps distinct credentials to
   distinct circuits (`IsolationToken`).
4. **Fingerprinting defences.** Uniform window sizes, fixed timezone and
   locale, a reduced font list, canvas/WebGL readback noise.
5. **Conflux** once Arti stabilises it. Turning it on is one feature flag in
   `tor_core/Cargo.toml`.
6. **Bridges / pluggable transports** for censored networks (Arti
   `bridge-client`, `pt-client` features).
