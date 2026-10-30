/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The browser's own pages and menu: the ⋮ menu, Settings, History and (on
 * phones) the tab switcher. They are drawn by the app over the page area,
 * like chrome://settings, and keep their data in chorium.wasm (settings and
 * history).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui.h"

/* Hit codes (browser.c uses below 400). */
enum {
  HIT_MENU_ITEM = 400, /* + item */
  HIT_MENU_BG = 410,
  HIT_MENU_ICON = 420, /* + icon in the phone menu's top row */
  HIT_ROW = 500,      /* + settings row */
  HIT_HISTORY = 600,  /* + history entry */
  HIT_CLEAR = 800,
  HIT_TABS_NEW = 801,
  HIT_TAB_ROW = 900,       /* + tab index */
  HIT_TAB_ROW_CLOSE = 1000, /* + tab index */
};

#define TAB_ROWS_MAX 100

#define VK_ESCAPE 0x1B
#define HISTORY_LIMIT 200

/* --- the ⋮ menu ----------------------------------------------------------- */

typedef struct {
  icon_t icon;
  const char* label;
  const char* keys;
} menu_item_t;

static const menu_item_t kMenu[] = {
    {ICON(ADD), "New tab", "Ctrl+T"},
    {ICON(HISTORY), "History", "Ctrl+H"},
    {ICON(SETTINGS), "Settings", ""},
    {ICON(LOCK), "Tor", ""}, /* with a switch */
};
#define MENU_TOR 3
#define MENU_COUNT (int)(sizeof kMenu / sizeof kMenu[0])
#define MENU_ITEM_H (g_phone ? 48 : 36)
#define MENU_ICONS_H 56 /* phones: Forward and Reload above the items */

static int menu_top_row(void) { return g_phone ? MENU_ICONS_H : 0; }

rect_t menu_rect(void) {
  int w = g_phone ? 240 : 260;
  /* Below the ⋮ button, right-aligned with it. */
  return rect((int)g_size.width - w - (g_phone ? 4 : 8), 0, w,
              menu_top_row() + MENU_COUNT * MENU_ITEM_H + 16);
}

static rect_t menu_box(void) {
  rect_t r = menu_rect();
  r.y = g_phone ? 4 : chrome_top() - 4; /* phones: over the button */
  return r;
}

static rect_t menu_item(rect_t box, int i) {
  return rect(box.x, box.y + 8 + menu_top_row() + i * MENU_ITEM_H,
              (int)box.width, MENU_ITEM_H);
}

static rect_t menu_icon(rect_t box, int i) {
  return rect(box.x + 8 + i * 56, box.y + 4, 56, MENU_ICONS_H);
}

void menu_draw(void) {
  rect_t r = menu_box();
  fill_round(rect(r.x - 1, r.y + 1, (int)r.width + 2, (int)r.height + 3), 9,
             T->shadow);
  fill_round(r, 8, T->card);
  if (g_phone) {
    tab_info_t info;
    bool have = umbra_browser_browser_info(g_active, &info);
    bool forward = have && info.can_go_forward && g_panel == PANEL_NONE;
    bool loading = have && info.loading;
    if (have) {
      umbra_browser_browser_tab_info_free(&info);
    }
    for (int i = 0; i < 2; ++i) {
      bool enabled = i == 0 ? forward : g_panel == PANEL_NONE;
      rect_t b = menu_icon(r, i);
      if (enabled && g_hover == HIT_MENU_ICON + i) {
        circle(b.x + 28, b.y + 28, 22, T->hover);
      }
      icon_centered(i == 0 ? ICON(FORWARD) : loading ? ICON(STOP) : ICON(RELOAD),
                    b, 22, enabled ? T->icon : T->icon_off);
    }
    fill(rect(r.x, r.y + 8 + MENU_ICONS_H - 4, (int)r.width, 1),
         T->separator);
  }
  for (int i = 0; i < MENU_COUNT; ++i) {
    rect_t row = menu_item(r, i);
    if (g_hover == HIT_MENU_ITEM + i) {
      fill(row, T->hover);
    }
    icon_centered(kMenu[i].icon, rect(row.x + 12, row.y, 24, MENU_ITEM_H),
                  g_phone ? 20 : 16, T->icon);
    text_in(rect(row.x + 48, row.y, 150, MENU_ITEM_H), kMenu[i].label,
            g_phone ? 15 : 14, T->text);
    if (i == MENU_TOR) {
      toggle_switch(rect(row.x, row.y, (int)row.width - 16, MENU_ITEM_H),
                    umbra_browser_browser_tor_enabled());
    }
    if (kMenu[i].keys[0] && !g_phone) {
      int w = text_width(kMenu[i].keys, 12.5f);
      text_in(rect(row.x + (int)row.width - 16 - w, row.y, w + 2, MENU_ITEM_H),
              kMenu[i].keys, 12.5f, T->text_dim);
    }
  }
}

