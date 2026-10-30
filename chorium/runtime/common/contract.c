/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The runtime side of chorium.wit, shared by every platform: loads the app
 * with Wasmtime, defines every import, and calls the app's exports.
 * Everything here runs on the UI thread; the platform gets engine callbacks
 * there (runtime.h).
 *
 * The kill switch holds for apps too: WASI gets no network (no sockets, no
 * name lookup) and no directories, so pages and app files go only through
 * the calls below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "runtime.h"

/* Portable C shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

#define PKG "umbra:chorium/"
#define VER "@0.3.0"

/* Host resource type tags. */
#define TY_PAGE 1
#define TY_DIR 2

static wasm_engine_t* g_engine;
static wasmtime_store_t* g_store;
static wasmtime_context_t* g_ctx;
static wasmtime_component_t* g_component;
static wasmtime_component_linker_t* g_linker;
static wasmtime_component_instance_t g_instance;
static bool g_loaded;
static bool g_dead;  /* the app trapped; it can't be entered again */
static bool g_started; /* start has run; nothing else is called before it */
static int g_depth;  /* calls into the app in progress */
static uint32_t g_next_page = 1;
static char g_app_dir[1024]; /* files.app-data, UTF-8 */
static char g_last_error[1024];

/* Open pages, by id (the `page` resource's rep). */
typedef struct {
  bool used;
  chorium_view* view;
} page_t;
static page_t g_pages[MAX_PAGES + 1];

static const char* const kModifiers[] = {"shift", "ctrl", "alt", "meta"};

/* --- errors ------------------------------------------------------------- */

static void print_error(const char* what, wasmtime_error_t* error,
                        wasm_trap_t* trap) {
  wasm_byte_vec_t message = {0};
  if (error) {
    wasmtime_error_message(error, &message);
    wasmtime_error_delete(error);
  } else if (trap) {
    wasm_trap_message(trap, &message);
    wasm_trap_delete(trap);
  }
  snprintf(g_last_error, sizeof g_last_error, "%s: %.*s", what,
           (int)message.size, message.data ? message.data : "");
  fprintf(stderr, "[host] %s\n", g_last_error);
  wasm_byte_vec_delete(&message);
}

const char* contract_last_error(void) { return g_last_error; }

static wasmtime_error_t* trap(const char* message) {
  return wasmtime_error_new(message);
}

/* --- resources ---------------------------------------------------------- */

static bool resource_rep(wasmtime_context_t* ctx, const val_t* v,
                         uint32_t type, uint32_t* rep) {
  if (!v || v->kind != WASMTIME_COMPONENT_RESOURCE) {
    return false;
  }
  wasmtime_component_resource_host_t* host = NULL;
  wasmtime_error_t* error =
      wasmtime_component_resource_any_to_host(ctx, v->of.resource, &host);
  if (error) {
    wasmtime_error_delete(error);
    return false;
  }
  bool ok = wasmtime_component_resource_host_type(host) == type;
  *rep = wasmtime_component_resource_host_rep(host);
  wasmtime_component_resource_host_delete(host);
  return ok;
}

static wasmtime_error_t* own_resource(wasmtime_context_t* ctx, uint32_t type,
                                      uint32_t rep, val_t* out) {
  wasmtime_component_resource_host_t* host =
      wasmtime_component_resource_host_new(true, rep, type);
  wasmtime_component_resource_any_t* any = NULL;
  wasmtime_error_t* error =
      wasmtime_component_resource_host_to_any(ctx, host, &any);
  wasmtime_component_resource_host_delete(host);
  if (error) {
    return error;
  }
  out->kind = WASMTIME_COMPONENT_RESOURCE;
  out->of.resource = any;
  return NULL;
}

static bool g_trace; /* CHORIUM_HOST_TRACE=1: log calls in and out */

/* The page behind a borrow<page> argument, or NULL. */
static page_t* arg_page(wasmtime_context_t* ctx, const val_t* v) {
  uint32_t rep = 0;
  if (g_trace) {
    fprintf(stderr, "[trace]   page arg kind=%d owned=%d\n", v ? v->kind : -1,
            v && v->kind == WASMTIME_COMPONENT_RESOURCE
                ? wasmtime_component_resource_any_owned(v->of.resource)
                : -1);
  }
  if (!resource_rep(ctx, v, TY_PAGE, &rep) || rep == 0 || rep > MAX_PAGES ||
      !g_pages[rep].used) {
    return NULL;
  }
  return &g_pages[rep];
}

