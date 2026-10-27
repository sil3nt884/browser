/* Copyright 2026 The Umbra Authors.
 * umbranet: see umbranet.h for what it does and why.
 *
 * Every datagram starts with a 6-byte header: magic, type, connection token
 * (chosen by the client, so stray or stale packets are ignored). Then, by
 * type (all numbers little-endian):
 *   HELLO   version u16              client -> server, until answered
 *   WELCOME version u16              server -> client
 *   DATA    seq u32, flags u8, data  reliable channel (flag 1 = last piece)
 *   ACK     next u32, sack u32       next seq wanted; bit i = next+1+i held
 *   FRAG    ch u8, msg u32, idx u16, count u16, pkt u32, sent_us u32, data
 *   PING    t u32, last frames msg u32, last sound msg u32
 *   PONG    t u32
 *   REPORT  pkts u32, lost u32, bytes u32, queue avg us u32, queue max us u32
 *   BYE
 */
#include "umbranet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <qos2.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
typedef SRWLOCK mutex_t;
#define mutex_init(m) InitializeSRWLock(m)
#define mutex_lock(m) AcquireSRWLockExclusive(m)
#define mutex_unlock(m) ReleaseSRWLockExclusive(m)
#define mutex_destroy(m) ((void)0)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
typedef pthread_mutex_t mutex_t;
#define mutex_init(m) pthread_mutex_init(m, NULL)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#define MAGIC 0xB7
#define VERSION 1
#define HEADER 6
#define PIECE 1200 /* payload per datagram: fits any Wi-Fi/IPv4/IPv6 MTU */
#define MAX_PEERS 4
#define MAX_MESSAGE (16u << 20)
#define WINDOW 2048 /* reliable packets in flight */
#define PARTIALS 8  /* unreliable messages being put together, per channel */
#define CHANNELS 3

#define PING_EVERY_US 200000
#define REPORT_EVERY_US 100000
#define HELLO_EVERY_US 100000
#define CONNECT_TIMEOUT_US 1200000
#define IDLE_TIMEOUT_US 3000000
#define CLOSE_LINGER_US 500000
#define PARTIAL_TIMEOUT_US 250000

/* The first BURST packets of a message go out back to back; after that
 * they're spaced to PACE_BPS so a keyframe doesn't overflow the access
 * point's queue (still faster than any phone's Wi-Fi link). */
#define BURST 32
#define PACE_BPS 300e6

#define RATE_MIN 1.5e6
#define RATE_MAX 100e6

enum { T_HELLO = 1, T_WELCOME, T_DATA, T_ACK, T_FRAG, T_PING, T_PONG, T_REPORT, T_BYE };

/* ---- small helpers -------------------------------------------------------- */

