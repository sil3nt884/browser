// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import org.jni_zero.NativeMethods;

/** Calls into the runtime's native side (jni/runtime_android.cc). Main thread only. */
final class ChoriumNatives {
    private ChoriumNatives() {}

    @NativeMethods
    interface Natives {
        /** Must run before Chromium creates its BrowserContext. */
        void profile(String path);
        /** Loads and starts the app; false if it couldn't (already reported). */
        boolean start(byte[] wasm, String appName, String filesDir, String[] args,
                boolean remoteControl);

        /** The surface's size in pixels, pixels per CSS pixel, and dark mode. */
        void surface(int widthPx, int heightPx, float density, boolean dark);

        void draw();
        /** The display list has reached Canvas and its touch regions are current. */
        void framePresented();

        /** x, y in CSS pixels; type 0 down, 1 move, 2 up, 3 cancel. */
        void pointer(int type, float x, float y);

        /** Windows virtual-key code (chorium.wit's `key`). */
        void key(boolean down, int code, int modifiers);

        void text(String text);

        void back();

        /** The file picker closed: paths or content:// URIs, none if cancelled. */
        void filesPicked(int request, String[] paths);

        void focus(boolean focused);

        void pause();

        void resume();

        void stop();
    }
}
