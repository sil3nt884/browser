// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.app.Application;
import android.content.Context;

/**
 * Runs in every process of the app: the app's own (the runtime and the engine's browser side)
 * and Chromium's page, GPU and network processes, which are services with a ":" in their name.
 */
public class ChoriumApplication extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        ChoriumBootstrap.attach(this);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ChoriumBootstrap.startServices();
    }
}
