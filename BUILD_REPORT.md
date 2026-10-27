# Umbra: first Windows build report

**Date:** 26–27 October 2026 (overnight)
**Result:** ✅ Umbra builds on Windows x64 and works. Pages load over Tor,
and the speed features and fail-closed protection behave as designed.

**Run it:** `C:\UmbraBrowser\chrome.exe` (window title and file details say
"Umbra 157.0.8103.15"). This is a portable copy (259 files, 471 MB; zip:
`dist\Umbra-157.0.8103.15-win-x64.zip`, 205 MB) made by
`scripts\package_win.py` and tested on its own with the build folder hidden.
The build checkout `C:\umbra` was deleted afterwards to free disk space. Every window is
incognito by design. Tor keeps its guard/directory cache in
`%LOCALAPPDATA%\Umbra\Tor`, so later starts are faster.

> **Committed** (`ef8bc32`, together with Umbra Remote) and pushed to
> <https://github.com/sil3nt884/browser> on 27 October 2026.

---

## Test results

| Check | Result |
|---|---|
| Is traffic going through Tor? | **Yes.** check.torproject.org: *"Congratulations. This browser is configured to use Tor."* (exit relays 204.8.96.141, 185.220.100.241) |
| Cold start (empty Tor cache) → first page | **12.9 s**. Tor bootstrapped in 5 s |
| Warm start → first page | **6.6 s**. Tor ready about 2 s after launch |
| Lockdown switches | `--proxy-server=socks5://127.0.0.1:<port>`, `--proxy-bypass-list=<-loopback>`, `--host-resolver-rules=MAP * ~NOTFOUND`, `--disable-quic`, `--force-webrtc-ip-handling-policy=disable_non_proxied_udp` all present |
| Hover preloading | Same-origin speculation rules (prerender + prefetch, "moderate") injected on every page |
| Video read-ahead | `<video preload="metadata">` boosted to `auto`; `preload="none"` respected |
| Settings | All 7 Umbra defaults applied (no "override skipped" warnings). **PDFs download instead of opening**, printing off |
| **Fail-closed** (Tor DLL removed) | Pages fail with `ERR_PROXY_CONNECTION_FAILED`, proxy points at dead port `127.0.0.1:1`, **zero internet connections**, error logged |
| Unit tests | `tor_core`: 8/8 pass on Windows. `hooks.py`: 6/6 pass; all hooks apply cleanly to Chromium 157 |

---

## What was done, in order

1. **Checked the machine.** 135 GB free on C:, Ryzen 9 3900X (24 threads),
   16 GB RAM. Git, Python 3.14 and Rust were present.
2. **Installed missing tools** (with your OK): the VS 2022 "Desktop
   development with C++" workload, ATL/MFC, Windows SDK Debugging Tools, and
   later **Windows SDK 10.0.28000**, which Chromium 157 requires.
3. **Downloaded Chromium 157.0.8103.15** (latest Stable), fixing three
   download problems along the way (below).
4. **Re-anchored the Chromium hooks** that had moved in Chromium 157.
5. **Built Chromium + Umbra**: 58,817 steps, about 5.5 hours with 12 jobs.
6. **Fixed Umbra's code** where Chromium 157 had changed, and found and fixed
   two real bugs during testing.
7. **Tested** everything in the table above.

---

## Problems hit and how each was fixed

### Downloading Chromium
| Problem | Cause | Fix |
|---|---|---|
| Sync stuck for over an hour at 0 MB/s | `--with_tags` on a no-history clone makes git negotiate every Chromium tag | `bootstrap.py` fetches only the pinned tag |
| Same stall again | gclient re-fetches all of `origin` for any revision that isn't a raw commit SHA (`gclient_scm.py`, `sha_only=True`) | `bootstrap.py` resolves the tag to its SHA and syncs to that |
| HTTP 429 "rate limit exceeded" | Google's git server throttles 24 parallel anonymous fetches | New `--jobs` option on `bootstrap.py` (default 8; 4 used) |
| gclient: "uncommitted changes" in pdfium | **ESET deletes** a pdfium test file (`get_page_aaction.pdf`, flagged as PDF/Exploit.CVE-2018-4993) | Hid that one file from git (`git update-index --skip-worktree`). ESET untouched, as you preferred |

