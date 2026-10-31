/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The Chorium runtime's shared core: runs an app (a .wasm component for the
 * `runnable` world of interfaces/chorium/chorium.wit) with Wasmtime and
 * implements every import on top of the engine (chorium.h).
 *
 * The core is the same on every platform. Each platform (runtime/win,
 * runtime/android) provides the functions in the "platform" section: the
 * surface, the window or screen, getting engine events onto the UI thread,
 * and where app files live. All core functions run on the UI thread.
 */
#ifndef CHORIUM_RUNTIME_COMMON_RUNTIME_H_
#define CHORIUM_RUNTIME_COMMON_RUNTIME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wasmtime.h>

#include "chorium.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Page ids are never reused, so a late event can't reach the wrong page. */
#define MAX_PAGES 256

typedef wasmtime_component_val_t val_t;

/* --- vals.c: building and reading component values ---------------------- */

val_t v_bool(bool b);
val_t v_u32(uint32_t n);
val_t v_u64(uint64_t n);
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

/* --- contract.c: the core ------------------------------------------------ */

/* Compiles and instantiates the app. |app_name| names its data folder. */
bool contract_load(const char* wasm_path);
bool contract_load_bytes(const uint8_t* bytes, size_t len,
                         const char* app_name);
/* What went wrong in the last failed load or call, for showing to people. */
const char* contract_last_error(void);
void contract_unload(void);

/* Calls into the app. Each takes ownership of the values passed in. */
void app_start(int argc, char** argv);
void app_input(val_t input);
void app_page_event(uint32_t page_id, val_t event);
void app_call(const char* export_name); /* on-draw, on-pause, ... */
/* The system file picker closed: |count| paths (none if cancelled). */
void app_files_picked(uint32_t request, const char* const* paths,
                      size_t count);

/* An engine event (JSON, see chorium.h) for a page, on the UI thread. */
void app_engine_event(uint32_t page_id, const char* json);
bool page_alive(uint32_t page_id);
/* The engine's view for a page, or NULL. */
chorium_view* page_view(uint32_t page_id);
void pages_close_all(void);

/* --- platform: surface (CSS pixels) -------------------------------------- */

void surface_dimensions(uint32_t* width, uint32_t* height);
float surface_scale(void);
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
/* |icon| is the index in chorium.wit's `icon` enum. */
void surface_draw_icon(int icon, int x, int y, int w, int h, uint32_t argb);
void surface_draw_page(uint32_t page_id, int x, int y, int w, int h);
void surface_push_clip(int x, int y, int w, int h);
void surface_pop_clip(void);
void surface_present(void);
bool surface_dark_mode(void);
void surface_show_keyboard(bool visible);
void surface_set_title(const char* utf8, size_t len);
void surface_copy_text(const char* utf8, size_t len);
void surface_set_fullscreen(bool on);
void surface_set_back_enabled(bool on);
/* chorium.wit's file-chooser-mode, in order. */
typedef enum {
  PICKER_OPEN,
  PICKER_OPEN_MULTIPLE,
  PICKER_OPEN_FOLDER,
  PICKER_SAVE,
} picker_mode_t;
/* Shows the system file picker; answers later with app_files_picked().
 * |accept|: MIME types and extensions, |count| of them, none for any. */
void surface_pick_files(uint32_t request, picker_mode_t mode,
                        const char* const* accept, size_t count,
                        const char* default_name);
/* Opens |path| with its app, or shows it in its folder. */
void surface_open_file(const char* path, bool in_folder);
/* The clipboard's text (malloc'd, free() it), or NULL. */
char* surface_read_clipboard(void);
/* CSS pixels. False if the system draws the title bar or there is none. */
bool title_bar_layout(uint32_t* height, uint32_t* buttons_width,
                      bool* maximized);
/* |xywh| holds |count| rects (x, y, width, height) in CSS pixels. */
void title_bar_set_drag_areas(const int* xywh, size_t count);

/* --- platform: everything else ------------------------------------------- */

/* The engine's frame callback, or NULL where pages draw themselves (the
 * engine then captures no frames). */
chorium_frame_fn platform_frame_callback(void);
/* The engine's event callback: hand the JSON to app_engine_event() on the
 * UI thread. |user| is the page id. */
void platform_page_event(chorium_view* view, const char* json, void* user);
/* A page closed: drop what the platform kept for it. */
void platform_page_closed(uint32_t page_id);
/* Folder for files.app-data (UTF-8, created if needed). */
bool platform_app_data_dir(const char* app_name, char* out, size_t cap);
/* The app trapped: tell the person and close. */
void platform_app_crashed(const char* message);
/* Have on-draw called soon (the core never re-enters the app). */
void platform_request_draw(void);
/* Tor was switched back on: report its progress again (as at start). */
void platform_tor_restarted(void);
/* The app's log lines (log.write): stderr on Windows, logcat on Android. */
void platform_log(const char* level, const char* text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CHORIUM_RUNTIME_COMMON_RUNTIME_H_ */
