// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.view.Choreographer;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Plays the agent's H.264 stream straight onto a Surface with the phone's
 * hardware decoder: no copies, no bitmaps.
 *
 * Every access unit is acked once, when it goes into the decoder or is
 * dropped, so the agent's count of frames in flight stays right. A decoder
 * starts on a keyframe (SPS + PPS + IDR); until one comes, frames are
 * dropped and one is asked for. After a frame is lost on the way (UDP), the
 * frames that build on it are dropped too until the next keyframe, so the
 * picture freezes for a moment instead of smearing. Everything runs on one
 * thread, including MediaCodec's callbacks.
 *
 * Two ways to show frames (as Moonlight does): lowest latency, each one as
 * soon as it's decoded, with the newest of a screen refresh winning; or
 * smooth, up to two held back and one shown per refresh, which evens out
 * Wi-Fi jitter at the cost of up to a refresh of delay.
 */
final class VideoDecoder {
    interface Listener {
        /** A frame left the queue; the agent may send another. */
        void onAck();
        /** The decoder needs a keyframe to (re)start from. */
        void onNeedKeyframe();
        /** The first picture of a stream is on screen. */
        void onFirstFrame();
    }

    private final HandlerThread thread = new HandlerThread("video", Process.THREAD_PRIORITY_DISPLAY);
    private final Handler handler;
    private final Listener listener;
    private final ArrayDeque<Integer> freeInputs = new ArrayDeque<>();
    private final ArrayDeque<byte[]> pending = new ArrayDeque<>();
    /** Decoded frames (output buffer indexes) waiting for a screen refresh (smooth mode). */
    private final ArrayDeque<Integer> ready = new ArrayDeque<>();
    private Surface surface;
    private int width, height;
    private MediaCodec codec;
    private boolean shown, smooth, vsyncWaiting;
    /** A frame was lost: wait for a keyframe. */
    private boolean broken;
    /** A keyframe that came before its stream's size or surface (not acked yet). */
    private byte[] heldKey;
    private long askedAt, startedAt;
    /**
     * How many of the low-latency settings to try (0 = all). Steps down
     * when a decoder refuses them or fails soon after starting with them.
     */
    private static int tier;

    VideoDecoder(Listener listener) {
        this.listener = listener;
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    /** A stream of this size (pixels) starts; 0 x 0 stops playing. */
    void configure(int w, int h) {
        handler.post(() -> {
            width = w;
            height = h;
            stop();
            if (w == 0) dropHeldKey();
            else playHeldKey();
        });
    }

    /** Smooth (one frame per screen refresh) or lowest latency. */
    void setSmooth(boolean on) {
        handler.post(() -> {
            smooth = on;
            if (!on) flushReady();
        });
    }

    /** Network thread: a frame never arrived (it was acked already). */
    void lost() {
        handler.post(() -> {
            if (codec == null) return;
            broken = true;
            askForKeyframe();
        });
    }

    /** The surface to draw on, or null when it goes away (waits until let go). */
    void setSurface(Surface s) {
        CountDownLatch done = new CountDownLatch(1);
        handler.post(() -> {
            surface = s;
            stop();
            if (s != null && heldKey == null) askForKeyframe();
            playHeldKey();
            done.countDown();
        });
        try {
            done.await(1, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
        }
    }

    /** Network thread: one access unit. */
    void feed(byte[] data, int offset, int length) {
        byte[] au = offset == 0 && length == data.length ? data : Arrays.copyOfRange(data, offset, offset + length);
        handler.post(() -> onAccessUnit(au));
    }

    void quit() {
        handler.post(() -> {
            stop();
            thread.quitSafely();
        });
    }

    // ---- decoder thread -----------------------------------------------------

    private void onAccessUnit(byte[] au) {
        boolean key = isKeyframe(au);
        if (codec == null) {
            boolean ok = width > 0 && surface != null && surface.isValid();
            if (ok && key) start(au);
            if (codec == null) {
                if (key && !ok) {
                    // The stream's size is probably still on its way (it
                    // comes on another channel): keep this for then.
                    dropHeldKey();
                    heldKey = au;
                    return;
                }
                listener.onAck();
                askForKeyframe();
                return;
            }
        }
        if (broken) {
            if (!key) {
                listener.onAck();
                askForKeyframe();
                return;
            }
            broken = false;
        }
        pending.add(au);
        drain();
    }

    private void playHeldKey() {
        byte[] au = heldKey;
        if (au == null || width == 0 || surface == null || !surface.isValid()) return;
        heldKey = null;
        onAccessUnit(au);
    }

    private void dropHeldKey() {
        if (heldKey == null) return;
        heldKey = null;
        listener.onAck();
    }

    private void start(byte[] key) {
        for (int t = tier; t <= 3 && codec == null; t++) {
            MediaCodec c = null;
            try {
                c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
                MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
                f.setByteBuffer("csd-0", ByteBuffer.wrap(nal(key, 7)));
                f.setByteBuffer("csd-1", ByteBuffer.wrap(nal(key, 8)));
                f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(1 << 20, width * height));
                lowLatency(f, c.getName().toLowerCase(), t);
                c.setCallback(callback, handler);
                c.configure(f, surface, null, 0);
                c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT);
                c.start();
                codec = c;
                tier = t;
                shown = false;
                startedAt = System.currentTimeMillis();
            } catch (Exception e) {
                if (c != null) c.release();
            }
        }
    }

