# Hooks into Chromium

## packaged-vs-version

`build/vs_toolchain.py`, `GetVisualStudioVersion()`: honors
`CHORIUM_VS_VERSION` for a custom packaged SDK. Chorium's Docker scripts set
it to 2022 when `WIN_TOOLCHAIN_HASH` is supplied. This prevents Chromium's
VS 2026 default from repeatedly revalidating an installed VS 2022 package.
Unsupported versions fail explicitly; without the variable upstream behavior
is preserved. Apply just this build hook with `--only packaged-vs-version`.

Everything Umbra does lives in `//umbra`. Chromium itself is edited in a
handful of places by `scripts/hooks.py`. Each edit is marked in the source with
`[umbra] <hook-name>`, so `git grep "\[umbra\]"` in Chromium's `src/` shows
them all.

```
python3 scripts/hooks.py list                       # what and why
python3 scripts/hooks.py check --src <src>          # dry run after a sync
python3 scripts/hooks.py apply --src <src> --platform android|win
python3 scripts/hooks.py revert --src <src>         # git checkout touched files
```

If a **required** hook can't find its anchor, nothing is written and the
script names the hook. Find where the code moved in the new Chromium version,
update the `anchor` regex in `HOOKS` (`scripts/hooks.py`) and the matching
fixture in `scripts/tests/fixtures/`, then run `python3 scripts/tests/test_hooks.py`.

## startup
`chrome/app/chrome_main_delegate.cc`, first statement of
`ChromeMainDelegate::BasicStartupComplete()`.
Starts Tor and appends the proxy/lockdown switches. It must run before any
network context exists. Any function that runs early in the browser process,
before `content::ContentMain` creates the network service, will do.

## prefs
`chrome/browser/profiles/pref_service_builder_utils.cc`, end of
`RegisterProfilePrefs(bool is_signin_profile, ...)`, right after
`BrowserContextDependencyManager::...RegisterProfilePrefsForServices()`.
Changes pref *defaults*. It needs the prefs to be registered already; any
point after registration works. It used to sit in `browser_prefs.cc`, but
that runs before keyed services register theirs (e.g.
`plugins.always_open_pdf_externally`), so those were skipped. Unknown pref
names are logged and skipped.

## renderer
`chrome/renderer/chrome_content_renderer_client.cc`, start of
`ChromeContentRendererClient::RenderFrameCreated(content::RenderFrame*)`.
Attaches the preload/video injector to each frame.

## isolated-world-id
`chrome/common/chrome_isolated_world_ids.h`, adds
`ISOLATED_WORLD_ID_UMBRA_PRELOAD` before `ISOLATED_WORLD_ID_EXTENSIONS`.
It must come before the extensions entry, because extension world IDs count
up from that value.

## browser-deps / renderer-deps
`chrome/browser/BUILD.gn` (`source_set("browser")` since M157,
`static_library` before) and
`chrome/renderer/BUILD.gn` (`static_library("renderer")`): adds
`//umbra/app`, `//umbra/browser` and `//umbra/renderer` to `deps`.

## android-assets (Android only)
`chrome/android/BUILD.gn`, `deps` of `android_library("chrome_java")`.
Pulls in `//umbra/tor_core:runtime_assets`, which packages
`libumbra_tor.so` uncompressed at `assets/umbra/`. Any Java target that ends
up in `chrome_public_apk` would work.

## media-buffer / media-buffer-cap (optional)
`third_party/blink/renderer/platform/media/multi_buffer_data_source.h` and
`.cc`. For progressive video, raises the default of the
`preload_seconds_` Tuneable (`SrcMediaMultiBufferPreloadSeconds`, 10 → 60 s,
which is its own max) and `kMaxBufferPreload` (50 → 200 MB). Before M157 the
seconds target was `kTargetSecondsBufferedAhead` in the `.cc`. If either is
missing, video still works with stock buffering.

## win-installer (optional, Windows only)
`chrome/installer/mini_installer/chrome.release`. Ships `umbra_tor.dll` in
the version directory next to `chrome.dll`. It isn't needed when running from
the build directory.

## branding (optional)
`chrome/app/theme/chromium/BRANDING`. Sets the product name to "Umbra" in
window titles and the installer. The Android launcher label comes from
`chrome/app/chromium_strings.grd`, which isn't changed yet.
