/* Copyright 2026 The Umbra Authors.
 *
 * umbranet: the phone <-> PC transport, in C, shared by the agent (Windows,
 * linked into the Rust exe) and the Android app (through JNI).
 *
 * One UDP socket per side, built the way game streamers (Parsec, Moonlight)
 * do it rather than TCP:
 *  - Video and sound are sent as fast as they are made, never resent. A
 *    message is split into packets; if any packet is missing the whole
 *    message is dropped and reported as lost (the app asks for a keyframe),
 *    so one lost packet never holds up the frames behind it.
 *  - Taps, keys and other control messages go on a reliable, ordered
 *    channel with selective acks and fast resends (a few ms on a LAN, not
 *    TCP's 200-300 ms minimum).
 *  - The receiver reports loss and queueing delay every 100 ms; the sender
 *    turns that into a suggested bit rate (UN_EV_RATE) for the encoder, so a
 *    busy Wi-Fi network gets a smaller picture instead of a growing queue.
 *  - No error correction (by choice): a loss costs a frame, not bandwidth.
 *
 * Threads: call un_poll from one thread; un_send and un_close_peer may be
 * called from any thread.
 */
#ifndef UMBRANET_H
#define UMBRANET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct un_ep un_ep;

/* Channels. */
enum {
  UN_RELIABLE = 0, /* ordered, never lost (control messages) */
  UN_FRAMES = 1,   /* pictures: dropped whole when incomplete */
  UN_SOUND = 2,    /* sound: same */
};

/* Events from un_poll. */
enum {
  UN_EV_NONE = 0,
  UN_EV_OPEN = 1,    /* a peer connected; the data is its IP address (text) */
  UN_EV_CLOSED = 2,  /* a peer is gone; value = UN_CLOSE_* */
  UN_EV_MESSAGE = 3, /* a whole message arrived on `channel` */
  UN_EV_LOST = 4,    /* message number `value` on `channel` was lost */
  UN_EV_RATE = 5,    /* suggested send rate to this peer, value = bits/s */
  UN_EV_TOO_BIG = 6, /* a message didn't fit the buffer and was dropped */
};

enum {
  UN_CLOSE_TIMEOUT = 0,   /* nothing heard for 3 s */
  UN_CLOSE_BYE = 1,       /* the other side closed */
  UN_CLOSE_NO_ANSWER = 2, /* un_connect: the server never answered */
  UN_CLOSE_HERE = 3,      /* un_close_peer finished */
};

typedef struct {
  int type;
  uint32_t peer;
  int channel;
  uint32_t len;
  uint32_t value;
} un_event;

/* Server: listens on UDP `port` (IPv4) for any number of peers (up to 4 at
 * once; a fifth replaces the oldest). NULL on failure, with a reason. */
un_ep *un_listen(uint16_t port, char *err, size_t err_len);

/* Client: starts connecting to host:port (an IPv4 address or name). The
 * peer is number 1; un_poll reports UN_EV_OPEN, or UN_EV_CLOSED with
 * UN_CLOSE_NO_ANSWER after about a second. */
un_ep *un_connect(const char *host, uint16_t port, char *err, size_t err_len);

/* Waits up to timeout_ms for the next event. Returns 1 with *ev filled (and
 * ev->len bytes of data in buf), or 0 on timeout. */
int un_poll(un_ep *ep, un_event *ev, uint8_t *buf, uint32_t cap, int timeout_ms);

/* Sends one message. 0 = sent (or queued, on UN_RELIABLE); negative = no
 * such peer / closing (-1), reliable window full (-2), too big (-3). */
int un_send(un_ep *ep, uint32_t peer, int channel, const uint8_t *data, uint32_t len);

/* Closes a peer once its reliable messages are delivered (at most 0.5 s),
 * then reports UN_EV_CLOSED with UN_CLOSE_HERE. */
void un_close_peer(un_ep *ep, uint32_t peer);

/* Smoothed round trip time to a peer, in microseconds (0 if unknown). */
uint32_t un_rtt_us(un_ep *ep, uint32_t peer);

/* Tests: drop this many packets per thousand, both ways. */
void un_set_drop(un_ep *ep, uint32_t per_mille);

void un_free(un_ep *ep);

#ifdef __cplusplus
}
#endif

#endif /* UMBRANET_H */