/* --- settings ------------------------------------------------------------- */

typedef struct {
  const char* label;
  const char* note;
  const char* value; /* NULL = custom */
} choice_t;

static const choice_t kSearch[] = {
    {"DuckDuckGo Lite", "no JavaScript, fastest over Tor",
     "https://html.duckduckgo.com/html/?q=%s"},
    {"DuckDuckGo", "", "https://duckduckgo.com/?q=%s"},
    {"Startpage", "", "https://www.startpage.com/do/search?q=%s"},
    {"Brave Search", "", "https://search.brave.com/search?q=%s"},
    {"Custom", "a URL with %s where the search words go", NULL},
};
static const choice_t kHome[] = {
    {"DuckDuckGo Lite", "", "https://html.duckduckgo.com/html/"},
    {"Blank page", "", "about:blank"},
    {"Custom", "any address", NULL},
};
static const choice_t kTheme[] = {
    {"Same as Windows", "", "system"},
    {"Light", "", "light"},
    {"Dark", "", "dark"},
};
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

typedef enum {
  ROW_RADIO,
  ROW_FIELD,
  ROW_NOTE,
  ROW_CLEAR,
  ROW_TOGGLE, /* the Tor switch */
} row_kind_t;

typedef struct {
  row_kind_t kind;
  rect_t r;
  const char* label;
  const char* note;
  bool selected;
  int group; /* 0 search, 1 home, 2 theme */
  int index;
} row_t;

#define MAX_ROWS 24
static row_t g_rows[MAX_ROWS];
static int g_row_count;
#define CARDS 5
static rect_t g_cards[CARDS];
static const char* g_card_titles[CARDS] = {
    "Connection", "Search engine", "On startup and new tabs", "Appearance",
    "Privacy"};
static int g_card_count;
static int g_content_h;

static rect_t g_area; /* page area at the last draw */
static int g_scroll;
static field_t g_custom_search, g_custom_home;
static bool g_search_custom_chosen, g_home_custom_chosen;
static char g_search_error[128];
static bool g_cleared;

static umbra_browser_browser_list_history_entry_t g_history;
static bool g_history_loaded;

/* Which choice the saved value is (the last one = custom). */
static int chosen(const choice_t* list, int count, const char* key,
                  bool custom_chosen) {
  char value[2048] = "";
  get_setting(key, value, sizeof value);
  if (custom_chosen) {
    return count - 1;
  }
  if (!value[0]) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    if (list[i].value && strcmp(list[i].value, value) == 0) {
      return i;
    }
  }
  return list[count - 1].value ? 0 : count - 1;
}

static void add_row(row_kind_t kind, int* y, int x, int w, int h,
                    const char* label, const char* note, bool selected,
                    int group, int index) {
  if (g_row_count == MAX_ROWS) {
    return;
  }
  row_t* row = &g_rows[g_row_count++];
  row->kind = kind;
  row->r = rect(x, *y, w, h);
  row->label = label;
  row->note = note;
  row->selected = selected;
  row->group = group;
  row->index = index;
  *y += h;
}