#define HOSTFN(name)                                                       \
  static wasmtime_error_t* name(                                           \
      void* host_data, wasmtime_context_t* ctx,                            \
      const wasmtime_component_func_type_t* func_type, val_t* args,        \
      size_t nargs, val_t* results, size_t nresults)

#define PAGE_ARG()                                  \
  page_t* page = arg_page(ctx, &args[0]);           \
  if (!page) {                                      \
    return trap("chorium: not a live page");        \
  }                                                 \
  uint32_t page_id = (uint32_t)(page - g_pages);    \
  (void)page_id

static uint32_t field_u32(const val_t* record, const char* name) {
  const val_t* v = r_field(record, name);
  return v && v->kind == WASMTIME_COMPONENT_U32 ? v->of.u32 : 0;
}

static int32_t field_s32(const val_t* record, const char* name) {
  const val_t* v = r_field(record, name);
  return v && v->kind == WASMTIME_COMPONENT_S32 ? v->of.s32 : 0;
}

static float field_f32(const val_t* record, const char* name) {
  const val_t* v = r_field(record, name);
  return v && v->kind == WASMTIME_COMPONENT_F32 ? v->of.f32 : 0.0f;
}

/* --- pages -------------------------------------------------------------- */

bool page_alive(uint32_t page_id) {
  return page_id >= 1 && page_id <= MAX_PAGES && g_pages[page_id].used;
}

chorium_view* page_view(uint32_t page_id) {
  return page_alive(page_id) ? g_pages[page_id].view : NULL;
}

static void page_close(uint32_t id) {
  chorium_close(g_pages[id].view);
  g_pages[id].used = false;
  g_pages[id].view = NULL;
  platform_page_closed(id);
}

void pages_close_all(void) {
  for (uint32_t i = 1; i <= MAX_PAGES; ++i) {
    if (g_pages[i].used) {
      page_close(i);
    }
  }
}

static wasmtime_error_t* page_destructor(void* data, wasmtime_context_t* ctx,
                                         uint32_t rep) {
  if (page_alive(rep)) {
    page_close(rep);
  }
  return NULL;
}

HOSTFN(page_new) {
  uint32_t w = field_u32(&args[0], "width");
  uint32_t h = field_u32(&args[0], "height");
  if (g_next_page > MAX_PAGES) {
    return trap("chorium: too many pages");
  }
  uint32_t id = g_next_page++;
  page_t* page = &g_pages[id];
  page->view = chorium_open(w ? (int)w : 1, h ? (int)h : 1,
                            platform_frame_callback(), platform_page_event,
                            (void*)(uintptr_t)id);
  if (!page->view) {
    return trap("chorium: could not open a page");
  }
  page->used = true;
  return own_resource(ctx, TY_PAGE, id, &results[0]);
}

HOSTFN(page_id) {
  PAGE_ARG();
  results[0] = v_u32(page_id);
  return NULL;
}

HOSTFN(page_load_url) {
  PAGE_ARG();
  char* url = r_cstring(&args[1]);
  chorium_load_url(page->view, url);
  free(url);
  return NULL;
}

HOSTFN(page_load_html) {
  PAGE_ARG();
  char* html = r_cstring(&args[1]);
  char* base = r_opt_cstring(&args[2]);
  chorium_load_html(page->view, html, base);
  free(html);
  free(base);
  return NULL;
}

HOSTFN(page_back) {
  PAGE_ARG();
  chorium_back(page->view);
  return NULL;
}

HOSTFN(page_forward) {
  PAGE_ARG();
  chorium_forward(page->view);
  return NULL;
}

HOSTFN(page_reload) {
  PAGE_ARG();
  chorium_reload(page->view);
  return NULL;
}

HOSTFN(page_stop) {
  PAGE_ARG();
  chorium_stop(page->view);
  return NULL;
}

