// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;

/**
 * Closes Umbra on the PC when this app is cleared from the recent apps.
 *
 * Android tells nobody when an app's process is cleared, except a service
 * of that app that is still running (onTaskRemoved), and it only keeps a
 * service running in the background if it is a foreground service, which
 * has to show a notification. So this one does nothing but sit there,
 * quietly, while the app is around, and on being told sends close_umbra
 * (protocol: remote/agent/src/hub.rs) before the process goes. If the
 * system kills the app some other way (a force stop), nothing is sent and
 * Umbra stays open.
 */
public final class ExitWatchService extends Service {
    private static final String CHANNEL = "session";

    static void start(Context c) {
        try {
            c.startForegroundService(new Intent(c, ExitWatchService.class));
        } catch (RuntimeException ignored) {
            // Not allowed right now (e.g. started from the background): the
            // next time the app comes to the front tries again.
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.session_channel),
                NotificationManager.IMPORTANCE_MIN));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_launcher_foreground)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(getString(R.string.session_note))
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, n);
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        String address = getSharedPreferences("umbra", MODE_PRIVATE).getString("address", null);
        if (address != null) {
            // The process ends soon after this returns: send and wait here.
            Thread t = new Thread(() -> sayClose(address), "close-umbra");
            t.start();
            try {
                t.join(2500);
            } catch (InterruptedException ignored) {
            }
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /** Sends close_umbra over UDP (the C link), or the WebSocket if that fails. */
    private static void sayClose(String hostPort) {
        byte[] msg = "{\"t\":\"close_umbra\"}".getBytes(StandardCharsets.UTF_8);
        if (NativeLink.AVAILABLE && overUdp(hostPort, msg)) return;
        overWebSocket(hostPort, new String(msg, StandardCharsets.UTF_8));
    }

    private static boolean overUdp(String hostPort, byte[] msg) {
        int colon = hostPort.lastIndexOf(':');
        String host = colon > 0 ? hostPort.substring(0, colon) : hostPort;
        int port = 7777;
        try {
            if (colon > 0) port = Integer.parseInt(hostPort.substring(colon + 1));
        } catch (NumberFormatException ignored) {
        }
        long ep = NativeLink.connect(host, port);
        if (ep == 0) return false;
        ByteBuffer buf = ByteBuffer.allocateDirect(64 << 10);
        int[] info = new int[3];
        boolean sent = false;
        long until = System.currentTimeMillis() + 1000;
        try {
            while (System.currentTimeMillis() < until) {
                int type = NativeLink.poll(ep, buf, info, 20);
                if (type == NativeLink.EV_CLOSED) return false;
                if (type == NativeLink.EV_OPEN) {
                    sent = NativeLink.send(ep, NativeLink.RELIABLE, msg, 0, msg.length) == 0;
                    break;
                }
            }
            if (!sent) return false;
            // Goodbye once the message is acked (or after half a second).
            NativeLink.closePeer(ep);
            until = System.currentTimeMillis() + 800;
            while (System.currentTimeMillis() < until) {
                if (NativeLink.poll(ep, buf, info, 20) == NativeLink.EV_CLOSED) break;
            }
            return true;
        } finally {
            NativeLink.free(ep);
        }
    }

    private static void overWebSocket(String hostPort, String msg) {
        CountDownLatch done = new CountDownLatch(1);
        OkHttpClient http = new OkHttpClient.Builder().connectTimeout(1, TimeUnit.SECONDS).build();
        http.newWebSocket(new Request.Builder().url("ws://" + hostPort + "/ws").build(), new WebSocketListener() {
            @Override
            public void onOpen(WebSocket ws, Response response) {
                ws.send(msg);
                ws.close(1000, null);
            }

            @Override
            public void onClosed(WebSocket ws, int code, String reason) {
                done.countDown();
            }

            @Override
            public void onFailure(WebSocket ws, Throwable t, Response response) {
                done.countDown();
            }
        });
        try {
            done.await(1500, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
    }
}
