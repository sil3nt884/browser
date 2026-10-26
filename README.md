# Umbra

A Chromium-based browser for **Windows and Android** that sends everything
through Tor but is tuned to *feel* like a normal connection: Tor runs inside the
browser, circuits are warm before you click, same-site links preload on hover,
and video buffers well ahead of playback.

One repo, two platforms. Umbra sits at `src/umbra` inside a Chromium checkout
(the same model Brave uses) and touches Chromium in only ten small, labelled
places.

## Status

| Piece | State |
|---|---|
| Tor core (Rust, Arti, SOCKS5 front door, C ABI) | Builds and passes its tests |
| Hook patcher (`scripts/hooks.py`) | Passes tests on Chromium-shaped fixtures |
| Chromium-side C++ (`app/`, `browser/`, `renderer/`) | Written against Chromium's current APIs, **not yet compiled**; expect a few small API fixes on the first build |
| Bootstrap / build scripts, CI | Written, not yet run end to end |

## Quick start

**Android** (on Linux x64, ~120 GB free, 32 GB RAM recommended):
```sh
python3 scripts/bootstrap.py --target android   # fetches Chromium (hours)
python3 scripts/build.py --target android       # -> dist/Umbra-arm64.apk
adb install -r dist/Umbra-arm64.apk
```

**Windows** (on Windows 10/11 x64 with Visual Studio 2022 + Windows SDK):
```bat
python scripts\bootstrap.py --target win
python scripts\build.py --target win --installer   :: -> dist\umbra-setup-x64.exe
```

Both need Rust (`rustup`). Each platform must be built on its own host OS;
that's a Chromium constraint. CI (`.github/workflows/build.yml`) runs the fast
tests on GitHub's runners and the full builds on your own self-hosted machines.

Before a big sync, `python3 scripts/hooks.py check --src <chromium>/src`
tells you whether every hook still has its anchor.

## How it gets the speed

| Technique | Where |
|---|---|
| Tor runs in-process (Arti) and the SOCKS port is bound before networking starts, so there's no separate daemon and no "connecting…" wall | `tor_core/`, `app/umbra_startup.cc` |
| Requests made during bootstrap wait instead of erroring | `tor_core/src/engine.rs` |
| Guard and directory state persist, so warm starts take seconds | `app/umbra_startup.cc` |
| Same-origin links prerender after ~200 ms hover (desktop) or on touch-down (Android), so clicks feel instant | `renderer/preload_script.h` |
| Videos get `preload="auto"`; Blink's buffer target raised from ~10 s to 60 s ahead | `renderer/preload_script.h`, hook `media-buffer` |
| Cache lives in RAM (forced incognito): fast, and nothing hits disk | `browser/umbra_prefs.cc` |

## What it protects, and what it doesn't

**It does:**
- Send all TCP traffic through Tor. DNS is resolved by the exit relay.
- Fail closed: if Tor can't start, the proxy points at a dead port, so pages fail instead of leaking.
- Block QUIC and non-proxied WebRTC UDP.
- Keep history, cookies and cache in memory only.

**It doesn't (yet):**
- Resist fingerprinting the way Tor Browser does. Canvas, fonts, screen size, timezone and so on are stock Chromium, so sites can still tell browsers apart.
- Isolate circuits per site. The SOCKS server supports it (per-username isolation), but Chromium's SOCKS client doesn't send credentials yet. See ARCHITECTURE.md for the planned patch.
- Use Conflux (multi-path circuits). Arti only has it as an experimental feature, whereas C tor enables it by default.

Treat Umbra as "private and fast", not as protection against a serious,
targeted adversary. For that, use Tor Browser.

See `docs/ARCHITECTURE.md` and `docs/HOOKS.md`.
