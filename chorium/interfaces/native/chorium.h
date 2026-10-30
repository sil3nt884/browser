/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Chorium: Chromium's page engine as a library, with Tor inside.
 *
 * No browser UI, no settings, no command-line flags, no DevTools. You pass
 * pages in and get rendered frames and events out; your own UI does the rest.
 * All traffic goes through Tor (Arti) running in the same process; if Tor
 * cannot start, every request fails rather than going out directly.
 *
 * Threads: every function may be called from any thread. Calls are queued to
 * the engine thread and return at once. Callbacks run on the engine thread:
 * keep them short and copy what you need.
 *
 * Minimal program:
 *
 *   int main(void) {
 *     int code = chorium_execute_process();
 *     if (code >= 0) return code;          // a Chromium helper process
 *     chorium_init(1.0f);
 *     chorium_view *v = chorium_open(1280, 800, on_frame, on_event, NULL);
 *     chorium_load_url(v, "https://example.com/");
 *     ... run until done ...
 *     chorium_shutdown();
 *   }
 */
#ifndef CHORIUM_INCLUDE_CHORIUM_H_
#define CHORIUM_INCLUDE_CHORIUM_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(CHORIUM_IMPLEMENTATION)
#define CHORIUM_API __declspec(dllexport)
#else
#define CHORIUM_API __declspec(dllimport)
#endif
#else
#define CHORIUM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a function, callback or event changes. A program built
 * against this header should check chorium_abi_version() == CHORIUM_ABI_VERSION
 * before calling anything else. */
#define CHORIUM_ABI_VERSION 2

typedef struct chorium_view chorium_view;

/* Rendered picture: 32-bit BGRA, premultiplied alpha, rows top to bottom,
 * |stride| bytes per row. Size is in device pixels (CSS size x scale). The
 * pointer is only valid during the call. */
typedef void (*chorium_frame_fn)(chorium_view* view,
                                 const uint8_t* bgra,
                                 int width,
                                 int height,
                                 int stride,
                                 void* user);

/* Something happened in the page, as one JSON object with a "type" field:
 *   {"type":"url","url":"...","can_go_back":true,"can_go_forward":false}
 *                                           a navigation committed
 *   {"type":"title","title":"..."}
 *   {"type":"loading","loading":true}       spinner on/off
 *   {"type":"loaded","url":"...","ok":true,"error":""}
 *   {"type":"new_window","url":"...","user_gesture":true}
 *                                           page asked for a new tab/window;
 *                                           open one with chorium_open if
 *                                           you want it
 *   {"type":"fullscreen","on":true}
 *   {"type":"dialog","id":7,"kind":"alert|confirm|prompt|beforeunload",
 *    "message":"...","default":"..."}      answer with chorium_dialog_reply
 *   {"type":"eval","id":3,"ok":true,"result":<JSON value>}
 *   {"type":"crashed"}                      page process died; reload it
 *   {"type":"closed"}                       page called window.close()
 */
typedef void (*chorium_event_fn)(chorium_view* view,
                                 const char* json,
                                 void* user);

/* Call first in main(), before anything else. Chromium runs its page, GPU
 * and network helpers by starting your executable again; in those helper
 * processes this runs the helper and returns its exit code (>= 0), which
 * main() must return. In your own process it returns -1 at once. */
CHORIUM_API int chorium_execute_process(void);

/* The CHORIUM_ABI_VERSION this library was built with. Safe to call any
 * time, even before chorium_execute_process(). */
CHORIUM_API int chorium_abi_version(void);

/* Starts the engine (and Tor) on its own thread. |scale| is the device pixel
 * ratio for all views, e.g. 1.0 or 2.0. Returns 0 on success. Can be called
 * once per process; the engine can't be restarted after chorium_shutdown. */
CHORIUM_API int chorium_init(float scale);

/* Closes all views and stops the engine. Blocks until it has stopped. */
CHORIUM_API void chorium_shutdown(void);

/* Tor bootstrap progress 0-100, or -1 if Tor failed (pages then fail). */
CHORIUM_API int chorium_tor_progress(void);

/* A page view, |width| x |height| CSS pixels. Starts on about:blank. */
CHORIUM_API chorium_view* chorium_open(int width,
                                       int height,
                                       chorium_frame_fn on_frame,
                                       chorium_event_fn on_event,
                                       void* user);
CHORIUM_API void chorium_close(chorium_view* view);

CHORIUM_API void chorium_load_url(chorium_view* view, const char* url);
/* |base_url| sets the page's origin for relative links; may be NULL. */
CHORIUM_API void chorium_load_html(chorium_view* view,
                                   const char* html,
                                   const char* base_url);
CHORIUM_API void chorium_back(chorium_view* view);
CHORIUM_API void chorium_forward(chorium_view* view);
CHORIUM_API void chorium_reload(chorium_view* view);
CHORIUM_API void chorium_stop(chorium_view* view);
CHORIUM_API void chorium_resize(chorium_view* view, int width, int height);
/* Asks for a fresh frame even if nothing changed. */
CHORIUM_API void chorium_refresh_frame(chorium_view* view);

/* Input. Coordinates are CSS pixels from the view's top-left. */
#define CHORIUM_MOUSE_MOVE 0
#define CHORIUM_MOUSE_DOWN 1
#define CHORIUM_MOUSE_UP 2
#define CHORIUM_MOUSE_LEAVE 3

#define CHORIUM_BUTTON_NONE 0
#define CHORIUM_BUTTON_LEFT 1
#define CHORIUM_BUTTON_MIDDLE 2
#define CHORIUM_BUTTON_RIGHT 3

#define CHORIUM_MOD_SHIFT 1
#define CHORIUM_MOD_CTRL 2
#define CHORIUM_MOD_ALT 4
#define CHORIUM_MOD_META 8

CHORIUM_API void chorium_mouse(chorium_view* view,
                               int type,
                               int x,
                               int y,
                               int button,
                               int click_count,
                               int modifiers);
/* Scroll by |dx|, |dy| CSS pixels (positive dy scrolls content up). */
CHORIUM_API void chorium_wheel(chorium_view* view,
                               int x,
                               int y,
                               float dx,
                               float dy,
                               int modifiers);
/* A key press or release; |key_code| is a Windows virtual-key code
 * (VK_RETURN, 'A', ...), the same on every platform. */
CHORIUM_API void chorium_key(chorium_view* view,
                             int down,
                             int key_code,
                             int modifiers);
/* Types text into the focused field (UTF-8). */
CHORIUM_API void chorium_text(chorium_view* view, const char* utf8);
CHORIUM_API void chorium_focus(chorium_view* view, int focused);

/* Runs JavaScript in the page; the result arrives as an "eval" event with
 * the same |id|. */
CHORIUM_API void chorium_eval(chorium_view* view, int id, const char* js);

/* Answers a "dialog" event. |text| is the prompt() answer; may be NULL. */
CHORIUM_API void chorium_dialog_reply(chorium_view* view,
                                      int id,
                                      int accept,
                                      const char* text);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHORIUM_INCLUDE_CHORIUM_H_ */