static void add_choices(int group, const choice_t* list, int count,
                        const char* key, bool custom_chosen, field_t* field,
                        int* y, int x, int w) {
  int pick = chosen(list, count, key, custom_chosen);
  for (int i = 0; i < count; ++i) {
    /* Phones put a note on a second line. */
    int h = g_phone && list[i].note[0] ? 60 : g_phone ? 52 : 44;
    add_row(ROW_RADIO, y, x, w, h, list[i].label, list[i].note, i == pick,
            group, i);
  }
  if (field && pick == count - 1) {
    add_row(ROW_FIELD, y, x, w, 48, "", "", false, group, 0);
    if (group == 0 && g_search_error[0]) {
      add_row(ROW_NOTE, y, x, w, 28, g_search_error, "", false, group, 0);
    }
  }
}

/* Lays out the settings page in |area|: rows and cards. */
static int column_width(rect_t area, int max) {
  int w = (int)area.width - (g_phone ? 24 : 48);
  return w < max ? w : max;
}

static void build_settings(rect_t area) {
  g_row_count = 0;
  g_card_count = 0;
  int w = column_width(area, 680);
  int x = area.x + ((int)area.width - w) / 2;
  int y = area.y + 72 - g_scroll;
  for (int card = 0; card < CARDS; ++card) {
    y += 36; /* section title */
    int start = y;
    switch (card) {
      case 0:
        add_row(ROW_TOGGLE, &y, x, w, 64, "Connect through Tor",
                umbra_browser_browser_tor_enabled()
                    ? "On: pages go through Tor, or fail if it's down"
                    : "Off: Tor is stopped and pages connect directly",
                umbra_browser_browser_tor_enabled(), 4, 0);
        break;
      case 1:
        add_choices(0, kSearch, COUNT(kSearch), "search",
                    g_search_custom_chosen, &g_custom_search, &y, x, w);
        break;
      case 2:
        add_choices(1, kHome, COUNT(kHome), "home", g_home_custom_chosen,
                    &g_custom_home, &y, x, w);
        break;
      case 3:
        add_choices(2, kTheme, COUNT(kTheme), "theme", false, NULL, &y, x, w);
        break;
      case 4:
        add_row(ROW_CLEAR, &y, x, w, 56, "Clear browsing history",
                "pages you visited, kept on this device", false, 3, 0);
        break;
    }
    g_cards[g_card_count++] = rect(x, start, w, y - start);
    y += 12;
  }
  g_content_h = y + g_scroll - area.y + 24;
}

static void radio(int cx, int cy, bool on) {
  circle(cx, cy, 9, on ? T->accent : T->icon_off);
  circle(cx, cy, 7, T->card);
  if (on) {
    circle(cx, cy, 4, T->accent);
  }
}

static void button(rect_t r, const char* label, bool hover) {
  float radius = (float)r.height / 2;
  fill_round(r, radius, T->separator);
  fill_round(rect(r.x + 1, r.y + 1, (int)r.width - 2, (int)r.height - 2),
             radius - 1, hover ? T->hover : T->card);
  int w = text_width(label, 13);
  text_in(rect(r.x + ((int)r.width - w) / 2, r.y, w + 2, (int)r.height), label,
          13, T->accent);
}

static rect_t clear_button(const row_t* row) {
  return rect(row->r.x + (int)row->r.width - 100, row->r.y + 12, 84, 32);
}