### Building
| Problem | Cause | Fix |
|---|---|---|
| `hooks.py`: browser-deps anchor not found | `chrome/browser` is now a `source_set`, not a `static_library` | Anchor accepts both (renderer-deps too) |
| `hooks.py`: media-buffer anchor not found | The 10 s read-ahead moved to a `Tuneable` in the `.h` | Split into `media-buffer` (.h: 10→60 s) and `media-buffer-cap` (.cc: 50→200 MB) |
| `gn gen`: SDK 10.0.28000 missing | Chromium 157 pins that SDK | Installed it |
| `gn gen`: PDF/printing asserts | You asked for PDF and printing off; `enable_pdf=false` needs `enable_printing=false` on Windows, and **desktop Chrome 157 can't build without either** | With your OK: turned off **by setting** in `umbra_prefs.cc` (PDFs download, printing disabled) |
| PDF setting would have been ignored | The `prefs` hook ran before per-profile services register their settings | Moved the hook to `pref_service_builder_utils.cc`, after all registration |
| Build tool: `deps input "../../umbra/app/BUILD.gn" not exist` | Siso (Chromium's new builder) can't follow the NTFS junction `src\umbra → C:\me-tor\umbra` | `use_siso = false` in `build/args/win.gn` (uses Ninja) |
| Slow build, heavy paging | 16 GB RAM, plus WSL/Docker holding 7.2 GB | 12 jobs; you shut down WSL/Docker. Paging dropped from ~60 MB/s to ~1 MB/s |
| `preload_injector.cc`: header not found | Blink moved `web_isolated_world_info.h` and renamed `WebString::FromUTF8` → `FromUtf8` | Updated include and calls |

`tor_library.cc` and `umbra_prefs.cc`, two of the three spots CLAUDE.md
expected to break, compiled without changes.

### Testing found two real bugs
**1. Tor never finished starting (stuck at 15%).** This was the big one.
Pages timed out. I narrowed it down step by step: it wasn't Chromium, the
network or ESET. Arti hung the same way on its own, while a plain TLS
handshake to the same relays worked. A debugger stack trace finally showed
one Tor thread spinning forever inside the **`saturating-time` crate**, a
dependency of Arti's directory parser. On Windows the clock has 100 ns
resolution, so its "find the minimum time" loop takes 1 ns steps that never
fail and never move. On Linux (1 ns clock) it ends, which is why the sandbox
tests never caught it.
**Fix:** a one-line patch in a vendored copy,
`tor_core/third_party/saturating-time` (see `UMBRA_PATCH.md` there), wired
in with `[patch.crates-io]`. Tor now bootstraps from scratch in about 5 s.
*This is an upstream Arti bug worth reporting* (see next steps).

**2. Hover preloading never ran.** Chromium 157 calls the DOMContentLoaded
hook inside a `ScriptForbiddenScope`. Umbra's script was compiled but
silently never executed.
**Fix:** `preload_injector.cc` now posts the injection as a task right after
DOMContentLoaded, the same pattern Chromium's extension code uses.

---

## Changes (now in commit `ef8bc32`)

| File | Change |
|---|---|
| `scripts/bootstrap.py` | Tag-only fetch, sync by SHA, `--jobs` |
| `scripts/build.py` | `--jobs` passed to autoninja; Windows builds are packaged into `dist\umbra-win-x64\` + zip |
| `scripts/package_win.py` | New: portable package from Chromium's installer file list, plus the sandbox folder permission |
| `scripts/hooks.py` | browser/renderer-deps anchors, media-buffer split, prefs hook moved |
| `scripts/tests/…` | Fixtures and tests updated to match (6/6 pass) |
| `build/args/win.gn` | `use_siso = false` |
| `browser/umbra_prefs.cc` | PDF → download, printing off |
| `renderer/preload_injector.{h,cc}` | Blink API drift; deferred injection fix |
| `tor_core/Cargo.toml`, `Cargo.lock`, `tor_core/third_party/saturating-time/` | Patched dependency (Tor hang fix) |
| `docs/HOOKS.md`, `docs/ARCHITECTURE.md`, `README.md`, `CLAUDE.md` | Updated to match |
| `BUILD_REPORT.md` | This file |

---

## State of your PC

- **Installed:** VS 2022 C++ workload + ATL/MFC, Windows SDK 10.0.28000
  (with Debugging Tools), plus the Debugging Tools for SDK 10.0.26100.
- **Disk:** Chromium checkout `C:\umbra` deleted after packaging; the
  browser lives in `C:\UmbraBrowser`.
- **ESET:** unchanged. The one pdfium test file it deletes is hidden from
  git. Every rebuild will be slower than necessary while ESET scans
  the build folder.
- **WSL/Docker:** shut down for the build. Start them normally whenever you
  like.
- No files deleted outside the build and scratch folders (and, at your
  request, the Chromium checkout `C:\umbra` after packaging).

## Not done / suggested next steps

1. **Review and commit** the changes above. Suggested message: *"First
   Windows build: Chromium 157 fixes, Tor bootstrap hang, deferred preload"*.
2. **Report the `saturating-time` bug to the Tor Project** (Arti GitLab) with
   the analysis in `tor_core/third_party/saturating-time/UMBRA_PATCH.md`.
   Then drop the patch once a fixed release ships.
3. **Consider an ESET exclusion for `C:\umbra`.** Rebuilds would be faster and
   no more test files would be deleted. Your call.
4. **Speed idea:** built-in ad/tracker blocking would probably cut page load
   times over Tor more than anything else left (fewer requests per page).
5. **Known limitations, unchanged:**
   - No per-site circuit isolation yet.
   - No anti-fingerprinting.
   - Headless mode doesn't work with forced incognito. Use a normal window
     with `--remote-debugging-port` for automated tests.
   - The startup hook's `VLOG(1)` lines don't show, because they run before
     logging starts.
6. **Android:** needs a Linux machine or VM (32 GB RAM, ~200 GB disk).
