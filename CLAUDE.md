# Umbra: notes for Claude Code

## Final release and cleanup handoff (2026-10-31)

Latest portable Windows app: `C:\me-tor\Chorium\Chorium-Windows-Final\Chorium.exe`.
Final Windows ZIP and Android APK in `C:\me-tor\Chorium` end in
`20261031-Final.zip` / `20261031-Final.apk`, with SHA-256 checksums.
Both packages contain the same browser.wasm; archive integrity and every
Windows ZIP file were verified against the portable folder.

JavaScript `window.open(url)`, button-triggered opens, `target="_blank"`,
and the exact W3Schools example pass four real-app tests on each platform.
The app chooses a foreground tab (and keeps empty requests blank). Native
popup hosting now installs Aura's missing WindowParentingClient; without it
Windows crashed in ParentWindowWithContext before delivering the request.
The DLL still does not decide tab/window UI policy. The URL-request contract
does not adopt the original popup WebContents or preserve its WindowProxy.
Shared checks now pass 206 assertions in 21 desktop/phone scenarios.
Address editing passed three real-app tests per platform. Earlier full-suite
results remain below; Android's separate 60 fps on a 120 Hz display remains
unresolved and must not be represented as a fully passing Android suite.

The user requested a push followed by removal of local source/build files,
Chorium containers and build caches. Cleanup must happen only after the push
is verified on origin/main. Preserve the final release directory, validation
logs and normal Windows/phone app data. Treat the historical Docker/output
paths below as disposable: removing chorium-work loses the incremental cache
and the next native build must fetch/compile Chromium again.

## Continuation (2026-10-31)

Chorium's previously failed Docker Windows build now succeeds, including
`Chorium.exe` linked to Chromium's sandbox. Portable output:
`C:\me-tor\chorium-out\Chorium-Windows`. The real Windows e2e suite passes
22/22 (with `--network`); the app suite passes 200 checks in 21 desktop/phone
scenarios. Hook tests: 7 pass. Page frame rate: 144 fps on the 144 Hz display.
Renderer audit: integrity RID 0, in a job, win32k system calls disabled.

Cookies and site storage now persist. Page HTTP cache stays in RAM. Windows
cookie values use Chromium OSCrypt with a DPAPI-protected key. Settings has
separate history/site-data clearing and an off-by-default clear-on-close
switch; a marker also clears the engine profile after forced termination.
The test checks cookies, localStorage, IndexedDB, service workers and
CacheStorage across restart and clearing, and checks encrypted cookie rows.
Contracts are now lower WIT 0.4.0, upper WIT 0.3.0, native ABI 4.

Build fixes include Windows headers, Blink edit flags/file-picker enum,
native C buffer-warning compatibility, and `packaged-vs-version` in
`scripts/hooks.py`: the VS 2022 package must not be repeatedly revalidated
against Chromium's default VS 2026. `build.sh` applies that hook and locks
the volume against concurrent native builds.

Sandboxed renderers need `content::PatchServiceManagerCalls()` before
lockdown so DirectWrite can use font bytes supplied by FontDataService.
Without it, pages had no visible text. Typed character events also need a
DOM key. Both are covered by actual window/input tests now. Keep the
sandbox enabled when fixing rendering/input problems.

Windows e2e opens and restarts visible windows. Its final pass is complete;
avoid unnecessary repeats. Use `adb -P 5038` explicitly: this adb build did
not honor `ADB_SERVER_PORT`. Phone serial: `6toz4p9pdm898p9d`.
The RAM-cache follow-up also disables GPU shader disk caching and compression
dictionary caching. Its two storage tests pass with the real Windows runtime
kept hidden (`C:\me-tor\chorium-out\check_cache_hidden.py`), including checks
that those cache directories are absent. New portable output:
`C:\me-tor\chorium-out\Chorium-Windows-RAM-cache`; the older tested folder
is left intact because the user is running MotionMark in it.

Android APK builds and installs on the connected Redmi. The latest full
phone run passed 19 tests, skipped two desktop-only tests, and had one ADB
daemon error before the frame-rate test. The isolated frame-rate rerun
still reports 60 fps against a 120 Hz display; this remains unresolved.
Downloads now use MediaStore shared Downloads, infer generic MIME types
from extensions (preserving extensions when numbering duplicates), and
open content URIs. Android background lifecycle flushes cookies/DOM storage.
Remote elements become visible only after Android draws their touch regions.
Test profiles live under `files/test-profiles/<name>` and require the
remote-control flag, so intent extras cannot clear normal app data.
APK: `C:\me-tor\Chorium\Chorium-157.0.8103.15-Android-arm64-20261031.apk`.
Logs: `C:\me-tor\chorium-out\android-e2e.log`, `android-fps.log`.