static uint64_t now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  LARGE_INTEGER c;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&c);
  return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000u +
         (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Sequence numbers wrap: a is after b. */
static int32_t diff32(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void set_err(char *err, size_t n, const char *what) {
  if (err && n) snprintf(err, n, "%s", what);
}

static uint64_t splitmix(uint64_t *s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/* ---- state ---------------------------------------------------------------- */

typedef struct {
  uint8_t *pkt; /* the whole datagram, NULL once acked */
  uint16_t len;
  uint8_t tries;
  uint64_t sent_us;
} unacked;

typedef struct {
  uint8_t present, flags;
  uint16_t len;
  uint8_t *data;
} held;

typedef struct {
  int used;
  uint32_t msg;
  uint16_t count, got;
  uint32_t last_len;
  uint64_t first_us;
  uint8_t *bits;
  uint8_t *data;
} partial;

typedef struct {
  int started;
  uint32_t delivered; /* newest message delivered or given up on */
  partial parts[PARTIALS];
} chan_rx;

typedef struct {
  uint32_t id, token;
  struct sockaddr_in addr;
  int open, closing;
  uint64_t created_us, last_recv_us, last_ping_us, last_report_us, last_hello_us, closing_since;

  /* reliable, sending */
  uint32_t snd_una, snd_nxt;
  unacked txq[WINDOW];
  /* reliable, receiving */
  uint32_t rcv_nxt;
  held rxq[WINDOW];
  uint8_t *msg;
  uint32_t msg_len, msg_cap;

  /* unreliable, sending: last message number started / fully sent */
  uint32_t msg_seq[CHANNELS], msg_done[CHANNELS];
  uint32_t pkt_seq;
  uint64_t sent_bytes;
  /* unreliable, receiving */
  chan_rx rx[CHANNELS];
  int r_started;
  uint32_t r_highest, r_pkts, r_lost, r_bytes;
  int d0_set;
  int32_t d0, dmin_cur, dmin_prev;
  uint64_t dmin_since;
  int64_t q_sum;
  uint32_t q_n, q_max;

  /* round trip */
  int rtt_set;
  double srtt, rttvar;

  /* rate control (sender) */
  double rate, send_rate;
  uint32_t loss_seen, loss_lost;
  uint64_t last_decrease_us, last_report_rx_us, report_bytes_mark;
  uint32_t emitted_rate;
#ifdef _WIN32
  QOS_FLOWID flow;
#endif
} peer;

typedef struct node {
  un_event ev;
  uint8_t *data;
  struct node *next;
} node;

struct un_ep {
  sock_t s;
  int server;
  mutex_t mu;
  peer *peers[MAX_PEERS];
  uint32_t next_id;
  node *head, *tail;
  uint32_t drop;
  uint64_t rng;
#ifdef _WIN32
  HANDLE qos;
#endif
};

/* ---- events --------------------------------------------------------------- */

static void push(un_ep *ep, int type, uint32_t peer_id, int ch, uint32_t value, uint8_t *data, uint32_t len) {
  node *n = (node *)calloc(1, sizeof *n);
  if (!n) {
    free(data);
    return;
  }
  n->ev.type = type;
  n->ev.peer = peer_id;
  n->ev.channel = ch;
  n->ev.value = value;
  n->ev.len = len;
  n->data = data;
  if (ep->tail) ep->tail->next = n;
  else ep->head = n;
  ep->tail = n;
}

/* ---- sending -------------------------------------------------------------- */

static void xmit(un_ep *ep, const struct sockaddr_in *to, const uint8_t *pkt, size_t len) {
  if (ep->drop && splitmix(&ep->rng) % 1000 < ep->drop) return;
  sendto(ep->s, (const char *)pkt, (int)len, 0, (const struct sockaddr *)to, sizeof *to);
}

static size_t header(uint8_t *p, int type, uint32_t token) {
  p[0] = MAGIC;
  p[1] = (uint8_t)type;
  put32(p + 2, token);
  return HEADER;
}

static void send_simple(un_ep *ep, peer *p, int type) {
  uint8_t b[HEADER + 2];
  size_t n = header(b, type, p->token);
  if (type == T_HELLO || type == T_WELCOME) {
    put16(b + n, VERSION);
    n += 2;
  }
  xmit(ep, &p->addr, b, n);
}

static void send_ack(un_ep *ep, peer *p) {
  uint8_t b[HEADER + 8];
  uint32_t sack = 0;
  size_t n = header(b, T_ACK, p->token);
  for (int i = 0; i < 32; i++) {
    if (p->rxq[(p->rcv_nxt + 1 + i) % WINDOW].present) sack |= 1u << i;
  }
  put32(b + n, p->rcv_nxt);
  put32(b + n + 4, sack);
  xmit(ep, &p->addr, b, n + 8);
}

static void send_ping(un_ep *ep, peer *p, uint64_t now) {
  uint8_t b[HEADER + 12];
  size_t n = header(b, T_PING, p->token);
  put32(b + n, (uint32_t)now);
  put32(b + n + 4, p->msg_done[UN_FRAMES]);
  put32(b + n + 8, p->msg_done[UN_SOUND]);
  xmit(ep, &p->addr, b, n + 12);
  p->last_ping_us = now;
}

/* ---- peers ---------------------------------------------------------------- */

static peer *find(un_ep *ep, uint32_t id) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (ep->peers[i] && ep->peers[i]->id == id) return ep->peers[i];
  }
  return NULL;
}

static peer *find_token(un_ep *ep, uint32_t token, const struct sockaddr_in *from) {
  for (int i = 0; i < MAX_PEERS; i++) {
    peer *p = ep->peers[i];
    if (p && p->token == token && p->addr.sin_addr.s_addr == from->sin_addr.s_addr &&
        p->addr.sin_port == from->sin_port)
      return p;
  }
  return NULL;
}

static peer *new_peer(un_ep *ep, uint32_t token, const struct sockaddr_in *addr, uint64_t now) {
  int slot = -1;
  for (int i = 0; i < MAX_PEERS && slot < 0; i++) {
    if (!ep->peers[i]) slot = i;
  }
  if (slot < 0) return NULL;
  peer *p = (peer *)calloc(1, sizeof *p);
  if (!p) return NULL;
  p->id = ++ep->next_id;
  p->token = token;
  p->addr = *addr;
  p->created_us = p->last_recv_us = now;
  p->rate = RATE_MAX;
  p->emitted_rate = (uint32_t)RATE_MAX;
  p->last_report_rx_us = now;
  ep->peers[slot] = p;
#ifdef _WIN32
  /* Mark our packets to this peer as audio/video (DSCP), so Wi-Fi access
   * points that honour WMM put them in the video queue. Best effort. */
  if (ep->qos) {
    QOSAddSocketToFlow(ep->qos, ep->s, (PSOCKADDR)&p->addr, QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW,
                       &p->flow);
  }
#endif
  return p;
}

static void free_peer(un_ep *ep, peer *p) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (ep->peers[i] == p) ep->peers[i] = NULL;
  }
