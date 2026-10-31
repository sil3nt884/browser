/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Shared pieces of the sample browser: theme colours, drawing helpers, the
 * text fields, and the state the files share.
 */
#ifndef CHORIUM_APPS_BROWSER_UI_H_
#define CHORIUM_APPS_BROWSER_UI_H_

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

typedef umbra_chorium_types_rect_t rect_t;
typedef umbra_chorium_types_pointer_t pointer_t;
typedef umbra_chorium_types_key_t keypress_t;
typedef umbra_chorium_surface_icon_t icon_t;
typedef umbra_browser_browser_tab_info_t tab_info_t;
typedef umbra_browser_browser_list_tab_info_t tab_list_t;

#define ICON(name) UMBRA_CHORIUM_SURFACE_ICON_##name

/* --- theme ---------------------------------------------------------------- */

typedef struct {
  uint32_t frame;       /* title bar / tab strip */
  uint32_t tab_hover;   /* inactive tab under the pointer */
  uint32_t toolbar;     /* toolbar and the active tab */
  uint32_t omnibox;     /* address field */
  uint32_t text, text_dim;
  uint32_t icon, icon_off;
  uint32_t hover;       /* round highlight behind toolbar buttons */
  uint32_t accent;
  uint32_t selection;
  uint32_t separator;
  uint32_t page_bg;     /* settings/history pages */
  uint32_t card;
  uint32_t shadow;
  uint32_t error;
  uint32_t close_hover; /* window close button */
} theme_t;

extern const theme_t* T;
/* Picks light/dark from the "theme" setting (system, light, dark). */
void theme_update(void);

/* --- shared state --------------------------------------------------------- */

typedef enum {
  FOCUS_PAGE,
  FOCUS_ADDRESS,
  FOCUS_CUSTOM_SEARCH,
  FOCUS_CUSTOM_HOME,
} focus_t;

typedef enum {
  PANEL_NONE,
  PANEL_SETTINGS,
  PANEL_HISTORY,
  PANEL_TABS,
  PANEL_DOWNLOADS,
} panel_t;

extern umbra_chorium_types_size_t g_size; /* surface, CSS pixels */
extern uint32_t g_active;                 /* tab id */
extern focus_t g_focus;
extern panel_t g_panel;
extern bool g_menu_open;
extern int g_hover; /* hit code under the pointer, see browser.c */
extern bool g_phone; /* phone layout: no tab strip, touch-sized */

/* --- geometry and drawing ------------------------------------------------- */

rect_t rect(int x, int y, int w, int h);
bool inside(rect_t r, int x, int y);
void fill(rect_t r, uint32_t c);
void fill_round(rect_t r, float radius, uint32_t c);
void circle(int cx, int cy, int radius, uint32_t c);
void stroke(rect_t r, uint32_t c, float width);
void text(int x, int baseline, const char* s, float size, uint32_t c);
int text_width(const char* s, float size);
/* Text vertically centred in |r|, clipped to it. */
void text_in(rect_t r, const char* s, float size, uint32_t c);
void icon(icon_t which, rect_t r, uint32_t c);
/* A |size| icon centred in |box|. */
void icon_centered(icon_t which, rect_t box, int size, uint32_t c);
void clip(rect_t r);
void unclip(void);
/* An on/off switch (Android style), 36 x 20, centred vertically in |r| at
 * its right edge. */
void toggle_switch(rect_t r, bool on);

/* --- strings and settings ------------------------------------------------- */

app_string_t str(const char* s);
void copy_str(char* out, size_t cap, const app_string_t* s);
bool get_setting(const char* key, char* out, size_t cap);
void set_setting(const char* key, const char* value);

/* --- text fields ---------------------------------------------------------- */

typedef struct {
  char buf[2048];
  bool selected; /* everything selected: typing replaces it */
} field_t;

void field_set(field_t* f, const char* value, bool select_all);
/* True if the key was used; *enter is set on Enter. */
bool field_key(field_t* f, const keypress_t* k, bool* enter);
void field_text(field_t* f, const app_string_t* s);
/* Ctrl+V: the clipboard's text, on one line. */
void field_paste(field_t* f);
/* Draws the contents at |x| in |r| (vertically centred), with selection or
 * caret when focused, else |placeholder| if empty. */
void field_draw(const field_t* f, rect_t r, int x, float size, bool focused,
                const char* placeholder);

/* --- gestures.c ----------------------------------------------------------- */

/* Hit codes of the long-press menu (panels.c has 400-410 and 500+). */
enum {
  HIT_CTX_ITEM = 430, /* + item */
  HIT_CTX_CARD = 440,
  HIT_CTX_DISMISS = 450,
};

void overscroll_event(const umbra_browser_browser_overscroll_t* o);
/* The pull / swipe indicator over the page area |page|. */
void overscroll_draw(rect_t page);
void context_menu_open(const umbra_browser_browser_context_menu_t* m,
                       rect_t page);
bool context_menu_is_open(void);
void context_menu_close(void);
void context_menu_draw(void);
int context_menu_hit(int x, int y);
void context_menu_click(int code);

/* --- files.c -------------------------------------------------------------- */

/* Hit codes of the download notice. */
enum {
  HIT_NOTICE = 1300, /* + button: 0 close, 1 show in folder, 2 open */
  HIT_NOTICE_BODY = 1310,
};

void format_size(char* out, size_t cap, uint64_t bytes);
/* "1.2 MB of 5.0 MB", "Failed", ... */
void download_status(char* out, size_t cap,
                     const umbra_chorium_types_download_t* d);
void open_download(const char* path, bool in_folder);
void downloads_event(const umbra_chorium_types_download_t* d);
void notice_draw(rect_t page);
int notice_hit(int x, int y);
void notice_click(int code);
void notice_close(void);
void file_chooser_event(uint32_t tab, umbra_chorium_types_file_chooser_t* c);
void files_picked(umbra_chorium_types_files_picked_t* p);

/* --- panels.c ------------------------------------------------------------- */

rect_t menu_rect(void);
void menu_draw(void);
/* A hit code for (x, y) in the menu or panel, or 0. */
int panels_hit(int x, int y);
void panels_click(int code);
void panel_draw(rect_t area);
void panel_open(panel_t panel);
void panel_wheel(float dy);
bool panel_key(const keypress_t* k);
void panel_text(const app_string_t* s);
/* The tab switcher row a hit code is on, or -1. */
int tab_row_of(int code);
void close_tab_row(int index);
/* A row being swiped sideways (to close it): its index or -1, and offset. */
extern int g_swipe_row;
extern int g_swipe_dx;

/* --- browser.c (used by panels.c) ----------------------------------------- */

/* A new tab on the home page, with the address field focused (Ctrl+T). */
void new_tab(void);
void navigate_active(const char* text);
void switch_tab(uint32_t id);
/* Closes a tab; if it was active another becomes active (or a new one). */
void close_tab(uint32_t id);
void go_forward(void);
void reload_or_stop(void);
/* A new tab for |text| (as typed in the address field); stays behind the
 * current one unless |foreground|. */
void open_tab_with(const char* text, bool foreground);
void request_draw(void);
/* Where the page area starts: below the tab strip and toolbar. */
int chrome_top(void);

#endif /* CHORIUM_APPS_BROWSER_UI_H_ */
