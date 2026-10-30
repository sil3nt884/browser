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

typedef enum { PANEL_NONE, PANEL_SETTINGS, PANEL_HISTORY } panel_t;

extern umbra_chorium_types_size_t g_size; /* surface, CSS pixels */
extern uint32_t g_active;                 /* tab id */
extern focus_t g_focus;
extern panel_t g_panel;
extern bool g_menu_open;
extern int g_hover; /* hit code under the pointer, see browser.c */

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
/* Draws the contents at |x| in |r| (vertically centred), with selection or
 * caret when focused, else |placeholder| if empty. */
void field_draw(const field_t* f, rect_t r, int x, float size, bool focused,
                const char* placeholder);

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

/* --- browser.c (used by panels.c) ----------------------------------------- */

/* A new tab on the home page, with the address field focused (Ctrl+T). */
void new_tab(void);
void navigate_active(const char* text);
void request_draw(void);
/* Where the page area starts: below the tab strip and toolbar. */
int chrome_top(void);

#endif /* CHORIUM_APPS_BROWSER_UI_H_ */