Use `chorium-android-fast` for incremental builds: eight jobs, 18 CPUs,
10 GB RAM, generator CPU count four. Full builds should use six jobs.
`build.sh` now defaults to nice 5 / best-effort I/O priority 7. Retain the
`chorium-work` volume and current GN args: measured Java APK updates take
30-35 seconds; the native download fix plus Chromium relink took 63 seconds.
The follow-up Windows native build also succeeds. Do not run Windows and
Android native builds together.

Address-bar editing continuation: the shared field now stores UTF-8 caret
and anchor offsets, supports click/tap placement, drag selection, Shift
arrows/Home/End, Ctrl word movement/deletion, and partial cut/copy/paste.
Long text scrolls to keep the caret visible. Windows captured selection
drags continue outside the window; Android commits IME composition before
pointer repositioning and supports forward deletion. Shared tests: 200
checks / 21 desktop-and-phone scenarios pass. The real Windows runtime
passes three address tests (pointer/drag edits, long Unicode URLs, typing),
kept hidden. Log: `C:\me-tor\chorium-out\windows-address-editing-v4.log`.
New Windows folder: `C:\me-tor\chorium-out\Chorium-Windows-Address-editing`.
Its matching ZIP is in `C:\me-tor\Chorium` with `-Address-editing.zip`.
The updated Android APK builds and installs; the three real phone address
tests also pass (`C:\me-tor\chorium-out\android-address-editing.log`).
HyperOS initially rejected updates with INSTALL_FAILED_USER_RESTRICTED.
When ADB then lost the phone, two servers were running on 5037 and 5038.
Stopping both and restarting only 5038 restored discovery; the user's
requested streamed retry then succeeded. Never uninstall or clear normal
app data to work around installation restrictions.

Chromium fork (Brave-style: this repo is linked in as `src/umbra` inside a
Chromium checkout) that routes all traffic through an in-process Tor (Arti)
and is tuned to feel fast: hover/touch prerendering of same-origin links,
bigger video read-ahead, RAM-only cache. Targets: Windows first, Android later.
Read README.md, docs/ARCHITECTURE.md and docs/HOOKS.md before changing code.

## Status: first Windows x64 build done (2026-10-27)

Built against Chromium 157.0.8103.15 and verified end to end (see "Verified"
below). The changes from that session are described in `BUILD_REPORT.md`.
Everything is committed on `main` and pushed to
<https://github.com/sil3nt884/browser> (remote `origin`).

The browser is packaged as a portable folder at `C:\UmbraBrowser` (run
`chrome.exe`; tested from there with the build dir hidden) and zipped at
`dist\Umbra-157.0.8103.15-win-x64.zip`. The Chromium checkout at `C:\umbra`
was then **deleted** to free space for Android, so a Windows rebuild starts
again from step 3 (VS, SDK and Rust are still installed). `build.py` now
packages automatically via `scripts/package_win.py`.

## Umbra Remote (`remote/`, 2026-10-27)

Phone remote for the PC build, see `remote/README.md`. Agent (Rust tray app,
`remote/agent`) drives Umbra over DevTools (screencast + Input.*), serves
phones on LAN port 7777; native Android app in `remote/android` (Java, no
WebView, OkHttp WebSocket, AudioTrack), built in WSL via `setup-sdk.sh` /
`build-apk.sh` (SDK in WSL `~/android-build`). Installed: agent at
`C:\UmbraBrowser\umbra-remote.exe`, APK at `dist\UmbraRemote.apk`.
- Verified on PC: video, taps, drag/wheel scroll, typing + auto keyboard,
  address bar, back (closes link-opened tabs), new-tab following, one-viewer
  takeover, window parked at -32000 keeps rendering, mobile UA/layout,
  sound routed off the default device and captured for the phone (tested with
  Sonar Chat standing in for the cable).
