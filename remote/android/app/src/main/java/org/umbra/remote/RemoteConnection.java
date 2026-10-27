// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * The WebSocket to the PC agent (protocol: remote/agent/src/hub.rs).
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
    private boolean replaced;

    RemoteConnection(Listener listener) {
        this.listener = listener;
    }

    void connect(String hostPort) {
        close();
        replaced = false;
        Request req = new Request.Builder().url("ws://" + hostPort + "/ws").build();
        socket = HTTP.newWebSocket(req, new WebSocketListener() {
            @Override
            public void onOpen(WebSocket ws, Response response) {
                main.post(() -> { if (ws == socket) listener.onOpen(); });
            }

            @Override
            public void onMessage(WebSocket ws, ByteString bytes) {
                if (ws != socket || bytes.size() < 2) return;
                byte[] b = bytes.toByteArray();
                if (b[0] == 1) listener.onFrame(b, 1, b.length - 1);
                else if (b[0] == 2) listener.onSound(b, 1, b.length - 1);
                else if (b[0] == 3) listener.onVideo(b, 1, b.length - 1);
            }

            @Override
            public void onMessage(WebSocket ws, String text) {
                JSONObject m;
                try {
                    m = new JSONObject(text);
                } catch (JSONException e) {
                    return;
                }
                if ("bye".equals(m.optString("t"))) replaced = true;
                main.post(() -> { if (ws == socket) listener.onJson(m); });
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

    private void ended(WebSocket ws) {
        main.post(() -> {
            if (ws != socket) return;
            socket = null;
            listener.onClosed(replaced);
        });
    }

    void send(JSONObject message) {
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
        return socket != null;
    }

    void close() {
        WebSocket s = socket;
        socket = null;
        if (s != null) s.close(1000, null);
    }
}