static void draw_settings(rect_t area) {
  build_settings(area);
  int w = g_cards[0].width;
  int x = g_cards[0].x;
  text(x, area.y + 52 - g_scroll, "Settings", 22, T->text);
  for (int c = 0; c < g_card_count; ++c) {
    rect_t card = g_cards[c];
    text(card.x + 4, card.y - 12, g_card_titles[c], 14, T->text_dim);
    fill_round(card, 8, T->card);
  }
  for (int i = 0; i < g_row_count; ++i) {
    const row_t* row = &g_rows[i];
    rect_t r = row->r;
    bool hover = g_hover == HIT_ROW + i;
    switch (row->kind) {
      case ROW_RADIO: {
        if (hover) {
          fill(rect(r.x, r.y, (int)r.width, (int)r.height), T->hover);
        }
        const char* label = row->label;
        if (g_phone && row->group == 2 && row->index == 0) {
          label = "Same as system";
        }
        radio(r.x + 24, r.y + (int)r.height / 2, row->selected);
        if (g_phone && row->note[0]) {
          text_in(rect(r.x + 48, r.y + 8, (int)r.width - 56, 22), label, 15,
                  T->text);
          text_in(rect(r.x + 48, r.y + 30, (int)r.width - 56, 20), row->note,
                  12.5f, T->text_dim);
          break;
        }
        text_in(rect(r.x + 48, r.y, (int)r.width - 56, (int)r.height), label,
                g_phone ? 15 : 14, T->text);
        if (row->note[0]) {
          int lw = text_width(row->label, 14);
          text_in(rect(r.x + 48 + lw + 10, r.y, (int)r.width - lw - 66, 44),
                  row->note, 12.5f, T->text_dim);
        }
        break;
      }
      case ROW_FIELD: {
        field_t* f = row->group == 0 ? &g_custom_search : &g_custom_home;
        bool focused = g_focus == (row->group == 0 ? FOCUS_CUSTOM_SEARCH
                                                   : FOCUS_CUSTOM_HOME);
        rect_t box = rect(r.x + 48, r.y + 6, (int)r.width - 64, 36);
        fill_round(box, 6, T->omnibox);
        if (focused) {
          stroke(box, T->accent, 2);
        }
        field_draw(f, box, box.x + 10, 13.5f, focused,
                   row->group == 0
                       ? "https://example.com/search?q=%s  (Enter to save)"
                       : "https://example.com/  (Enter to save)");
        break;
      }
      case ROW_NOTE:
        text_in(rect(r.x + 48, r.y, (int)r.width - 56, (int)r.height),
                row->label, 12.5f, T->error);
        break;
      case ROW_TOGGLE:
        if (hover) {
          fill(r, T->hover);
        }
        text_in(rect(r.x + 20, r.y + 12, (int)r.width - 90, 20), row->label,
                g_phone ? 15 : 14, T->text);
        text_in(rect(r.x + 20, r.y + 34, (int)r.width - 90, 18), row->note,
                12.5f, row->selected ? T->text_dim : T->error);
        toggle_switch(rect(r.x, r.y, (int)r.width - 20, (int)r.height),
                      row->selected);
        break;
      case ROW_CLEAR: {
        text_in(rect(r.x + 20, r.y + 10, (int)r.width - 130, 20), row->label,
                14, T->text);
        text_in(rect(r.x + 20, r.y + 30, (int)r.width - 130, 18), row->note,
                12.5f, T->text_dim);
        if (g_cleared) {
          text_in(rect(r.x + (int)r.width - 110, r.y, 100, (int)r.height),
                  "Cleared", 13, T->text_dim);
        } else {
          button(clear_button(row), "Clear", hover);
        }
        break;
      }
    }
    /* Divider above rows that follow another row in the same card. */
    if (i > 0 && g_rows[i - 1].r.y + (int)g_rows[i - 1].r.height == r.y &&
        row->kind != ROW_NOTE && row->kind != ROW_FIELD) {
      fill(rect(r.x + 16, r.y, (int)r.width - 32, 1), T->page_bg);
    }
  }
  (void)w;
}

static void save_custom(int group) {
  if (group == 0) {
    if (!strstr(g_custom_search.buf, "%s")) {
      snprintf(g_search_error, sizeof g_search_error,
               "Put %%s in the address where the search words should go.");
      return;
    }
    g_search_error[0] = '\0';
    set_setting("search", g_custom_search.buf);
    g_search_custom_chosen = false;
  } else {
    if (!g_custom_home.buf[0]) {
      return;
    }
    char url[2100];
    if (strstr(g_custom_home.buf, "://") ||
        strncmp(g_custom_home.buf, "about:", 6) == 0) {
      snprintf(url, sizeof url, "%s", g_custom_home.buf);
    } else {
      snprintf(url, sizeof url, "https://%s", g_custom_home.buf);
    }
    set_setting("home", url);
    g_home_custom_chosen = false;
  }
  g_focus = FOCUS_PAGE;
}

