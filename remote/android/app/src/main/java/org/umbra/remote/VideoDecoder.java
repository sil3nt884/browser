// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Plays the agent's H.264 stream straight onto a Surface with the phone's
 * hardware decoder: no copies, no bitmaps, each frame shown the moment it
 * is decoded.
 *
 * Every access unit is acked once, when it goes into the decoder or is
 * dropped, so the agent's count of frames in flight stays right. A decoder
 * starts on a keyframe (SPS + PPS + IDR); until one comes, frames are
 * dropped and one is asked for. Everything runs on one thread, including
 * MediaCodec's callbacks.
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

    private final HandlerThread thread = new HandlerThread("video");
    private final Handler handler;
    private final Listener listener;
    private final ArrayDeque<Integer> freeInputs = new ArrayDeque<>();
    private final ArrayDeque<byte[]> pending = new ArrayDeque<>();
    private Surface surface;
    private int width, height;
    private MediaCodec codec;
    private boolean shown;
    private long askedAt;

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
        });
    }

    /** The surface to draw on, or null when it goes away (waits until let go). */
    void setSurface(Surface s) {
        CountDownLatch done = new CountDownLatch(1);
        handler.post(() -> {
            surface = s;
            stop();
            if (s != null) askForKeyframe();
            done.countDown();
        });
        try {
            done.await(1, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
        }
    }

    /** Network thread: one access unit. */
    void feed(byte[] data, int offset, int length) {
        byte[] au = Arrays.copyOfRange(data, offset, offset + length);
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
        if (codec == null) {
            if (width > 0 && surface != null && surface.isValid() && isKeyframe(au)) start(au);
            if (codec == null) {
                listener.onAck();
                askForKeyframe();
                return;
            }
        }
        pending.add(au);
        drain();
    }

    private void start(byte[] key) {
        try {
            MediaCodec c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
            f.setByteBuffer("csd-0", ByteBuffer.wrap(nal(key, 7)));
            f.setByteBuffer("csd-1", ByteBuffer.wrap(nal(key, 8)));
            f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(1 << 20, width * height));
            // Show each frame as soon as it's decoded instead of buffering a
            // few for smooth film playback.
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);
            if (Build.VERSION.SDK_INT >= 30) f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            String name = c.getName().toLowerCase();
            if (name.contains("qti") || name.contains("qcom")) {
                f.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
            }
            c.setCallback(callback, handler);
            c.configure(f, surface, null, 0);
            c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT);
            c.start();
            codec = c;
            shown = false;
        } catch (Exception e) {
            codec = null;
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
        freeInputs.clear();
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
        long now = System.currentTimeMillis();
        if (now - askedAt < 500) return;
        askedAt = now;
        listener.onNeedKeyframe();
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
            c.releaseOutputBuffer(index, true);
            if (!shown) {
                shown = true;
                listener.onFirstFrame();
            }
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            if (c != codec) return;
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