- Not verified: the APK on a real phone (no emulator: WSL has no /dev/kvm);
  LAN access (needs the user's firewall/Private-network step); VB-Cable
  itself (user installs it).
- Sound: mute is applied *before* process-loopback capture, so muting Umbra
  silences the phone. Umbra's audio session is routed to the cable with the
  undocumented AudioPolicyConfig per-app endpoint API (only accepted for
  processes that own a mixer session). Never touch other apps' sessions or
  the system volume (user requirement).
- User choices: no pairing (LAN only), forced incognito is fine, no WebView.
- v0.2: every target is auto-attached paused (`Target.setAutoAttach`
  waitForDebuggerOnStart, recursively) and gets the Pixel 8 identity before
  running; launch adds `--user-agent` (mobile) and
  `--disable-blink-features=AutomationControlled` (webdriver=false).
  Verified page/iframe/worker consistent except iframe screen (CDP can't
  emulate OOPIF screens). Phone menu: full screen, lock rotation, New Tor
  identity (restart Umbra, reopen page), Restart PC agent (relaunches with
  `--restarted`, browser reused). Test caveat: a second DevTools client
  detaching clears a page's navigator.platform override, so probe with one
  connection. Don't build challenge solving/bypassing (declined on purpose).
- v0.3 (speed): 3 frames in flight, adopt() is fire-and-forget, phone decodes
  on its own thread + low-latency WifiLock. tools/fps_test.py: 31.8 fps at a
  simulated 40 ms phone delay. Screencast frames are at CSS size (headed
  window at 1x), not the emulated DPR.
- v0.4/v0.5: H.264 window capture, full screen, pull to refresh, tabs (see
  remote/README.md). v0.6: Parsec-style UDP link in C (`remote/net`,
  linked into the agent via `build.rs`/cc and into the APK via JNI, built by
  `build-apk.sh` with the NDK). WebSocket stays as the fallback. Verified on
  the PC only (`cargo test`, `--udp-test` against the real agent); the APK's
  UDP path has not run on a phone. Sockets must stay non-inheritable: the
  agent starts Umbra, which otherwise keeps UDP 7777 after an agent restart.
  UDP needs its own firewall rule (the user adds it).
- v0.7: capture every composited frame, encode at the phone's refresh rate
  (hello.hz), first change after a still moment at once; Moonlight's decoder
  settings with fallback tiers; "Smooth motion" pacing option. NVENC's SPS
  already has max_num_reorder_frames 0, so no SPS patch. v0.8: finger-locked
  scrolling (shifting the picture by the finger's travel since the touch each
  frame showed) was tried in 0.7 and removed: on the phone it looked jumpy.
- v0.9: media pauses when the phone leaves and resumes when it's back
  (marked with a random property, main world); clearing the app from recents
  sends close_umbra from a foreground service (onTaskRemoved; Android gives
  no other signal, user chose this over adb/heartbeat) and the agent sleeps
  until the next hello. Some of Windows' UDP ports (seen: 47811, 47900) are
  held without a listed owner, so tests pick a free port.
  Tap timing on loopback swings 13-30 ms between runs (Chrome), too noisy to
  A/B small changes; run tests from the Bash tool (PowerShell runs couldn't
  bind the UDP test ports).
- Auto-mode's safety classifier was flaky ("no verdict"); the user added
  `.claude/settings.local.json` allowing only: cargo build/test of
  remote/agent (with `--manifest-path C:/me-tor/umbra/remote/agent/Cargo.toml`),
  `powershell -NoProfile -ExecutionPolicy Bypass -File C:/me-tor/umbra/remote/agent/deploy.ps1`,
  `wsl -d Ubuntu -- bash /mnt/c/me-tor/umbra/remote/android/build-apk.sh`,
  and the fps_test.py run. Use those exact forms to avoid the classifier.

Next (not started; the user wants to wait): Android, most likely built in
WSL2 Ubuntu on this PC. Keep that checkout inside the WSL filesystem, not
`/mnt/c`, raise WSL's memory limit in `.wslconfig`, and check free space
first (~120 GB).

## Chorium (`chorium/`, 2026-10-30)

Chromium's engine as an embeddable library with Tor inside, plus a
WebAssembly app layer; read `chorium/README.md`. Layers: `engine/`
(chorium.dll, //content embedder, C ABI in `interfaces/native/chorium.h`),
`runtime/win/` (Chorium.exe: Wasmtime + Direct2D window), `portable/`
(chorium.wasm: tabs, history, settings), `apps/browser/` (Chrome-style
sample). Contracts: `interfaces/chorium/chorium.wit` (lower) and
`interfaces/browser/browser.wit` (upper). Everything builds in Docker
(`chorium/docker`, image `chorium-build:157`, volume `chorium-work`); the
Windows engine is cross-compiled. Verified on Windows 11: pages through
Tor, fail closed without umbra_tor.dll, composed app with tabs, settings,
history. The 2026-10-31 continuation adds the Windows sandbox and file
pickers; Linux runtime and colour pickers remain unfinished.

Android runtime (`runtime/android/`, Chorium.apk, arm64) verified on a
Redmi Note 14 (Android 16), 2026-10-30: phone layout, Chrome-style gestures
(toolbar swipes, swipe tabs away, pull to refresh, edge swipe back,
long-press menu), full-screen video, session restore, pages following the
app's theme, mobile user agent. Most Android bugs were setup content shell
does and we hadn't (resource data pack, Compositor::Initialize, text
selection controller, network change factory, GPU Java libs, APK version,
mobile UA switch); compare with `content/shell` first when something breaks.
Test over USB: Google platform-tools in `C:\me-tor\chorium-tools\
platform-tools`, `ADB_SERVER_PORT=5038` (5037 is held), each install needs
the user to accept HyperOS's "Install via USB" prompt; HyperOS blocks adb
taps. DevTools: `adb forward tcp:9333 localabstract:chorium_devtools_remote`
and `C:\me-tor\chorium-out\android-arm64\cdp.py`.

Tor switch (both platforms): off stops Tor (`umbra_tor_stop`, an optional
export; the Tor ABI stays 1) and pages go direct; on restarts it, fail
closed until up. Chorium drops Umbra's `--host-resolver-rules` DNS block
(direct mode needs DNS; the user accepted this). Windows wheel notches are
sent as plain pixels in one gesture so Chromium animates them (smooth
scrolling); touchpad deltas stay precise.

## Windows build recipe

1. **Disk space first.** Chromium needs ~120 GB free on an NTFS drive. The
   user's C: had ~90 GB. Before fetching anything, check free space
   (`Get-PSDrive -PSProvider FileSystem`). If it's under ~110 GB, help the user
   free space or pick another drive (pass `--work D:\umbra`). Never delete user
   files yourself; show what's large and let the user decide.
2. **Prerequisites** (check, and ask before installing anything):
   Visual Studio 2022 with "Desktop development with C++" plus the
   "MFC/ATL" components, Windows 11 SDK *with Debugging Tools for Windows*,
   Git, Python 3, Rust (rustup, MSVC toolchain). Chromium 157 needs SDK
   **10.0.28000** exactly (`build/vs_toolchain.py` `SDK_VERSION`); VS 2022 is
   still accepted.
3. `python scripts\bootstrap.py --target win --work C:\umbra`
   (several hours; Chromium fetch with no history.)
   If the sync is rate-limited (HTTP 429), rerun with `--jobs 4`.
4. `python scripts\build.py --target win --work C:\umbra --jobs 12`
   (~5.5 h on a Ryzen 3900X / 16 GB.) Run it in the background and log to a
   file, e.g. `... *> C:\umbra\build.log`. With 16 GB RAM keep `--jobs` at
   ~12 and shut down WSL/Docker first (`wsl --shutdown`); otherwise it pages.
5. Fix compile errors in `//umbra` code. If `hooks.py` can't find an anchor,
   re-anchor it in `scripts/hooks.py` and update the fixture under
   `scripts/tests/fixtures/`.
6. Result: `C:\umbra\chromium\src\out\umbra-win-x64-release\chrome.exe`.
   `UMBRA_TOR_LOG=info` prints Arti's log (bootstrap progress) to stderr.
   Run with `--enable-logging=stderr` to see `[umbra]` lines; note that
   `VLOG(1)` from the startup hook is dropped (it runs before logging init),
   only `LOG(ERROR)` shows.

## Windows pitfalls found on the first build
- **ESET** deletes `third_party/pdfium/testing/resources/get_page_aaction.pdf`
  (flagged as PDF/Exploit.CVE-2018-4993). Workaround used:
  `git update-index --skip-worktree` on that file. An ESET exclusion for
  `C:\umbra` would be better and faster; that is the user's call.
- **Siso** (Chromium's default builder) cannot read `src/umbra` because it is
  an NTFS junction, so `build/args/win.gn` sets `use_siso = false` (Ninja).
- **Headless** `--dump-dom` and DevTools `Target.createTarget` fail ("Failed
  to open a new tab") because Umbra forces incognito. For automated checks,
  launch normally with `--remote-debugging-port` and drive the existing tab.
- `enable_pdf=false` / `enable_printing=false` don't build on Windows in M157
  (desktop UI depends on both). PDF and printing are switched off through
  prefs in `browser/umbra_prefs.cc` instead.

## Verified (Windows 11, 2026-10-27)
- `cargo test` in `tor_core/`: 8 pass. `scripts/tests/test_hooks.py`: 6 pass.
- `chrome.exe` (Umbra 157.0.8103.15): check.torproject.org says
  "Congratulations. This browser is configured to use Tor." Cold start
  12.9 s to first page, warm start 6.6 s.
- Proxy/QUIC/WebRTC/DNS switches present on the command line.
- Same-origin speculation rules injected; `<video>` preload boosted.
- All 7 pref overrides applied; PDFs download instead of rendering.
- Fail-closed: without `umbra_tor.dll` pages fail with
  `ERR_PROXY_CONNECTION_FAILED` and no non-loopback connections are made.

## Don't
- Don't weaken the fail-closed proxy logic in `app/umbra_startup.cc`.
- Don't make preloading cross-origin.
- Don't edit Chromium files directly. Go through `scripts/hooks.py`.
