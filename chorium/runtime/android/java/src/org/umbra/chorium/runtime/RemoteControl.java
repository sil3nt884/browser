// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;

import org.jni_zero.CalledByNative;

/**
 * The remote control's Android side (runtime/common/remote.h): input from the end-to-end tests
 * goes into the activity's views as real touch and key events, the way the system delivers them,
 * so pages get Chromium's own gesture handling (long press, scrolling). Main thread only.
 */
final class RemoteControl {
    private RemoteControl() {}

    // remote_input_kind_t in remote.h.
    private static final int DOWN = 0;
    private static final int MOVE = 1;
    private static final int UP = 2;
    private static final int KEY_DOWN = 6;
    private static final int KEY_UP = 7;
    private static final int TEXT = 8;
    private static final int BACK = 9;

    private static long sDownTime;

    private static ChoriumActivity activity() {
        AppView view = AppView.instance();
        return view != null && view.getContext() instanceof ChoriumActivity
                ? (ChoriumActivity) view.getContext()
                : null;
    }

    /** x, y: CSS pixels on the app's surface. */
    @CalledByNative
    private static void input(int kind, int x, int y, int code, int mods, String text) {
        ChoriumActivity activity = activity();
        if (activity == null) return;
        View root = activity.getWindow().getDecorView();
        long now = SystemClock.uptimeMillis();
        switch (kind) {
            case DOWN:
            case MOVE:
            case UP: {
                AppView view = AppView.instance();
                int[] at = new int[2];
                view.getLocationInWindow(at);
                float density = view.getResources().getDisplayMetrics().density;
                if (kind == DOWN) sDownTime = now;
                MotionEvent.PointerProperties[] props = {new MotionEvent.PointerProperties()};
                props[0].id = 0;
                props[0].toolType = MotionEvent.TOOL_TYPE_FINGER;
                MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
                coords[0].x = at[0] + x * density;
                coords[0].y = at[1] + y * density;
                coords[0].pressure = 1;
                coords[0].size = 1;
                int action = kind == DOWN ? MotionEvent.ACTION_DOWN
                        : kind == MOVE    ? MotionEvent.ACTION_MOVE
                                          : MotionEvent.ACTION_UP;
                MotionEvent event = MotionEvent.obtain(sDownTime, now, action, 1, props, coords,
                        0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
                root.dispatchTouchEvent(event);
                event.recycle();
                break;
            }
            case KEY_DOWN:
            case KEY_UP: {
                int keyCode = androidKey(code);
                if (keyCode == 0) return;
                int meta = ((mods & 1) != 0 ? KeyEvent.META_SHIFT_ON : 0)
                        | ((mods & 2) != 0 ? KeyEvent.META_CTRL_ON : 0)
                        | ((mods & 4) != 0 ? KeyEvent.META_ALT_ON : 0);
                root.dispatchKeyEvent(new KeyEvent(now, now,
                        kind == KEY_DOWN ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, keyCode, 0,
                        meta, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
                        InputDevice.SOURCE_KEYBOARD));
                break;
            }
            case TEXT: {
                View focused = activity.getCurrentFocus();
                if (focused instanceof AppView) {
                    // As the on-screen keyboard does.
                    focused.onCreateInputConnection(new EditorInfo()).commitText(text, 1);
                } else {
                    KeyEvent[] events = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD)
                            .getEvents(text.toCharArray());
                    if (events == null) return;
                    for (KeyEvent event : events) root.dispatchKeyEvent(event);
                }
                break;
            }
            case BACK:
                activity.onBackPressed();
                break;
            default:
                break;
        }
    }

    /** Windows virtual-key code -> Android key code, or 0. */
    private static int androidKey(int vk) {
        if (vk >= 'A' && vk <= 'Z') return KeyEvent.KEYCODE_A + (vk - 'A');
        if (vk >= '0' && vk <= '9') return KeyEvent.KEYCODE_0 + (vk - '0');
        switch (vk) {
            case 0x08: return KeyEvent.KEYCODE_DEL;
            case 0x09: return KeyEvent.KEYCODE_TAB;
            case 0x0D: return KeyEvent.KEYCODE_ENTER;
            case 0x1B: return KeyEvent.KEYCODE_ESCAPE;
            case 0x20: return KeyEvent.KEYCODE_SPACE;
            case 0x25: return KeyEvent.KEYCODE_DPAD_LEFT;
            case 0x26: return KeyEvent.KEYCODE_DPAD_UP;
            case 0x27: return KeyEvent.KEYCODE_DPAD_RIGHT;
            case 0x28: return KeyEvent.KEYCODE_DPAD_DOWN;
            case 0x2E: return KeyEvent.KEYCODE_FORWARD_DEL;
            default: return 0;
        }
    }

    @CalledByNative
    private static boolean answerPicker(String[] uris) {
        ChoriumActivity activity = activity();
        return activity != null && activity.answerPicker(uris);
    }
}