#ifdef _WIN32
  if (ep->qos && p->flow) QOSRemoveSocketFromFlow(ep->qos, ep->s, p->flow, 0);
#endif
  for (int i = 0; i < WINDOW; i++) {
    free(p->txq[i].pkt);
    free(p->rxq[i].data);
  }
  for (int c = 0; c < CHANNELS; c++) {
    for (int i = 0; i < PARTIALS; i++) {
      free(p->rx[c].parts[i].bits);
      free(p->rx[c].parts[i].data);
    }
  }
  free(p->msg);
  free(p);
}

static void close_now(un_ep *ep, peer *p, uint32_t why) {
  if (why != UN_CLOSE_TIMEOUT && why != UN_CLOSE_BYE && why != UN_CLOSE_NO_ANSWER) {
    send_simple(ep, p, T_BYE);
    send_simple(ep, p, T_BYE);
  }
  push(ep, UN_EV_CLOSED, p->id, 0, why, NULL, 0);
  free_peer(ep, p);
}

static void mark_open(un_ep *ep, peer *p) {
  if (p->open) return;
  p->open = 1;
  char text[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &p->addr.sin_addr, text, sizeof text);
  uint32_t len = (uint32_t)strlen(text);
  uint8_t *data = (uint8_t *)malloc(len + 1);
  if (data) memcpy(data, text, len + 1);
  push(ep, UN_EV_OPEN, p->id, 0, 0, data, data ? len : 0);
}

/* ---- round trip and rate ---------------------------------------------------- */

static void rtt_sample(peer *p, double us) {
  if (us <= 0 || us > 5e6) return;
  if (!p->rtt_set) {
    p->srtt = us;
    p->rttvar = us / 2;
    p->rtt_set = 1;
  } else {
    double d = p->srtt > us ? p->srtt - us : us - p->srtt;
    p->rttvar = 0.75 * p->rttvar + 0.25 * d;
    p->srtt = 0.875 * p->srtt + 0.125 * us;
  }
}

static uint64_t rto(const peer *p) {
  double r = p->rtt_set ? p->srtt + 4 * p->rttvar : 30000;
  if (r < 4000) r = 4000;
  if (r > 250000) r = 250000;
  return (uint64_t)r;
}

/* The receiver's report: back off quickly on heavy loss or a growing queue,
 * climb back while the network keeps up, hold in between. A few percent of
 * scattered loss is normal on Wi-Fi and not a sign of a full link (the
 * same split WebRTC's congestion control uses: < 2% / > 10%). */
static void on_report(un_ep *ep, peer *p, const uint8_t *b, size_t n, uint64_t now) {
  if (n < 20) return;
  uint32_t pkts = get32(b), lost = get32(b + 4), q_avg = get32(b + 12);
  double interval = (double)(now - p->last_report_rx_us);
  if (interval < 50000) interval = 50000;
  if (interval > 1000000) interval = 1000000;
  p->last_report_rx_us = now;
  double sent = (double)(p->sent_bytes - p->report_bytes_mark) * 8 * 1e6 / interval;
  p->report_bytes_mark = p->sent_bytes;
  p->send_rate = p->send_rate > 0 ? 0.5 * p->send_rate + 0.5 * sent : sent;

  /* Loss is judged over at least 100 packets: a quiet page sends a handful
   * per report, and two of ten lost isn't a 20% loss rate. */
  p->loss_seen += pkts + lost;
  p->loss_lost += lost;
  double loss = -1; /* not enough to go on yet */
  if (p->loss_seen >= 100) {
    loss = (double)p->loss_lost / p->loss_seen;
    p->loss_seen = p->loss_lost = 0;
  }
  if (loss > 0.10 || q_avg > 15000) {
    if (now - p->last_decrease_us > 300000) {
      /* Below what was actually getting through when it filled up. */
      double base = p->send_rate > 0 && p->send_rate < p->rate ? p->send_rate : p->rate;
      p->rate = base * 0.8;
      if (p->rate < RATE_MIN) p->rate = RATE_MIN;
      p->last_decrease_us = now;
    }
  } else if (loss >= 0 && loss < 0.02 && q_avg < 5000) {
    p->rate *= 1.15;
    if (p->rate > RATE_MAX) p->rate = RATE_MAX;
  }
  double e = p->emitted_rate;
  if (p->rate > e * 1.05 || p->rate < e * 0.95) {
    p->emitted_rate = (uint32_t)p->rate;
    push(ep, UN_EV_RATE, p->id, 0, p->emitted_rate, NULL, 0);
  }
}

/* ---- receiving: reliable ---------------------------------------------------- */

