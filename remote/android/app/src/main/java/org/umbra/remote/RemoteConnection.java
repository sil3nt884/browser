// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONException;
import org.json.JSONObject;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * The link to the PC agent (protocol: remote/agent/src/hub.rs). UDP first
 * (NativeLink: pictures and sound are never resent, a lost picture is
 * reported through onLost), the WebSocket if the PC doesn't answer on UDP
 * (an older agent, or the firewall only lets TCP through).
 * Binary messages start with a kind byte: 1 = JPEG frame, 2 = PCM sound,
 * 3 = H.264 access unit.
 * Frames and sound are delivered on the network thread (to decode/queue off
 * the UI thread); JSON and connection state arrive on the main thread.
 */
final class RemoteConnection {
    interface Listener {
        void onOpen();
        void onFrame(byte[] data, int offset, int length);
        void onVideo(byte[] data, int offset, int length);
        void onSound(byte[] data, int offset, int length);
        /** Network thread: a picture didn't make it (it still needs an ack). */
        void onLost();
        void onJson(JSONObject message);
        void onClosed(boolean replaced);
    }

    private static final OkHttpClient HTTP = new OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .pingInterval(20, TimeUnit.SECONDS)
            .build();

    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile WebSocket socket;
    private volatile Udp udp;
    private volatile boolean replaced;

    RemoteConnection(Listener listener) {
        this.listener = listener;
    }

    void connect(String hostPort) {
        close();
        replaced = false;
        if (NativeLink.AVAILABLE) {
            udp = new Udp(hostPort);
            udp.start();
        } else {
            connectWebSocket(hostPort);
        }
    }

    // ---- UDP ----------------------------------------------------------------

    /** One UDP session; its thread polls the C side until the session ends. */
    private final class Udp extends Thread {
        private final String hostPort;
        private final Object lock = new Object();
        private long ep; // guarded by lock; 0 once freed
        private volatile boolean open, closing;

        Udp(String hostPort) {
            super("udp");
            this.hostPort = hostPort;
        }

        @Override
        public void run() {
            int colon = hostPort.lastIndexOf(':');
            String host = colon > 0 ? hostPort.substring(0, colon) : hostPort;
            int port = 7777;
            try {
                if (colon > 0) port = Integer.parseInt(hostPort.substring(colon + 1));
            } catch (NumberFormatException ignored) {
            }
            long e = NativeLink.connect(host, port);
            if (e == 0) {
                fallBack();
                return;
            }
            synchronized (lock) {
                ep = e;
            }
            // Big enough for a keyframe at the highest quality.
            ByteBuffer buf = ByteBuffer.allocateDirect(8 << 20);
            int[] info = new int[3];
            boolean saidBye = false;
            while (true) {
                if (closing && !saidBye) {
                    NativeLink.closePeer(e);
                    saidBye = true;
                }
                int type = NativeLink.poll(e, buf, info, 50);
                if (type == NativeLink.EV_OPEN) {
                    open = true;
                    main.post(() -> { if (udp == this) listener.onOpen(); });
                } else if (type == NativeLink.EV_MESSAGE) {
                    byte[] b = new byte[info[1]];
                    buf.position(0);
                    buf.get(b);
                    if (info[0] == NativeLink.RELIABLE) onText(this, new String(b, StandardCharsets.UTF_8));
                    else if (udp == this) onBinary(b);
                } else if (type == NativeLink.EV_LOST) {
                    if (info[0] == NativeLink.FRAMES && udp == this) listener.onLost();
                } else if (type == NativeLink.EV_CLOSED) {
                    synchronized (lock) {
                        NativeLink.free(e);
                        ep = 0;
                    }
                    if (!open && info[2] == NativeLink.CLOSE_NO_ANSWER) fallBack();
                    else ended(this);
                    return;
                }
            }
        }

        private void fallBack() {
            main.post(() -> {
                if (udp != this) return;
                udp = null;
                connectWebSocket(hostPort);
            });
        }

        boolean send(byte[] b) {
            synchronized (lock) {
                return ep != 0 && open && NativeLink.send(ep, NativeLink.RELIABLE, b, 0, b.length) == 0;
            }
        }
    }

    // ---- WebSocket ----------------------------------------------------------

    private void connectWebSocket(String hostPort) {
        Request req = new Request.Builder().url("ws://" + hostPort + "/ws").build();
        socket = HTTP.newWebSocket(req, new WebSocketListener() {
            @Override
            public void onOpen(WebSocket ws, Response response) {
                main.post(() -> { if (ws == socket) listener.onOpen(); });
            }

            @Override
            public void onMessage(WebSocket ws, ByteString bytes) {
                if (ws == socket) onBinary(bytes.toByteArray());
            }

            @Override
            public void onMessage(WebSocket ws, String text) {
                onText(ws, text);
            }

            @Override
            public void onClosing(WebSocket ws, int code, String reason) {
                ws.close(1000, null);
            }

            @Override
            public void onClosed(WebSocket ws, int code, String reason) {
                ended(ws);
            }

            @Override
            public void onFailure(WebSocket ws, Throwable t, Response response) {
                ended(ws);
            }
        });
    }

    // ---- either -------------------------------------------------------------

    private boolean current(Object link) {
        return link == socket || link == udp;
    }

    private void onBinary(byte[] b) {
        if (b.length < 2) return;
        if (b[0] == 1) listener.onFrame(b, 1, b.length - 1);
        else if (b[0] == 2) listener.onSound(b, 1, b.length - 1);
        else if (b[0] == 3) listener.onVideo(b, 1, b.length - 1);
    }

    private void onText(Object link, String text) {
        JSONObject m;
        try {
            m = new JSONObject(text);
        } catch (JSONException e) {
            return;
        }
        if ("bye".equals(m.optString("t"))) replaced = true;
        main.post(() -> { if (current(link)) listener.onJson(m); });
    }

    private void ended(Object link) {
        main.post(() -> {
            if (!current(link)) return;
            socket = null;
            udp = null;
            listener.onClosed(replaced);
        });
    }

    void send(JSONObject message) {
        Udp u = udp;
        if (u != null) {
            u.send(message.toString().getBytes(StandardCharsets.UTF_8));
            return;
        }
        WebSocket s = socket;
        if (s != null) s.send(message.toString());
    }

    /** Sends {"t": type, k1: v1, k2: v2, ...}. */
    void send(String type, Object... kv) {
        try {
            JSONObject o = new JSONObject().put("t", type);
            for (int i = 0; i + 1 < kv.length; i += 2) o.put((String) kv[i], kv[i + 1]);
            send(o);
        } catch (JSONException ignored) {
        }
    }

    boolean isOpen() {
        Udp u = udp;
        return socket != null || (u != null && u.open);
    }

    /** Over UDP rather than the WebSocket. */
    boolean isUdp() {
        Udp u = udp;
        return u != null && u.open;
    }

    void close() {
        WebSocket s = socket;
        socket = null;
        if (s != null) s.close(1000, null);
        Udp u = udp;
        udp = null;
        if (u != null) u.closing = true;
    }
}
