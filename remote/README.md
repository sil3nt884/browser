# Umbra Remote

Use the Umbra on your PC from your Android phone. The PC runs Umbra (and so
Tor) off-screen; the phone app shows it at the phone's size and sends touches,
typing and navigation back. Sound plays on the phone, not the PC. Pages are
never loaded on the phone: only video, input and sound cross your Wi-Fi.

```
 phone (UmbraRemote.apk)  ── Wi-Fi, ws://PC:7777 ──  umbra-remote.exe (tray)
   hardware H.264 decode,                              │ DevTools (127.0.0.1 only),
   touch, keyboard, AudioTrack                         │ window capture + NVENC
                                                       ▼
                                                      Umbra (off-screen) ── Tor ── web
```

| Part | Where |
|---|---|
| PC agent: tray app, browser bridge, phone server | `agent/` (Rust) |
| Phone app: native Android, no WebView | `android/` (Java) |
| Test viewer (same protocol, in any browser) | `agent/viewer/index.html`, served at `http://PC:7777/` |

The protocol is documented at the top of `agent/src/hub.rs`.

## Set up

1. **Run the agent.** `umbra-remote.exe` lives next to `chrome.exe` in
   `C:\UmbraBrowser`. It starts Umbra off-screen and shows a ring icon in the
   tray. The tray menu shows the address phones should use (e.g.
   `192.168.0.39:7777`).
2. **Let the phone in through the firewall (once).** Windows blocks incoming
   connections on networks marked *Public*. Mark your home Wi-Fi as *Private*
   (Settings > Network & internet > Wi-Fi > your network > Network profile
   type), then allow Umbra Remote on private networks when Windows asks. If
   it doesn't ask, run this once in an administrator PowerShell:
   ```powershell
   New-NetFirewallRule -DisplayName "Umbra Remote" -Direction Inbound -Action Allow -Profile Private -Program "C:\UmbraBrowser\umbra-remote.exe" -Protocol TCP -LocalPort 7777
   ```
   The agent also refuses anything that isn't a local-network address.