    /**
     * The settings that make decoders hand frames over as soon as they're
     * done (from Moonlight's MediaCodecHelper): Android's own low-latency
     * key, then each chip maker's, and on Qualcomm the decoder's clock at
     * its maximum. Tier 1 drops the chip makers' keys, tier 2 the clock,
     * tier 3 is plain.
     */
    private static void lowLatency(MediaFormat f, String name, int tier) {
        if (tier >= 3) return;
        if (Build.VERSION.SDK_INT >= 30) f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        boolean qcom = name.startsWith("omx.qcom") || name.startsWith("c2.qti");
        if (qcom && tier < 2) {
            // Not together with realtime priority: a decoder that can't
            // keep the asked-for rate may fail.
            f.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE);
        } else {
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);
        }
        if (tier >= 1) return;
        if (qcom) {
            f.setInteger("vendor.qti-ext-dec-picture-order.enable", 1);
            f.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
        } else if (name.startsWith("omx.exynos") || name.startsWith("c2.exynos")) {
            f.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1);
        } else if (name.startsWith("omx.hisi") || name.startsWith("c2.hisi")) {
            f.setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1);
            f.setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy", -1);
        } else if (name.startsWith("omx.amlogic") || name.startsWith("c2.amlogic")) {
            f.setInteger("vendor.low-latency.enable", 1);
        } else if (name.contains("mtk") || name.contains("mediatek")) {
            f.setInteger("vdec-lowlatency", 1);
        }
    }

    private void stop() {
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            codec.release();
            codec = null;
        }
        broken = false;
        freeInputs.clear();
        ready.clear();
        while (!pending.isEmpty()) {
            pending.poll();
            listener.onAck();
        }
    }

    private void drain() {
        while (codec != null && !pending.isEmpty() && !freeInputs.isEmpty()) {
            byte[] au = pending.poll();
            int index = freeInputs.poll();
            try {
                ByteBuffer b = codec.getInputBuffer(index);
                if (b == null || au.length > b.capacity()) throw new IllegalStateException("frame too big");
                b.clear();
                b.put(au);
                codec.queueInputBuffer(index, 0, au.length, System.nanoTime() / 1000, 0);
            } catch (Exception e) {
                stop();
                askForKeyframe();
            }
            listener.onAck();
        }
    }

    private void askForKeyframe() {
        // About one round trip plus an encode; asking again sooner would
        // only get two keyframes.
        long now = System.currentTimeMillis();
        if (now - askedAt < 150) return;
        askedAt = now;
        listener.onNeedKeyframe();
    }

    /** Puts a decoded frame on screen (render time in System.nanoTime terms). */
    private void show(MediaCodec c, int index, long at) {
        try {
            c.releaseOutputBuffer(index, at);
        } catch (IllegalStateException e) {
            return;
        }
        if (!shown) {
            shown = true;
            listener.onFirstFrame();
        }
    }

    private void flushReady() {
        while (!ready.isEmpty() && codec != null) {
            int index = ready.poll();
            try {
                codec.releaseOutputBuffer(index, false);
            } catch (IllegalStateException ignored) {
            }
        }
    }

    private final Choreographer.FrameCallback vsync = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            vsyncWaiting = false;
            if (codec == null || ready.isEmpty()) return;
            show(codec, ready.poll(), frameTimeNanos);
            if (!ready.isEmpty()) waitForVsync();
        }
    };

    private void waitForVsync() {
        if (vsyncWaiting) return;
        vsyncWaiting = true;
        Choreographer.getInstance().postFrameCallback(vsync);
    }

    private final MediaCodec.Callback callback = new MediaCodec.Callback() {
        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            if (c != codec) return;
            freeInputs.add(index);
            drain();
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index, MediaCodec.BufferInfo info) {
            if (c != codec) return;
            if (!smooth) {
                // Due now: if another comes before this refresh, the newer
                // one is shown and this one dropped (not queued behind it).
                show(c, index, System.nanoTime());
                return;
            }
            if (ready.size() >= 2) {
                try {
                    c.releaseOutputBuffer(ready.poll(), false);
                } catch (IllegalStateException ignored) {
                }
            }
            ready.add(index);
            waitForVsync();
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            if (c != codec) return;
            // Failing soon after starting: the low-latency settings may be
            // what it can't take; try with fewer next time.
            if (System.currentTimeMillis() - startedAt < 3000 && tier < 3) tier++;
            stop();
            askForKeyframe();
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
        }
    };

    // ---- Annex B ------------------------------------------------------------

    static boolean isKeyframe(byte[] au) {
        return nal(au, 7).length > 0 && nal(au, 8).length > 0 && nal(au, 5).length > 0;
    }

    /** The first NAL unit of this type, with its start code (or nothing). */
    static byte[] nal(byte[] au, int type) {
        int start = -1;
        for (int i = 0; i + 3 < au.length; i++) {
            if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
                if (start >= 0) {
                    int end = i > 0 && au[i - 1] == 0 ? i - 1 : i;
                    return prefixed(au, start, end);
                }
                if ((au[i + 3] & 0x1f) == type) start = i + 3;
                i += 2;
            }
        }
        return start >= 0 ? prefixed(au, start, au.length) : new byte[0];
    }

    private static byte[] prefixed(byte[] au, int from, int to) {
        byte[] out = new byte[4 + to - from];
        out[3] = 1;
        System.arraycopy(au, from, out, 4, to - from);
        return out;
    }
}
