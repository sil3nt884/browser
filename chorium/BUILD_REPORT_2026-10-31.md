# Chorium continuation — 2026-10-31

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

Fast checks: **188 app checks**, **7 hook tests**, Python compilation and shell
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
Android build and phone validation are still in progress. The connected Redmi
is available as `6toz4p9pdm898p9d`; no Android success is claimed yet.
The changed native and Java targets pass their build checks, including strict
Java Error Prone checks. Android test-profile cleanup is restricted to a
dedicated `files/test-profiles/<name>` tree.
