// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.app.Activity;
import android.app.DownloadManager;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Insets;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.MimeTypeMap;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.widget.FrameLayout;
import android.widget.Toast;

import org.chromium.base.CommandLine;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.build.annotations.Nullable;
import org.chromium.content_public.browser.BrowserStartupController;
import org.chromium.content_public.browser.BrowserStartupController.StartupMetrics;
import org.chromium.content_public.browser.DeviceUtils;
import org.chromium.ui.base.ActivityWindowAndroid;
import org.chromium.ui.base.IntentRequestTracker;

import org.umbra.chorium.engine.PageHost;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Chorium's runtime on Android: starts the engine (Chromium and Tor), then runs the app
 * (assets/apps/APP.wasm) with the shared runtime core. The app draws in an AppView that fills the
 * screen; the page it shows is a PageHost placed under it, seen through a hole in its drawing.
 */
public class ChoriumActivity extends Activity {
    /** The app in the APK's assets, as built by chorium/docker/build_wasm.sh. */
    private static final String APP = "browser";
    /** startActivityForResult code of the file picker. */
    private static final int REQUEST_PICK_FILES = 0x4317;

    private FrameLayout mRoot;
    private AppView mAppView;
    private PageHost mPageHost;
    private ActivityWindowAndroid mWindow;
    private IntentRequestTracker mIntentRequestTracker;
    private boolean mRunning;
    private boolean mFullscreen;
    /** The app has something to go back to (see setBackEnabled). */
    private boolean mBackEnabled;
    private @Nullable OnBackInvokedCallback mBackCallback;
    /** The app's request id for the open file picker, or -1. */
    private int mPickRequest = -1;
    private String mDataDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Chorium takes no command line from outside (no switch can turn Tor off).
        if (!CommandLine.isInitialized()) CommandLine.init(null);
        // As content shell: a phone gets Chrome's mobile user agent.
        DeviceUtils.updateDeviceSpecificUserAgentSwitch(this);
        LibraryLoader.getInstance().ensureInitialized();
        mDataDir = getFilesDir().getPath();
        String testProfile = getIntent() != null ? getIntent().getStringExtra("data_dir") : null;
        if (getIntent() != null && getIntent().getBooleanExtra("remote_control", false)
                && testProfile != null && testProfile.matches("[A-Za-z0-9_-]+")) {
            // Test extras can arrive through an exported activity. Keep every
            // fresh-profile deletion under a dedicated test-only directory.
            mDataDir += "/test-profiles/" + testProfile;
            if (getIntent().getBooleanExtra("data_dir_fresh", false)) {
                deleteTree(new File(mDataDir));
            }
            new File(mDataDir).mkdirs();
        }
        ChoriumNativesJni.get().profile(mDataDir + "/profile");