HOSTFN(page_resize) {
  PAGE_ARG();
  uint32_t w = field_u32(&args[1], "width");
  uint32_t h = field_u32(&args[1], "height");
  if (w && h) {
    chorium_resize(page->view, (int)w, (int)h);
  }
  return NULL;
}

static int button_code(const val_t* v) {
  if (!v || v->kind != WASMTIME_COMPONENT_ENUM) {
    return CHORIUM_BUTTON_NONE;
  }
  if (r_name_is(&v->of.enumeration, "left")) return CHORIUM_BUTTON_LEFT;
  if (r_name_is(&v->of.enumeration, "middle")) return CHORIUM_BUTTON_MIDDLE;
  if (r_name_is(&v->of.enumeration, "right")) return CHORIUM_BUTTON_RIGHT;
  return CHORIUM_BUTTON_NONE;
}

/* WIT modifiers -> CHORIUM_MOD_* (same bit order). */
static int mods(const val_t* record) {
  return (int)r_flag_bits(r_field(record, "modifiers"), kModifiers, 4);
}

HOSTFN(page_send_input) {
  PAGE_ARG();
  const val_t* e = &args[1];
  const val_t* p = r_payload(e);
  int type = -1;
  if (r_case(e, "pointer-move")) type = CHORIUM_MOUSE_MOVE;
  if (r_case(e, "pointer-down")) type = CHORIUM_MOUSE_DOWN;
  if (r_case(e, "pointer-up")) type = CHORIUM_MOUSE_UP;
  if (type >= 0) {
    chorium_mouse(page->view, type, field_s32(p, "x"), field_s32(p, "y"),
                  button_code(r_field(p, "button")),
                  (int)field_u32(p, "click-count"), mods(p));
  } else if (r_case(e, "pointer-leave")) {
    chorium_mouse(page->view, CHORIUM_MOUSE_LEAVE, 0, 0, CHORIUM_BUTTON_NONE,
                  0, 0);
  } else if (r_case(e, "wheel")) {
    chorium_wheel(page->view, field_s32(p, "x"), field_s32(p, "y"),
                  field_f32(p, "dx"), field_f32(p, "dy"), mods(p));
  } else if (r_case(e, "key")) {
    const val_t* down = r_field(p, "down");
    chorium_key(page->view, down && down->of.boolean,
                (int)field_u32(p, "code"), mods(p));
  } else if (r_case(e, "text")) {
    char* text = r_cstring(p);
    chorium_text(page->view, text);
    free(text);
  } else if (r_case(e, "focus")) {
    chorium_focus(page->view, p && p->of.boolean);
  } else if (r_case(e, "back")) {
    chorium_back(page->view);
  }
  /* Touch: chorium.dll has no touch input on Windows yet. */
  return NULL;
}

HOSTFN(page_eval) {
  PAGE_ARG();
  char* js = r_cstring(&args[2]);
  chorium_eval(page->view, (int)args[1].of.u32, js);
  free(js);
  return NULL;
}

HOSTFN(page_dialog_reply) {
  PAGE_ARG();
  char* text = r_opt_cstring(&args[3]);
  chorium_dialog_reply(page->view, (int)args[1].of.u32, args[2].of.boolean,
                       text);
  free(text);
  return NULL;
}

/* --- surface ------------------------------------------------------------ */

#define RECT_ARGS(v)                                              \
  field_s32(v, "x"), field_s32(v, "y"), (int)field_u32(v, "width"), \
      (int)field_u32(v, "height")

HOSTFN(surface_dimensions_fn) {
  uint32_t w, h;
  surface_dimensions(&w, &h);
  val_t vals[] = {v_u32(w), v_u32(h)};
  static const char* const names[] = {"width", "height"};
  results[0] = v_record(2, names, vals);
  return NULL;
}

HOSTFN(surface_scale_fn) {
  results[0] = v_f32(surface_scale());
  return NULL;
}

HOSTFN(surface_clear_fn) {
  surface_clear(args[0].of.u32);
  return NULL;
}

HOSTFN(surface_fill_rect_fn) {
  surface_fill_rect(RECT_ARGS(&args[0]), args[1].of.u32);
  return NULL;
}

HOSTFN(surface_stroke_rect_fn) {
  surface_stroke_rect(RECT_ARGS(&args[0]), args[1].of.u32, args[2].of.f32);
  return NULL;
}

