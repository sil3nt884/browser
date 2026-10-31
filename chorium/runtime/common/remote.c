/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The remote control (see remote.h): records what the app draws, and serves
 * one test client at a time on its own thread. Each request runs on the UI
 * thread; timed input (a long press, a swipe) is paced from the server
 * thread, one step at a time, like a real finger.
 */
#include "remote.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

/* Portable C shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

static bool g_enabled;

static char* dup_cstr(const char* s) {
  size_t n = strlen(s) + 1;
  char* out = malloc(n);
  memcpy(out, s, n);
  return out;
}

static uint64_t now_ms(void) {
#if defined(_WIN32)
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms) {
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)ms * 1000);
#endif
}

/* --- text building ---------------------------------------------------------- */

typedef struct {
  char* s;
  size_t len, cap;
} sb_t;

static void sb_add(sb_t* b, const char* s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 1024;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    b->s = realloc(b->s, cap);
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void sb_cat(sb_t* b, const char* s) { sb_add(b, s, strlen(s)); }

static void sb_printf(sb_t* b, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    sb_add(b, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
  }
}

/* |s| as a JSON string, quotes included. */
static void sb_json(sb_t* b, const char* s, size_t n) {
  sb_cat(b, "\"");
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      char esc[2] = {'\\', (char)c};
      sb_add(b, esc, 2);
    } else if (c < 0x20) {
      sb_printf(b, "\\u%04x", c);
    } else {
      sb_add(b, (const char*)&c, 1);
    }
  }
  sb_cat(b, "\"");
}

static void sb_base64(sb_t* b, const uint8_t* data, size_t len) {
  static const char k[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[4];
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) v |= data[i + 2];
    out[0] = k[(v >> 18) & 63];
    out[1] = k[(v >> 12) & 63];
    out[2] = i + 1 < len ? k[(v >> 6) & 63] : '=';
    out[3] = i + 2 < len ? k[v & 63] : '=';
    sb_add(b, out, 4);
  }
}

/* --- what the app drew -------------------------------------------------------- */

typedef struct {
  int x, y, w, h;
} box_t;

typedef struct {
  char* text;
  box_t box;
  float size;
} r_text_t;

typedef struct {
  int icon;
  box_t box;
} r_icon_t;

typedef struct {
  uint32_t id;
  box_t box;
} r_page_t;

typedef struct {
  r_text_t* texts;
  size_t text_count, text_cap;
  r_icon_t* icons;
  size_t icon_count, icon_cap;
  r_page_t pages[8];
  size_t page_count;
} r_frame_t;

static r_frame_t g_drawing, g_shown;
#define MAX_CLIPS 32
static box_t g_clips[MAX_CLIPS];
static int g_clip_depth;

/* chorium.wit's icon enum, by index. */
static const char* const kIconNames[] = {
    "back",     "forward",  "reload",  "stop",         "close",
    "add",      "menu",     "search",  "lock",         "info",
    "page",     "settings", "history", "home",         "check",
    "minimize", "maximize", "restore", "close-window", "download",
    "folder",
};

static struct {
  uint64_t frames, page_frames;
  uint64_t present_times[128]; /* ring, for fps */
  bool keyboard, fullscreen;
  char title[512];
  bool picker_open;
  uint32_t picker_request;
  int picker_mode;
  char picker_accept[1024];
  bool intercept_open;
  sb_t opened; /* JSON objects, comma separated */
} g_state;

static void frame_clear(r_frame_t* f) {
  for (size_t i = 0; i < f->text_count; ++i) {
    free(f->texts[i].text);
  }
  f->text_count = 0;
  f->icon_count = 0;
  f->page_count = 0;
}

/* |b| cut to the current clip; false if nothing of it shows. */
static bool visible(box_t* b) {
  if (g_clip_depth > 0) {
    box_t c = g_clips[g_clip_depth - 1];
    int x1 = b->x > c.x ? b->x : c.x;
    int y1 = b->y > c.y ? b->y : c.y;
    int x2 = b->x + b->w < c.x + c.w ? b->x + b->w : c.x + c.w;
    int y2 = b->y + b->h < c.y + c.h ? b->y + b->h : c.y + c.h;
    b->x = x1;
    b->y = y1;
    b->w = x2 - x1;
    b->h = y2 - y1;
  }
  return b->w > 0 && b->h > 0;
}

