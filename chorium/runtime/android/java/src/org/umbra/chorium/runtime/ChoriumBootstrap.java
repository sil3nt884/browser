// Copyright 2026 The Umbra Authors. BSD-style license; see LICENSE.
package org.umbra.chorium.runtime;

import android.app.Activity;
import android.app.Application;

import org.chromium.base.ApplicationStatus;
import org.chromium.base.CommandLine;
import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.PathUtils;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.build.annotations.Nullable;
import org.chromium.content_public.browser.BrowserStartupController;
import org.chromium.content_public.browser.ChildProcessCreationParams;
import org.chromium.content_public.browser.DeviceUtils;
import org.chromium.net.NetworkChangeNotifier;
import org.chromium.ui.base.ResourceBundle;

/** Android entry-point policy. Called on the main thread; static state is per process. */
final class ChoriumBootstrap {
    private static final String TAG = "ChoriumBootstrap";
    private static boolean sAttached;
    private static boolean sBrowserProcess;
    private static boolean sServicesStarted;
    private static boolean sBrowserPrepared;
    private static @Nullable ChoriumActivity sRuntimeOwner;

    private ChoriumBootstrap() {}

    /** Earliest app-owned entry: runs before any Activity or child service. */
    static void attach(Application application) {
        if (sAttached) return;
        ContextUtils.initApplicationContext(application);
        sBrowserProcess = application.getPackageName().equals(ContextUtils.getProcessName());
        ResourceBundle.setNoAvailableLocalePaks();
        LibraryLoader.getInstance().setLibraryProcessType(sBrowserProcess
                ? LibraryProcessType.PROCESS_BROWSER : LibraryProcessType.PROCESS_CHILD);
        if (sBrowserProcess) {
            // Native-only services bypass the JNI_OnLoad hook this embedder requires.
            // Select the Java child services explicitly; their sandbox stays enabled.
            ChildProcessCreationParams.set(application.getPackageName(),
                    application.getPackageName(), /* isExternalSandboxedService= */ false,
                    LibraryProcessType.PROCESS_CHILD, /* bindToCallerCheck= */ false,
                    /* ignoreVisibilityForImportance= */ false,
                    /* forceNativeSandboxedService= */ false);
            PathUtils.setPrivateDataDirectorySuffix("chorium");
            ApplicationStatus.initialize(application);
            // Policy must be set before any native browser/child startup. Child
            // command lines arrive from Chromium's service connection instead.
            if (!CommandLine.isInitialized()) CommandLine.init(null);
            // Let page animations use high refresh rate displays.
            disableFeature("disable-features", "ThrottleMainFrameTo60Hz");
            // No InstalledAppProvider binder is implemented by this embedder.
            disableFeature("disable-blink-features", "InstalledApp");
        }
        sAttached = true;
    }

    /** Application.onCreate: process-wide browser services, once. */
    static void startServices() {
        if (!sAttached) throw new IllegalStateException("Bootstrap not attached");
        if (!sBrowserProcess || sServicesStarted) return;
        NetworkChangeNotifier.init();
        NetworkChangeNotifier.setAutoDetectConnectivityState(true);
        sServicesStarted = true;
    }

    /** Activity entry: load the native browser once, before setting its profile. */
    static boolean prepareBrowser(Activity activity) {
        if (!sAttached || !sBrowserProcess || !sServicesStarted) {
            Log.e(TAG, "Browser preparation called outside browser startup");
            return false;
        }
        if (sBrowserPrepared) return true;
        try {
            DeviceUtils.updateDeviceSpecificUserAgentSwitch(activity);
            LibraryLoader.getInstance().ensureInitialized();
            sBrowserPrepared = true;
            return true;
        } catch (RuntimeException | LinkageError error) {
            Log.e(TAG, "Native browser initialization failed", error);
            return false;
        }
    }

    /** The Activity supplies its lifecycle-bound success/failure callbacks. */
    static void startBrowser(BrowserStartupController.StartupCallback callback) {
        if (!sBrowserPrepared) throw new IllegalStateException("Browser not prepared");
        BrowserStartupController.getInstance().startBrowserProcessesAsync(
                LibraryProcessType.PROCESS_BROWSER, /* startGpuProcess= */ true,
                /* startMinimalBrowser= */ false, /* singleProcess= */ false, callback);
    }

    /** Android can create a replacement Activity before destroying the old one. */
    static void claimRuntime(ChoriumActivity activity) {
        if (!sBrowserPrepared) throw new IllegalStateException("Browser not prepared");
        if (sRuntimeOwner != null && sRuntimeOwner != activity) {
            sRuntimeOwner.stopRuntime();
        }
        sRuntimeOwner = activity;
    }

    static void releaseRuntime(ChoriumActivity activity) {
        if (sRuntimeOwner != activity) return;
        sRuntimeOwner = null;
        activity.stopRuntime();
    }

    private static void disableFeature(String switchName, String feature) {
        CommandLine commandLine = CommandLine.getInstance();
        String disabled = commandLine.getSwitchValue(switchName);
        if (disabled == null || disabled.isEmpty()) {
            disabled = feature;
        } else if (!("," + disabled + ",").contains("," + feature + ",")) {
            disabled += "," + feature;
        }
        commandLine.appendSwitchWithValue(switchName, disabled);
    }
}
