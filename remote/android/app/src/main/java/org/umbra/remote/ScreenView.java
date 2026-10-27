// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import org.json.JSONArray;
import org.json.JSONException;

import java.util.ArrayDeque;

/**
 * Shows the PC's page and turns touches and typing into the agent's input
 * messages. Coordinates are sent in the page's CSS pixels.
 */
public final class ScreenView extends View {
    interface Host {
        /** A frame was put on screen; the agent may send the next one. */
        void onFrameShown();
        void onSizeChanged(int widthPx, int heightPx);
        void onTouch(String kind, JSONArray points);
        void onText(String text);
        void onKey(String key);
    }

    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect src = new Rect(), dst = new Rect();
    /** Spare bitmaps for decoding into, so frames don't churn the heap. */
    private final ArrayDeque<Bitmap> spare = new ArrayDeque<>();
    private Host host;
    private Bitmap frame;
    /** The page's viewport in CSS px, as the agent reports it. */
    private float pageW, pageH;
    private long lastMove;

    public ScreenView(Context context, AttributeSet attrs) {
        super(context, attrs);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    void setHost(Host host) {
        this.host = host;
    }

    void setPageSize(float w, float h) {
        pageW = w;
        pageH = h;
    }

    // ---- frames -----------------------------------------------------------

    /** Network thread: decodes a JPEG, reusing a spare bitmap when it fits. */
    Bitmap decode(byte[] data, int offset, int length) {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inMutable = true;
        o.inPreferredConfig = Bitmap.Config.ARGB_8888;
        synchronized (spare) {
            o.inBitmap = spare.poll();
        }
        try {
            return BitmapFactory.decodeByteArray(data, offset, length, o);
        } catch (IllegalArgumentException e) {
            // The spare didn't fit (the size changed): decode fresh.
            o.inBitmap = null;
            return BitmapFactory.decodeByteArray(data, offset, length, o);
        }
    }

    /** Main thread. */
    void show(Bitmap b) {
        if (b == null) {
            if (host != null) host.onFrameShown();
            return;
        }
        Bitmap old = frame;
        frame = b;
        if (old != null && old != b) {
            synchronized (spare) {
                if (spare.size() < 2) spare.add(old);
            }
        }
        // Ack now rather than after the next vsync: the agent keeps a few
        // frames in flight, so this only shaves latency, it can't flood us.
        if (host != null) host.onFrameShown();
        postInvalidateOnAnimation();
    }

    boolean hasFrame() {
        return frame != null;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        Bitmap b = frame;
        if (b != null) {
            src.set(0, 0, b.getWidth(), b.getHeight());
            fitFrame(b.getWidth(), b.getHeight());
            canvas.drawBitmap(b, src, dst, paint);
        }
    }

    /**
     * Where a frame of this size is drawn: the whole view normally. While the
     * phone rotates, the last frame still has the old shape until the PC
     * sends one at the new size, so it is letterboxed instead of stretched.
     */
    private void fitFrame(int fw, int fh) {
        int vw = getWidth(), vh = getHeight();
        float frameAspect = (float) fw / Math.max(1, fh), viewAspect = (float) vw / Math.max(1, vh);
        if (Math.abs(frameAspect - viewAspect) < 0.02f * viewAspect) {
            dst.set(0, 0, vw, vh);
        } else if (frameAspect > viewAspect) {
            int h = Math.round(vw / frameAspect);
            dst.set(0, (vh - h) / 2, vw, (vh + h) / 2);
        } else {
            int w = Math.round(vh * frameAspect);
            dst.set((vw - w) / 2, 0, (vw + w) / 2, vh);
        }
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (host != null && w > 0 && h > 0) host.onSizeChanged(w, h);
    }

    // ---- touch ------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (host == null) return false;
        int action = e.getActionMasked();
        String kind;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                kind = "start";
                break;
            case MotionEvent.ACTION_MOVE:
                // Phones report moves at up to 240 Hz; ~120 is plenty.
                long now = SystemClock.uptimeMillis();
                if (now - lastMove < 8) return true;
                lastMove = now;
                kind = "move";
                break;
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
                kind = "end";
                break;
            case MotionEvent.ACTION_CANCEL:
                host.onTouch("cancel", new JSONArray());
                return true;
            default:
                return true;
        }
        // The points still down after this event (a lifted finger is left out).
        boolean lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP;
        JSONArray points = new JSONArray();
        // Map onto the frame as drawn. Until the agent reports the page size,
        // assume the size we asked for (this view in dp).
        float density = getResources().getDisplayMetrics().density;
        Bitmap b = frame;
        if (b != null) fitFrame(b.getWidth(), b.getHeight());
        else dst.set(0, 0, getWidth(), getHeight());
        float dw = Math.max(1, dst.width()), dh = Math.max(1, dst.height());
        float sx = pageW > 0 ? pageW / dw : 1 / density;
        float sy = pageH > 0 ? pageH / dh : 1 / density;
        try {
            for (int i = 0; i < e.getPointerCount(); i++) {
                if (lifting && i == e.getActionIndex()) continue;
                points.put(new JSONArray()
                        .put(Math.round((e.getX(i) - dst.left) * sx * 10) / 10.0)
                        .put(Math.round((e.getY(i) - dst.top) * sy * 10) / 10.0)
                        .put(e.getPointerId(i)));
            }
        } catch (JSONException ignored) {
        }
        host.onTouch(kind, points);
        return true;
    }

    // ---- keyboard ---------------------------------------------------------
    //
    // The soft keyboard edits a private buffer; every change is diffed against
    // what was already sent and replayed on the page as Backspaces plus
    // inserted text. That keeps autocorrect, suggestions and swipe typing
    // working, since the keyboard sees a normal text field.

    private RemoteInput input;
    private String sent = "";

    void showKeyboard() {
        requestFocus();
        resetInput();
        InputMethodManager imm = getContext().getSystemService(InputMethodManager.class);
        if (imm != null) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
    }

    void hideKeyboard() {
        InputMethodManager imm = getContext().getSystemService(InputMethodManager.class);
        if (imm != null) imm.hideSoftInputFromWindow(getWindowToken(), 0);
    }

    boolean keyboardShown() {
        return getRootWindowInsets() != null && android.os.Build.VERSION.SDK_INT >= 30
                && getRootWindowInsets().isVisible(android.view.WindowInsets.Type.ime());
    }

    /** Starts the keyboard on an empty buffer (e.g. after Enter). */
    private void resetInput() {
        sent = "";
        if (input != null) input.getEditable().clear();
        InputMethodManager imm = getContext().getSystemService(InputMethodManager.class);
        if (imm != null) imm.restartInput(this);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
        out.imeOptions = EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        out.initialSelStart = out.initialSelEnd = 0;
        input = new RemoteInput(this);
        sent = "";
        return input;
    }

    /** Sends whatever changed in the keyboard buffer since the last call. */
    private void sync() {
        if (host == null || input == null) return;
        String now = input.getEditable().toString();
        int i = 0;
        while (i < sent.length() && i < now.length() && sent.charAt(i) == now.charAt(i)) i++;
        for (int n = sent.length() - i; n > 0; n--) host.onKey("Backspace");
        int nl = now.indexOf('\n', i);
        String added = now.substring(i, nl < 0 ? now.length() : nl);
        if (!added.isEmpty()) host.onText(added);
        if (nl >= 0) {
            host.onKey("Enter");
            post(this::resetInput);
            return;
        }
        sent = now;
    }

    private boolean key(int code, KeyEvent e) {
        String k;
        switch (code) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                k = "Enter";
                break;
            case KeyEvent.KEYCODE_DEL:
                k = "Backspace";
                break;
            case KeyEvent.KEYCODE_FORWARD_DEL:
                k = "Delete";
                break;
            case KeyEvent.KEYCODE_TAB:
                k = "Tab";
                break;
            case KeyEvent.KEYCODE_ESCAPE:
                k = "Escape";
                break;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                k = "ArrowLeft";
                break;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                k = "ArrowRight";
                break;
            case KeyEvent.KEYCODE_DPAD_UP:
                k = "ArrowUp";
                break;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                k = "ArrowDown";
                break;
            default:
                int ch = e.getUnicodeChar();
                if (ch == 0 || host == null) return false;
                host.onText(String.valueOf((char) ch));
                return true;
        }
        if (host != null) host.onKey(k);
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, event);
        return key(keyCode, event) || super.onKeyDown(keyCode, event);
    }

    private static final class RemoteInput extends BaseInputConnection {
        private final ScreenView view;

        RemoteInput(ScreenView view) {
            super(view, true);
            this.view = view;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            boolean r = super.commitText(text, newCursorPosition);
            view.sync();
            return r;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            boolean r = super.setComposingText(text, newCursorPosition);
            view.sync();
            return r;
        }

        @Override
        public boolean finishComposingText() {
            boolean r = super.finishComposingText();
            view.sync();
            return r;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            Editable ed = getEditable();
            if (before > 0 && ed.length() == 0) {
                // Nothing typed here yet: delete on the page itself.
                for (int i = 0; i < before; i++) view.host.onKey("Backspace");
                return true;
            }
            boolean r = super.deleteSurroundingText(before, after);
            view.sync();
            return r;
        }

        @Override
        public boolean performEditorAction(int action) {
            view.host.onKey("Enter");
            view.post(view::resetInput);
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent e) {
            // Keyboards send some keys (Backspace on an empty field, Enter,
            // arrows) as raw key events instead of text edits.
            if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
            if (e.getKeyCode() == KeyEvent.KEYCODE_DEL && getEditable().length() > 0) {
                Editable ed = getEditable();
                ed.delete(ed.length() - 1, ed.length());
                view.sync();
                return true;
            }
            view.key(e.getKeyCode(), e);
            if (e.getKeyCode() == KeyEvent.KEYCODE_ENTER) view.post(view::resetInput);
            return true;
        }
    }
}