HOSTFN(surface_fill_round_rect_fn) {
  surface_fill_round_rect(RECT_ARGS(&args[0]), args[1].of.f32,
                          args[2].of.u32);
  return NULL;
}

HOSTFN(surface_draw_text_fn) {
  surface_draw_text(args[0].of.s32, args[1].of.s32, args[2].of.string.data,
                    args[2].of.string.size, args[3].of.f32, args[4].of.u32);
  return NULL;
}

HOSTFN(surface_measure_text_fn) {
  uint32_t w, h;
  surface_measure_text(args[0].of.string.data, args[0].of.string.size,
                       args[1].of.f32, &w, &h);
  val_t vals[] = {v_u32(w), v_u32(h)};
  static const char* const names[] = {"width", "height"};
  results[0] = v_record(2, names, vals);
  return NULL;
}

HOSTFN(pages_set_dark_mode) {
  chorium_set_dark_mode(args[0].of.boolean);
  return NULL;
}

HOSTFN(page_draw) {
  PAGE_ARG();
  surface_draw_page(page_id, RECT_ARGS(&args[1]));
  return NULL;
}

HOSTFN(surface_push_clip_fn) {
  surface_push_clip(RECT_ARGS(&args[0]));
  return NULL;
}

HOSTFN(surface_pop_clip_fn) {
  surface_pop_clip();
  return NULL;
}

HOSTFN(surface_present_fn) {
  surface_present();
  return NULL;
}

HOSTFN(surface_show_keyboard_fn) {
  surface_show_keyboard(args[0].of.boolean);
  return NULL;
}

static const char* const kIcons[] = {
    "back",     "forward",  "reload",   "stop",    "close",
    "add",      "menu",     "search",   "lock",    "info",
    "page",     "settings", "history",  "home",    "check",
    "minimize", "maximize", "restore",  "close-window",
};

HOSTFN(surface_draw_icon_fn) {
  int icon = -1;
  for (int i = 0; i < (int)(sizeof kIcons / sizeof kIcons[0]); ++i) {
    if (r_name_is(&args[0].of.enumeration, kIcons[i])) {
      icon = i;
    }
  }
  surface_draw_icon(icon, RECT_ARGS(&args[1]), args[2].of.u32);
  return NULL;
}

HOSTFN(surface_dark_mode_fn) {
  results[0] = v_bool(surface_dark_mode());
  return NULL;
}

HOSTFN(surface_title_bar_layout_fn) {
  uint32_t height, buttons;
  bool maximized;
  if (!title_bar_layout(&height, &buttons, &maximized)) {
    results[0] = v_option(NULL);
    return NULL;
  }
  static const char* const names[] = {"height", "buttons-width", "maximized"};
  val_t vals[] = {v_u32(height), v_u32(buttons), v_bool(maximized)};
  val_t record = v_record(3, names, vals);
  results[0] = v_option(&record);
  return NULL;
}

HOSTFN(surface_set_drag_areas_fn) {
  const wasmtime_component_vallist_t* list = &args[0].of.list;
  int* xywh = malloc(sizeof(int) * 4 * (list->size ? list->size : 1));
  for (size_t i = 0; i < list->size; ++i) {
    const val_t* r = &list->data[i];
    xywh[i * 4 + 0] = field_s32(r, "x");
    xywh[i * 4 + 1] = field_s32(r, "y");
    xywh[i * 4 + 2] = (int)field_u32(r, "width");
    xywh[i * 4 + 3] = (int)field_u32(r, "height");
  }
  title_bar_set_drag_areas(xywh, list->size);
  free(xywh);
  return NULL;
}

HOSTFN(surface_set_title_fn) {
  surface_set_title(args[0].of.string.data, args[0].of.string.size);
  return NULL;
}

HOSTFN(surface_copy_text_fn) {
  surface_copy_text(args[0].of.string.data, args[0].of.string.size);
  return NULL;
}

HOSTFN(surface_set_fullscreen_fn) {
  surface_set_fullscreen(args[0].of.boolean);
  return NULL;
}

HOSTFN(surface_set_back_enabled_fn) {
  surface_set_back_enabled(args[0].of.boolean);
  return NULL;
}

/* --- net ---------------------------------------------------------------- */