void remote_text(int x, int baseline, const char* utf8, size_t len,
                 float size) {
  if (!g_enabled || !len) {
    return;
  }
  uint32_t w, h;
  surface_measure_text(utf8, len, size, &w, &h);
  box_t b = {x, baseline - (int)(size * 0.8f), (int)w, (int)(size * 1.1f)};
  if (!visible(&b)) {
    return;
  }
  if (g_drawing.text_count == g_drawing.text_cap) {
    g_drawing.text_cap = g_drawing.text_cap ? g_drawing.text_cap * 2 : 256;
    g_drawing.texts =
        realloc(g_drawing.texts, sizeof(r_text_t) * g_drawing.text_cap);
  }
  r_text_t* t = &g_drawing.texts[g_drawing.text_count++];
  t->text = malloc(len + 1);
  memcpy(t->text, utf8, len);
  t->text[len] = '\0';
  t->box = b;
  t->size = size;
}

void remote_icon(int icon, int x, int y, int w, int h) {
  box_t b = {x, y, w, h};
  if (!g_enabled || icon < 0 || !visible(&b)) {
    return;
  }
  if (g_drawing.icon_count == g_drawing.icon_cap) {
    g_drawing.icon_cap = g_drawing.icon_cap ? g_drawing.icon_cap * 2 : 64;
    g_drawing.icons =
        realloc(g_drawing.icons, sizeof(r_icon_t) * g_drawing.icon_cap);
  }
  g_drawing.icons[g_drawing.icon_count].icon = icon;
  g_drawing.icons[g_drawing.icon_count++].box = b;
}

void remote_page(uint32_t page_id, int x, int y, int w, int h) {
  box_t b = {x, y, w, h};
  if (!g_enabled || g_drawing.page_count == 8 || !visible(&b)) {
    return;
  }
  g_drawing.pages[g_drawing.page_count].id = page_id;
  g_drawing.pages[g_drawing.page_count++].box = b;
}

void remote_clip(int x, int y, int w, int h) {
  if (!g_enabled) {
    return;
  }
  box_t b = {x, y, w, h};
  if (g_clip_depth > 0) {
    visible(&b); /* nested clips intersect */
    if (b.w < 0) b.w = 0;
    if (b.h < 0) b.h = 0;
  }
  if (g_clip_depth < MAX_CLIPS) {
    g_clips[g_clip_depth] = b;
  }
  ++g_clip_depth;
}

void remote_unclip(void) {
  if (g_enabled && g_clip_depth > 0) {
    --g_clip_depth;
  }
}

void remote_present(void) {
  if (!g_enabled) {
    return;
  }
  frame_clear(&g_shown);
  r_frame_t done = g_drawing;
  g_drawing = g_shown; /* reuse the arrays */
  g_shown = done;
  g_clip_depth = 0;
  g_state.present_times[g_state.frames % 128] = now_ms();
  ++g_state.frames;
}

void remote_page_frame(void) { ++g_state.page_frames; }
void remote_keyboard(bool shown) { g_state.keyboard = shown; }
void remote_fullscreen(bool on) { g_state.fullscreen = on; }

void remote_title(const char* utf8, size_t len) {
  if (len >= sizeof g_state.title) {
    len = sizeof g_state.title - 1;
  }
  memcpy(g_state.title, utf8, len);
  g_state.title[len] = '\0';
}

void remote_picker(uint32_t request, int mode, const char* const* accept,
                   size_t count) {
  g_state.picker_open = true;
  g_state.picker_request = request;
  g_state.picker_mode = mode;
  g_state.picker_accept[0] = '\0';
  for (size_t i = 0; i < count; ++i) {
    size_t used = strlen(g_state.picker_accept);
    snprintf(g_state.picker_accept + used, sizeof g_state.picker_accept - used,
             "%s%s", i ? "\n" : "", accept[i]);
  }
}

void remote_picker_closed(void) { g_state.picker_open = false; }

bool remote_open_file(const char* path, bool in_folder) {
  if (!g_enabled || !g_state.intercept_open) {
    return false;
  }
  if (g_state.opened.len) {
    sb_cat(&g_state.opened, ",");
  }
  sb_cat(&g_state.opened, "{\"path\":");
  sb_json(&g_state.opened, path, strlen(path));
  sb_printf(&g_state.opened, ",\"in_folder\":%s}", in_folder ? "true" : "false");
  return true;
}

/* --- requests (UI thread) ----------------------------------------------------- */

typedef struct {
  char* line;
  remote_input_t input; /* for one input step */
  bool is_input;
  sb_t reply;
  bool done;
#if defined(_WIN32)
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE cv;
#else
  pthread_mutex_t lock;
  pthread_cond_t cv;
#endif
} request_t;