static void click_settings_row(int i) {
  const row_t* row = &g_rows[i];
  if (row->kind == ROW_TOGGLE) {
    umbra_browser_browser_set_tor(!umbra_browser_browser_tor_enabled());
    return;
  }
  if (row->kind == ROW_CLEAR) {
    umbra_browser_browser_clear_history();
    g_cleared = true;
    g_history_loaded = false;
    return;
  }
  if (row->kind == ROW_FIELD) {
    g_focus = row->group == 0 ? FOCUS_CUSTOM_SEARCH : FOCUS_CUSTOM_HOME;
    return;
  }
  if (row->kind != ROW_RADIO) {
    return;
  }
  const choice_t* list = row->group == 0   ? kSearch
                         : row->group == 1 ? kHome
                                           : kTheme;
  const char* key = row->group == 0 ? "search"
                    : row->group == 1 ? "home"
                                      : "theme";
  const choice_t* c = &list[row->index];
  if (c->value) {
    set_setting(key, c->value);
    if (row->group == 0) {
      g_search_custom_chosen = false;
      g_search_error[0] = '\0';
    } else if (row->group == 1) {
      g_home_custom_chosen = false;
    }
    g_focus = FOCUS_PAGE;
    return;
  }
  /* Custom: open the field with what's saved, ready to type. */
  char saved[2048] = "";
  get_setting(key, saved, sizeof saved);
  bool known = false;
  for (int k = 0; list[k].value; ++k) {
    known |= strcmp(list[k].value, saved) == 0;
  }
  if (row->group == 0) {
    g_search_custom_chosen = true;
    field_set(&g_custom_search, known ? "" : saved, true);
    g_focus = FOCUS_CUSTOM_SEARCH;
  } else {
    g_home_custom_chosen = true;
    field_set(&g_custom_home, known ? "" : saved, true);
    g_focus = FOCUS_CUSTOM_HOME;
  }
}

/* --- history -------------------------------------------------------------- */

static void load_history(void) {
  if (g_history_loaded) {
    umbra_browser_browser_list_history_entry_free(&g_history);
  }
  umbra_browser_browser_history(HISTORY_LIMIT, &g_history);
  g_history_loaded = true;
}

static rect_t history_column(rect_t area) {
  int w = column_width(area, 760);
  return rect(area.x + ((int)area.width - w) / 2, area.y, w, (int)area.height);
}

static rect_t history_clear(rect_t area) {
  rect_t col = history_column(area);
  int w = g_phone ? 120 : 200, h = g_phone ? 40 : 32;
  return rect(col.x + (int)col.width - w, area.y + 46 - h / 2 - g_scroll, w,
              h);
}

static rect_t history_row(rect_t area, size_t i) {
  rect_t col = history_column(area);
  return rect(col.x, area.y + 84 + (int)i * 52 - g_scroll, (int)col.width, 52);
}

static void when(char* out, size_t cap, uint64_t t) {
  time_t now = time(NULL);
  long long ago = (long long)now - (long long)t;
  if (ago < 60) {
    snprintf(out, cap, "just now");
  } else if (ago < 3600) {
    snprintf(out, cap, "%lld min ago", ago / 60);
  } else if (ago < 86400) {
    snprintf(out, cap, "%lld h ago", ago / 3600);
  } else {
    snprintf(out, cap, "%lld days ago", ago / 86400);
  }
}

