// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
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
import java.io.IOException;
import java.io.InputStream;

/**
 * Chorium's runtime on Android: starts the engine (Chromium and Tor), then runs the app
 * (assets/apps/APP.wasm) with the shared runtime core. The app draws in an AppView that fills the
 * screen; the page it shows is a PageHost placed under it, seen through a hole in its drawing.
 */
public class ChoriumActivity extends Activity {
    /** The app in the APK's assets, as built by chorium/docker/build_wasm.sh. */
    private static final String APP = "browser";

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Chorium takes no command line from outside (no switch can turn Tor off).
        if (!CommandLine.isInitialized()) CommandLine.init(null);
        // As content shell: a phone gets Chrome's mobile user agent.
        DeviceUtils.updateDeviceSpecificUserAgentSwitch(this);
        LibraryLoader.getInstance().ensureInitialized();

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
        if (ChoriumNativesJni.get().start(wasm, APP, getFilesDir().getPath(), args)) {
            mRunning = true;
            mAppView.setStarted();
        }
    }

    private static @Nullable String urlFromIntent(Intent intent) {
        Uri data = intent != null ? intent.getData() : null;
        return data != null ? data.toString() : null;
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
