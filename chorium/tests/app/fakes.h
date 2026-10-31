/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * A fake engine (chorium.h) and a fake screen (runtime.h's platform part)
 * for running a real app (.wasm) on the real runtime core without
 * Chromium. The fake engine records what the app asked of it and answers
 * navigations with the events a real page would send; the fake screen
 * records what the app drew, so tests can find a label and click it.
 */
#ifndef CHORIUM_TESTS_APP_FAKES_H_
#define CHORIUM_TESTS_APP_FAKES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

/* --- setup ---------------------------------------------------------------- */

typedef struct {
  uint32_t width, height; /* CSS pixels */
  bool title_bar;         /* desktop: the app draws the title bar */
  bool dark;              /* the system's dark mode */
} screen_t;

/* Fresh fakes for one scenario; |data_dir| holds the app's files (keep it
 * between two runs to test what survives a restart). */
void fakes_reset(screen_t screen, const char* data_dir);

/* Delivers queued engine events and draws until nothing is left. */
void pump(void);

/* --- what the app asked the engine and the platform for ------------------ */

/* Every call worth checking, one line each, e.g. "edit 2",
 * "load_url 1 https://example.com/", "pick_files 1 1 .png|image/png". */
bool called(const char* prefix);   /* any line starting with |prefix| */
int count_calls(const char* prefix);
const char* last_call(const char* prefix); /* NULL if none */
bool any_call_contains(const char* prefix, const char* text);
void clear_calls(void);
void dump_calls(void);

/* The newest open page's engine id, and how many pages are open. */
uint32_t newest_page(void);
int open_pages(void);

/* --- the engine side ------------------------------------------------------ */

/* Sends |json| (an event from chorium.h) from page |page|. */
void engine_event(uint32_t page, const char* json);

/* --- the screen ----------------------------------------------------------- */

/* The last presented frame. */
bool shown(const char* text);           /* some text contains |text| */
bool shown_exactly(const char* text);
/* Where the last drawn text containing |text| is, as a point to click. */
bool find_text(const char* text, int* x, int* y);
/* The last drawn icon (chorium.wit's icon enum index) and its centre. */
bool find_icon(int icon, int* x, int* y);
/* Icon indices (chorium.wit `icon`). */
enum {
  ICON_BACK = 0, ICON_FORWARD, ICON_RELOAD, ICON_STOP, ICON_CLOSE, ICON_ADD,
  ICON_MENU, ICON_SEARCH, ICON_LOCK, ICON_INFO, ICON_PAGE, ICON_SETTINGS,
  ICON_HISTORY, ICON_HOME, ICON_CHECK, ICON_MINIMIZE, ICON_MAXIMIZE,
  ICON_RESTORE, ICON_CLOSE_WINDOW, ICON_DOWNLOAD, ICON_FOLDER,
};

const char* clipboard(void);
void set_clipboard(const char* text);
bool fullscreen(void);

/* --- input ---------------------------------------------------------------- */

void click(int x, int y);
bool click_text(const char* text); /* false if it isn't shown */
bool click_text_exactly(const char* text);
bool click_icon(int icon);
void type_text(const char* text);
#define MOD_SHIFT 1
#define MOD_CTRL 2
void press_key(int code, int mods); /* down and up */
void press_back(void);

#endif /* CHORIUM_TESTS_APP_FAKES_H_ */