static void on_ack(un_ep *ep, peer *p, uint32_t next, uint32_t sack, uint64_t now) {
  if (diff32(next, p->snd_nxt) > 0) return; /* acks something never sent */
  while (diff32(next, p->snd_una) > 0) {
    unacked *u = &p->txq[p->snd_una % WINDOW];
    if (u->pkt) {
      if (u->tries == 1) rtt_sample(p, (double)(now - u->sent_us));
      free(u->pkt);
      u->pkt = NULL;
    }
    p->snd_una++;
  }
  int highest = -1;
  for (int i = 0; i < 32; i++) {
    if (!(sack & (1u << i))) continue;
    uint32_t seq = next + 1 + (uint32_t)i;
    if (diff32(seq, p->snd_nxt) >= 0) break;
    unacked *u = &p->txq[seq % WINDOW];
    free(u->pkt);
    u->pkt = NULL;
    highest = i;
  }
  /* Holes below a packet that did arrive: resend them now rather than
   * waiting for the timer (each at most once per half round trip). */
  uint64_t gap = p->rtt_set ? (uint64_t)(p->srtt / 2) + 1000 : 2000;
  for (int i = -1; i < highest; i++) {
    uint32_t seq = next + 1 + (uint32_t)i;
    unacked *u = &p->txq[seq % WINDOW];
    if (u->pkt && now - u->sent_us > gap) {
      xmit(ep, &p->addr, u->pkt, u->len);
      u->sent_us = now;
      if (u->tries < 255) u->tries++;
    }
  }
}

static void on_data(un_ep *ep, peer *p, const uint8_t *b, size_t n) {
  if (n < 5) return;
  uint32_t seq = get32(b);
  uint8_t flags = b[4];
  b += 5;
  n -= 5;
  int32_t ahead = diff32(seq, p->rcv_nxt);
  if (ahead >= 0 && ahead < WINDOW) {
    held *h = &p->rxq[seq % WINDOW];
    if (!h->present) {
      h->data = (uint8_t *)malloc(n ? n : 1);
      if (!h->data) return;
      memcpy(h->data, b, n);
      h->len = (uint16_t)n;
      h->flags = flags;
      h->present = 1;
    }
  }
  /* Hand over everything now in order. */
  for (;;) {
    held *h = &p->rxq[p->rcv_nxt % WINDOW];
    if (!h->present) break;
    if (p->msg_len + h->len > p->msg_cap) {
      uint32_t cap = p->msg_cap ? p->msg_cap * 2 : 4096;
      while (cap < p->msg_len + h->len) cap *= 2;
      uint8_t *m = cap <= MAX_MESSAGE ? (uint8_t *)realloc(p->msg, cap) : NULL;
      if (!m) {
        p->msg_len = 0; /* drop the message; keep the stream going */
      } else {
        p->msg = m;
        p->msg_cap = cap;
      }
    }
    if (p->msg_len + h->len <= p->msg_cap) {
      memcpy(p->msg + p->msg_len, h->data, h->len);
      p->msg_len += h->len;
    }
    if (h->flags & 1) {
      push(ep, UN_EV_MESSAGE, p->id, UN_RELIABLE, 0, p->msg, p->msg_len);
      p->msg = NULL;
      p->msg_len = p->msg_cap = 0;
    }
    free(h->data);
    h->data = NULL;
    h->present = 0;
    p->rcv_nxt++;
  }
  send_ack(ep, p);
}

/* ---- receiving: frames and sound ---------------------------------------------- */

static void drop_partial(partial *q) {
  free(q->bits);
  free(q->data);
  memset(q, 0, sizeof *q);
}

/* Gives up on every message on `ch` before `msg` that isn't in yet. */
static void lose_before(un_ep *ep, peer *p, int ch, uint32_t msg) {
  chan_rx *c = &p->rx[ch];
  int reported = 0;
  for (uint32_t m = c->delivered + 1; diff32(msg, m) > 0; m++) {
    for (int i = 0; i < PARTIALS; i++) {
      if (c->parts[i].used && c->parts[i].msg == m) drop_partial(&c->parts[i]);
    }
    /* Report each one (the app acks it), within reason after a long gap. */
    if (reported++ < 256) push(ep, UN_EV_LOST, p->id, ch, m, NULL, 0);
  }
  if (diff32(msg - 1, c->delivered) > 0) c->delivered = msg - 1;
}