static void finish(request_t* r) {
#if defined(_WIN32)
  EnterCriticalSection(&r->lock);
  r->done = true;
  WakeConditionVariable(&r->cv);
  LeaveCriticalSection(&r->lock);
#else
  pthread_mutex_lock(&r->lock);
  r->done = true;
  pthread_cond_signal(&r->cv);
  pthread_mutex_unlock(&r->lock);
#endif
}

static void box_json(sb_t* b, box_t box) {
  sb_printf(b, "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", box.x, box.y, box.w,
            box.h);
}

static void reply_elements(sb_t* b) {
  uint32_t w, h;
  surface_dimensions(&w, &h);
  sb_printf(b, "{\"ok\":true,\"width\":%u,\"height\":%u,\"scale\":%.3f,"
               "\"texts\":[", w, h, surface_scale());
  for (size_t i = 0; i < g_shown.text_count; ++i) {
    const r_text_t* t = &g_shown.texts[i];
    sb_cat(b, i ? ",{\"text\":" : "{\"text\":");
    sb_json(b, t->text, strlen(t->text));
    sb_cat(b, ",");
    box_json(b, t->box);
    sb_printf(b, ",\"size\":%.1f}", t->size);
  }
  sb_cat(b, "],\"icons\":[");
  for (size_t i = 0; i < g_shown.icon_count; ++i) {
    const r_icon_t* ic = &g_shown.icons[i];
    int n = (int)(sizeof kIconNames / sizeof kIconNames[0]);
    sb_printf(b, "%s{\"icon\":\"%s\",", i ? "," : "",
              ic->icon < n ? kIconNames[ic->icon] : "?");
    box_json(b, ic->box);
    sb_cat(b, "}");
  }
  sb_cat(b, "],\"pages\":[");
  for (size_t i = 0; i < g_shown.page_count; ++i) {
    sb_printf(b, "%s{\"id\":%u,", i ? "," : "", g_shown.pages[i].id);
    box_json(b, g_shown.pages[i].box);
    sb_cat(b, "}");
  }
  sb_cat(b, "]}");
}

static void reply_state(sb_t* b) {
  /* Frames presented over the last second. */
  uint64_t now = now_ms(), n = 0;
  uint64_t have = g_state.frames < 128 ? g_state.frames : 128;
  for (uint64_t i = 0; i < have; ++i) {
    n += now - g_state.present_times[i] <= 1000;
  }
  sb_printf(b, "{\"ok\":true,\"frames\":%llu,\"fps\":%llu,"
               "\"page_frames\":%llu,\"keyboard\":%s,\"fullscreen\":%s,"
               "\"tor\":%d,\"tor_enabled\":%s,\"title\":",
            (unsigned long long)g_state.frames, (unsigned long long)n,
            (unsigned long long)g_state.page_frames,
            g_state.keyboard ? "true" : "false",
            g_state.fullscreen ? "true" : "false", chorium_tor_progress(),
            chorium_tor_enabled() ? "true" : "false");
  sb_json(b, g_state.title, strlen(g_state.title));
  sb_cat(b, ",\"picker\":");
  if (g_state.picker_open) {
    sb_printf(b, "{\"request\":%u,\"mode\":%d,\"accept\":",
              g_state.picker_request, g_state.picker_mode);
    sb_json(b, g_state.picker_accept, strlen(g_state.picker_accept));
    sb_cat(b, "}");
  } else {
    sb_cat(b, "null");
  }
  sb_cat(b, ",\"opened\":[");
  if (g_state.opened.len) {
    sb_cat(b, g_state.opened.s);
  }
  sb_cat(b, "]}");
}

void remote_screenshot_done(void* token, const char* format,
                            const uint8_t* data, size_t len) {
  request_t* r = token;
  if (!data || !len) {
    sb_cat(&r->reply, "{\"ok\":false,\"error\":\"no screenshot\"}");
  } else {
    sb_printf(&r->reply, "{\"ok\":true,\"format\":\"%s\",\"data\":\"", format);
    sb_base64(&r->reply, data, len);
    sb_cat(&r->reply, "\"}");
  }
  finish(r);
}

