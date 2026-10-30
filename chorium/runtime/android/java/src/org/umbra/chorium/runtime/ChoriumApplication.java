// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.app.Application;
import android.content.Context;

import org.chromium.base.ApplicationStatus;
import org.chromium.base.ContextUtils;
import org.chromium.base.PathUtils;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.net.NetworkChangeNotifier;
import org.chromium.ui.base.ResourceBundle;

/**
 * Runs in every process of the app: the app's own (the runtime and the engine's browser side)
 * and Chromium's page, GPU and network processes, which are services with a ":" in their name.
 */
public class ChoriumApplication extends Application {
    private static boolean isBrowserProcess() {
        return !ContextUtils.getProcessName().contains(":");
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        ContextUtils.initApplicationContext(this);
        ResourceBundle.setNoAvailableLocalePaks();
        LibraryLoader.getInstance().setLibraryProcessType(isBrowserProcess()
                        ? LibraryProcessType.PROCESS_BROWSER
                        : LibraryProcessType.PROCESS_CHILD);
        if (isBrowserProcess()) {
            PathUtils.setPrivateDataDirectorySuffix("chorium");
            ApplicationStatus.initialize(this);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        if (isBrowserProcess()) {
            NetworkChangeNotifier.init();
            NetworkChangeNotifier.setAutoDetectConnectivityState(true);
        }
    }
}