        mRoot = new FrameLayout(this);
        // Android 15+ draws apps edge to edge: keep the app and its pages
        // clear of the status and navigation bars, and of the keyboard.
        // (Android 10 never goes edge to edge.)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            mRoot.setOnApplyWindowInsetsListener((view, insets) -> {
                // Full screen (video) uses the whole display.
                int types = mFullscreen ? WindowInsets.Type.ime()
                                        : WindowInsets.Type.systemBars()
                                                | WindowInsets.Type.displayCutout()
                                                | WindowInsets.Type.ime();
                Insets bars = insets.getInsets(types);
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                return WindowInsets.CONSUMED;
            });
        }
        useFastestRefreshRate();
        mAppView = new AppView(this);
        mRoot.addView(mAppView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(mRoot);

        mIntentRequestTracker = IntentRequestTracker.createFromActivity(this);
        mWindow = new ActivityWindowAndroid(this, /* listenToActivityState= */ true,
                mIntentRequestTracker, /* insetObserver= */ null,
                /* occlusionTrackingAllowed= */ true);
        mIntentRequestTracker.restoreInstanceState(savedInstanceState);

        BrowserStartupController.getInstance().startBrowserProcessesAsync(
                LibraryProcessType.PROCESS_BROWSER, /* startGpuProcess= */ true,
                /* startMinimalBrowser= */ false, /* singleProcess= */ false,
                new BrowserStartupController.StartupCallback() {
                    @Override
                    public void onSuccess(@Nullable StartupMetrics metrics) {
                        startApp();
                    }

                    @Override
                    public void onFailure() {
                        Toast.makeText(ChoriumActivity.this, "The engine didn't start",
                                Toast.LENGTH_LONG).show();
                        finish();
                    }
                });
    }

    private void startApp() {
        mPageHost = new PageHost(this, mWindow);
        // Under the AppView, which leaves a hole where the page shows.
        mRoot.addView(mPageHost, 0, new FrameLayout.LayoutParams(1, 1));
        byte[] wasm;
        try {
            wasm = readAsset("apps/" + APP + ".wasm");
        } catch (IOException e) {
            Toast.makeText(this, "No app in the APK", Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        String url = urlFromIntent(getIntent());
        String[] args = url != null ? new String[] {APP + ".wasm", url}
                                    : new String[] {APP + ".wasm"};
        // The end-to-end tests' remote control (runtime/common/remote.h):
        // adb shell am start ... --ez remote_control true
        boolean remote = getIntent() != null
                && getIntent().getBooleanExtra("remote_control", false);
        // ... --es data_dir NAME: the app's files in their own folder (the
        // tests' profile, so they never touch the person's own).
        if (ChoriumNativesJni.get().start(wasm, APP, mDataDir, args, remote)) {
            mRunning = true;
            mAppView.setStarted();
        }
    }

    private static @Nullable String urlFromIntent(Intent intent) {
        Uri data = intent != null ? intent.getData() : null;
        return data != null ? data.toString() : null;
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        file.delete();
    }

    private byte[] readAsset(String name) throws IOException {
        try (InputStream in = getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[64 * 1024];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
            return out.toByteArray();
        }
    }

    /**
     * Many phones run apps at 60 Hz unless they ask for more: ask for the display's fastest
     * mode at its current resolution.
     */
    private void useFastestRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(params);
    }

    /** The MIME types for a page's accept list (".png", "image/*"). */
    private static List<String> mimeTypes(String[] accept) {
        List<String> types = new ArrayList<>();
        for (String type : accept) {
            if (type.startsWith(".")) {
                String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                        type.substring(1).toLowerCase(Locale.ROOT));
                if (mime != null) types.add(mime);
            } else if (type.contains("/")) {
                types.add(type);
            }
        }
        return types;
    }

    /** chorium.wit's file-chooser-mode: 0 open, 1 open-multiple, 2 open-folder, 3 save. */
    void pickFiles(int request, int mode, String[] accept, String defaultName) {
        if (mPickRequest >= 0) {
            ChoriumNativesJni.get().filesPicked(request, new String[0]);
            return;
        }
        Intent intent;
        List<String> types = mimeTypes(accept);
        if (mode == 3) {
            intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.setType(types.size() == 1 ? types.get(0) : "*/*");
            if (!defaultName.isEmpty()) intent.putExtra(Intent.EXTRA_TITLE, defaultName);
        } else {
            // A folder upload becomes a pick of several files: the system
            // folder picker hands out a tree that no page can read.
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            if (types.size() == 1) {
                intent.setType(types.get(0));
            } else {
                intent.setType("*/*");
                if (!types.isEmpty()) {
                    intent.putExtra(Intent.EXTRA_MIME_TYPES, types.toArray(new String[0]));
                }
            }
            if (mode != 0) intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        try {
            startActivityForResult(intent, REQUEST_PICK_FILES);
            mPickRequest = request;
        } catch (ActivityNotFoundException e) {
            ChoriumNativesJni.get().filesPicked(request, new String[0]);
        }
    }

    /** The remote control answers the open picker (tests); false if none is open. */
    boolean answerPicker(String[] uris) {
        if (mPickRequest < 0) return false;
        int request = mPickRequest;
        mPickRequest = -1;
        finishActivity(REQUEST_PICK_FILES);
        if (mRunning) ChoriumNativesJni.get().filesPicked(request, uris);
        return true;
    }

    private void onFilesPicked(int resultCode, @Nullable Intent data) {
        if (mPickRequest < 0) return; // already answered (answerPicker)
        int request = mPickRequest;
        mPickRequest = -1;
        List<String> uris = new ArrayList<>();
        if (resultCode == RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) {
                    uris.add(clip.getItemAt(i).getUri().toString());
                }
            } else if (data.getData() != null) {
                uris.add(data.getData().toString());
            }
        }
        if (mRunning) ChoriumNativesJni.get().filesPicked(request, uris.toArray(new String[0]));
    }

    /** Opens a download with its app, or shows the Downloads list. */
    void openFile(String path, boolean inFolder) {
        if (inFolder) {
            startQuietly(new Intent(DownloadManager.ACTION_VIEW_DOWNLOADS));
            return;
        }
        // The file's content:// URI comes from the media scanner, which also
        // makes sure the Files app lists it.
        MediaScannerConnection.scanFile(this, new String[] {path}, null, (scanned, uri) -> {
            if (uri == null) return;
            String extension =
                    MimeTypeMap.getFileExtensionFromUrl(Uri.fromFile(new File(scanned)).toString());
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                    extension.toLowerCase(Locale.ROOT));
            Intent view = new Intent(Intent.ACTION_VIEW);
            view.setDataAndType(uri, mime != null ? mime : "*/*");
            view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            runOnUiThread(() -> startQuietly(Intent.createChooser(view, null)));
        });
    }

    private void startQuietly(Intent intent) {
        try {
            startActivity(intent);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, "No app can open this", Toast.LENGTH_SHORT).show();
        }
    }

    /** Hides the status and navigation bars (a page's full-screen video). */
    void setFullscreen(boolean on) {
        if (on == mFullscreen) return;
        mFullscreen = on;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                if (on) {
                    controller.setSystemBarsBehavior(
                            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    controller.hide(WindowInsets.Type.systemBars());
                } else {
                    controller.show(WindowInsets.Type.systemBars());
                }
            }
            mRoot.requestApplyInsets();
        }
    }

    /**
     * Whether the back gesture goes to the app (it has a page or panel to go back from) or
     * leaves it. Android 13+ (predictive back) only asks the app while a callback is
     * registered; older versions call onBackPressed.
     */
    void setBackEnabled(boolean on) {
        if (on == mBackEnabled) return;
        mBackEnabled = on;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (mBackCallback == null) {
            mBackCallback = () -> {
                if (mRunning) ChoriumNativesJni.get().back();
            };
        }
        if (on) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, mBackCallback);
        } else {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(mBackCallback);
        }
    }

    @Override
    public void onBackPressed() {
        // Before Android 13: the app decides (close a panel, go back in the
        // tab, ...) when it has something to go back to.
        if (mRunning && mBackEnabled) {
            ChoriumNativesJni.get().back();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (mPageHost != null) mPageHost.setActivityVisible(true);
    }

    @Override
    protected void onStop() {
        super.onStop();
        // As content shell: the page stops rendering while the app isn't seen.
        if (mPageHost != null) mPageHost.setActivityVisible(false);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (mRunning) ChoriumNativesJni.get().pause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mRunning) ChoriumNativesJni.get().resume();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        mIntentRequestTracker.saveInstanceState(outState);
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_PICK_FILES) {
            onFilesPicked(resultCode, data);
            return;
        }
        mIntentRequestTracker.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    protected void onDestroy() {
        if (mRunning) {
            ChoriumNativesJni.get().stop();
            mRunning = false;
        }
        if (mPageHost != null) mPageHost.destroy();
        if (mWindow != null) mWindow.destroy();
        super.onDestroy();
    }
}
