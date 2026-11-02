# Android bootstrap

Android still enters through the manifest's `ChoriumApplication` and
`ChoriumActivity`. `ChoriumBootstrap` owns process-wide startup policy and
runtime ownership. All its methods run on Android's main thread; its static
state belongs to one process and holds no Activity after runtime release.

1. `Application.attachBaseContext` calls `attach(application)` once per
   process. It registers the context, identifies the browser by exact package/
   process-name equality, and selects the Chromium library process type.
   Child processes stop here; their command lines come from Chromium's child
   service connection. Only the browser configures isolated Java child services,
   paths, application status, and the two compatibility feature switches.
2. `Application.onCreate` calls `startServices()`. Network monitoring starts
   once in the browser process. An out-of-order call fails explicitly.
3. `Activity.onCreate` calls `prepareBrowser(activity)` before setting the
   native profile. Browser-only phase checks run first, followed by the mobile
   user agent and native library initialisation. Later Activities reuse that
   preparation. Native initialisation exceptions/linkage errors are logged
   under `ChoriumBootstrap`; failure returns to the Activity's existing
   startup-error message and finish path.
4. Once the window is ready, `startBrowser(callback)` starts Chromium through
   its existing async controller. The Activity owns its success/failure UI.
   `startApp()` ignores callbacks for finishing/destroyed Activities.
5. Before creating a PageHost and running browser.wasm, `claimRuntime(activity)`
   retires any previous Activity's runtime. Android can create the replacement
   before stopping/destroying the previous Activity. Old views stop sending
   focus/size callbacks and their PageHost is retired before WebContents close.
   `releaseRuntime(activity)` only stops the current owner, preventing delayed
   teardown of an old Activity from closing the replacement's pages.

The 120 fps and unsupported InstalledApp feature policies are preserved in
`disableFeature`, including existing switch entries and duplicate avoidance.
The renderer sandbox and Chromium bad-IPC validation remain enabled. Window
layout, refresh-mode requests and normal foreground/background behaviour
remain Activity responsibilities.

The real-app Activity relaunch test uses NEW_TASK | CLEAR_TASK without a
force-stop. It verifies that the PID is unchanged, pages navigate, localStorage
survives, and InstalledApp remains unsupported. It exposed the original
late-teardown bug and passes with explicit runtime ownership. The full suite
also exercises cold launches, address editing, Tor, popups and sandbox UIDs.

## Building this update

The normal GN target includes `ChoriumBootstrap.java`. The native Chromium
checkout/cache was deleted earlier, so this delivered APK uses a matching
Java-bytecode repackage of the verified Site-Tor APK. Run
`tools/patch_android_bootstrap.py <decoded-dir>` on that decoded version;
the helper checks its required compatibility methods and startup anchors,
and refuses a bootstrap that is already present. Then build with Apktool,
align, sign with the matching certificate, compare payloads and test. The
equivalent switch methods are retained in bytecode instead of compiling the
generic Java helper. No full GN/native build or Java Error Prone run is claimed.

Bootstrap APK:
`Chorium-157.0.8103.15-Android-arm64-20261102-Bootstrap.apk`.
SHA-256: `631f8fd1a1a5e03f059525b549506ee3029adb815e9e9b11a1858204c3ceacf4`.
Original certificate, 16 KB zip alignment and CRC verified. All 512 non-DEX
payload entries match the prior Site-Tor APK, including native libraries,
manifest, resources and browser.wasm. The Windows package is unchanged.

Final real Pixel suite: **32 tests, 30 pass, two desktop-only skips, zero
failures**, 164 seconds. Animation measures **120 fps**. The focused same-
process Activity relaunch test also passes independently. Logs are retained
under `C:\me-tor\Chorium\validation`. Testing used an isolated profile;
normal app data is preserved and the original screen setting is restored.
