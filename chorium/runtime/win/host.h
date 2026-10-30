/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Chorium.exe, the Chorium runtime for Windows: runs a composed app
 * (.wasm component) with Wasmtime and implements the runtime contract
 * (interfaces/chorium/chorium.wit) on top of chorium.dll, Win32 and GDI.
 */
#ifndef CHORIUM_HOST_WIN_HOST_H_
#define CHORIUM_HOST_WIN_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

#include <wasmtime.h>

#include "chorium.h"

#ifdef __cplusplus
extern "C" { /* surface_d2d.cpp is C++ (DirectWrite has no C headers) */
#endif

/* Window messages posted from Chromium's thread to the UI thread. */
#define WM_APP_FRAME (WM_APP + 1) /* wParam = page id */
#define WM_APP_EVENT (WM_APP + 2) /* wParam = page id, lParam = char* JSON */
#define WM_APP_DRAW (WM_APP + 3)

/* Page ids are never reused, so a late message can't reach the wrong page. */
#define MAX_PAGES 256

typedef struct host_page {
  bool used;
  chorium_view* view;
  /* Guards the frame fields; Chromium's thread writes, the UI thread draws. */
  CRITICAL_SECTION lock;
  bool closing;
  uint8_t* frame; /* BGRA, rows packed */
  int frame_w, frame_h;
  size_t frame_cap;
  unsigned frame_seq; /* bumped with every new frame */
  volatile LONG frame_posted;
  /* UI thread only: the frame as a Direct2D bitmap, and which frame it is. */
  void* bitmap;
  unsigned bitmap_seq;
} host_page;

extern HWND g_hwnd;
extern float g_scale; /* device pixels per CSS pixel */
extern host_page g_pages[MAX_PAGES + 1]; /* index = page id, 0 unused */

/* --- vals.c: building and reading component values ---------------------- */
typedef wasmtime_component_val_t val_t;

val_t v_bool(bool b);
val_t v_u32(uint32_t n);
val_t v_s32(int32_t n);
val_t v_f32(float f);
val_t v_string(const char* s, size_t len);
val_t v_cstring(const char* s);
val_t v_enum(const char* name);
/* A flags value with the names whose bit is set in |bits|. */
val_t v_flags(const char* const* names, size_t count, uint32_t bits);
/* Moves |vals| into a record. */
val_t v_record(size_t count, const char* const* names, val_t* vals);
/* Moves |payload| (may be NULL) into a variant. */
val_t v_variant(const char* name, val_t* payload);
val_t v_list(size_t count, val_t* items);
val_t v_option(val_t* some); /* NULL = none */
val_t v_result(bool ok, val_t* payload);
val_t v_bytes(const uint8_t* data, size_t len); /* list<u8> */

const val_t* r_field(const val_t* record, const char* name);
bool r_case(const val_t* variant, const char* name);
const val_t* r_payload(const val_t* variant);
bool r_name_is(const wasm_name_t* name, const char* s);
/* A NUL-terminated copy of a string value; free() it. */
char* r_cstring(const val_t* v);
/* NULL for none, else a NUL-terminated copy; free() it. */
char* r_opt_cstring(const val_t* v);
uint32_t r_flag_bits(const val_t* flags, const char* const* names,
                     size_t count);

/* --- json.c: reading chorium.dll's flat event objects -------------------- */
/* All return false if |key| is missing or has another type. */
bool json_string(const char* json, const char* key, char** out); /* free() */
bool json_bool(const char* json, const char* key, bool* out);
bool json_int(const char* json, const char* key, long long* out);
/* The value's JSON text as written; not copied. */
bool json_raw(const char* json, const char* key, const char** start,
              size_t* len);

/* --- surface_d2d.cpp: Direct2D + DirectWrite, coordinates in CSS pixels -- */
bool surface_init(HWND hwnd);
void surface_resize(int width_px, int height_px);
void surface_set_scale(float scale);
void surface_dimensions(uint32_t* width, uint32_t* height);
void surface_clear(uint32_t argb);
void surface_fill_rect(int x, int y, int w, int h, uint32_t argb);
void surface_stroke_rect(int x, int y, int w, int h, uint32_t argb,
                         float width);
void surface_fill_round_rect(int x, int y, int w, int h, float radius,
                             uint32_t argb);
void surface_draw_text(int x, int y, const char* utf8, size_t len,
                       float font_size, uint32_t argb);
void surface_measure_text(const char* utf8, size_t len, float font_size,
                          uint32_t* width, uint32_t* height);
void surface_draw_page(host_page* page, int x, int y, int w, int h);
void surface_push_clip(int x, int y, int w, int h);
void surface_pop_clip(void);
void surface_present(void);
/* |icon| is the index in chorium.wit's `icon` enum. */
void surface_draw_icon(int icon, int x, int y, int w, int h, uint32_t argb);
/* A page is closing: drop its bitmap (UI thread). */
void surface_forget_page(host_page* page);
bool surface_dark_mode(void);

/* --- main.c: the window's own title bar ---------------------------------- */
/* CSS pixels. False if the system draws the title bar. */
bool title_bar_layout(uint32_t* height, uint32_t* buttons_width,
                      bool* maximized);
/* |xywh| holds |count| rects (x, y, width, height) in CSS pixels. */
void title_bar_set_drag_areas(const int* xywh, size_t count);

/* --- loader.c: starting without a command line --------------------------- */
/* The app (.wasm) in the apps folder next to Chorium.exe, as a UTF-8 path;
 * asks when there are several. False (after telling the person) if none. */
bool loader_pick_app(char* out, size_t cap);
/* Shows a problem in a message box (and on stderr, if there is one). */
void loader_error(const char* fmt, ...);

/* --- contract.c: Wasmtime, the imports, and calls into the app ----------- */
bool contract_load(const char* wasm_path);
/* What went wrong in the last failed load or call, for showing to people. */
const char* contract_last_error(void);
void contract_unload(void);
/* Each call takes ownership of the values passed in. */
void app_start(int argc, char** argv);
void app_input(val_t input);
void app_page_event(uint32_t page_id, val_t event);
void app_call(const char* export_name); /* on-draw, on-pause, ... */
void page_frame(chorium_view* view, const uint8_t* bgra, int width,
                int height, int stride, void* user);
void page_event(chorium_view* view, const char* json, void* user);
void pages_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CHORIUM_HOST_WIN_HOST_H_ */
