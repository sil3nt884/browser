// Copyright 2026 The Umbra Authors.
package org.umbra.remote;

import java.nio.ByteBuffer;

/**
 * The UDP link to the PC: umbranet (remote/net/umbranet.h), in C, through
 * JNI. One thread polls; sends may come from any thread.
 */
final class NativeLink {
    static final boolean AVAILABLE;

    static {
        boolean ok;
        try {
            System.loadLibrary("umbranet");
            ok = true;
        } catch (Throwable t) {
            ok = false;
        }
        AVAILABLE = ok;
    }

    static final int RELIABLE = 0, FRAMES = 1, SOUND = 2;
    static final int EV_OPEN = 1, EV_CLOSED = 2, EV_MESSAGE = 3, EV_LOST = 4;
    static final int CLOSE_NO_ANSWER = 2;

    private NativeLink() {
    }

    /** Starts connecting; 0 if the address is no good. */
    static native long connect(String host, int port);

    /** The next event (0 = none yet); info = {channel, length, value}, data at the start of buf. */
    static native int poll(long ep, ByteBuffer buf, int[] info, int timeoutMs);

    /** 0 = sent; negative = not connected, or too much queued. */
    static native int send(long ep, int channel, byte[] data, int offset, int length);

    /** Says goodbye once queued messages are through; poll then reports EV_CLOSED. */
    static native void closePeer(long ep);

    static native void free(long ep);
}