static void on_frag(un_ep *ep, peer *p, const uint8_t *b, size_t n, uint64_t now) {
  if (n < 17) return;
  int ch = b[0];
  uint32_t msg = get32(b + 1);
  uint16_t idx = get16(b + 5), count = get16(b + 7);
  uint32_t pkt = get32(b + 9), sent = get32(b + 13);
  const uint8_t *data = b + 17;
  size_t len = n - 17;
  if (ch != UN_FRAMES && ch != UN_SOUND) return;
  if (!count || idx >= count || len > PIECE || (idx + 1 < count && len != PIECE)) return;

  /* Loss and queueing delay, for the report. */
  p->r_pkts++;
  p->r_bytes += (uint32_t)n;
  if (!p->r_started) {
    p->r_started = 1;
    p->r_highest = pkt;
  } else if (diff32(pkt, p->r_highest) > 0) {
    p->r_lost += (uint32_t)diff32(pkt, p->r_highest) - 1;
    p->r_highest = pkt;
  } else if (p->r_lost) {
    p->r_lost--; /* arrived late, not lost */
  }
  int32_t d = (int32_t)((uint32_t)now - sent);
  if (!p->d0_set) {
    p->d0 = d;
    p->d0_set = 1;
    p->dmin_cur = p->dmin_prev = 0;
    p->dmin_since = now;
  }
  int32_t rel = d - p->d0;
  if (now - p->dmin_since > 1000000) {
    p->dmin_prev = p->dmin_cur;
    p->dmin_cur = rel;
    p->dmin_since = now;
  }
  if (rel < p->dmin_cur) p->dmin_cur = rel;
  int32_t base = p->dmin_cur < p->dmin_prev ? p->dmin_cur : p->dmin_prev;
  uint32_t q = rel > base ? (uint32_t)(rel - base) : 0;
  p->q_sum += q;
  p->q_n++;
  if (q > p->q_max) p->q_max = q;

  chan_rx *c = &p->rx[ch];
  if (!c->started) {
    c->started = 1;
    c->delivered = msg - 1;
  }
  if (diff32(msg, c->delivered) <= 0) return; /* too late */
  partial *q2 = NULL;
  for (int i = 0; i < PARTIALS && !q2; i++) {
    if (c->parts[i].used && c->parts[i].msg == msg) q2 = &c->parts[i];
  }
  if (!q2) {
    for (int i = 0; i < PARTIALS && !q2; i++) {
      if (!c->parts[i].used) q2 = &c->parts[i];
    }
    if (!q2) {
      /* All busy: the oldest can't be coming any more. */
      partial *old = &c->parts[0];
      for (int i = 1; i < PARTIALS; i++) {
        if (diff32(old->msg, c->parts[i].msg) > 0) old = &c->parts[i];
      }
      lose_before(ep, p, ch, old->msg + 1);
      q2 = old;
    }
    q2->bits = (uint8_t *)calloc((count + 7u) / 8u, 1);
    q2->data = (uint8_t *)malloc((size_t)count * PIECE);
    if (!q2->bits || !q2->data) {
      drop_partial(q2);
      return;
    }
    q2->used = 1;
    q2->msg = msg;
    q2->count = count;
    q2->first_us = now;
  }
  if (q2->count != count || (q2->bits[idx / 8] & (1u << (idx % 8)))) return;
  q2->bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
  memcpy(q2->data + (size_t)idx * PIECE, data, len);
  if (idx + 1 == count) q2->last_len = (uint32_t)len;
  if (++q2->got < count) return;

  uint32_t total = (uint32_t)(count - 1) * PIECE + q2->last_len;
  uint8_t *whole = q2->data;
  q2->data = NULL;
  drop_partial(q2);
  lose_before(ep, p, ch, msg);
  push(ep, UN_EV_MESSAGE, p->id, ch, msg, whole, total);
  c->delivered = msg;
}

/* A ping says which messages were fully sent before it: anything older that
 * hasn't started arriving was lost on the way. */
static void on_ping_marks(un_ep *ep, peer *p, uint32_t frames, uint32_t sound) {
  uint32_t last[CHANNELS] = {0, frames, sound};
  for (int ch = UN_FRAMES; ch <= UN_SOUND; ch++) {
    chan_rx *c = &p->rx[ch];
    if (!c->started || diff32(last[ch], c->delivered) <= 0) continue;
    int busy = 0;
    for (int i = 0; i < PARTIALS; i++) busy |= c->parts[i].used;
    if (!busy) lose_before(ep, p, ch, last[ch] + 1);
  }
}