3. **Sound on the phone:** install the free
   [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (run its installer as
   administrator, then reboot). The agent finds it and does the rest. Without
   it, Umbra is simply muted on the PC.
4. **Install the app:** copy `dist/UmbraRemote.apk` to the phone and open it
   (allow "install unknown apps" for your file manager when asked). Enter the
   address from the tray menu.

## In the app

- **Menu:** Forward, Desktop site, Picture quality, **Full screen** (hides the
  toolbar and system bars; swipe from an edge to see a button that brings
  them back), **Lock rotation** (otherwise the app follows the phone and the
  page re-lays itself out, even with the system's auto-rotate off), **New Tor
  identity** (restarts Umbra on the PC with new circuits, so a new exit
  address, and reopens the page), **Restart PC agent** (Umbra keeps running),
  Change PC address.
- Tapping a text field on the page brings up the keyboard; the back button
  goes back (or closes a tab a link opened).
- **Pull down** at the top of a page to reload it (Chrome's own
  pull-to-refresh, switched on with `--pull-to-refresh=1`).
- **Tabs:** the square with a number opens the tab list (tap one to switch,
  X to close, New tab). New tab is also in the menu. DevTools can't open tabs
  in Umbra's always-private profile, so the page on screen opens it with
  `window.open` from an isolated world, as a user gesture.

- When a page goes full screen (a video, a game) the app hides its toolbar
  and the system bars for as long; Back leaves full screen.

## How the picture gets to the phone

Umbra runs at the phone's own pixel density (e.g. 2.625x), in a window sized
to the phone's screen, parked off-screen and invisible. Windows Graphics
Capture takes each frame Chrome composites (at the monitor's refresh rate);
the agent crops it to the page, converts it to NV12 and encodes it with the
graphics card's H.264 encoder (NVENC, AMF or Quick Sync through Media
Foundation; low-latency, constant bit rate, Baseline, no B-frames). The
phone decodes it with its hardware decoder straight onto a SurfaceView. So:

- the page is pixel for pixel at the phone's resolution (sharp text);
- up to 60+ fps, ~1-10 Mbit/s, and a frame is never waited on: at most 4
  are in flight, past that new pictures wait on the GPU, not in the Wi-Fi;
- after the page settles, the last picture is re-encoded a few times, so
  what a fast scroll blurred sharpens within a fraction of a second;
- touches carry the phone's own timestamps, so flings have the speed of the
  finger, not of the Wi-Fi.

Measured on the PC (RTX 3060), tap to encoded frame: 16 ms median.
`python -I agent/tools/fps_test.py 8 70 2.625 30` simulates a phone (frames,
fps, Mbit/s) and saves the stream; `cargo run --release --example h264_check
-- %TEMP%\fps_test.h264 100 out.bmp` decodes a frame of it.

Windows normally caps a window at the monitor's size; the agent resizes
without the message that applies the cap, so the window can be as tall as a
phone at full density. When a page goes full screen Chrome stretches the
window over the monitor, so the window is kept transparent and click-through
(capture still sees it) and is put straight back. The phone's density is
fixed when Umbra starts, so the first time a phone with a different density
connects, Umbra restarts once (the page reopens).

Without a hardware encoder the agent falls back to DevTools' JPEG
screencast (about 30 fps, softer).

## Looking like a phone

Every target Umbra creates (tabs, pop-ups, cross-site iframes, workers and
service workers) is attached by the agent *while paused*, given one
consistent identity and only then let run, so even first requests and
embedded challenge frames agree: Chrome for Android's UA string, matching
client hints (Android 14, Pixel 8, mobile), `navigator.platform`, touch
(5 points, coarse pointer), the phone's screen size, density and
orientation, and 8 CPU cores. `--user-agent` at launch backs this up for
anything that runs outside a target. `navigator.webdriver` is false (it would
otherwise read true just because DevTools is attached).

**"Are you a robot?" pages.** Over Tor most of these come from the exit
relay's IP reputation, not the browser. *New Tor identity* gets you a
different exit in a few seconds; DuckDuckGo (the default) challenges far less
than Google. The agent does not solve or bypass challenges.

Known gap: Chrome can't emulate the screen inside cross-site iframes
(`Emulation.setDeviceMetricsOverride` is top-level only), so those frames see
the PC's real screen size.

## How sound stays off the PC

Windows applies an app's mixer volume and mute *before* per-process capture
sees its audio, so muting Umbra would silence the phone too. Instead, the
agent sets Umbra's own **Output device** (the per-app setting in Settings >
Sound > Volume mixer) to the virtual cable, which nobody listens to, and
records Umbra's process tree there with process-loopback capture. The system
volume, the default device and every other app are never touched. Windows
remembers the choice for `C:\UmbraBrowser\chrome.exe`; reset it in the volume
mixer if you ever want Umbra's sound back on the PC.

Use `--audio-device "<part of a device name>"` to park the sound on another
output instead of the cable.

## Build

- Agent (Windows): `cd agent && cargo build --release` → `target\release\umbra-remote.exe`.
- APK (WSL or Linux): `android/setup-sdk.sh` once (JDK 21, Android SDK 35,
  Gradle; no sudo; accepts the Android SDK license), then
  `android/build-apk.sh` → `dist/UmbraRemote.apk`.
- `umbra-remote.exe --video-test C:\UmbraBrowser\chrome.exe 6 out.h264`
  encodes a test page in a throwaway Umbra window and logs fps and bit rate
  (to `%LOCALAPPDATA%\UmbraRemote\agent.log`).
- `cargo run --example levels` (in `agent/`) prints the mixer sessions and
  what the PC's speakers are actually getting, handy for checking that Umbra
  is silent on the PC.

## Limits (v0.4)

- One viewer at a time. A second one takes over and the first shows "open on
  another device".
- Streaming pauses when the app is in the background (no background audio
  yet).
- Pop-ups Chrome draws as separate windows (`<select>` lists, date and
  colour pickers, autofill) are not in the picture yet.
- Quality presets set the video bit rate: 5 / 10 / 18 Mbit/s.
- Deploy a new agent build with `agent/deploy.ps1` (Umbra keeps running).
- Plain WebSocket on the LAN, no pairing (by choice). Anyone on your Wi-Fi who
  knows the port can view and drive this browser.
