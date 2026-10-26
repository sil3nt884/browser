# Umbra: notes for Claude Code

Chromium fork (Brave-style: this repo is linked in as `src/umbra` inside a
Chromium checkout) that routes all traffic through an in-process Tor (Arti)
and is tuned to feel fast: hover/touch prerendering of same-origin links,
bigger video read-ahead, RAM-only cache. Targets: Windows first, Android later.
Read README.md, docs/ARCHITECTURE.md and docs/HOOKS.md before changing code.

## Current goal: first Windows x64 build on this PC

1. **Disk space first.** Chromium needs ~120 GB free on an NTFS drive. The
   user's C: had ~90 GB. Before fetching anything, check free space
   (`Get-PSDrive -PSProvider FileSystem`). If it's under ~110 GB, help the user
   free space or pick another drive (pass `--work D:\umbra`). Never delete user
   files yourself; show what's large and let the user decide.
2. **Prerequisites** (check, and ask before installing anything):
   Visual Studio 2022 with "Desktop development with C++" plus the
   "MFC/ATL" components, Windows 11 SDK *with Debugging Tools for Windows*,
   Git, Python 3, Rust (rustup, MSVC toolchain).
3. `python scripts\bootstrap.py --target win --work C:\umbra`
   (several hours; Chromium fetch with no history.)
4. `python scripts\build.py --target win --work C:\umbra`
   (several hours on first build.) Run it in the background and log to a
   file, e.g. `... *> C:\umbra\build.log`.
5. Fix compile errors in `//umbra` code. Expected drift spots:
   `app/tor_library.cc`, `renderer/preload_injector.cc` (Blink isolated-world
   API), `browser/umbra_prefs.cc` (`PrefStore::GetValue` signature).
   If `hooks.py` can't find an anchor, re-anchor it in `scripts/hooks.py` and
   update the fixture under `scripts/tests/fixtures/`.
6. Result: `C:\umbra\chromium\src\out\umbra-win-x64-release\chrome.exe`.
   Check it shows Tor bootstrap progress and that pages load. Run with
   `--enable-logging=stderr --v=1` to see `[umbra]` log lines.

## Verified so far (in a Linux sandbox)
- `cargo test` in `tor_core/`: 8 tests pass, including an FFI smoke test.
- `python scripts/tests/test_hooks.py`: 6 tests pass.
- Chromium-side C++ has never been compiled.

## Don't
- Don't weaken the fail-closed proxy logic in `app/umbra_startup.cc`.
- Don't make preloading cross-origin.
- Don't edit Chromium files directly. Go through `scripts/hooks.py`.