HOSTFN(net_fetch_fn) {
  /* TODO(chorium): route through chorium.dll's network service (Tor). */
  val_t message = v_cstring("net.fetch is not available in this host yet");
  results[0] = v_result(false, &message);
  return NULL;
}

HOSTFN(net_tor_progress_fn) {
  results[0] = v_s32(chorium_tor_progress());
  return NULL;
}

HOSTFN(net_set_tor_fn) {
  bool on = args[0].of.boolean;
  chorium_set_tor(on);
  if (on) {
    platform_tor_restarted();
  }
  return NULL;
}

HOSTFN(net_tor_enabled_fn) {
  results[0] = v_bool(chorium_tor_enabled() != 0);
  return NULL;
}

/* --- files -------------------------------------------------------------- */

/* UTF-8 paths on every platform: Windows needs its wide-character calls. */
static FILE* open_file(const char* path, const char* mode) {
#ifdef _WIN32
  wchar_t wpath[1024], wmode[8];
  if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) ||
      !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8)) {
    return NULL;
  }
  return _wfopen(wpath, wmode);
#else
  return fopen(path, mode);
#endif
}

static bool remove_file(const char* path) {
#ifdef _WIN32
  wchar_t wpath[1024];
  return MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) &&
         _wremove(wpath) == 0;
#else
  return remove(path) == 0;
#endif
}

/* A plain file name inside the app's directory, or NULL. free() it. */
static char* app_file(const val_t* name) {
  const wasm_name_t* s = &name->of.string;
  if (s->size == 0 || s->size > 200) {
    return NULL;
  }
  for (size_t i = 0; i < s->size; ++i) {
    unsigned char c = (unsigned char)s->data[i];
    if (c < 0x20 || strchr("/\\:*?\"<>|", c)) {
      return NULL;
    }
  }
  if ((s->size == 1 && s->data[0] == '.') ||
      (s->size == 2 && s->data[0] == '.' && s->data[1] == '.')) {
    return NULL;
  }
  size_t cap = strlen(g_app_dir) + s->size + 2;
  char* path = malloc(cap);
  snprintf(path, cap, "%s/%.*s", g_app_dir, (int)s->size, s->data);
  return path;
}

static val_t err_val(const char* message) {
  val_t m = v_cstring(message);
  return v_result(false, &m);
}

/* Every host function must take its borrow<dir> argument back from the
 * guest, or Wasmtime traps with "borrow handles still remain". */
#define DIR_ARG()                                       \
  uint32_t dir_rep;                                     \
  if (!resource_rep(ctx, &args[0], TY_DIR, &dir_rep)) { \
    return trap("chorium: not a directory handle");     \
  }

HOSTFN(files_app_data_fn) { return own_resource(ctx, TY_DIR, 1, &results[0]); }

static wasmtime_error_t* dir_destructor(void* data, wasmtime_context_t* ctx,
                                        uint32_t rep) {
  return NULL;
}

HOSTFN(dir_read_fn) {
  DIR_ARG();
  char* path = app_file(&args[1]);
  if (!path) {
    results[0] = err_val("bad file name");
    return NULL;
  }
  FILE* f = open_file(path, "rb");
  free(path);
  if (!f) {
    results[0] = err_val("not found");
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0 || size > 64 * 1024 * 1024) {
    fclose(f);
    results[0] = err_val("too big");
    return NULL;
  }
  uint8_t* data = malloc((size_t)size + 1);
  size_t got = fread(data, 1, (size_t)size, f);
  fclose(f);
  val_t bytes = v_bytes(data, got);
  free(data);
  results[0] = v_result(true, &bytes);
  return NULL;
}

HOSTFN(dir_write_fn) {
  DIR_ARG();
  char* path = app_file(&args[1]);
  if (!path) {
    results[0] = err_val("bad file name");
    return NULL;
  }
  const wasmtime_component_vallist_t* list = &args[2].of.list;
  uint8_t* data = malloc(list->size ? list->size : 1);
  for (size_t i = 0; i < list->size; ++i) {
    data[i] = list->data[i].of.u8;
  }
  FILE* f = open_file(path, "wb");
  free(path);
  bool ok = f && fwrite(data, 1, list->size, f) == list->size;
  if (f && fclose(f) != 0) {
    ok = false;
  }
  free(data);
  results[0] = ok ? v_result(true, NULL) : err_val("write failed");
  return NULL;
}