static void handle(un_ep *ep, const uint8_t *b, size_t n, const struct sockaddr_in *from, uint64_t now) {
  if (n < HEADER || b[0] != MAGIC) return;
  int type = b[1];
  uint32_t token = get32(b + 2);
  b += HEADER;
  n -= HEADER;
  peer *p = find_token(ep, token, from);
  if (!p) {
    if (!ep->server || type != T_HELLO || n < 2) return;
    p = new_peer(ep, token, from, now);
    if (!p) {
      /* Full: the oldest gives way to the newest. */
      peer *old = NULL;
      for (int i = 0; i < MAX_PEERS; i++) {
        if (ep->peers[i] && (!old || ep->peers[i]->created_us < old->created_us)) old = ep->peers[i];
      }
      if (old) close_now(ep, old, UN_CLOSE_HERE);
      p = new_peer(ep, token, from, now);
      if (!p) return;
    }
  }
  p->last_recv_us = now;
  switch (type) {
    case T_HELLO:
      if (ep->server) {
        send_simple(ep, p, T_WELCOME);
        mark_open(ep, p);
      }
      break;
    case T_WELCOME:
      if (!ep->server) mark_open(ep, p);
      break;
    case T_DATA:
      if (!ep->server) mark_open(ep, p);
      on_data(ep, p, b, n);
      break;
    case T_ACK:
      if (n >= 8) on_ack(ep, p, get32(b), get32(b + 4), now);
      break;
    case T_FRAG:
      if (!ep->server) mark_open(ep, p);
      on_frag(ep, p, b, n, now);
      break;
    case T_PING:
      if (n >= 4) {
        uint8_t r[HEADER + 4];
        size_t h = header(r, T_PONG, p->token);
        memcpy(r + h, b, 4);
        xmit(ep, &p->addr, r, h + 4);
      }
      if (n >= 12) on_ping_marks(ep, p, get32(b + 4), get32(b + 8));
      break;
    case T_PONG:
      if (n >= 4) rtt_sample(p, (double)(uint32_t)((uint32_t)now - get32(b)));
      break;
    case T_REPORT:
      on_report(ep, p, b, n, now);
      break;
    case T_BYE:
      push(ep, UN_EV_CLOSED, p->id, 0, UN_CLOSE_BYE, NULL, 0);
      free_peer(ep, p);
      break;
  }
}

/* ---- timers ----------------------------------------------------------------- */

static void timers(un_ep *ep, uint64_t now) {
  for (int i = 0; i < MAX_PEERS; i++) {
    peer *p = ep->peers[i];
    if (!p) continue;
    if (!p->open && !ep->server) {
      if (now - p->created_us > CONNECT_TIMEOUT_US) {
        close_now(ep, p, UN_CLOSE_NO_ANSWER);
        continue;
      }
      if (now - p->last_hello_us > HELLO_EVERY_US) {
        send_simple(ep, p, T_HELLO);
        p->last_hello_us = now;
      }
      continue;
    }
    if (now - p->last_recv_us > IDLE_TIMEOUT_US) {
      close_now(ep, p, UN_CLOSE_TIMEOUT);
      continue;
    }
    if (p->closing && (p->snd_una == p->snd_nxt || now - p->closing_since > CLOSE_LINGER_US)) {
      close_now(ep, p, UN_CLOSE_HERE);
      continue;
    }
    if (now - p->last_ping_us > PING_EVERY_US) send_ping(ep, p, now);
    /* Resend what the timer says is overdue. */
    uint64_t t = rto(p);
    for (uint32_t s = p->snd_una; s != p->snd_nxt; s++) {
      unacked *u = &p->txq[s % WINDOW];
      uint64_t wait = t * (u->tries < 4 ? u->tries : 4);
      if (u->pkt && now - u->sent_us > wait) {
        xmit(ep, &p->addr, u->pkt, u->len);
        u->sent_us = now;
        if (u->tries < 255) u->tries++;
      }
    }
    /* Messages that stopped arriving halfway. */
    for (int ch = UN_FRAMES; ch <= UN_SOUND; ch++) {
      chan_rx *c = &p->rx[ch];
      for (int k = 0; k < PARTIALS; k++) {
        partial *q = &c->parts[k];
        if (q->used && now - q->first_us > PARTIAL_TIMEOUT_US) {
          uint32_t m = q->msg;
          lose_before(ep, p, ch, m);
          drop_partial(q);
          push(ep, UN_EV_LOST, p->id, ch, m, NULL, 0);
          if (diff32(m, c->delivered) > 0) c->delivered = m;
        }
      }
    }
    if (p->r_pkts + p->r_lost && now - p->last_report_us > REPORT_EVERY_US) {
      uint8_t b[HEADER + 20];
      size_t h = header(b, T_REPORT, p->token);
      put32(b + h, p->r_pkts);
      put32(b + h + 4, p->r_lost);
      put32(b + h + 8, p->r_bytes);
      put32(b + h + 12, p->q_n ? (uint32_t)(p->q_sum / p->q_n) : 0);
      put32(b + h + 16, p->q_max);
      xmit(ep, &p->addr, b, h + 20);
      p->r_pkts = p->r_lost = p->r_bytes = 0;
      p->q_sum = 0;
      p->q_n = p->q_max = 0;
      p->last_report_us = now;
    }
  }
}

/* ---- public ----------------------------------------------------------------- */

static int net_init(char *err, size_t n) {
#ifdef _WIN32
  static int done;
  if (!done) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
      set_err(err, n, "WSAStartup failed");
      return 0;
    }
    /* 1 ms timer resolution: the receive loop's waits are that short. */
    timeBeginPeriod(1);
    done = 1;
  }
#else
  (void)err;
  (void)n;
#endif
  return 1;
}

