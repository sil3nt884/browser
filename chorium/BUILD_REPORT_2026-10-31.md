# Chorium continuation — 2026-10-31

## Final release

`window.open()` had a native Windows access violation in
`aura::client::ParentWindowWithContext`: the windowless host had no parenting
client. The host now supplies its root as the generic native parent. Opening
the requested URL as a foreground tab is handled explicitly in the browser
app; empty requests stay blank. No native tab/window UI policy was added.
The existing URL-request contract still does not preserve a popup's returned
WindowProxy/opener relationship.

Four real-app popup tests pass on Windows and the connected Redmi: direct
JavaScript, a real button click, a target=_blank link, and the user's exact
`window.open("https://www.w3schools.com")` example. The opener remains alive.
Shared app tests: 206 checks in 21 desktop/phone scenarios pass. Windows was
tested hidden to avoid reopening visible windows. Logs are retained with the
release. Android's separate 120 Hz failure remains open.

Final artifacts in `C:\me-tor\Chorium`:

- `Chorium-Windows-Final\Chorium.exe` (portable folder)
- `Chorium-157.0.8103.15-Windows-x64-20261031-Final.zip`
- `Chorium-157.0.8103.15-Android-arm64-20261031-Final.apk`
- `SHA256SUMS.txt` and per-archive `.sha256` files

All 15 files in the Windows ZIP match the portable folder. Both archives
pass CRC checks, and the APK's browser.wasm exactly matches Windows'.
The final APK is installed on the phone. The final Windows native change
rebuilt three objects and relinked in 83 seconds; Android repackaging took
31 seconds. Local source/build cleanup was requested after a verified Git
push; final releases, validation logs and normal app data must be retained.

The failed Docker Windows build is repaired. Claude's downloads, file picker,
context menu, remote control and real-app test work is integrated with persistent
site data, RAM HTTP caching, Windows cookie encryption and privacy controls.

## Verified on Windows

- Native Docker cross-build: success, including the sandbox-linked runtime.
- Real-app suite: **22/22 pass**, including the Tor network check.
- Actual window pixels contain rendered page text; page typing and clipboard
  commands work through the native input path.
- Renderer integrity RID **0**, job containment, and win32k system-call lockdown.
- Page animation: **144 fps** on a **144 Hz** display.
- Download completion, duplicate naming, cancellation, Downloads UI, clipboard
  image copy, link/image/field menus, and real system upload dialog.
- Session restore, themes, fullscreen, back, tabs, online status and Tor switch.
- Cookies, localStorage, IndexedDB, service workers and CacheStorage survive
  restart and clear through Settings. Clear-on-close also passes.
- Cookie database stores encrypted values with empty plaintext columns; the
  AES key is protected by Windows DPAPI in the profile's `Local State` file.

Fast checks: **200 app checks**, **7 hook tests**, Python compilation and shell
syntax checks pass. Windows e2e log:
`C:\me-tor\chorium-out\windows-e2e.log`.

## Implementation

Hosts select a persistent profile before engine startup. Native callers that
omit `chorium_set_profile_dir` retain an off-the-record profile. HTTP caching
remains in memory; Chromium's generated-code disk cache remains disabled.
GPU shader disk caching and compression-dictionary transport are disabled to
avoid their otherwise persistent caches. Normal gzip/Brotli compression and
explicit site CacheStorage remain available. Two additional hidden-runtime
storage tests pass, including absence of shader/dictionary cache directories.
Site data clearing preserves app settings, history and tab sessions. The
clear-on-close switch defaults to off. Its marker also handles the next launch
after abrupt termination; live pages can create fresh data after a manual clear.

Native ABI: **4**. Lower WIT: **0.4.0**. Upper WIT: **0.3.0**. Rebuild the
runtime, engine and composed app together when updating.

