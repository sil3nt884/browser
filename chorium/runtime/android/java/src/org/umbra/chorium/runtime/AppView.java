// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

package org.umbra.chorium.runtime;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

import org.jni_zero.CalledByNative;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * The app's surface on Android. The app draws through chorium.wit's `surface`; the native side
 * records that as a display list (CSS pixels) and hands it over on present, and this view replays
 * it on a Canvas. Touches and the soft keyboard go back to the app as chorium.wit input. Pages are
 * separate native views (PageHost) under this one: where the app shows a page the display list
 * clears a hole, and touches there go to the page unless the app drew over it since (a menu).
 */
public class AppView extends View {
    private static AppView sInstance;

    // Display list operations; jni/runtime_android.cc writes the same numbers.
    private static final int OP_CLEAR = 1;
    private static final int OP_FILL_RECT = 2;
    private static final int OP_STROKE_RECT = 3;
    private static final int OP_ROUND_RECT = 4;
    private static final int OP_TEXT = 5;
    private static final int OP_ICON = 6;
    private static final int OP_CLIP = 7;
    private static final int OP_UNCLIP = 8;
    private static final int OP_HOLE = 9;

    // Material Icons code points, in the order of chorium.wit's `icon` enum.
    private static final String[] ICONS = {
        "", // back (arrow_back)
        "", // forward (arrow_forward)
        "", // reload (refresh)
        "", // stop (close)
        "", // close
        "", // add
        "", // menu (more_vert)
        "", // search
        "", // lock
        "", // info
        "", // page (public)
        "", // settings
        "", // history
        "", // home
        "", // check
        "", // minimize
        "", // maximize (crop_square)
        "", // restore (filter_none)
        "", // close-window
    };

    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mText = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint mIcon = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint.FontMetrics mMetrics = new Paint.FontMetrics();
    private final Paint mClear = new Paint();
    private final RectF mRect = new RectF();
    /** Where this frame shows the page (CSS pixels), or empty. */
    private final RectF mHole = new RectF();
    /** Fills drawn after the hole: touches on them are the app's. */
    private final ArrayList<RectF> mCovers = new ArrayList<>();
    private final float mDensity;
    private byte[] mOps = new byte[0];
    private boolean mStarted;
    /** Text the keyboard is composing, already sent to the app as typed text. */
    private String mComposing = "";

