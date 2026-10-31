/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 */
#include "fakes.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "chorium.h"

/* --- recorded calls ------------------------------------------------------- */

#define MAX_CALLS 4096
static char* g_calls[MAX_CALLS];
static int g_call_count;

static void record(const char* fmt, ...) {
  if (g_call_count == MAX_CALLS) {
    return;
  }
  char line[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  g_calls[g_call_count++] = strdup(line);
}

void clear_calls(void) {
  for (int i = 0; i < g_call_count; ++i) {
    free(g_calls[i]);
  }
  g_call_count = 0;
}

bool called(const char* prefix) { return last_call(prefix) != NULL; }

int count_calls(const char* prefix) {
  int n = 0;
  for (int i = 0; i < g_call_count; ++i) {
    n += strncmp(g_calls[i], prefix, strlen(prefix)) == 0;
  }
  return n;
}

const char* last_call(const char* prefix) {
  for (int i = g_call_count - 1; i >= 0; --i) {
    if (strncmp(g_calls[i], prefix, strlen(prefix)) == 0) {
      return g_calls[i];
    }
  }
  return NULL;
}

bool any_call_contains(const char* prefix, const char* text) {
  for (int i = 0; i < g_call_count; ++i) {
    if (strncmp(g_calls[i], prefix, strlen(prefix)) == 0 &&
        strstr(g_calls[i], text)) {
      return true;
    }
  }
  return false;
}

void dump_calls(void) {
  for (int i = 0; i < g_call_count; ++i) {
    fprintf(stderr, "    | %s\n", g_calls[i]);
  }
}

/* --- the fake engine ------------------------------------------------------ */

void chorium_clear_site_data(void) { record("clear_site_data"); }
void chorium_set_clear_site_data_on_close(int on) {
  record("set_clear_on_close %d", on);
}

#define MAX_HISTORY 32
struct chorium_view {
  uint32_t page; /* the runtime's page id (the |user| pointer) */
  bool open;
  chorium_event_fn on_event;
  char* history[MAX_HISTORY];
  int at, count;
};

#define MAX_VIEWS 256
static struct chorium_view g_views[MAX_VIEWS];
static int g_view_count;
static bool g_tor = true;

/* Engine events wait here until pump(), as they would on a real UI thread. */
typedef struct {
  uint32_t page;
  char* json;
} queued_t;
#define MAX_QUEUE 1024
static queued_t g_queue[MAX_QUEUE];
static int g_queue_len;
static bool g_draw_requested;

static void queue_event(uint32_t page, const char* fmt, ...) {
  char json[8192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(json, sizeof json, fmt, ap);
  va_end(ap);
  if (g_queue_len < MAX_QUEUE) {
    g_queue[g_queue_len].page = page;
    g_queue[g_queue_len].json = strdup(json);
    ++g_queue_len;
  }
}

void engine_event(uint32_t page, const char* json) {
  queue_event(page, "%s", json);
  pump();
}

uint32_t newest_page(void) {
  for (int i = g_view_count - 1; i >= 0; --i) {
    if (g_views[i].open) {
      return g_views[i].page;
    }
  }
  return 0;
}

int open_pages(void) {
  int n = 0;
  for (int i = 0; i < g_view_count; ++i) {
    n += g_views[i].open;
  }
  return n;
}

/* What a page sends when it lands on |url|. */
static void arrive(chorium_view* v) {
  const char* url = v->history[v->at];
  queue_event(v->page, "{\"type\":\"loading\",\"loading\":true}");
  queue_event(v->page,
              "{\"type\":\"url\",\"url\":\"%s\",\"can_go_back\":%s,"
              "\"can_go_forward\":%s}",
              url, v->at > 0 ? "true" : "false",
              v->at < v->count - 1 ? "true" : "false");
  queue_event(v->page, "{\"type\":\"title\",\"title\":\"Title of %s\"}", url);
  queue_event(v->page, "{\"type\":\"loaded\",\"url\":\"%s\",\"ok\":true,"
                       "\"error\":\"\"}", url);
  queue_event(v->page, "{\"type\":\"loading\",\"loading\":false}");
}

int chorium_abi_version(void) { return CHORIUM_ABI_VERSION; }

chorium_view* chorium_open(int width, int height, chorium_frame_fn on_frame,
                           chorium_event_fn on_event, void* user) {
  if (g_view_count == MAX_VIEWS) {
    return NULL;
  }
  chorium_view* v = &g_views[g_view_count++];
  memset(v, 0, sizeof *v);
  v->page = (uint32_t)(uintptr_t)user;
  v->open = true;
  v->on_event = on_event;
  record("open %u %dx%d", v->page, width, height);
  return v;
}

void chorium_close(chorium_view* v) {
  record("close %u", v->page);
  v->open = false;
  for (int i = 0; i < v->count; ++i) {
    free(v->history[i]);
  }
  v->count = 0;
}

void chorium_load_url(chorium_view* v, const char* url) {
  record("load_url %u %s", v->page, url);
  if (v->at < v->count - 1) {
    for (int i = v->at + 1; i < v->count; ++i) {
      free(v->history[i]);
    }
    v->count = v->at + 1;
  }
  if (v->count == MAX_HISTORY) {
    return;
  }
  v->history[v->count] = strdup(url);
  v->at = v->count++;
  arrive(v);
}

void chorium_load_html(chorium_view* v, const char* html,
                       const char* base_url) {
  record("load_html %u", v->page);
}

void chorium_back(chorium_view* v) {
  record("back %u", v->page);
  if (v->at > 0) {
    --v->at;
    arrive(v);
  }
}

void chorium_forward(chorium_view* v) {
  record("forward %u", v->page);
  if (v->at < v->count - 1) {
    ++v->at;
    arrive(v);
  }
}

void chorium_reload(chorium_view* v) {
  record("reload %u", v->page);
  if (v->count) {
    arrive(v);
  }
}

void chorium_stop(chorium_view* v) { record("stop %u", v->page); }
void chorium_resize(chorium_view* v, int w, int h) {}
void chorium_refresh_frame(chorium_view* v) {}
void chorium_mouse(chorium_view* v, int type, int x, int y, int button,
                   int clicks, int mods) {
  if (type == CHORIUM_MOUSE_DOWN) {
    record("mouse_down %u %d %d %d", v->page, x, y, button);
  }
}
void chorium_wheel(chorium_view* v, int x, int y, float dx, float dy,
                   int mods) {
  record("wheel %u %.0f %.0f", v->page, dx, dy);
}
void chorium_key(chorium_view* v, int down, int code, int mods) {
  if (down) {
    record("key %u %d %d", v->page, code, mods);
  }
}
void chorium_text(chorium_view* v, const char* utf8) {
  record("text %u %s", v->page, utf8);
}
void chorium_focus(chorium_view* v, int focused) {}
void chorium_eval(chorium_view* v, int id, const char* js) {
  record("eval %u %d", v->page, id);
  queue_event(v->page, "{\"type\":\"eval\",\"id\":%d,\"ok\":true,"
                       "\"result\":null}", id);
}
void chorium_dialog_reply(chorium_view* v, int id, int accept,
                          const char* text) {
  record("dialog_reply %u %d %d", v->page, id, accept);
}
void chorium_edit(chorium_view* v, int command) {
  record("edit %u %d", v->page, command);
}
void chorium_save_url(chorium_view* v, const char* url) {
  record("save_url %u %s", v->page, url);
}
void chorium_copy_image_at(chorium_view* v, int x, int y) {
  record("copy_image_at %u %d %d", v->page, x, y);
}
void chorium_file_chooser_reply(chorium_view* v, int id,
                                const char* const* paths, int count) {
  char line[2048] = "";
  for (int i = 0; i < count; ++i) {
    strncat(line, i ? "|" : "", sizeof line - strlen(line) - 1);
    strncat(line, paths[i], sizeof line - strlen(line) - 1);
  }
  record("file_chooser_reply %u %d %d %s", v->page, id, count, line);
}
void chorium_cancel_download(int id) { record("cancel_download %d", id); }
void chorium_set_tor(int on) {
  g_tor = on != 0;
  record("set_tor %d", on);
}
int chorium_tor_enabled(void) { return g_tor; }
int chorium_tor_progress(void) { return g_tor ? 100 : 0; }
void chorium_set_dark_mode(int dark) { record("set_dark_mode %d", dark); }

/* --- the fake screen ------------------------------------------------------ */

typedef struct {
  int x, y;
  float size;
  char* text;
} drawn_text_t;
typedef struct {
  int icon, x, y, w, h;
} drawn_icon_t;
typedef struct {
  drawn_text_t texts[2048];
  int text_count;
  drawn_icon_t icons[512];
  int icon_count;
} frame_t;

static frame_t g_drawing, g_shown; /* being drawn, last presented */
static screen_t g_screen;
static char g_data_dir[1024];
static char* g_clipboard;
static bool g_fullscreen;

static void frame_clear(frame_t* f) {
  for (int i = 0; i < f->text_count; ++i) {
    free(f->texts[i].text);
  }
  f->text_count = 0;
  f->icon_count = 0;
}

void fakes_reset(screen_t screen, const char* data_dir) {
  for (int i = 0; i < g_view_count; ++i) {
    for (int j = 0; j < g_views[i].count; ++j) {
      free(g_views[i].history[j]);
    }
  }
  g_view_count = 0;
  for (int i = 0; i < g_queue_len; ++i) {
    free(g_queue[i].json);
  }
  g_queue_len = 0;
  g_draw_requested = false;
  g_tor = true;
  g_fullscreen = false;
  frame_clear(&g_drawing);
  frame_clear(&g_shown);
  clear_calls();
  set_clipboard("");
  g_screen = screen;
  snprintf(g_data_dir, sizeof g_data_dir, "%s", data_dir);
}

void pump(void) {
  for (int rounds = 0; rounds < 100; ++rounds) {
    if (g_queue_len) {
      queued_t q = g_queue[0];
      memmove(&g_queue[0], &g_queue[1], sizeof(queued_t) * --g_queue_len);
      app_engine_event(q.page, q.json);
      free(q.json);
    } else if (g_draw_requested) {
      g_draw_requested = false;
      app_call("on-draw");
    } else {
      return;
    }
  }
}

const char* clipboard(void) { return g_clipboard ? g_clipboard : ""; }

void set_clipboard(const char* text) {
  free(g_clipboard);
  g_clipboard = strdup(text);
}

bool fullscreen(void) { return g_fullscreen; }

bool shown(const char* text) {
  for (int i = 0; i < g_shown.text_count; ++i) {
    if (strstr(g_shown.texts[i].text, text)) {
      return true;
    }
  }
  return false;
}

bool shown_exactly(const char* text) {
  for (int i = 0; i < g_shown.text_count; ++i) {
    if (strcmp(g_shown.texts[i].text, text) == 0) {
      return true;
    }
  }
  return false;
}

bool find_text(const char* text, int* x, int* y) {
  for (int i = g_shown.text_count - 1; i >= 0; --i) {
    const drawn_text_t* t = &g_shown.texts[i];
    if (strstr(t->text, text)) {
      /* |y| is the baseline: aim a little above it. */
      *x = t->x + 3;
      *y = t->y - (int)(t->size * 0.3f);
      return true;
    }
  }
  return false;
}

bool find_icon(int icon, int* x, int* y) {
  for (int i = g_shown.icon_count - 1; i >= 0; --i) {
    const drawn_icon_t* d = &g_shown.icons[i];
    if (d->icon == icon) {
      *x = d->x + d->w / 2;
      *y = d->y + d->h / 2;
      return true;
    }
  }
  return false;
}

void surface_dimensions(uint32_t* width, uint32_t* height) {
  *width = g_screen.width;
  *height = g_screen.height;
}
float surface_scale(void) { return 1.0f; }
void surface_clear(uint32_t argb) {}
void surface_fill_rect(int x, int y, int w, int h, uint32_t argb) {}
void surface_stroke_rect(int x, int y, int w, int h, uint32_t argb,
                         float width) {}
void surface_fill_round_rect(int x, int y, int w, int h, float radius,
                             uint32_t argb) {}

void surface_draw_text(int x, int y, const char* utf8, size_t len,
                       float font_size, uint32_t argb) {
  if (g_drawing.text_count == 2048) {
    return;
  }
  drawn_text_t* t = &g_drawing.texts[g_drawing.text_count++];
  t->x = x;
  t->y = y;
  t->size = font_size;
  t->text = malloc(len + 1);
  memcpy(t->text, utf8, len);
  t->text[len] = '\0';
}

void surface_measure_text(const char* utf8, size_t len, float font_size,
                          uint32_t* width, uint32_t* height) {
  *width = (uint32_t)((float)len * font_size * 0.5f);
  *height = (uint32_t)(font_size * 1.3f);
}

void surface_draw_icon(int icon, int x, int y, int w, int h, uint32_t argb) {
  if (g_drawing.icon_count < 512) {
    drawn_icon_t d = {icon, x, y, w, h};
    g_drawing.icons[g_drawing.icon_count++] = d;
  }
}

void surface_draw_page(uint32_t page_id, int x, int y, int w, int h) {}
void surface_push_clip(int x, int y, int w, int h) {}
void surface_pop_clip(void) {}

void surface_present(void) {
  frame_clear(&g_shown);
  g_shown = g_drawing;
  g_drawing.text_count = 0; /* the strings now belong to g_shown */
  g_drawing.icon_count = 0;
}

bool surface_dark_mode(void) { return g_screen.dark; }
void surface_show_keyboard(bool visible) {
  record("show_keyboard %d", visible);
}
void surface_set_title(const char* utf8, size_t len) {}
void surface_copy_text(const char* utf8, size_t len) {
  char* s = malloc(len + 1);
  memcpy(s, utf8, len);
  s[len] = '\0';
  record("copy_text %s", s);
  free(g_clipboard);
  g_clipboard = s;
}
void surface_set_fullscreen(bool on) {
  g_fullscreen = on;
  record("set_fullscreen %d", on);
}
void surface_set_back_enabled(bool on) {}

void surface_pick_files(uint32_t request, picker_mode_t mode,
                        const char* const* accept, size_t count,
                        const char* default_name) {
  char types[1024] = "";
  for (size_t i = 0; i < count; ++i) {
    strncat(types, i ? "|" : "", sizeof types - strlen(types) - 1);
    strncat(types, accept[i], sizeof types - strlen(types) - 1);
  }
  record("pick_files %u %d %s", request, (int)mode, types);
}

void surface_open_file(const char* path, bool in_folder) {
  record("open_file %d %s", in_folder, path);
}

char* surface_read_clipboard(void) { return strdup(clipboard()); }

bool title_bar_layout(uint32_t* height, uint32_t* buttons_width,
                      bool* maximized) {
  if (!g_screen.title_bar) {
    return false;
  }
  *height = 40;
  *buttons_width = 3 * 46;
  *maximized = false;
  return true;
}

void title_bar_set_drag_areas(const int* xywh, size_t count) {}

chorium_frame_fn platform_frame_callback(void) { return NULL; }

void platform_page_event(chorium_view* view, const char* json, void* user) {
  queue_event((uint32_t)(uintptr_t)user, "%s", json);
}

void platform_page_closed(uint32_t page_id) {}

bool platform_app_data_dir(const char* app_name, char* out, size_t cap) {
  mkdir(g_data_dir, 0700);
  return snprintf(out, cap, "%s", g_data_dir) < (int)cap;
}

void platform_app_crashed(const char* message) {
  record("crashed %s", message);
  fprintf(stderr, "    app crashed: %s\n", message);
}

void platform_request_draw(void) { g_draw_requested = true; }
void platform_tor_restarted(void) { record("tor_restarted"); }
void platform_log(const char* level, const char* text, size_t len) {}

/* The remote control isn't started here; the core still links it. */
#include "remote.h"
void platform_post_to_ui(void (*fn)(void*), void* arg) { fn(arg); }
void platform_remote_input(const remote_input_t* in) {}
void platform_screenshot(void* token) {
  remote_screenshot_done(token, NULL, NULL, 0);
}
bool platform_answer_picker(const char* const* paths, size_t count) {
  return false;
}

/* --- input ---------------------------------------------------------------- */

static const char* const kModifiers[] = {"shift", "ctrl", "alt", "meta"};

static void pointer(const char* kind, int x, int y) {
  static const char* const names[] = {"x", "y", "button", "click-count",
                                      "modifiers"};
  val_t fields[] = {v_s32(x), v_s32(y), v_enum("left"), v_u32(1),
                    v_flags(kModifiers, 4, 0)};
  val_t p = v_record(5, names, fields);
  app_input(v_variant(kind, &p));
}

void click(int x, int y) {
  pointer("pointer-down", x, y);
  pump();
  pointer("pointer-up", x, y);
  pump();
}

bool click_text(const char* text) {
  int x, y;
  if (!find_text(text, &x, &y)) {
    return false;
  }
  click(x, y);
  return true;
}

bool click_text_exactly(const char* text) {
  for (int i = g_shown.text_count - 1; i >= 0; --i) {
    const drawn_text_t* t = &g_shown.texts[i];
    if (strcmp(t->text, text) == 0) {
      click(t->x + 3, t->y - (int)(t->size * 0.3f));
      return true;
    }
  }
  return false;
}

bool click_icon(int icon) {
  int x, y;
  if (!find_icon(icon, &x, &y)) {
    return false;
  }
  click(x, y);
  return true;
}

void type_text(const char* text) {
  val_t s = v_cstring(text);
  app_input(v_variant("text", &s));
  pump();
}

void press_key(int code, int mods) {
  static const char* const names[] = {"down", "code", "modifiers"};
  for (int down = 1; down >= 0; --down) {
    val_t fields[] = {v_bool(down), v_u32((uint32_t)code),
                      v_flags(kModifiers, 4, (uint32_t)mods)};
    val_t k = v_record(3, names, fields);
    app_input(v_variant("key", &k));
    pump();
  }
}

void press_back(void) {
  app_input(v_variant("back", NULL));
  pump();
}
