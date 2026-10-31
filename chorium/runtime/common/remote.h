/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The remote control: lets the end-to-end tests (tests/e2e) see and use a
 * running app the way a person does. The app draws its own UI, which no
 * accessibility API can see, so the runtime records every text, icon and
 * page it draws, and takes taps, keys and text it then delivers through the
 * platform's real input path.
 *
 * Off unless started (Chorium.exe --remote-control; on Android the
 * "remote_control" intent extra). Windows: 127.0.0.1:9334. Android: the
 * abstract socket "chorium_remote" (adb forward tcp:9334
 * localabstract:chorium_remote), for adb and the app itself only.
 *
 * Protocol: one JSON object per line each way. Requests have "cmd":
 *   elements       what the last presented frame showed:
 *                  {"width":..,"height":..,"scale":..,
 *                   "texts":[{"text","x","y","w","h","size"}],
 *                   "icons":[{"icon","x","y","w","h"}],
 *                   "pages":[{"id","x","y","w","h"}]}  (CSS pixels, visible
 *                  part only: what clipping hid is left out)
 *   state          {"frames","fps","page_frames","keyboard","fullscreen",
 *                   "title","tor","tor_enabled","picker":{..} or null,
 *                   "opened":[{"path","in_folder"}]}
 *   tap x y [button:"right"]   press x y ms   swipe x1 y1 x2 y2 ms
 *   wheel x y dy   key code [mods: 1 shift, 2 ctrl, 4 alt]   text "..."
 *   back           screenshot -> {"format":"bmp"|"png","data":base64}
 *   intercept_open on     record surface.open-file instead of opening
 *   picker_answer paths   answer an open system picker (Android)
 * Replies always have "ok"; failures have "error".
 */
#ifndef CHORIUM_RUNTIME_COMMON_REMOTE_H_
#define CHORIUM_RUNTIME_COMMON_REMOTE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_PORT 9334
#define REMOTE_SOCKET_NAME "chorium_remote"

/* Starts listening (Windows: on REMOTE_PORT; Android: REMOTE_SOCKET_NAME). */
bool remote_start(void);
bool remote_enabled(void);

/* --- recording, from the core's surface host functions (UI thread) ------ */
void remote_text(int x, int baseline, const char* utf8, size_t len,
                 float size);
void remote_icon(int icon, int x, int y, int w, int h);
void remote_page(uint32_t page_id, int x, int y, int w, int h);
void remote_clip(int x, int y, int w, int h);
void remote_unclip(void);
void remote_present(void);
void remote_page_frame(void); /* a page frame arrived (Windows) */
void remote_keyboard(bool shown);
void remote_fullscreen(bool on);
void remote_title(const char* utf8, size_t len);
/* A system picker opened / closed. */
void remote_picker(uint32_t request, int mode, const char* const* accept,
                   size_t count);
void remote_picker_closed(void);
/* True if the open-file call was recorded instead (intercept_open). */
bool remote_open_file(const char* path, bool in_folder);

/* --- what each platform provides ----------------------------------------- */

typedef enum {
  RI_DOWN,        /* left button / finger */
  RI_MOVE,
  RI_UP,
  RI_RIGHT_DOWN,  /* desktop */
  RI_RIGHT_UP,
  RI_WHEEL,       /* dy in CSS pixels */
  RI_KEY_DOWN,
  RI_KEY_UP,
  RI_TEXT,
  RI_BACK,
} remote_input_kind_t;

typedef struct {
  remote_input_kind_t kind;
  int x, y; /* CSS pixels from the surface's top left */
  float dy;
  int code, mods;
  const char* text;
} remote_input_t;

/* Runs |fn(arg)| on the UI thread, soon. Any thread. */
void platform_post_to_ui(void (*fn)(void*), void* arg);
/* Delivers |in| through the platform's own input path (UI thread). */
void platform_remote_input(const remote_input_t* in);
/* A picture of the window, page included: "bmp" or "png" bytes, handed to
 * remote_screenshot_done (UI thread, now or later). */
void platform_screenshot(void* token);
void remote_screenshot_done(void* token, const char* format,
                            const uint8_t* data, size_t len);
/* Answers the open system picker with |paths| (none: cancel). False if the
 * platform can't (Windows: the tests drive the real dialog). */
bool platform_answer_picker(const char* const* paths, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* CHORIUM_RUNTIME_COMMON_REMOTE_H_ */