    public AppView(Context context) {
        super(context);
        mDensity = getResources().getDisplayMetrics().density;
        mStroke.setStyle(Paint.Style.STROKE);
        mClear.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.CLEAR));
        mIcon.setTypeface(
                Typeface.createFromAsset(context.getAssets(), "fonts/MaterialIcons-Regular.ttf"));
        setFocusable(true);
        setFocusableInTouchMode(true);
        sInstance = this;
    }

    /** The app is running: tell it about the surface and let it draw. */
    void setStarted() {
        mStarted = true;
        sendSurface();
    }

    private boolean isDark() {
        int night = getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return night == Configuration.UI_MODE_NIGHT_YES;
    }

    private void sendSurface() {
        if (mStarted && getWidth() > 0) {
            ChoriumNativesJni.get().surface(getWidth(), getHeight(), mDensity, isDark());
        }
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        sendSurface();
    }

    @Override
    protected void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        sendSurface(); // dark mode may have changed
    }

    // --- drawing -------------------------------------------------------------

    @Override
    protected void onDraw(Canvas canvas) {
        ByteBuffer ops = ByteBuffer.wrap(mOps).order(ByteOrder.LITTLE_ENDIAN);
        mHole.setEmpty();
        mCovers.clear();
        int base = canvas.save();
        canvas.scale(mDensity, mDensity); // the app draws in CSS pixels
        while (ops.remaining() >= 4) {
            int op = ops.getInt();
            switch (op) {
                case OP_CLEAR:
                    canvas.drawColor(ops.getInt());
                    break;
                case OP_FILL_RECT: {
                    readRect(ops);
                    mFill.setColor(ops.getInt());
                    canvas.drawRect(mRect, mFill);
                    cover();
                    break;
                }
                case OP_STROKE_RECT: {
                    readRect(ops);
                    mStroke.setColor(ops.getInt());
                    float width = ops.getFloat();
                    mStroke.setStrokeWidth(width);
                    mRect.inset(width / 2, width / 2);
                    canvas.drawRect(mRect, mStroke);
                    break;
                }
                case OP_ROUND_RECT: {
                    readRect(ops);
                    float radius = ops.getFloat();
                    mFill.setColor(ops.getInt());
                    canvas.drawRoundRect(mRect, radius, radius, mFill);
                    cover();
                    break;
                }
                case OP_TEXT: {
                    float x = ops.getInt();
                    float y = ops.getInt();
                    mText.setTextSize(ops.getFloat());
                    mText.setColor(ops.getInt());
                    canvas.drawText(readString(ops), x, y, mText);
                    break;
                }
                case OP_ICON: {
                    readRect(ops);
                    int icon = ops.getInt();
                    mIcon.setColor(ops.getInt());
                    if (icon >= 0 && icon < ICONS.length) drawIcon(canvas, ICONS[icon]);
                    break;
                }
                case OP_CLIP:
                    readRect(ops);
                    canvas.save();
                    canvas.clipRect(mRect);
                    break;
                case OP_UNCLIP:
                    if (canvas.getSaveCount() > base + 1) canvas.restore();
                    break;
                case OP_HOLE:
                    readRect(ops);
                    canvas.drawRect(mRect, mClear);
                    mHole.set(mRect);
                    mCovers.clear();
                    break;
                default:
                    ops.position(ops.limit()); // unknown: stop
                    break;
            }
        }
        canvas.restoreToCount(base);
    }

    private void cover() {
        if (!mHole.isEmpty() && RectF.intersects(mHole, mRect)) mCovers.add(new RectF(mRect));
    }

    /** Whether a touch at (x, y), CSS pixels, belongs to the page under this view. */
    private boolean onPage(float x, float y) {
        if (!mHole.contains(x, y)) return false;
        for (RectF r : mCovers) {
            if (r.contains(x, y)) return false;
        }
        return true;
    }

    private void readRect(ByteBuffer ops) {
        float x = ops.getInt();
        float y = ops.getInt();
        float w = ops.getInt();
        float h = ops.getInt();
        mRect.set(x, y, x + w, y + h);
    }

    /** |ops| wraps mOps, so the string's bytes are read from mOps directly. */
    private String readString(ByteBuffer ops) {
        int len = ops.getInt();
        String s = new String(mOps, ops.position(), len, StandardCharsets.UTF_8);
        ops.position(ops.position() + len);
        return s;
    }

    /** An icon centred in mRect, as big as its smaller side. */
    private void drawIcon(Canvas canvas, String glyph) {
        mIcon.setTextSize(Math.min(mRect.width(), mRect.height()));
        mIcon.getFontMetrics(mMetrics);
        float x = mRect.centerX() - mIcon.measureText(glyph) / 2;
        float y = mRect.centerY() - (mMetrics.ascent + mMetrics.descent) / 2;
        canvas.drawText(glyph, x, y, mIcon);
    }

    @CalledByNative
    private static void present(byte[] ops) {
        if (sInstance == null) return;
        sInstance.mOps = ops;
        sInstance.invalidate();
    }

    /** Width in the high 32 bits, height in the low, both CSS pixels. */
    @CalledByNative
    private static long measureText(String text, float size) {
        if (sInstance == null) return 0;
        Paint paint = sInstance.mText;
        paint.setTextSize(size);
        paint.getFontMetrics(sInstance.mMetrics);
        long w = (long) Math.ceil(paint.measureText(text));
        long h = (long) Math.ceil(sInstance.mMetrics.descent - sInstance.mMetrics.ascent);
        return (w << 32) | (h & 0xFFFFFFFFL);
    }

    @CalledByNative
    private static void showKeyboard(boolean visible) {
        AppView view = sInstance;
        if (view == null) return;
        InputMethodManager imm =
                (InputMethodManager) view.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (visible) {
            view.requestFocus();
            view.mComposing = "";
            imm.restartInput(view);
            imm.showSoftInput(view, 0);
        } else if (view.hasFocus()) {
            // Not when the page has focus: the keyboard may be the page's.
            imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
        }
    }

    @CalledByNative
    private static void setFullscreen(boolean on) {
        if (sInstance != null && sInstance.getContext() instanceof ChoriumActivity) {
            ((ChoriumActivity) sInstance.getContext()).setFullscreen(on);
        }
    }

    @CalledByNative
    private static void setBackEnabled(boolean on) {
        if (sInstance != null && sInstance.getContext() instanceof ChoriumActivity) {
            ((ChoriumActivity) sInstance.getContext()).setBackEnabled(on);
        }
    }

    @CalledByNative
    private static void copyText(String text) {
        if (sInstance == null) return;
        ClipboardManager clipboard = (ClipboardManager) sInstance.getContext().getSystemService(
                Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("text", text));
    }

    @CalledByNative
    private static void setTitle(String title) {
        if (sInstance != null && sInstance.getContext() instanceof Activity) {
            ((Activity) sInstance.getContext()).setTitle(title);
        }
    }

    @CalledByNative
    private static void crashed(String message) {
        if (sInstance == null) return;
        Toast.makeText(sInstance.getContext(), "The app stopped: " + message, Toast.LENGTH_LONG)
                .show();
        if (sInstance.getContext() instanceof Activity) {
            ((Activity) sInstance.getContext()).finish();
        }
    }

    // --- input ---------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!mStarted) return false;
        int type;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                // Not taking the down leaves the whole gesture to the page.
                if (onPage(event.getX() / mDensity, event.getY() / mDensity)) return false;
                type = 0;
                requestFocus();
                break;
            case MotionEvent.ACTION_MOVE:
                type = 1;
                break;
            case MotionEvent.ACTION_UP:
                type = 2;
                break;
            case MotionEvent.ACTION_CANCEL:
                type = 3;
                break;
            default:
                return true;
        }
        ChoriumNativesJni.get().pointer(type, event.getX() / mDensity, event.getY() / mDensity);
        return true;
    }

    @Override
    protected void onFocusChanged(boolean gainFocus, int direction, android.graphics.Rect previous) {
        super.onFocusChanged(gainFocus, direction, previous);
        if (mStarted) ChoriumNativesJni.get().focus(gainFocus);
    }

    /** Android key code -> Windows virtual-key code (chorium.wit), or 0. */
    private static int virtualKey(int keyCode) {
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z) {
            return 'A' + (keyCode - KeyEvent.KEYCODE_A);
        }
        if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9) {
            return '0' + (keyCode - KeyEvent.KEYCODE_0);
        }
        switch (keyCode) {
            case KeyEvent.KEYCODE_DEL: return 0x08;
            case KeyEvent.KEYCODE_TAB: return 0x09;
            case KeyEvent.KEYCODE_ENTER: return 0x0D;
            case KeyEvent.KEYCODE_ESCAPE: return 0x1B;
            case KeyEvent.KEYCODE_SPACE: return 0x20;
            case KeyEvent.KEYCODE_MOVE_END: return 0x23;
            case KeyEvent.KEYCODE_MOVE_HOME: return 0x24;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x25;
            case KeyEvent.KEYCODE_DPAD_UP: return 0x26;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x27;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x28;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0x2E;
            case KeyEvent.KEYCODE_F5: return 0x74;
            default: return 0;
        }
    }

    private static int modifiers(KeyEvent event) {
        int mods = 0;
        if (event.isShiftPressed()) mods |= 1;
        if (event.isCtrlPressed()) mods |= 2;
        if (event.isAltPressed()) mods |= 4;
        if (event.isMetaPressed()) mods |= 8;
        return mods;
    }

    private boolean sendKey(KeyEvent event, boolean down) {
        int vk = virtualKey(event.getKeyCode());
        if (!mStarted || vk == 0) return false;
        ChoriumNativesJni.get().key(down, vk, modifiers(event));
        if (down && !event.isCtrlPressed() && !event.isAltPressed()) {
            int unicode = event.getUnicodeChar();
            if (unicode >= 0x20) ChoriumNativesJni.get().text(new String(Character.toChars(unicode)));
        }
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return sendKey(event, true) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return sendKey(event, false) || super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        outAttrs.imeOptions = EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                replaceComposing(text.toString());
                mComposing = "";
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                replaceComposing(text.toString());
                return true;
            }

            @Override
            public boolean finishComposingText() {
                mComposing = "";
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                for (int i = 0; i < beforeLength; i++) tapKey(0x08);
                return true;
            }

            @Override
            public boolean performEditorAction(int actionCode) {
                tapKey(0x0D);
                return true;
            }
        };
    }

    /** The app has no composing text: send the difference as backspaces and text. */
    private void replaceComposing(String text) {
        if (!mStarted) return;
        int common = 0;
        int max = Math.min(text.length(), mComposing.length());
        while (common < max && text.charAt(common) == mComposing.charAt(common)) common++;
        for (int i = common; i < mComposing.length(); i++) tapKey(0x08);
        if (common < text.length()) ChoriumNativesJni.get().text(text.substring(common));
        mComposing = text;
    }

    private void tapKey(int vk) {
        if (!mStarted) return;
        ChoriumNativesJni.get().key(true, vk, 0);
        ChoriumNativesJni.get().key(false, vk, 0);
    }
}