static void draw_history(rect_t area) {
  if (!g_history_loaded) {
    load_history();
  }
  rect_t col = history_column(area);
  text(col.x, area.y + 52 - g_scroll, "History", 22, T->text);
  if (g_history.len) {
    button(history_clear(area), g_phone ? "Clear all" : "Clear browsing history",
           g_hover == HIT_CLEAR);
  }
  if (!g_history.len) {
    text(col.x, area.y + 110 - g_scroll, "Pages you visit will show here.",
         14, T->text_dim);
    g_content_h = 140;
    return;
  }
  rect_t card = rect(col.x, area.y + 84 - g_scroll, (int)col.width,
                     (int)g_history.len * 52);
  fill_round(card, 8, T->card);
  char title[512], url[2048], ago[32];
  for (size_t i = 0; i < g_history.len; ++i) {
    rect_t r = history_row(area, i);
    if (r.y + (int)r.height < area.y || r.y > area.y + (int)area.height) {
      continue;
    }
    if (g_hover == HIT_HISTORY + (int)i) {
      fill(r, T->hover);
    }
    copy_str(title, sizeof title, &g_history.ptr[i].title);
    copy_str(url, sizeof url, &g_history.ptr[i].url);
    when(ago, sizeof ago, g_history.ptr[i].last_visit);
    icon_centered(ICON(PAGE), rect(r.x + 12, r.y, 24, 52), 16, T->icon);
    int ago_w = text_width(ago, 12.5f);
    int text_w = (int)r.width - 64 - ago_w - 16;
    text_in(rect(r.x + 48, r.y + 6, text_w, 22), title[0] ? title : url, 14,
            T->text);
    text_in(rect(r.x + 48, r.y + 26, text_w, 20), url, 12, T->text_dim);
    text_in(rect(r.x + (int)r.width - ago_w - 16, r.y, ago_w + 2, 52), ago,
            12.5f, T->text_dim);
    if (i > 0) {
      fill(rect(r.x + 16, r.y, (int)r.width - 32, 1), T->page_bg);
    }
  }
  g_content_h = 84 + (int)g_history.len * 52 + 24;
}

/* --- tabs (phones) -------------------------------------------------------- */

#define TAB_ROW_H 64

int g_swipe_row = -1;
int g_swipe_dx;

static rect_t tabs_new(rect_t area) {
  rect_t col = history_column(area);
  return rect(col.x + (int)col.width - 120, area.y + 26 - g_scroll, 120, 40);
}

static rect_t tab_row(rect_t area, size_t i) {
  rect_t col = history_column(area);
  return rect(col.x, area.y + 84 + (int)i * TAB_ROW_H - g_scroll,
              (int)col.width, TAB_ROW_H);
}

static rect_t tab_row_close(rect_t row) {
  return rect(row.x + (int)row.width - 56, row.y + (TAB_ROW_H - 48) / 2, 48,
              48);
}

static void draw_tabs(rect_t area) {
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  size_t count = tabs.len < TAB_ROWS_MAX ? tabs.len : TAB_ROWS_MAX;
  rect_t col = history_column(area);
  char heading[32];
  snprintf(heading, sizeof heading, count == 1 ? "1 tab" : "%zu tabs", count);
  text(col.x, area.y + 54 - g_scroll, heading, 22, T->text);
  button(tabs_new(area), "New tab", g_hover == HIT_TABS_NEW);
  fill_round(rect(col.x, area.y + 84 - g_scroll, (int)col.width,
                  (int)count * TAB_ROW_H),
             8, T->card);
  char title[512], url[2048];
  clip(rect(col.x, area.y, (int)col.width, (int)area.height));
  for (size_t i = 0; i < count; ++i) {
    const tab_info_t* t = &tabs.ptr[i];
    rect_t r = tab_row(area, i);
    bool active = t->id == g_active;
    if ((int)i == g_swipe_row) {
      /* Slides with the finger, over a gap. */
      fill(r, T->page_bg);
      r.x += g_swipe_dx;
      fill(r, T->card);
    }
    if (g_hover == HIT_TAB_ROW + (int)i || active) {
      fill(r, T->hover);
    }
    if (active) {
      fill(rect(r.x, r.y + 12, 3, TAB_ROW_H - 24), T->accent);
    }
    copy_str(title, sizeof title, &t->title);
    copy_str(url, sizeof url, &t->url);
    if (!url[0] || strncmp(url, "about:", 6) == 0) {
      snprintf(url, sizeof url, "New tab");
    }
    icon_centered(t->loading ? ICON(RELOAD) : ICON(PAGE),
                  rect(r.x + 12, r.y, 24, TAB_ROW_H), 18,
                  t->loading ? T->accent : T->icon);
    int text_w = (int)r.width - 48 - 64;
    text_in(rect(r.x + 48, r.y + 10, text_w, 24), title[0] ? title : url, 15,
            T->text);
    text_in(rect(r.x + 48, r.y + 34, text_w, 20), url, 12.5f, T->text_dim);
    rect_t x = tab_row_close(r);
    if (g_hover == HIT_TAB_ROW_CLOSE + (int)i) {
      circle(x.x + 24, x.y + 24, 18, T->separator);
    }
    icon_centered(ICON(CLOSE), x, 18, T->icon);
    if (i > 0) {
      fill(rect(r.x + 16, r.y, (int)r.width - 32, 1), T->page_bg);
    }
  }
  unclip();
  g_content_h = 84 + (int)count * TAB_ROW_H + 24;
  umbra_browser_browser_list_tab_info_free(&tabs);
}

