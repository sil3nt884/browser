# Chorium tests

Two layers, both run against the real code.

## `e2e/`: the real app, on Windows and on a phone

`Chorium.exe` or `Chorium.apk` started with its **remote control**
(`runtime/common/remote.h`): the app draws its own UI, which no
accessibility API can see, so the runtime records every text, icon and page
it draws and takes taps, long presses, swipes, keys and text, delivered
through the platform's real input path (window messages on Windows, touch
and key events into the views on Android). Page contents are checked through
DevTools (Windows: `127.0.0.1:9335`, on with `--remote-control`; Android:
the `chorium_devtools_remote` socket). A local server (`127.0.0.1:8765`,
reached from the phone with `adb reverse`) serves the test pages; tests
switch Tor off for them through the app's own menu, since loopback goes
through Tor otherwise.

```bash
python chorium/tests/e2e/run.py --platform win --dist <folder with Chorium.exe>
```

```bash
python chorium/tests/e2e/run.py --platform android --apk Chorium.apk
```

Options: `--network` also checks that pages go through Tor
(check.torproject.org), `-k <part of a test name>`, `--serial` for adb.
Python 3's standard library only. Failed tests leave a screenshot and a Windows host log in
`e2e-failures/`.

- Windows: its own profile (`--data-dir`, deleted afterwards), so your
  history and settings stay as they are. Downloads go to your Downloads
  folder under unique test names and are deleted again; the clipboard is used. The real Open dialog
  is filled in and confirmed.
- Android: over USB (`CHORIUM_ADB`, default the platform-tools in
  `C:\me-tor\chorium-tools`, `ADB_SERVER_PORT` 5038). The app runs with its
  own test profile (`data_dir` intent extra). HyperOS blocks taps from adb,
  so input comes through the remote control; the tests check that the
  system file picker opened, then answer it through the remote control.

What they check: the context menu on real text fields (desktop), links and
images (Copy, Paste, Cut, Select all, Save, Copy image, Open in new tab);
downloads (in the Downloads folder, a free name for a second copy, the
notice, the Downloads page, Open / Show in folder, cancel); uploads (picker,
file contents in the page, cancel); frame rate against the display's; the
sandbox (Windows: page processes at low integrity in a job; Android:
isolated users); and regressions: address bar, typing twice in a page's
search box (the DuckDuckGo crash), the Tor switch, Tor itself, navigator
.onLine, back, full screen, dark theme reaching pages, new tab and session
restore.

Site data checks cover restart persistence and manual/automatic clearing.
On Windows the stopped profile's cookie database is checked for encrypted
values and an empty plaintext column. Every scenario starts with clean test
state; restarts within a scenario keep that scenario's profile.

## `app/`: the app's logic, in seconds

`browser.wasm` (the sample app plugged into chorium.wasm) on the real
runtime core with a fake engine and screen: menus, notices, panels, the file
chooser flow, session restore, in a desktop and a phone layout. Docker:

```bash
docker run --rm -v chorium-work:/work -v <repo>:/umbra:ro chorium-build:157 bash /umbra/chorium/tests/app/run.sh
```

## `engine/chorium_test.c`

Loads one page through Tor with only `chorium.dll` and saves a picture of
it: the smallest check that the library works.