static void handle_on_ui(void* arg) {
  request_t* r = arg;
  if (r->is_input) {
    platform_remote_input(&r->input);
    sb_cat(&r->reply, "{\"ok\":true}");
    finish(r);
    return;
  }
  char* cmd = NULL;
  json_string(r->line, "cmd", &cmd);
  if (!cmd) {
    sb_cat(&r->reply, "{\"ok\":false,\"error\":\"no cmd\"}");
  } else if (strcmp(cmd, "elements") == 0) {
    reply_elements(&r->reply);
  } else if (strcmp(cmd, "state") == 0) {
    reply_state(&r->reply);
  } else if (strcmp(cmd, "screenshot") == 0) {
    free(cmd);
    platform_screenshot(r); /* finishes through remote_screenshot_done */
    return;
  } else if (strcmp(cmd, "intercept_open") == 0) {
    bool on = true;
    json_bool(r->line, "on", &on);
    g_state.intercept_open = on;
    g_state.opened.len = 0;
    sb_cat(&r->reply, "{\"ok\":true}");
  } else if (strcmp(cmd, "picker_answer") == 0) {
    /* "paths": one string, a path or content:// URI per line. */
    char* paths = NULL;
    json_string(r->line, "paths", &paths);
    const char* list[64];
    size_t count = 0;
    for (char* p = paths; p && *p && count < 64;) {
      list[count++] = p;
      char* nl = strchr(p, '\n');
      if (!nl) {
        break;
      }
      *nl = '\0';
      p = nl + 1;
    }
    bool ok = g_state.picker_open && platform_answer_picker(list, count);
    sb_cat(&r->reply, ok ? "{\"ok\":true}"
                         : "{\"ok\":false,\"error\":\"no picker to answer\"}");
    free(paths);
  } else {
    sb_printf(&r->reply, "{\"ok\":false,\"error\":\"unknown cmd\"}");
  }
  free(cmd);
  finish(r);
}

/* Runs one request on the UI thread and waits for its reply. */
static void run(request_t* r) {
#if defined(_WIN32)
  InitializeCriticalSection(&r->lock);
  InitializeConditionVariable(&r->cv);
  platform_post_to_ui(handle_on_ui, r);
  EnterCriticalSection(&r->lock);
  while (!r->done) {
    SleepConditionVariableCS(&r->cv, &r->lock, INFINITE);
  }
  LeaveCriticalSection(&r->lock);
  DeleteCriticalSection(&r->lock);
#else
  pthread_mutex_init(&r->lock, NULL);
  pthread_cond_init(&r->cv, NULL);
  platform_post_to_ui(handle_on_ui, r);
  pthread_mutex_lock(&r->lock);
  while (!r->done) {
    pthread_cond_wait(&r->cv, &r->lock);
  }
  pthread_mutex_unlock(&r->lock);
  pthread_cond_destroy(&r->cv);
  pthread_mutex_destroy(&r->lock);
#endif
}

static void input_step(remote_input_kind_t kind, int x, int y) {
  request_t r = {0};
  r.is_input = true;
  r.input.kind = kind;
  r.input.x = x;
  r.input.y = y;
  run(&r);
  free(r.reply.s);
}

static int int_field(const char* json, const char* key, int fallback) {
  long long n;
  return json_int(json, key, &n) ? (int)n : fallback;
}

/* One request line -> its reply (server thread). Timed gestures happen
 * here, a step at a time. */
