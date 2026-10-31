# Chorium

Chromium's page engine as an embeddable library, with Tor inside (and a Tor
switch). No browser UI, no command-line flags. Runs on Windows and Android.
Three parts, joined by two versioned contracts:

```
 app.wasm ─┐  (your UI, any language that targets WASI 0.2)
           │  browser.wit   umbra:browser@0.3.0   (upper contract)
 chorium.wasm  portable browser layer: tabs, history, settings, URL handling
           │  chorium.wit   umbra:chorium@0.4.0   (lower contract)
 runtime       small native C host per platform: Wasmtime + window + input
           │  chorium.h     C ABI, CHORIUM_ABI_VERSION 4
 engine        chorium.dll / libchorium.so: Chromium //content (no //chrome),
               JSON events, Tor (umbra_tor, Arti): fail closed, or switched off
```

app.wasm and chorium.wasm are built once and run on every runtime; the
runtime and the engine are built once per platform. Calls only go down:
the runtime delivers events to the app, the app passes page events to
chorium.wasm and gets browser events back (the component model allows no
cycles).

| Folder | What |
|---|---|
| `interfaces/` | `chorium/chorium.wit`, `browser/browser.wit`, `native/chorium.h` |
| `engine/` | the native engine (GN target, built inside the Chromium tree) |
| `runtime/common/` | the runtime core both hosts share (contract, events, values, JSON) |
| `runtime/win/` | Windows runtime: `Chorium.exe` (GN target, with Chromium's sandbox linked in; Wasmtime 50 C API) |
| `runtime/android/` | Android runtime: `Chorium.apk` (Java views, JNI, Wasmtime, the app) |
| `portable/` | chorium.wasm source (C) |
| `apps/browser/` | sample app: Chrome-style browser (tabs in the title bar on Windows, a phone layout on Android; menu, Settings, History, Downloads, context menu, gestures, Tor switch) |
| `tests/` | `e2e/`: the real app on Windows and Android through its remote control (`runtime/common/remote.h`) and DevTools; `app/`: the app's logic on the real runtime core with a fake engine; `engine/chorium_test.c` (see `tests/README.md`) |
| `docker/` | build image and scripts (engine, wasm) |
| `tools/` | `update_engine.sh`: check for, build and package a new Chromium release |

Native programs can also use the engine directly through `chorium.h`, with no
WebAssembly involved:

## Using it

```c
int main(void) {
  int code = chorium_execute_process();   // must be first: Chromium starts
  if (code >= 0) return code;             // its helpers by relaunching you
  chorium_init(1.0f);
  chorium_view *v = chorium_open(1280, 800, on_frame, on_event, NULL);
  chorium_load_url(v, "https://example.com/");
  ...
  chorium_shutdown();
}
```

Ship these files next to your .exe: `chorium.dll`, `chorium.pak`,
`umbra_tor.dll`, `icudtl.dat`, `snapshot_blob.bin`, and Chromium's GPU DLLs
(`libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`, `vk_swiftshader.dll`,
`vk_swiftshader_icd.json`, `vulkan-1.dll`, `dxcompiler.dll`).
[`tests/engine/chorium_test.c`](tests/engine/chorium_test.c) is a complete
example: it loads a page through Tor and saves the frame as a .bmp.

## How Tor is enforced

Chromium's engine layer ignores `--proxy-server` (the browser layer reads
it), so Chorium does not rely on switches for the important part:

- `BrowserClient::ConfigureNetworkContextParams` sets one SOCKS5 proxy on
  every network context, with no DIRECT fallback and no bypass (not even
  localhost). SOCKS5 sends host names, so DNS goes through Tor.
- The port comes from `umbra::OnBasicStartupComplete()` (shared with the
  Umbra browser). If Tor failed, it's a dead loopback port and every request
  fails.
- WebRTC is limited to proxied UDP (none, since Tor is TCP) per page.
- Chorium replaces the process command line at `chorium_init`, so the app's
  own arguments can't inject switches.

**The Tor switch** (`chorium_set_tor`, `net.set-tor`; on at every start).
Off stops Tor completely (`umbra_tor_stop`: its SOCKS port and every circuit)
and pages connect directly; on starts it again on a new port and stays fail
closed until it's up. Each network context gets its proxy through a live
`ProxyConfigClient`, and every switch closes all open connections, so nothing
made in one mode is reused in the other. Umbra's start-up also makes every
local DNS lookup fail; Chromium reads that rule once, and direct mode needs
DNS, so Chorium removes it. With Tor on, names are still resolved by the Tor
exit (SOCKS5 sends host names, and there is no direct fallback).

## Building (Docker, any OS)