HOSTFN(dir_remove_fn) {
  DIR_ARG();
  char* path = app_file(&args[1]);
  if (!path) {
    results[0] = err_val("bad file name");
    return NULL;
  }
  bool ok = remove_file(path);
  free(path);
  results[0] = ok ? v_result(true, NULL) : err_val("remove failed");
  return NULL;
}

HOSTFN(dir_names_fn) {
  DIR_ARG();
  size_t count = 0, cap = 16;
  val_t* names = malloc(sizeof(val_t) * cap);
#define ADD_NAME(utf8, len)                             \
  do {                                                  \
    if (count == cap) {                                 \
      cap *= 2;                                         \
      names = realloc(names, sizeof(val_t) * cap);      \
    }                                                   \
    names[count++] = v_string((utf8), (len));           \
  } while (0)
#ifdef _WIN32
  wchar_t pattern[1100];
  int n = MultiByteToWideChar(CP_UTF8, 0, g_app_dir, -1, pattern, 1090);
  if (n > 0) {
    wcscpy(pattern + n - 1, L"\\*");
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        char utf8[1024];
        int len = WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, utf8,
                                      sizeof utf8, NULL, NULL);
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && len > 1) {
          ADD_NAME(utf8, (size_t)len - 1);
        }
      } while (FindNextFileW(h, &found));
      FindClose(h);
    }
  }
#else
  DIR* d = opendir(g_app_dir);
  if (d) {
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
      if (e->d_type == DT_REG) {
        ADD_NAME(e->d_name, strlen(e->d_name));
      }
    }
    closedir(d);
  }
#endif
#undef ADD_NAME
  results[0] = v_list(count, names);
  free(names);
  return NULL;
}

/* --- log ---------------------------------------------------------------- */

HOSTFN(log_write_fn) {
  const char* level = "info";
  const wasm_name_t* e = &args[0].of.enumeration;
  if (r_name_is(e, "debug")) level = "debug";
  if (r_name_is(e, "warn")) level = "warn";
  if (r_name_is(e, "error")) level = "error";
  platform_log(level, args[1].of.string.data, args[1].of.string.size);
  return NULL;
}

/* --- linking ------------------------------------------------------------ */

typedef struct {
  const char* name;
  wasmtime_component_func_callback_t fn;
} func_def;

static bool check(const char* what, wasmtime_error_t* error) {
  if (error) {
    print_error(what, error, NULL);
    return false;
  }
  return true;
}

static bool add_funcs(wasmtime_component_linker_instance_t* li,
                      const func_def* defs, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!check(defs[i].name,
               wasmtime_component_linker_instance_add_func(
                   li, defs[i].name, strlen(defs[i].name), defs[i].fn, NULL,
                   NULL))) {
      return false;
    }
  }
  return true;
}