int tab_row_of(int code) {
  if (code >= HIT_TAB_ROW && code < HIT_TAB_ROW + TAB_ROWS_MAX) {
    return code - HIT_TAB_ROW;
  }
  if (code >= HIT_TAB_ROW_CLOSE && code < HIT_TAB_ROW_CLOSE + TAB_ROWS_MAX) {
    return code - HIT_TAB_ROW_CLOSE;
  }
  return -1;
}

static int tabs_hit(int x, int y) {
  if (inside(tabs_new(g_area), x, y)) {
    return HIT_TABS_NEW;
  }
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  int code = 0;
  for (size_t i = 0; i < tabs.len && i < TAB_ROWS_MAX; ++i) {
    rect_t r = tab_row(g_area, i);
    if (inside(r, x, y)) {
      code = inside(tab_row_close(r), x, y) ? HIT_TAB_ROW_CLOSE + (int)i
                                             : HIT_TAB_ROW + (int)i;
    }
  }
  umbra_browser_browser_list_tab_info_free(&tabs);
  return code;
}

static uint32_t tab_at(int index) {
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  uint32_t id = (size_t)index < tabs.len ? tabs.ptr[index].id : 0;
  umbra_browser_browser_list_tab_info_free(&tabs);
  return id;
}

void close_tab_row(int index) {
  uint32_t id = tab_at(index);
  if (id) {
    close_tab(id);
  }
}

/* --- panels: shared ------------------------------------------------------- */

void panel_open(panel_t panel) {
  g_panel = panel;
  g_scroll = 0;
  g_cleared = false;
  g_menu_open = false;
  if (g_focus != FOCUS_ADDRESS) {
    g_focus = FOCUS_PAGE;
  }
  if (panel == PANEL_HISTORY) {
    load_history();
  }
}

void panel_draw(rect_t area) {
  g_area = area;
  fill(area, T->page_bg);
  clip(area);
  if (g_panel == PANEL_SETTINGS) {
    draw_settings(area);
  } else if (g_panel == PANEL_TABS) {
    draw_tabs(area);
  } else {
    draw_history(area);
  }
  unclip();
}

void panel_wheel(float dy) {
  int max = g_content_h - (int)g_area.height;
  g_scroll += (int)dy;
  if (g_scroll > max) g_scroll = max;
  if (g_scroll < 0) g_scroll = 0;
}

int panels_hit(int x, int y) {
  if (g_menu_open) {
    rect_t m = menu_box();
    if (inside(m, x, y)) {
      for (int i = 0; i < MENU_COUNT; ++i) {
        if (inside(menu_item(m, i), x, y)) {
          return HIT_MENU_ITEM + i;
        }
      }
      for (int i = 0; i < 2 && g_phone; ++i) {
        if (inside(menu_icon(m, i), x, y)) {
          return HIT_MENU_ICON + i;
        }
      }
      return HIT_MENU_BG;
    }
    return 0;
  }
  if (!inside(g_area, x, y)) {
    return 0;
  }
  if (g_panel == PANEL_SETTINGS) {
    for (int i = 0; i < g_row_count; ++i) {
      const row_t* row = &g_rows[i];
      if (row->kind == ROW_CLEAR) {
        if (!g_cleared && inside(clear_button(row), x, y)) {
          return HIT_ROW + i;
        }
      } else if (row->kind != ROW_NOTE && inside(row->r, x, y)) {
        return HIT_ROW + i;
      }
    }
  } else if (g_panel == PANEL_TABS) {
    int code = tabs_hit(x, y);
    if (code) {
      return code;
    }
  } else if (g_panel == PANEL_HISTORY && g_history_loaded) {
    if (g_history.len && inside(history_clear(g_area), x, y)) {
      return HIT_CLEAR;
    }
    for (size_t i = 0; i < g_history.len; ++i) {
      if (inside(history_row(g_area, i), x, y)) {
        return HIT_HISTORY + (int)i;
      }
    }
  }
  return 1; /* the panel's background: not the page */
}