static un_ep *make_ep(int server, char *err, size_t n) {
  if (!net_init(err, n)) return NULL;
  un_ep *ep = (un_ep *)calloc(1, sizeof *ep);
  if (!ep) {
    set_err(err, n, "out of memory");
    return NULL;
  }
  ep->server = server;
  ep->rng = now_us() ^ (uint64_t)(uintptr_t)ep;
  mutex_init(&ep->mu);
  ep->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ep->s == BAD_SOCK) {
    set_err(err, n, "no UDP socket");
    free(ep);
    return NULL;
  }
  /* Not passed on to programs we start (the agent starts Umbra, which
   * would otherwise keep the port after the agent restarts). */
#ifdef _WIN32
  SetHandleInformation((HANDLE)ep->s, HANDLE_FLAG_INHERIT, 0);
#else
  fcntl(ep->s, F_SETFD, FD_CLOEXEC);
#endif
  /* Room for a few keyframes in each direction. */
  int big = 4 << 20;
  setsockopt(ep->s, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
  setsockopt(ep->s, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
#ifdef _WIN32
  /* Otherwise an ICMP "port unreachable" makes the next recv fail. */
  BOOL no = FALSE;
  DWORD ret = 0;
  WSAIoctl(ep->s, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &ret, NULL, NULL);
  u_long nb = 1;
  ioctlsocket(ep->s, FIONBIO, &nb);
  QOS_VERSION v = {1, 0};
  if (!QOSCreateHandle(&v, &ep->qos)) ep->qos = NULL;
#else
  fcntl(ep->s, F_SETFL, fcntl(ep->s, F_GETFL) | O_NONBLOCK);
  /* Expedited forwarding: Wi-Fi (WMM) sends it in the voice queue, ahead
   * of downloads. Our uplink is only taps, acks and reports. */
  int tos = 0xB8;
  setsockopt(ep->s, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
#endif
  return ep;
}

un_ep *un_listen(uint16_t port, char *err, size_t err_len) {
  un_ep *ep = make_ep(1, err, err_len);
  if (!ep) return NULL;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ep->s, (struct sockaddr *)&a, sizeof a) != 0) {
    set_err(err, err_len, "UDP port in use");
    un_free(ep);
    return NULL;
  }
  return ep;
}

un_ep *un_connect(const char *host, uint16_t port, char *err, size_t err_len) {
  un_ep *ep = make_ep(0, err, err_len);
  if (!ep) return NULL;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
    set_err(err, err_len, "unknown host");
    un_free(ep);
    return NULL;
  }
  struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
  freeaddrinfo(res);
  to.sin_port = htons(port);
  uint32_t token = (uint32_t)splitmix(&ep->rng) | 1u;
  uint64_t now = now_us();
  mutex_lock(&ep->mu);
  peer *p = new_peer(ep, token, &to, now);
  if (p) {
    send_simple(ep, p, T_HELLO);
    p->last_hello_us = now;
  }
  mutex_unlock(&ep->mu);
  if (!p) {
    set_err(err, err_len, "out of memory");
    un_free(ep);
    return NULL;
  }
  return ep;
}