static bool add_resource(wasmtime_component_linker_instance_t* li,
                         const char* name, uint32_t type,
                         wasmtime_component_resource_destructor_t dtor) {
  wasmtime_component_resource_type_t* ty =
      wasmtime_component_resource_type_new_host(type);
  bool ok = check(name, wasmtime_component_linker_instance_add_resource(
                            li, name, strlen(name), ty, dtor, NULL, NULL));
  wasmtime_component_resource_type_delete(ty);
  return ok;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool define_imports(void) {
  static const func_def pages[] = {
      {"set-dark-mode", pages_set_dark_mode},
      {"[constructor]page", page_new},
      {"[method]page.id", page_id},
      {"[method]page.load-url", page_load_url},
      {"[method]page.load-html", page_load_html},
      {"[method]page.back", page_back},
      {"[method]page.forward", page_forward},
      {"[method]page.reload", page_reload},
      {"[method]page.stop", page_stop},
      {"[method]page.resize", page_resize},
      {"[method]page.send-input", page_send_input},
      {"[method]page.eval", page_eval},
      {"[method]page.dialog-reply", page_dialog_reply},
      {"[method]page.draw", page_draw},
  };
  static const func_def surface[] = {
      {"dimensions", surface_dimensions_fn},
      {"scale", surface_scale_fn},
      {"clear", surface_clear_fn},
      {"fill-rect", surface_fill_rect_fn},
      {"stroke-rect", surface_stroke_rect_fn},
      {"fill-round-rect", surface_fill_round_rect_fn},
      {"draw-text", surface_draw_text_fn},
      {"measure-text", surface_measure_text_fn},
      {"push-clip", surface_push_clip_fn},
      {"pop-clip", surface_pop_clip_fn},
      {"present", surface_present_fn},
      {"show-keyboard", surface_show_keyboard_fn},
      {"set-title", surface_set_title_fn},
      {"copy-text", surface_copy_text_fn},
      {"set-fullscreen", surface_set_fullscreen_fn},
      {"set-back-enabled", surface_set_back_enabled_fn},
      {"draw-icon", surface_draw_icon_fn},
      {"dark-mode", surface_dark_mode_fn},
      {"title-bar-layout", surface_title_bar_layout_fn},
      {"set-drag-areas", surface_set_drag_areas_fn},
  };
  static const func_def net[] = {
      {"fetch", net_fetch_fn},
      {"tor-progress", net_tor_progress_fn},
      {"set-tor", net_set_tor_fn},
      {"tor-enabled", net_tor_enabled_fn},
  };
  static const func_def files[] = {
      {"app-data", files_app_data_fn},
      {"[method]dir.read", dir_read_fn},
      {"[method]dir.write", dir_write_fn},
      {"[method]dir.remove", dir_remove_fn},
      {"[method]dir.names", dir_names_fn},
  };
  static const func_def log[] = {
      {"write", log_write_fn},
  };

  wasmtime_component_linker_instance_t* root =
      wasmtime_component_linker_root(g_linker);
  wasmtime_component_linker_instance_t* li = NULL;
  bool ok = true;

#define INSTANCE(iface, body)                                              \
  if (ok) {                                                                \
    ok = check(iface, wasmtime_component_linker_instance_add_instance(     \
                          root, PKG iface VER, strlen(PKG iface VER), &li)); \
    if (ok) {                                                              \
      body;                                                                \
      wasmtime_component_linker_instance_delete(li);                       \
    }                                                                      \
  }

  INSTANCE("types", (void)0);
  INSTANCE("pages", ok = add_resource(li, "page", TY_PAGE, page_destructor) &&
                         add_funcs(li, pages, COUNT(pages)));
  INSTANCE("surface", ok = add_funcs(li, surface, COUNT(surface)));
  INSTANCE("net", ok = add_funcs(li, net, COUNT(net)));
  INSTANCE("files", ok = add_resource(li, "dir", TY_DIR, dir_destructor) &&
                         add_funcs(li, files, COUNT(files)));
  INSTANCE("log", ok = add_funcs(li, log, COUNT(log)));
#undef INSTANCE

  wasmtime_component_linker_instance_delete(root);
  return ok;
}

bool contract_load(const char* wasm_path) {
  FILE* f = open_file(wasm_path, "rb");
  if (!f) {
    snprintf(g_last_error, sizeof g_last_error, "can't open %s", wasm_path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* bytes = malloc(size > 0 ? (size_t)size : 1);
  size_t got = fread(bytes, 1, size > 0 ? (size_t)size : 0, f);
  fclose(f);

  /* The app's name is its file name without ".wasm". */
  const char* leaf = wasm_path;
  for (const char* p = wasm_path; *p; ++p) {
    if (*p == '\\' || *p == '/') {
      leaf = p + 1;
    }
  }
  char name[128];
  snprintf(name, sizeof name, "%s", leaf);
  char* dot = strrchr(name, '.');
  if (dot) {
    *dot = '\0';
  }
  bool ok = contract_load_bytes(bytes, got, name);
  free(bytes);
  return ok;
}

bool contract_load_bytes(const uint8_t* bytes, size_t len,
                         const char* app_name) {
  contract_unload(); /* a previous run in this process (Android) */
  g_trace = getenv("CHORIUM_HOST_TRACE") != NULL;
  g_engine = wasm_engine_new();
  if (!check("compiling the app",
             wasmtime_component_new(g_engine, bytes, len, &g_component))) {
    return false;
  }

  g_store = wasmtime_store_new(g_engine, NULL, NULL);
  g_ctx = wasmtime_store_context(g_store);
  /* stdout/stderr for the app's own logging; nothing else. No network, no
   * name lookup, no directories. */
  wasi_config_t* wasi = wasi_config_new();
  wasi_config_inherit_stdout(wasi);
  wasi_config_inherit_stderr(wasi);
  if (!check("WASI", wasmtime_context_set_wasi(g_ctx, wasi))) {
    return false;
  }

  g_linker = wasmtime_component_linker_new(g_engine);
  if (!check("WASI imports", wasmtime_component_linker_add_wasip2(g_linker)) ||
      !define_imports()) {
    return false;
  }
  if (!platform_app_data_dir(app_name, g_app_dir, sizeof g_app_dir)) {
    snprintf(g_last_error, sizeof g_last_error,
             "no folder for the app's files");
    return false;
  }
  if (!check("instantiating the app",
             wasmtime_component_linker_instantiate(g_linker, g_ctx,
                                                   g_component,
                                                   &g_instance))) {
    return false;
  }
  g_loaded = true;
  return true;
}

void contract_unload(void) {
  if (g_store) {
    wasmtime_store_delete(g_store);
    g_store = NULL;
  }
  if (g_linker) {
    wasmtime_component_linker_delete(g_linker);
    g_linker = NULL;
  }
  if (g_component) {
    wasmtime_component_delete(g_component);
    g_component = NULL;
  }
  if (g_engine) {
    wasm_engine_delete(g_engine);
    g_engine = NULL;
  }
  g_loaded = false;
  /* The next load starts a new app (Android reopens the activity in the
   * same process). */
  g_dead = false;
  g_started = false;
}

/* --- calling the app ---------------------------------------------------- */

static void call(const char* name, val_t* args, size_t nargs) {
  if (!g_loaded || g_dead || (!g_started && strcmp(name, "start") != 0)) {
    goto done;
  }
  if (g_trace) {
    if (nargs == 1 && args[0].kind == WASMTIME_COMPONENT_VARIANT) {
      const wasm_name_t* d = &args[0].of.variant.discriminant;
      fprintf(stderr, "[trace] -> %s %.*s\n", name, (int)d->size, d->data);
    } else {
      fprintf(stderr, "[trace] -> %s\n", name);
    }
  }
  if (g_depth > 0) {
    /* Never re-enter the app; only redraws are worth keeping. */
    if (strcmp(name, "on-draw") == 0) {
      platform_request_draw();
    }
    goto done;
  }
  wasmtime_component_export_index_t* index =
      wasmtime_component_instance_get_export_index(&g_instance, g_ctx, NULL,
                                                   name, strlen(name));
  wasmtime_component_func_t func;
  bool found =
      index && wasmtime_component_instance_get_func(&g_instance, g_ctx, index,
                                                    &func);
  if (index) {
    wasmtime_component_export_index_delete(index);
  }
  if (!found) {
    fprintf(stderr, "[host] the app has no export %s\n", name);
    goto done;
  }
  ++g_depth;
  wasmtime_error_t* error =
      wasmtime_component_func_call(&func, g_ctx, args, nargs, NULL, 0);
  --g_depth;
  if (strcmp(name, "on-stop") == 0) {
    g_dead = true; /* the app has let go of everything; no more calls */
  }
  if (error) {
    print_error(name, error, NULL);
    g_dead = true;
    platform_app_crashed(g_last_error);
  }
done:
  for (size_t i = 0; i < nargs; ++i) {
    wasmtime_component_val_delete(&args[i]);
  }
}

void app_start(int argc, char** argv) {
  val_t* items = malloc(sizeof(val_t) * (size_t)(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    items[i] = v_cstring(argv[i]);
  }
  val_t args[] = {v_list((size_t)argc, items)};
  free(items);
  call("start", args, 1);
  g_started = true;
}

void app_input(val_t input) { call("on-input", &input, 1); }

void app_page_event(uint32_t page_id, val_t event) {
  val_t args[] = {v_u32(page_id), event};
  call("on-page-event", args, 2);
}

void app_call(const char* export_name) { call(export_name, NULL, 0); }
