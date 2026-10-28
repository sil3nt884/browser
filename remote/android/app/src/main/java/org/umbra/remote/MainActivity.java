// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Menu;
import android.view.MenuItem;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Umbra Remote: the PC runs Umbra (over Tor) and streams it here; this
 * screen shows it at the phone's size and sends touches, typing and
 * navigation back. Nothing is rendered or fetched on the phone itself.
 */
public final class MainActivity extends Activity
        implements RemoteConnection.Listener, ScreenView.Host, VideoDecoder.Listener {
    private static final String PREF_ADDRESS = "address";
    private static final String PREF_DESKTOP = "desktop";
    private static final String PREF_QUALITY = "quality";
    private static final String PREF_FULLSCREEN = "fullscreen";
    private static final String PREF_LOCK_ROTATION = "lock_rotation";
    private static final String PREF_SMOOTH = "smooth";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService decoder = Executors.newSingleThreadExecutor();
    private final AtomicReference<byte[]> nextFrame = new AtomicReference<>();
    private android.net.wifi.WifiManager.WifiLock wifiLock;
    private final AudioPlayer audio = new AudioPlayer();
    private final RemoteConnection conn = new RemoteConnection(this);
    private VideoDecoder video;
    private SharedPreferences prefs;
    private ScreenView screen;
    private SurfaceView videoView;
    /** The video stream's size in pixels (0 while not streaming video). */
    private int videoW, videoH;
    /** The screen's refresh rate, as asked for (pictures come at it). */
    private float hz = 60;
    private EditText url;
    private ProgressBar progress;
    private TextView status;
    private ImageButton keyboard, exitFullscreen;
    private TextView tabsButton;
    /** The PC's open tabs ({id, title, url}, oldest first) and the one on screen. */
    private JSONArray tabList = new JSONArray();
    private String currentTab = "";
    /** The tab list while it is open (it follows changes on the PC). */
    private TabsAdapter tabsAdapter;
    private View root, bar;
    /** fullscreen: the user's setting; pageFullscreen: the page (a video,
     *  a game) is full screen, which hides the toolbar too, for as long;
     *  immersive: what's applied (either of them). */
    private boolean started, replaced, soundHintShown, fullscreen, pageFullscreen, immersive;
    private final Runnable sendHello = this::hello;
    private final Runnable reconnect = this::connect;
    private final Runnable hideExitButton = () -> exitFullscreen.setVisibility(View.GONE);

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);
        prefs = getSharedPreferences("umbra", Context.MODE_PRIVATE);
        screen = findViewById(R.id.screen);
        videoView = findViewById(R.id.video);
        video = new VideoDecoder(this);
        video.setSmooth(prefs.getBoolean(PREF_SMOOTH, false));
        hz = fastestRefresh();
        videoView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder h) {
                if (Build.VERSION.SDK_INT >= 30) {
                    // Keep the panel at the stream's rate instead of letting
                    // it drop to 60 Hz for "video".
                    h.getSurface().setFrameRate(hz, android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                }
                video.setSurface(h.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder h) {
                video.setSurface(null);
            }
        });
        url = findViewById(R.id.url);
        progress = findViewById(R.id.progress);
        status = findViewById(R.id.status);
        keyboard = findViewById(R.id.keyboard);
        exitFullscreen = findViewById(R.id.exit_fullscreen);
        root = findViewById(R.id.root);
        bar = findViewById(R.id.bar);
        screen.setHost(this);
        fitToWindow(root);
        applyRotation();
        setFullscreen(prefs.getBoolean(PREF_FULLSCREEN, false));
        exitFullscreen.setOnClickListener(v -> setFullscreen(false));

        findViewById(R.id.back).setOnClickListener(v -> conn.send("back"));
        findViewById(R.id.reload).setOnClickListener(v -> conn.send("reload"));
        keyboard.setOnClickListener(v -> {
            if (screen.keyboardShown() && screen.hasFocus()) screen.hideKeyboard();
            else screen.showKeyboard();
        });
        findViewById(R.id.menu).setOnClickListener(this::showMenu);
        tabsButton = findViewById(R.id.tabs);
        tabsButton.setOnClickListener(v -> showTabs());
        url.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_GO || actionId == EditorInfo.IME_NULL) {
                String text = url.getText().toString().trim();
                if (!text.isEmpty()) conn.send("nav", "url", text);
                url.clearFocus();
                hideIme(url);
                return true;
            }
            return false;
        });
    }

    /**
     * Asks for the display's fastest refresh rate at its current resolution
     * (phones otherwise often sit at 60 Hz), and returns it: the agent sends
     * pictures at this rate.
     */
    @SuppressWarnings("deprecation")
    private float fastestRefresh() {
        android.view.Display d = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
        if (d == null) return 60;
        android.view.Display.Mode cur = d.getMode(), best = cur;
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                    && m.getRefreshRate() > best.getRefreshRate()) {
                best = m;
            }
        }
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
        return best.getRefreshRate();
    }

    /**
     * Keeps the toolbar clear of the status bar and shrinks the page when the
     * keyboard opens, so the page re-lays itself out like a real phone browser.
     */
    private void fitToWindow(View root) {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((v, insets) -> {
                // In full screen the system bars only slide in over the page
                // when swiped, so they don't push it around.
                int types = WindowInsets.Type.ime();
                // A full-screen page (a video) goes edge to edge, into the cutout too.
                if (!pageFullscreen) types |= WindowInsets.Type.displayCutout();
                if (!immersive) types |= WindowInsets.Type.systemBars();
                Insets i = insets.getInsets(types);
                v.setPadding(i.left, i.top, i.right, i.bottom);
                if (immersive && !pageFullscreen && insets.isVisible(WindowInsets.Type.statusBars())) flashExitButton();
                return WindowInsets.CONSUMED;
            });
        } else {
            root.setFitsSystemWindows(true);
        }
    }

    /**
     * Full screen: no toolbar, no status or navigation bar; the page gets
     * the whole display. Swiping from an edge shows the system bars and,
     * for a few seconds, a button to come back.
     */
    private void setFullscreen(boolean on) {
        fullscreen = on;
        prefs.edit().putBoolean(PREF_FULLSCREEN, on).apply();
        applyFullscreen(on || pageFullscreen);
    }

    private void applyFullscreen(boolean on) {
        immersive = on;
        bar.setVisibility(on ? View.GONE : View.VISIBLE);
        if (on) progress.setVisibility(View.GONE);
        else if (progress.getVisibility() == View.GONE) progress.setVisibility(View.INVISIBLE);
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                if (on) c.hide(WindowInsets.Type.systemBars());
                else c.show(WindowInsets.Type.systemBars());
            }
            root.requestApplyInsets();
        } else {
            legacyFullscreen(on);
        }
        if (on && !pageFullscreen) {
            flashExitButton();
        } else {
            main.removeCallbacks(hideExitButton);
            exitFullscreen.setVisibility(View.GONE);
        }
    }

    @SuppressWarnings("deprecation")
    private void legacyFullscreen(boolean on) {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(on
                ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                : 0);
        decor.setOnSystemUiVisibilityChangeListener(vis -> {
            if (immersive && !pageFullscreen && (vis & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) flashExitButton();
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // Before Android 11 the system forgets immersive mode after dialogs.
        if (hasFocus && immersive && Build.VERSION.SDK_INT < 30) legacyFullscreen(true);
    }

    private void flashExitButton() {
        exitFullscreen.setVisibility(View.VISIBLE);
        main.removeCallbacks(hideExitButton);
        main.postDelayed(hideExitButton, 3000);
    }

    /** Follows the phone's rotation (even with the system's auto-rotate off) unless locked here. */
    private void applyRotation() {
        setRequestedOrientation(prefs.getBoolean(PREF_LOCK_ROTATION, false)
                ? ActivityInfo.SCREEN_ORIENTATION_LOCKED
                : ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
    }

    /**
     * Phones nap their Wi-Fi radio between packets to save power, which adds
     * tens of milliseconds to every frame and tap. A low-latency lock (the
     * mode meant for game streaming) keeps it awake while we're on screen.
     */
    @SuppressWarnings("deprecation")
    private void holdWifi(boolean on) {
        if (on && wifiLock == null) {
            android.net.wifi.WifiManager wm = getApplicationContext().getSystemService(android.net.wifi.WifiManager.class);
            if (wm == null) return;
            int mode = Build.VERSION.SDK_INT >= 29
                    ? android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    : android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wm.createWifiLock(mode, "UmbraRemote:stream");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        } else if (!on && wifiLock != null) {
            wifiLock.release();
            wifiLock = null;
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        holdWifi(true);
        ExitWatchService.start(this);
        if (address() == null) askAddress(false);
        else connect();
    }

    @Override
    protected void onStop() {
        super.onStop();
        // Stop streaming in the background: saves the phone's battery and the
        // PC's bandwidth. It picks up again when the app comes back.
        started = false;
        holdWifi(false);
        main.removeCallbacks(reconnect);
        conn.close();
        audio.stop();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (url.hasFocus()) {
            url.clearFocus();
            hideIme(url);
        } else if (pageFullscreen) {
            conn.send("exit_fullscreen"); // like Back in Chrome on a phone
        } else {
            conn.send("back");
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        video.quit();
    }

    // ---- connection -------------------------------------------------------

    private String address() {
        return prefs.getString(PREF_ADDRESS, null);
    }

    private void connect() {
        main.removeCallbacks(reconnect);
        String a = address();
        if (!started || a == null) return;
        replaced = false;
        say(getString(R.string.status_connecting, a));
        conn.connect(a);
    }

    private void askAddress(boolean cancelable) {
        EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        field.setHint("192.168.1.20:7777");
        String a = address();
        if (a != null) field.setText(a);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        field.setPadding(pad, pad / 2, pad, pad / 2);
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(R.string.pc_address_title)
                .setMessage(R.string.pc_address_message)
                .setView(field)
                .setCancelable(cancelable)
                .setPositiveButton(R.string.connect, (d, w) -> {
                    String v = field.getText().toString().trim()
                            .replaceFirst("^(?i)(http|ws)://", "").replaceAll("/+$", "");
                    if (!v.isEmpty() && !v.contains(":")) v += ":7777";
                    if (v.isEmpty()) {
                        askAddress(cancelable);
                        return;
                    }
                    prefs.edit().putString(PREF_ADDRESS, v).apply();
                    connect();
                });
        if (cancelable) b.setNegativeButton(R.string.cancel, null);
        b.show();
    }

    private void hello() {
        if (!conn.isOpen() || screen.getWidth() == 0) return;
        float d = getResources().getDisplayMetrics().density;
        int sw, sh;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Rect r = getWindowManager().getCurrentWindowMetrics().getBounds();
            sw = r.width();
            sh = r.height();
        } else {
            sw = getResources().getDisplayMetrics().widthPixels;
            sh = getResources().getDisplayMetrics().heightPixels;
        }
        conn.send("hello",
                "w", Math.round(screen.getWidth() / d),
                "h", Math.round(screen.getHeight() / d),
                "sw", Math.round(sw / d),
                "sh", Math.round(sh / d),
                "dpr", d,
                "mobile", !prefs.getBoolean(PREF_DESKTOP, false),
                "quality", prefs.getInt(PREF_QUALITY, 70),
                "hz", Math.round(hz));
    }

    @Override
    public void onOpen() {
        say(screen.hasFrame() ? null : getString(R.string.status_waiting));
        hello();
    }

    @Override
    public void onClosed(boolean wasReplaced) {
        audio.stop();
        stopVideo();
        setPageFullscreen(false);
        replaced = wasReplaced;
        if (wasReplaced) {
            say(getString(R.string.status_replaced));
        } else if (started) {
            say(getString(R.string.status_lost));
            main.postDelayed(reconnect, 2000);
        }
    }

    /**
     * Network thread. Decoding happens on its own thread so a slow decode
     * never holds up sound or the next frame; if frames arrive faster than
     * they decode, only the newest is drawn and the skipped one is acked
     * straight away (the agent counts frames in flight).
     */
    @Override
    public void onFrame(byte[] data, int offset, int length) {
        if (videoW != 0) main.post(this::stopVideo); // the PC fell back to JPEG
        byte[] jpeg = offset == 0 && length == data.length ? data : Arrays.copyOfRange(data, offset, offset + length);
        if (nextFrame.getAndSet(jpeg) != null) conn.send("ack");
        else decoder.execute(this::decodeNewest);
    }

    private void decodeNewest() {
        byte[] jpeg = nextFrame.getAndSet(null);
        if (jpeg == null) return;
        Bitmap b = screen.decode(jpeg, 0, jpeg.length);
        main.post(() -> {
            if (b != null && status.getVisibility() == View.VISIBLE && !replaced) say(null);
            screen.show(b);
        });
    }

    @Override
    public void onVideo(byte[] data, int offset, int length) {
        video.feed(data, offset, length);
    }

    /** A new video stream of this size (pixels) starts. */
    private void startVideo(int w, int h) {
        videoW = w;
        videoH = h;
        screen.setVideoSize(w, h);
        videoView.getHolder().setFixedSize(w, h);
        placeVideo();
        videoView.setVisibility(View.VISIBLE);
        video.configure(w, h);
    }

    private void stopVideo() {
        if (videoW == 0) return;
        videoW = videoH = 0;
        video.configure(0, 0);
    }

    /** Fits the video to the screen; mid-rotation it is letterboxed, not stretched. */
    private void placeVideo() {
        if (videoW == 0 || screen.getWidth() == 0) return;
        android.graphics.Rect r = screen.placeFor(videoW, videoH);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) videoView.getLayoutParams();
        if (lp.width != r.width() || lp.height != r.height() || lp.leftMargin != r.left || lp.topMargin != r.top) {
            lp.width = r.width();
            lp.height = r.height();
            lp.leftMargin = r.left;
            lp.topMargin = r.top;
            lp.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
            videoView.setLayoutParams(lp);
        }
    }

    private void setPageFullscreen(boolean on) {
        if (pageFullscreen == on) return;
        pageFullscreen = on;
        applyFullscreen(fullscreen || on);
    }

    // ---- video decoder callbacks (decoder thread) --------------------------

    @Override
    public void onAck() {
        conn.send("ack");
    }

    @Override
    public void onNeedKeyframe() {
        conn.send("idr");
    }

    @Override
    public void onFirstFrame() {
        main.post(() -> {
            if (status.getVisibility() == View.VISIBLE && !replaced) say(null);
        });
    }

    @Override
    public void onSound(byte[] data, int offset, int length) {
        audio.write(data, offset, length);
    }

    /** Network thread: a picture was lost on the way (UDP). */
    @Override
    public void onLost() {
        conn.send("ack");
        video.lost();
    }

    @Override
    public void onJson(JSONObject m) {
        switch (m.optString("t")) {
            case "audio":
                audio.start(m.optInt("rate", 48000), m.optInt("ch", 2));
                String state = m.optString("state");
                if (state.startsWith("off") && !soundHintShown) {
                    soundHintShown = true;
                    Toast.makeText(this, R.string.sound_off, Toast.LENGTH_LONG).show();
                }
                break;
            case "meta":
                String u = m.optString("url");
                if (!url.hasFocus()) url.setText(u.startsWith("chrome:") ? "" : u);
                break;
            case "load":
                if (!fullscreen) progress.setVisibility(m.optBoolean("on") ? View.VISIBLE : View.INVISIBLE);
                break;
            case "vp":
                screen.setPageSize((float) m.optDouble("w"), (float) m.optDouble("h"));
                break;
            case "video":
                startVideo(m.optInt("w"), m.optInt("h"));
                break;
            case "fullscreen":
                setPageFullscreen(m.optBoolean("on"));
                break;
            case "tabs":
                JSONArray list = m.optJSONArray("list");
                tabList = list != null ? list : new JSONArray();
                currentTab = m.optString("current");
                int n = tabList.length();
                tabsButton.setText(n < 100 ? String.valueOf(n) : ":D");
                if (tabsAdapter != null) tabsAdapter.notifyDataSetChanged();
                break;
            case "kbd":
                if (m.optBoolean("on")) screen.showKeyboard();
                else if (screen.hasFocus()) screen.hideKeyboard();
                break;
            case "dialog":
                showDialog(m);
                break;
            default:
                break;
        }
    }

    private void showDialog(JSONObject m) {
        String type = m.optString("type");
        AlertDialog.Builder b = new AlertDialog.Builder(this).setMessage(m.optString("message")).setCancelable(false);
        if ("prompt".equals(type)) {
            EditText field = new EditText(this);
            field.setText(m.optString("value"));
            b.setView(field)
                    .setPositiveButton(R.string.ok, (d, w) ->
                            conn.send("dialog", "accept", true, "text", field.getText().toString()))
                    .setNegativeButton(R.string.cancel, (d, w) -> conn.send("dialog", "accept", false));
        } else if ("confirm".equals(type) || "beforeunload".equals(type)) {
            b.setPositiveButton(R.string.ok, (d, w) -> conn.send("dialog", "accept", true))
                    .setNegativeButton(R.string.cancel, (d, w) -> conn.send("dialog", "accept", false));
        } else {
            b.setPositiveButton(R.string.ok, (d, w) -> conn.send("dialog", "accept", true));
        }
        b.show();
    }

    // ---- screen callbacks -------------------------------------------------

    @Override
    public void onFrameShown() {
        conn.send("ack");
    }

    @Override
    public void onSizeChanged(int widthPx, int heightPx) {
        placeVideo();
        main.removeCallbacks(sendHello);
        main.postDelayed(sendHello, 150);
    }

    @Override
    public void onTouch(String kind, JSONArray points, long time) {
        if (replaced) {
            if ("start".equals(kind)) connect(); // take Umbra over from the other device
            return;
        }
        if (url.hasFocus()) {
            url.clearFocus();
            hideIme(url);
        }
        try {
            conn.send(new JSONObject().put("t", "touch").put("k", kind).put("p", points).put("ts", time));
        } catch (org.json.JSONException ignored) {
        }
    }

    @Override
    public void onText(String text) {
        conn.send("text", "s", text);
    }

    @Override
    public void onKey(String key) {
        conn.send("key", "key", key);
    }

    // ---- menu & helpers ---------------------------------------------------

    private void showMenu(View anchor) {
        PopupMenu p = new PopupMenu(this, anchor);
        Menu m = p.getMenu();
        m.add(0, 9, 0, R.string.new_tab);
        m.add(0, 1, 0, R.string.forward);
        MenuItem desktop = m.add(0, 2, 1, R.string.desktop_site);
        desktop.setCheckable(true).setChecked(prefs.getBoolean(PREF_DESKTOP, false));
        android.view.SubMenu q = m.addSubMenu(0, 3, 2, R.string.quality);
        int cur = prefs.getInt(PREF_QUALITY, 70);
        q.add(1, 45, 0, R.string.quality_low).setChecked(cur == 45);
        q.add(1, 70, 1, R.string.quality_medium).setChecked(cur == 70);
        q.add(1, 88, 2, R.string.quality_high).setChecked(cur == 88);
        q.setGroupCheckable(1, true, true);
        m.add(0, 5, 3, R.string.full_screen).setCheckable(true).setChecked(fullscreen);
        m.add(0, 6, 4, R.string.lock_rotation).setCheckable(true)
                .setChecked(prefs.getBoolean(PREF_LOCK_ROTATION, false));
        m.add(0, 10, 4, R.string.smooth_motion).setCheckable(true)
                .setChecked(prefs.getBoolean(PREF_SMOOTH, false));
        m.add(0, 7, 5, R.string.new_identity);
        m.add(0, 8, 6, R.string.restart_agent);
        m.add(0, 4, 7, R.string.change_pc);
        p.setOnMenuItemClickListener(item -> {
            int id = item.getItemId();
            if (id == 5) {
                setFullscreen(!item.isChecked());
            } else if (id == 6) {
                prefs.edit().putBoolean(PREF_LOCK_ROTATION, !item.isChecked()).apply();
                applyRotation();
            } else if (id == 7) {
                confirm(R.string.new_identity, R.string.new_identity_message, () -> {
                    conn.send("new_identity");
                    say(getString(R.string.status_waiting));
                });
            } else if (id == 8) {
                confirm(R.string.restart_agent, R.string.restart_agent_message, () -> conn.send("restart_agent"));
            } else if (id == 9) {
                newTab();
            } else if (id == 10) {
                boolean on = !item.isChecked();
                prefs.edit().putBoolean(PREF_SMOOTH, on).apply();
                video.setSmooth(on);
            } else if (id == 1) {
                conn.send("fwd");
            } else if (id == 2) {
                prefs.edit().putBoolean(PREF_DESKTOP, !item.isChecked()).apply();
                hello();
            } else if (id == 45 || id == 70 || id == 88) {
                prefs.edit().putInt(PREF_QUALITY, id).apply();
                hello();
            } else if (id == 4) {
                askAddress(true);
            } else {
                return false;
            }
            return true;
        });
        p.show();
    }

    // ---- tabs -----------------------------------------------------------------

    private void showTabs() {
        conn.send("tabs"); // fresh titles; the list updates when they arrive
        TabsAdapter adapter = new TabsAdapter();
        ListView list = new ListView(this);
        list.setAdapter(adapter);
        list.setDivider(null);
        AlertDialog d = new AlertDialog.Builder(this)
                .setTitle(R.string.tabs)
                .setView(list)
                .setPositiveButton(R.string.new_tab, (dialog, w) -> newTab())
                .setNegativeButton(R.string.close, null)
                .create();
        list.setOnItemClickListener((parent, view, pos, id) -> {
            conn.send("switch_tab", "id", adapter.id(pos));
            d.dismiss();
        });
        d.setOnDismissListener(dialog -> {
            if (tabsAdapter == adapter) tabsAdapter = null;
        });
        tabsAdapter = adapter;
        d.show();
    }

    /** Opens a tab on the PC and gets the address bar ready, like Chrome does. */
    private void newTab() {
        conn.send("new_tab");
        if (fullscreen) setFullscreen(false);
        url.setText("");
        url.requestFocus();
        url.post(() -> {
            InputMethodManager imm = getSystemService(InputMethodManager.class);
            if (imm != null) imm.showSoftInput(url, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    private final class TabsAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return tabList.length();
        }

        @Override
        public Object getItem(int pos) {
            return tabList.optJSONObject(pos);
        }

        @Override
        public long getItemId(int pos) {
            return pos;
        }

        String id(int pos) {
            JSONObject t = tabList.optJSONObject(pos);
            return t != null ? t.optString("id") : "";
        }

        @Override
        public View getView(int pos, View convert, android.view.ViewGroup parent) {
            float dp = getResources().getDisplayMetrics().density;
            JSONObject t = tabList.optJSONObject(pos);
            String title = t != null ? t.optString("title") : "";
            String address = t != null ? t.optString("url") : "";
            boolean current = id(pos).equals(currentTab);

            LinearLayout row = new LinearLayout(MainActivity.this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            row.setMinimumHeight((int) (60 * dp));
            row.setPadding((int) (24 * dp), (int) (6 * dp), (int) (8 * dp), (int) (6 * dp));

            LinearLayout texts = new LinearLayout(MainActivity.this);
            texts.setOrientation(LinearLayout.VERTICAL);
            TextView name = new TextView(MainActivity.this);
            name.setText(!title.isEmpty() ? title : !address.isEmpty() ? address : getString(R.string.new_tab));
            name.setTextSize(16);
            name.setSingleLine(true);
            name.setEllipsize(android.text.TextUtils.TruncateAt.END);
            if (current) {
                name.setTextColor(getColor(R.color.accent));
                name.setTypeface(name.getTypeface(), android.graphics.Typeface.BOLD);
            }
            TextView where = new TextView(MainActivity.this);
            where.setText(address.replaceFirst("^https?://", "").replaceFirst("/$", ""));
            where.setTextSize(12);
            where.setAlpha(0.6f);
            where.setSingleLine(true);
            where.setEllipsize(android.text.TextUtils.TruncateAt.END);
            texts.addView(name);
            if (!address.isEmpty() && !address.equals(title)) texts.addView(where);
            row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

            ImageButton close = new ImageButton(MainActivity.this);
            close.setImageResource(R.drawable.ic_close);
            android.util.TypedValue ripple = new android.util.TypedValue();
            getTheme().resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, ripple, true);
            close.setBackgroundResource(ripple.resourceId);
            close.setContentDescription(getString(R.string.close_tab));
            close.setFocusable(false); // so taps on the row still pick the tab
            String id = id(pos);
            close.setOnClickListener(v -> conn.send("close_tab", "id", id));
            row.addView(close, new LinearLayout.LayoutParams((int) (48 * dp), (int) (48 * dp)));
            return row;
        }
    }

    private void confirm(int title, int message, Runnable action) {
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton(R.string.restart, (d, w) -> action.run())
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    private void say(String text) {
        status.setText(text);
        status.setVisibility(text == null ? View.GONE : View.VISIBLE);
    }

    private void hideIme(View v) {
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm != null) imm.hideSoftInputFromWindow(v.getWindowToken(), 0);
    }
}