Build repairs cover Windows SDK include order, shell APIs, explicit Blink edit
flags, the additional directory-picker mode, raw C buffer diagnostics, and a
version-selection hook for the custom VS 2022 toolchain. Native builds now use
a volume-wide lock before removing stale Ninja locks. Renderers retain the
sandbox and initialize DirectWrite's service-manager hooks before lockdown.
Character events carry DOM keys, including supplementary Unicode characters.

The e2e harness waits for document readiness, closes SQLite handles before
restarting Chromium, uses unique download names, scopes sandbox checks to the
test browser, and explicitly selects adb server port 5038. Session restore
checks saved addresses separately from loading loopback fixtures through Tor,
which intentionally resets to on at startup.

## Artifacts and Android

Original tested Windows folder: `C:\me-tor\chorium-out\Chorium-Windows`.
The RAM-cache follow-up is in `C:\me-tor\chorium-out\Chorium-Windows-RAM-cache`.
The Android APK builds and installs on Redmi `6toz4p9pdm898p9d` with an
in-place update that preserves normal app data. APK:
`C:\me-tor\Chorium\Chorium-157.0.8103.15-Android-arm64-20261031.apk`.
Phone validation passes downloads (including duplicates, image saving,
cancellation and opening), the system upload picker, navigation, themes,
fullscreen, Tor, sandbox process isolation, and persistent/cleared site data.
The latest full run passed 19 tests, skipped two desktop-only tests, and hit
an ADB daemon connection error before the frame-rate scenario. Its isolated
rerun still finds 60 fps on the 120 Hz display; this remains under investigation.
Logs: `C:\me-tor\chorium-out\android-e2e.log` and `android-fps.log`.

Android now flushes cookie and DOM storage on background lifecycle events.
Remote UI snapshots are published after Canvas drawing updates the actual
touch regions. Shared Downloads use Chromium's MediaStore publishing path,
not the app-private downloads directory; generic MIME types are inferred
from filename extensions. Opening downloads accepts content URIs. Test
profiles remain restricted to `files/test-profiles/<name>`.

Build scheduling now uses nice 5 / best-effort I/O priority 7 instead of
idle-only CPU/disk priority. The existing Chromium volume and GN arguments
are retained. On this PC, Java-only rebuilds took 30-35 seconds and the
native download change plus Chromium relink took 63 seconds. Full builds
use six jobs with 10 GB Docker RAM; small incremental builds use eight.
The Windows follow-up native build also succeeds (14:18:59 UTC).

## Address-bar editing

The shared editor now has a caret and selection anchor at UTF-8 character
boundaries. Click/tap positions the caret; dragging selects part of the URL.
Arrow keys, Shift selection, Home/End, Ctrl word movement, forward/backward
deletion and partial cut/copy/paste operate at that position. Long text
scrolls with the caret. Android commits the keyboard's composing word before
pointer repositioning. Windows keeps captured selection drags active outside
the window. Initial URL focus and Ctrl+L retain select-all behavior.

Validation: 200 shared app checks in 21 desktop-and-phone scenarios pass.
Three real Windows address tests pass, including pointer/drag replacement,
navigation after partial deletion, and long URLs containing emoji, Chinese
and accented characters. The test window stays hidden. Log:
`C:\me-tor\chorium-out\windows-address-editing-v4.log`.

New Windows executable:
`C:\me-tor\chorium-out\Chorium-Windows-Address-editing\Chorium.exe`.
Matching ZIP and SHA-256 sidecar:
`C:\me-tor\Chorium\Chorium-157.0.8103.15-Windows-x64-20261031-Address-editing.zip`.
The dated Android APK above includes the editor and has a refreshed SHA-256
sidecar. It installs successfully and the same three real address tests
pass on the Redmi. Log: `C:\me-tor\chorium-out\android-address-editing.log`.
Two ADB servers (5037 and 5038) were stopped; restarting only 5038 restored
device discovery. The subsequent streamed update succeeded. Normal app
data remains intact. The separate 120 Hz frame-rate failure remains open.
