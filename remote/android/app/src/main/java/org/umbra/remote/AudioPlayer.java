// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;

import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Plays the PC's 20 ms PCM chunks. A short queue absorbs Wi-Fi jitter;
 * if it backs up (the phone stalled), old sound is dropped so audio never
 * drifts far behind the picture.
 */
final class AudioPlayer {
    /** 15 x 20 ms: the most sound that may wait to be played. */
    private static final int MAX_QUEUED = 15;
    /** After running dry, wait for this many chunks before playing again. */
    private static final int PREBUFFER = 3;

    private final ArrayBlockingQueue<byte[]> queue = new ArrayBlockingQueue<>(MAX_QUEUED);
    private AudioTrack track;
    private Thread thread;
    private volatile boolean running;
    private int rate, channels;

    synchronized void start(int rate, int channels) {
        if (running && rate == this.rate && channels == this.channels) return;
        stop();
        this.rate = rate;
        this.channels = channels;
        int mask = channels == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        int min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT);
        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(rate)
                        .setChannelMask(mask)
                        .build())
                .setBufferSizeInBytes(Math.max(min * 2, rate * channels * 2 / 10))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        track.play();
        running = true;
        AudioTrack t = track;
        thread = new Thread(() -> loop(t), "audio");
        thread.start();
    }

    /** Called on the network thread. */
    void write(byte[] data, int offset, int length) {
        if (!running) return;
        byte[] chunk = Arrays.copyOfRange(data, offset, offset + length);
        if (!queue.offer(chunk)) {
            queue.clear(); // we fell behind: skip ahead rather than lag
            queue.offer(chunk);
        }
    }

    private void loop(AudioTrack t) {
        boolean dry = true;
        while (running) {
            try {
                if (dry) {
                    if (queue.size() < PREBUFFER) {
                        Thread.sleep(10);
                        continue;
                    }
                    dry = false;
                }
                byte[] chunk = queue.poll(100, TimeUnit.MILLISECONDS);
                if (chunk == null) {
                    dry = true;
                    continue;
                }
                t.write(chunk, 0, chunk.length);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    synchronized void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
            try {
                thread.join(300); // let a write in progress finish before release()
            } catch (InterruptedException ignored) {
            }
            thread = null;
        }
        if (track != null) {
            try {
                track.stop();
            } catch (IllegalStateException ignored) {
            }
            track.release();
            track = null;
        }
        queue.clear();
    }
}