int un_poll(un_ep *ep, un_event *ev, uint8_t *buf, uint32_t cap, int timeout_ms) {
  uint64_t deadline = now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
  uint8_t pkt[2048];
  int looked = 0;
  for (;;) {
    mutex_lock(&ep->mu);
    uint64_t now = now_us();
    timers(ep, now);
    node *n = ep->head;
    if (n) {
      ep->head = n->next;
      if (!ep->head) ep->tail = NULL;
    }
    mutex_unlock(&ep->mu);
    if (n) {
      *ev = n->ev;
      if (n->ev.len > cap) {
        ev->type = UN_EV_TOO_BIG;
        ev->len = 0;
      } else if (n->ev.len) {
        memcpy(buf, n->data, n->ev.len);
      }
      free(n->data);
      free(n);
      return 1;
    }
    if (looked && now >= deadline) return 0;
    looked = 1;
    /* Wake for the next packet, or in 2 ms for the timers. */
    uint64_t left = deadline > now ? deadline - now : 0;
    if (left > 2000) left = 2000;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(ep->s, &r);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = (long)left;
    int ready = select((int)ep->s + 1, &r, NULL, NULL, &tv);
    if (ready <= 0) continue;
    mutex_lock(&ep->mu);
    for (int i = 0; i < 256; i++) {
      struct sockaddr_in from;
      socklen_t fl = sizeof from;
      int got = (int)recvfrom(ep->s, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
      if (got <= 0) break;
      if (ep->drop && splitmix(&ep->rng) % 1000 < ep->drop) continue;
      handle(ep, pkt, (size_t)got, &from, now_us());
    }
    mutex_unlock(&ep->mu);
  }
}

static void spin_until(uint64_t t) {
  while (now_us() < t) {
#ifdef _WIN32
    YieldProcessor();
#endif
  }
}

int un_send(un_ep *ep, uint32_t peer_id, int ch, const uint8_t *data, uint32_t len) {
  if (len > MAX_MESSAGE) return -3;
  uint32_t pieces = len ? (len + PIECE - 1) / PIECE : 1;
  uint8_t b[HEADER + 17 + PIECE];
  mutex_lock(&ep->mu);
  peer *p = find(ep, peer_id);
  if (!p || !p->open || p->closing) {
    mutex_unlock(&ep->mu);
    return -1;
  }
  uint64_t now = now_us();
  if (ch == UN_RELIABLE) {
    if (p->snd_nxt - p->snd_una + pieces > WINDOW) {
      mutex_unlock(&ep->mu);
      return -2;
    }
    for (uint32_t i = 0; i < pieces; i++) {
      uint32_t off = i * PIECE, part = len - off < PIECE ? len - off : PIECE;
      size_t n = header(b, T_DATA, p->token);
      put32(b + n, p->snd_nxt);
      b[n + 4] = i + 1 == pieces ? 1 : 0;
      memcpy(b + n + 5, data + off, part);
      n += 5 + part;
      unacked *u = &p->txq[p->snd_nxt % WINDOW];
      free(u->pkt);
      u->pkt = (uint8_t *)malloc(n);
      if (u->pkt) memcpy(u->pkt, b, n);
      u->len = (uint16_t)n;
      u->tries = 1;
      u->sent_us = now;
      p->snd_nxt++;
      xmit(ep, &p->addr, b, n);
    }
    mutex_unlock(&ep->mu);
    return 0;
  }
  if ((ch != UN_FRAMES && ch != UN_SOUND) || pieces > 65535) {
    mutex_unlock(&ep->mu);
    return -3;
  }
  uint32_t msg = ++p->msg_seq[ch];
  uint32_t first_pkt = p->pkt_seq;
  p->pkt_seq += pieces;
  uint32_t token = p->token;
  struct sockaddr_in to = p->addr;
  p->sent_bytes += len + pieces * (HEADER + 17u);
  mutex_unlock(&ep->mu);

  /* Out of the lock: a paced keyframe mustn't hold up acks and taps. */
  uint64_t spacing = (uint64_t)((HEADER + 17 + PIECE) * 8 * 1e6 / PACE_BPS);
  uint64_t next = now_us();
  for (uint32_t i = 0; i < pieces; i++) {
    uint32_t off = i * PIECE, part = len - off < PIECE ? len - off : PIECE;
    size_t n = header(b, T_FRAG, token);
    b[n] = (uint8_t)ch;
    put32(b + n + 1, msg);
    put16(b + n + 5, (uint16_t)i);
    put16(b + n + 7, (uint16_t)pieces);
    put32(b + n + 9, first_pkt + i);
    if (i >= BURST) {
      next += spacing;
      spin_until(next);
    }
    put32(b + n + 13, (uint32_t)now_us());
    if (part) memcpy(b + n + 17, data + off, part);
    xmit(ep, &to, b, n + 17 + part);
  }
  mutex_lock(&ep->mu);
  p = find(ep, peer_id);
  if (p && diff32(msg, p->msg_done[ch]) > 0) p->msg_done[ch] = msg;
  mutex_unlock(&ep->mu);
  return 0;
}

void un_close_peer(un_ep *ep, uint32_t peer_id) {
  mutex_lock(&ep->mu);
  peer *p = find(ep, peer_id);
  if (p && !p->closing) {
    p->closing = 1;
    p->closing_since = now_us();
  }
  mutex_unlock(&ep->mu);
}

uint32_t un_rtt_us(un_ep *ep, uint32_t peer_id) {
  mutex_lock(&ep->mu);
  peer *p = find(ep, peer_id);
  uint32_t r = p && p->rtt_set ? (uint32_t)p->srtt : 0;
  mutex_unlock(&ep->mu);
  return r;
}

void un_set_drop(un_ep *ep, uint32_t per_mille) {
  mutex_lock(&ep->mu);
  ep->drop = per_mille;
  mutex_unlock(&ep->mu);
}

void un_free(un_ep *ep) {
  if (!ep) return;
  mutex_lock(&ep->mu);
  for (int i = 0; i < MAX_PEERS; i++) {
    peer *p = ep->peers[i];
    if (!p) continue;
    if (p->open) send_simple(ep, p, T_BYE);
    free_peer(ep, p);
  }
  while (ep->head) {
    node *n = ep->head;
    ep->head = n->next;
    free(n->data);
    free(n);
  }
  mutex_unlock(&ep->mu);
#ifdef _WIN32
  if (ep->qos) QOSCloseHandle(ep->qos);
#endif
  if (ep->s != BAD_SOCK) close_sock(ep->s);
  mutex_destroy(&ep->mu);
  free(ep);
}