Everything builds in the `chorium-build` image; the Chromium checkout lives
in the `chorium-work` volume. Windows is cross-compiled
([Chromium docs/win_cross.md](https://chromium.googlesource.com/chromium/src/+/main/docs/win_cross.md)).

1. Image: `docker build -t chorium-build:157 chorium/docker`
2. Windows SDK package (once, on a Windows PC with VS 2022 + SDK 10.0.28000):
   depot_tools' `win_toolchain/package_from_installed.py` from before VS 2026
   support (depot_tools commit `1867bb56`):
   `python package_from_installed.py 2022 -w 10.0.28000.0 --noarm`.
   Copy the resulting `<hash>.zip` into the volume at `/work/winsdk/`.
3. Tor library (on Windows): `python scripts/build_tor_core.py --os win --cpu x64`
4. Source: `docker run --rm -v chorium-work:/work -v <repo>:/umbra:ro
   -e WIN_TOOLCHAIN_HASH=<hash> chorium-build:157 bash /umbra/chorium/docker/fetch.sh win android`
   (a shallow checkout: the tag only, no history). The other tools
   (wasi-sdk, wit-bindgen, wac, Wasmtime's C APIs, the icon font, rustup's
   Rust, the NDK) come from `docker/tools.sh`, run the same way.
5. Build: same, with `--cpus=18 --memory=10g -e JOBS=6 -e CHORIUM_CPU_COUNT=4`
   and `build.sh win` (see the header of `docker/build.sh` for the SDK
   mount). Blink's heaviest files need 1.5-2.5 GB each, so with 10 GB keep
   `JOBS` at 6; 10 ran out of memory. A full build is a few hours; after
   that, changing Chorium's own files rebuilds in about 3 minutes.

Builds are capped (`--cpus`, `--memory`) and use CPU nice level 5 and
best-effort I/O priority 7. Set `BUILD_NICE=19 BUILD_IO_CLASS=3` for the old
idle-only scheduling. On this 16 GB PC use six jobs for full builds; eight
jobs work for small incremental edits. Keep the existing `chorium-work`
volume: after the first Chromium compile, only changed targets rebuild.
Measured on this PC: Java-only APK edits took 30-35 seconds; a native
download change, including relinking Chromium, took 63 seconds. Changes to
GN build files can also require regenerating the build graph.
App-only changes need `build_wasm.sh app browser` followed by `build.sh android`;
without native changes the latter only repackages the APK.

**Android** (arm64): `build.sh android` builds `out/chorium-android/apks/
Chorium.apk` (engine, runtime, Wasmtime and the app in one APK). Before it:
the Tor library (`scripts/build_tor_core.py --os android --cpu arm64 --ndk
<r27c root>`, with rustup's Rust and the official NDK: Chromium's NDK copy has
no clang), Wasmtime's Android C API and the icon font in `/work/wasm-tools`,
and the app (`build_wasm.sh app browser`). Don't run an Android and a Windows
build in the same 10 GB container at once: Blink's heaviest files together
run it out of memory. Install over USB with adb; the APK has DevTools on the
abstract socket `chorium_devtools_remote` (`adb forward tcp:9333
localabstract:chorium_devtools_remote`; GN arg `chorium_android_devtools`,
turn it off for releases).

## Apps and chorium.wasm

Built in the same Docker image; wasi-sdk 34, wit-bindgen 0.63 and wac 0.12
are unpacked in the volume at `/work/wasm-tools`.

- `docker run --rm -v chorium-work:/work -v <repo>:/umbra:ro -v <out>:/out
  chorium-build:157 bash /umbra/chorium/docker/build_wasm.sh app browser`
  builds `chorium.wasm` (world `portable`), the app (world `app`), and plugs
  them together with `wac plug` into `<out>/browser.wasm`, a component for
  the runtime's `runnable` world. `build_wasm.sh portable` builds only
  chorium.wasm.
- **Windows runtime**: `build.sh win` also builds `Chorium.exe` (GN target
  `//umbra/chorium/runtime/win`, Wasmtime 50 C API) next to `chorium.dll`;
  copy `wasmtime.dll` from the C API next to it. It is built in Chromium's
  tree because Chromium's sandbox has to be linked into the .exe: page, GPU
  and network helpers are this .exe started again, and the broker patches
  them at the .exe's addresses (`sandbox_win.cc`, `chorium_set_sandbox`).
  For that, nothing it or `chorium.dll` loads at start may touch win32k
  (user32, gdi32, Direct2D, Wasmtime are delay-loaded). Put apps in an `apps` folder next to it and
  double-click `Chorium.exe`: one app starts directly, several give a picker,
  none gives a message. `Chorium.exe app.wasm [args]` runs a given app;
  `Chorium.exe <url>` passes the URL to the app from the folder. It's a
  windowed program (no console); problems show as message boxes, and
  `CHORIUM_HOST_TRACE=1` logs calls into the app to stderr when run from a
  console with stderr redirected. It refuses to start if `chorium.dll`'s
  `chorium_abi_version()` doesn't match. Drawing is Direct2D/DirectWrite
  (`surface_d2d.cpp`, C++ only because DirectWrite has no C headers); icons
  come from Segoe Fluent Icons. The window has no system caption: the app
  draws the title bar (`surface.title-bar-layout`), the runtime does the
  window buttons, dragging (`set-drag-areas`), snap and resizing.
- The kill switch covers WebAssembly too: the runtime gives WASI no network
  and no directories. Only chorium.wasm imports `pages` and `net`, everything
  goes through the engine (Tor), and files only through `files.app-data`
  (`%LOCALAPPDATA%\Chorium\apps\<name>`, the app's files folder on Android:
  chorium.wasm keeps `settings.txt`, `history.tsv` and `session.txt` there).
  Settings keys: `search` (URL template with `%s`), `home`, `theme`
  (`system`, `light`, `dark`); the sample's Settings page edits them. The
  open tabs are saved as they change and come back at the next start
  (`restore-session`).
- **Android runtime**: `ChoriumActivity` starts the engine, then the app;
  `AppView` replays the app's drawing (a display list) on a Canvas, and each
  page is a native view under it, seen through a hole, so app menus can draw
  over pages. The back gesture goes to the app only while it has something to
  go back to (`surface.set-back-enabled`), else Android leaves the app.
- Pages follow the app's theme (`pages.set-dark-mode`: prefers-color-scheme,
  form controls, scrollbars). Long press / right click on links, images or
  selected text arrive as `context-menu` events, Android's pull to refresh
  and edge swipes as `overscroll` events; the app draws both. Full screen
  (`fullscreen` event, `surface.set-fullscreen`) hides the app's UI and the
  system bars. `surface.copy-text` puts text on the clipboard,
  `surface.read-clipboard` reads it (paste in the app's own fields).
- Text fields (desktop) get the app's context menu too (`editable`, `edit`
  flags; `page.edit` runs Cut, Copy, Paste, Select all); links and images
  can be saved (`page.save-url`) and images copied (`page.copy-image-at`).
- Downloads go straight to the Downloads folder (Windows: the user's;
  Android: the shared Download folder) under a free name, reported as
  `download` events; `surface.open-file` opens one or shows it in its folder.
  A page's `<input type=file>` arrives as a `file-chooser` event: the app
  calls `surface.pick-files` (the system picker; Android hands back
  `content://` URIs), gets a `files-picked` input and answers with
  `page.file-chooser-reply`.
- Pages draw at the screen's refresh rate: the Windows runtime passes the
  monitor's to `chorium_set_frame_rate`, the Android runtime asks for the
  display's fastest mode.

## Site data

The runtimes supply an engine profile directory before Chromium starts:
`%LOCALAPPDATA%\Chorium\profile` on Windows, and `files/profile` in the
Android app's private storage. Test profiles keep engine data underneath
their own `profile` subdirectory. Native embedders that do not call
`chorium_set_profile_dir` retain the memory-only behavior.

Cookies, localStorage, IndexedDB, service workers and site storage survive
restarts. The HTTP page cache stays in RAM; generated script disk caching
remains disabled. On Windows, Chromium's OSCrypt encrypts cookie values
with an AES key protected by the user's DPAPI login; the encrypted key is
in the profile's `Local State`. Android uses private app storage.

GPU shader disk caching is disabled too. Compression-dictionary transport is
disabled because Chromium otherwise creates a persistent dictionary cache;
normal gzip/Brotli compression remains available. Explicit site storage such
as CacheStorage remains persistent and is cleared with cookies/site data.

Settings → Privacy has **Clear cookies and site data**, independently of
history, plus **Clear when Chorium closes**, off by default. The latter
also clears the engine profile at the next start after a force-stop or
power loss. App settings, history and saved tabs are preserved. A page
that remains open can write fresh site data after a manual clear.

## Limits

- Sandbox: Android's page processes are isolated as in Chrome. On Windows
  only `Chorium.exe` turns it on; other programs using `chorium.dll` without
  `chorium_set_sandbox` (e.g. `chorium_test`) run helpers unsandboxed.
- Windows: `<select>` dropdowns draw inside the page (`appearance:
  base-select`, injected as a user stylesheet); date pickers still open as
  popup widgets that aren't in the captured frames.
- No colour picker yet: `<input type=color>` does nothing.
- One device scale for all views (`chorium_init`).
- `chorium_eval` runs in a private JavaScript world: it sees the DOM, not the
  page's own variables.
- Not in chorium.wasm yet: request blocking (needs an engine hook),
  per-site cookie controls, dialog UI (the sample accepts dialogs).
- `net.fetch` returns an error (not wired to Chorium's network service yet).
- The sample's text fields only edit at the end (no caret movement).