static char* handle_line(const char* line) {
  char* cmd = NULL;
  json_string(line, "cmd", &cmd);
  int x = int_field(line, "x", 0), y = int_field(line, "y", 0);
  bool input = true;
  if (!cmd) {
    input = false;
  } else if (strcmp(cmd, "tap") == 0) {
    char* button = NULL;
    json_string(line, "button", &button);
    bool right = button && strcmp(button, "right") == 0;
    free(button);
    input_step(right ? RI_RIGHT_DOWN : RI_DOWN, x, y);
    sleep_ms(40);
    input_step(right ? RI_RIGHT_UP : RI_UP, x, y);
  } else if (strcmp(cmd, "press") == 0) {
    input_step(RI_DOWN, x, y);
    sleep_ms(int_field(line, "ms", 800));
    input_step(RI_UP, x, y);
  } else if (strcmp(cmd, "swipe") == 0) {
    int x2 = int_field(line, "x2", x), y2 = int_field(line, "y2", y);
    int ms = int_field(line, "ms", 300);
    int steps = ms / 16 > 2 ? ms / 16 : 2;
    input_step(RI_DOWN, x, y);
    for (int i = 1; i <= steps; ++i) {
      sleep_ms(ms / steps);
      input_step(RI_MOVE, x + (x2 - x) * i / steps, y + (y2 - y) * i / steps);
    }
    input_step(RI_UP, x2, y2);
  } else if (strcmp(cmd, "wheel") == 0 || strcmp(cmd, "key") == 0 ||
             strcmp(cmd, "text") == 0 || strcmp(cmd, "back") == 0) {
    request_t r = {0};
    r.is_input = true;
    r.input.x = x;
    r.input.y = y;
    r.input.code = int_field(line, "code", 0);
    r.input.mods = int_field(line, "mods", 0);
    char* text = NULL;
    if (strcmp(cmd, "wheel") == 0) {
      r.input.kind = RI_WHEEL;
      r.input.dy = (float)int_field(line, "dy", 100);
      run(&r);
    } else if (strcmp(cmd, "text") == 0) {
      json_string(line, "text", &text);
      r.input.kind = RI_TEXT;
      r.input.text = text ? text : "";
      run(&r);
    } else if (strcmp(cmd, "back") == 0) {
      r.input.kind = RI_BACK;
      run(&r);
    } else {
      r.input.kind = RI_KEY_DOWN;
      run(&r);
      free(r.reply.s);
      memset(&r.reply, 0, sizeof r.reply);
      r.done = false;
      r.input.kind = RI_KEY_UP;
      run(&r);
    }
    free(text);
    free(r.reply.s);
  } else {
    input = false;
  }
  free(cmd);
  if (input) {
    return dup_cstr("{\"ok\":true}");
  }
  request_t r = {0};
  r.line = (char*)line;
  run(&r);
  return r.reply.s ? r.reply.s : dup_cstr("{\"ok\":false}");
}

/* --- the server ----------------------------------------------------------- */

static sock_t g_listen = BAD_SOCKET;

static bool send_all(sock_t s, const char* data, size_t len) {
  while (len) {
    int n = send(s, data, (int)(len > 65536 ? 65536 : len), 0);
    if (n <= 0) {
      return false;
    }
    data += n;
    len -= (size_t)n;
  }
  return true;
}

static void serve(sock_t client) {
  sb_t in = {0};
  char buf[4096];
  for (;;) {
    int n = recv(client, buf, sizeof buf, 0);
    if (n <= 0) {
      break;
    }
    sb_add(&in, buf, (size_t)n);
    char* nl;
    while (in.len && (nl = memchr(in.s, '\n', in.len)) != NULL) {
      *nl = '\0';
      char* reply = handle_line(in.s);
      bool ok = send_all(client, reply, strlen(reply)) &&
                send_all(client, "\n", 1);
      free(reply);
      size_t used = (size_t)(nl - in.s) + 1;
      memmove(in.s, nl + 1, in.len - used);
      in.len -= used;
      in.s[in.len] = '\0';
      if (!ok) {
        goto done;
      }
    }
  }
done:
  free(in.s);
  close_socket(client);
}

#if defined(_WIN32)
static DWORD WINAPI server_main(void* arg) {
#else
static void* server_main(void* arg) {
#endif
  for (;;) {
    sock_t client = accept(g_listen, NULL, NULL);
    if (client == BAD_SOCKET) {
      break;
    }
#if defined(__ANDROID__)
    /* Like DevTools: only adb's shell and this app itself. */
    struct ucred cred;
    socklen_t size = sizeof cred;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 ||
        (cred.uid != 2000 && cred.uid != getuid())) {
      close_socket(client);
      continue;
    }
#endif
    serve(client);
  }
  return 0;
}

bool remote_start(void) {
  if (g_enabled) {
    return true;
  }
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  g_listen = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_NO_HANDLE_INHERIT);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(REMOTE_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
  g_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  /* Abstract namespace: a leading NUL, no file. */
  memcpy(addr.sun_path + 1, REMOTE_SOCKET_NAME, strlen(REMOTE_SOCKET_NAME));
#endif
  if (g_listen == BAD_SOCKET) {
    return false;
  }
#if defined(_WIN32)
  int size = sizeof addr;
#else
  socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
                               strlen(REMOTE_SOCKET_NAME));
#endif
  if (bind(g_listen, (struct sockaddr*)&addr, size) != 0 ||
      listen(g_listen, 2) != 0) {
    close_socket(g_listen);
    g_listen = BAD_SOCKET;
    return false;
  }
  g_enabled = true;
#if defined(_WIN32)
  HANDLE t = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
  if (t) {
    CloseHandle(t);
  }
#else
  pthread_t t;
  pthread_create(&t, NULL, server_main, NULL);
  pthread_detach(t);
#endif
  return true;
}

bool remote_enabled(void) { return g_enabled; }
