# Chorium

Chromium's page engine as an embeddable library, with Tor inside. No browser
UI, no settings, no command-line flags, no DevTools. Three parts, joined by
two versioned contracts:

```
 app.wasm ─┐  (your UI, any language that targets WASI 0.2)
           │  browser.wit   umbra:browser@0.2.0   (upper contract)
 chorium.wasm  portable browser layer: tabs, history, settings, URL handling
           │  chorium.wit   umbra:chorium@0.3.0   (lower contract)
 runtime       small native C host per platform: Wasmtime + window + input
           │  chorium.h     C ABI, CHORIUM_ABI_VERSION 2
 engine        chorium.dll / libchorium.so: Chromium //content (no //chrome),
               BGRA frames, JSON events, Tor (umbra_tor.dll, Arti), fail closed
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
| `runtime/win/` | Windows runtime (`build.cmd`, VS 2022 + Wasmtime 50 C API) |
| `portable/` | chorium.wasm source (C) |
| `apps/browser/` | sample app: Chrome-style browser (tabs in the title bar, menu, Settings, History) |
| `tests/engine/` | `chorium_test.c`: loads a page through Tor, saves a .bmp |
| `docker/` | build image and scripts (engine, wasm) |

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
   -e WIN_TOOLCHAIN_HASH=<hash> chorium-build:157 bash /umbra/chorium/docker/fetch.sh win`
5. Build: same, with `--cpus=18 --memory=10g -e JOBS=6 -e CHORIUM_CPU_COUNT=4`
   and `build.sh win` (see the header of `docker/build.sh` for the SDK
   mount). Blink's heaviest files need 1.5-2.5 GB each, so with 10 GB keep
   `JOBS` at 6; 10 ran out of memory. A full build is a few hours; after
   that, changing Chorium's own files rebuilds in about 3 minutes.

Builds are capped (`--cpus`, `--memory`) and run at the lowest priority
inside the container, so the PC stays usable.

## Apps and chorium.wasm

Built in the same Docker image; wasi-sdk 34, wit-bindgen 0.63 and wac 0.12
are unpacked in the volume at `/work/wasm-tools`.

- `docker run --rm -v chorium-work:/work -v <repo>:/umbra:ro -v <out>:/out
  chorium-build:157 bash /umbra/chorium/docker/build_wasm.sh app browser`
  builds `chorium.wasm` (world `portable`), the app (world `app`), and plugs
  them together with `wac plug` into `<out>/browser.wasm`, a component for
  the runtime's `runnable` world. `build_wasm.sh portable` builds only
  chorium.wasm.
- **Windows runtime**: `runtime/win/build.cmd [runtime dir] [wasmtime c-api
  dir]` (VS 2022, Wasmtime 50 C API) puts `Chorium.exe` and `wasmtime.dll`
  next to `chorium.dll`. Put apps in an `apps` folder next to it and
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
  (`%LOCALAPPDATA%\Chorium\apps\<name>`: chorium.wasm keeps
  `settings.txt` and `history.tsv` there). Settings keys: `search` (URL
  template with `%s`), `home`, `theme` (`system`, `light`, `dark`); the
  sample's Settings page edits them.

## Limits (first version)

- Page processes run without Chromium's sandbox. Linking the sandbox into
  the app's .exe (like CEF's `cef_sandbox.lib`) is the next step.
- `<select>` dropdowns and date pickers open as separate popup widgets and
  are not in the captured frames yet.
- One device scale for all views (`chorium_init`).
- `chorium_eval` runs in a private JavaScript world: it sees the DOM, not the
  page's own variables.
- Windows only so far; Android is next, from the same image and volume.
- Not in chorium.wasm yet: request blocking (needs an engine hook),
  cookie controls, downloads, dialog UI (the sample accepts dialogs).
- Windows runtime: `net.fetch` returns an error (not wired to Chorium's network
  service yet); no touch input; `show-keyboard` does nothing. No clipboard
  in the contract yet, so there is no paste in app text fields.
- The sample's text fields only edit at the end (no caret movement).