void panels_click(int code) {
  if (code >= HIT_MENU_ITEM && code < HIT_MENU_ITEM + MENU_COUNT) {
    g_menu_open = false;
    switch (code - HIT_MENU_ITEM) {
      case 0:
        new_tab();
        break;
      case 1:
        panel_open(PANEL_HISTORY);
        break;
      case 2:
        panel_open(PANEL_SETTINGS);
        break;
      case MENU_TOR:
        umbra_browser_browser_set_tor(!umbra_browser_browser_tor_enabled());
        break;
    }
    return;
  }
  if (code == HIT_MENU_BG) {
    g_menu_open = true; /* clicks on the menu's padding keep it open */
    return;
  }
  if (code == HIT_MENU_ICON) {
    g_menu_open = false;
    if (g_panel == PANEL_NONE) {
      go_forward();
    }
    return;
  }
  if (code == HIT_MENU_ICON + 1) {
    g_menu_open = false;
    reload_or_stop();
    return;
  }
  if (code == HIT_TABS_NEW) {
    new_tab();
    return;
  }
  if (code >= HIT_TAB_ROW && code < HIT_TAB_ROW + TAB_ROWS_MAX) {
    uint32_t id = tab_at(code - HIT_TAB_ROW);
    if (id) {
      switch_tab(id);
    }
    return;
  }
  if (code >= HIT_TAB_ROW_CLOSE && code < HIT_TAB_ROW_CLOSE + TAB_ROWS_MAX) {
    uint32_t id = tab_at(code - HIT_TAB_ROW_CLOSE);
    if (id) {
      close_tab(id);
    }
    return;
  }
  /* A click anywhere in a panel leaves a custom field it isn't on. */
  bool on_field = code >= HIT_ROW && code < HIT_HISTORY &&
                  g_rows[code - HIT_ROW].kind == ROW_FIELD;
  if (!on_field && (g_focus == FOCUS_CUSTOM_SEARCH ||
                    g_focus == FOCUS_CUSTOM_HOME)) {
    g_focus = FOCUS_PAGE;
  }
  if (code >= HIT_ROW && code < HIT_HISTORY) {
    click_settings_row(code - HIT_ROW);
  } else if (code >= HIT_HISTORY && code < HIT_CLEAR) {
    size_t i = (size_t)(code - HIT_HISTORY);
    if (g_history_loaded && i < g_history.len) {
      char url[2048];
      copy_str(url, sizeof url, &g_history.ptr[i].url);
      navigate_active(url);
    }
  } else if (code == HIT_CLEAR) {
    umbra_browser_browser_clear_history();
    load_history();
  }
}

bool panel_key(const keypress_t* k) {
  field_t* f = g_focus == FOCUS_CUSTOM_SEARCH ? &g_custom_search
               : g_focus == FOCUS_CUSTOM_HOME ? &g_custom_home
                                              : NULL;
  if (!f) {
    return false;
  }
  if (k->down && k->code == VK_ESCAPE) {
    g_focus = FOCUS_PAGE;
    return true;
  }
  bool enter;
  field_key(f, k, &enter);
  if (enter) {
    save_custom(g_focus == FOCUS_CUSTOM_SEARCH ? 0 : 1);
  }
  return true;
}

void panel_text(const app_string_t* s) {
  if (g_focus == FOCUS_CUSTOM_SEARCH) {
    field_text(&g_custom_search, s);
  } else if (g_focus == FOCUS_CUSTOM_HOME) {
    field_text(&g_custom_home, s);
  }
}
