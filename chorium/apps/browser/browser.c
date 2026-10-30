/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Sample Chorium app: a tabbed browser that looks like Chrome. It draws its
 * own title bar (tabs and window buttons), toolbar, menu, Settings and
 * History with `surface`, and leaves pages, history and settings storage to
 * chorium.wasm through the `browser` interface (interfaces/browser).
 * docker/build_wasm.sh composes the two into one browser.wasm.
 *
 *   Chorium.exe [browser.wasm] [url]
 *
 * Keys: Ctrl+T new tab, Ctrl+W close tab, Ctrl+Tab next tab, Ctrl+L or
 * Alt+D address field, Ctrl+H history, F5 reload, Alt+Left/Right back and
 * forward, Esc closes the menu or a page like Settings. In the address
 * field: Ctrl+A select all, Backspace/Delete clear a selection,
 * Ctrl+Backspace delete a word.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define TOOLBAR 40   /* toolbar height, CSS pixels */
#define TAB_MAX 240
#define TAB_MIN 48
#define TAB_RADIUS 8
#define STRIP_NO_TITLE_BAR 36 /* tab strip height under a system title bar */

#define VK_TAB 0x09
#define VK_ESCAPE 0x1B
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_F5 0x74

/* Hit codes: what is under a point (see hit()). panels.c uses 400+. */
enum {
  HIT_NONE = 0,
  HIT_TAB = 100,       /* + tab index */
  HIT_TAB_CLOSE = 200, /* + tab index */
  HIT_NEW_TAB = 300,
  HIT_BACK,
  HIT_FORWARD,
  HIT_RELOAD,
  HIT_ADDRESS,
  HIT_MENU,
  HIT_MINIMIZE,
  HIT_MAXIMIZE,
  HIT_CLOSE_WINDOW,
  HIT_PAGE,
  HIT_DRAG,
};

umbra_chorium_types_size_t g_size;
uint32_t g_active;
focus_t g_focus = FOCUS_PAGE;
panel_t g_panel = PANEL_NONE;
bool g_menu_open;
int g_hover;

static field_t g_address;
static bool g_has_title_bar;
static int g_strip;     /* tab strip height (= title bar height if ours) */
static int g_buttons_w; /* window buttons at the right of the strip */
static bool g_maximized;
static bool g_draw_pending;

/* --- layout --------------------------------------------------------------- */

static void update_layout(void) {
  umbra_chorium_surface_dimensions(&g_size);
  umbra_chorium_surface_title_bar_t bar;
  g_has_title_bar = umbra_chorium_surface_title_bar_layout(&bar);
  g_strip = g_has_title_bar ? (int)bar.height : STRIP_NO_TITLE_BAR;
  g_buttons_w = g_has_title_bar ? (int)bar.buttons_width : 0;
  g_maximized = g_has_title_bar && bar.maximized;
}

int chrome_top(void) { return g_strip + TOOLBAR; }

static int tab_top(void) { return g_maximized ? 2 : 6; }

static int tabs_room(void) {
  return (int)g_size.width - g_buttons_w - 8 - 8 - 36 - 48;
}

static int tab_width(size_t count) {
  int w = count ? tabs_room() / (int)count : TAB_MAX;
  return w > TAB_MAX ? TAB_MAX : w < TAB_MIN ? TAB_MIN : w;
}

static rect_t tab_rect(size_t i, size_t count) {
  int w = tab_width(count);
  return rect(8 + (int)i * w, tab_top(), w, g_strip - tab_top());
}

static rect_t tab_close_rect(rect_t tab) {
  return rect(tab.x + (int)tab.width - 28, tab.y + ((int)tab.height - 20) / 2,
              20, 20);
}

static rect_t new_tab_rect(size_t count) {
  int x = 8 + (int)count * tab_width(count) + 4;
  return rect(x, tab_top() + ((g_strip - tab_top()) - 28) / 2, 28, 28);
}

static rect_t window_button(int i) {
  return rect((int)g_size.width - g_buttons_w + i * (g_buttons_w / 3), 0,
              g_buttons_w / 3, g_strip);
}

static rect_t toolbar_button(int i) {
  return rect(6 + i * 36, g_strip + 3, 34, 34);
}

static rect_t menu_button(void) {
  return rect((int)g_size.width - 42, g_strip + 3, 34, 34);
}

static rect_t omnibox(void) {
  int x = 6 + 3 * 36 + 6;
  return rect(x, g_strip + 4, (int)g_size.width - 50 - x, TOOLBAR - 8);
}

static rect_t page_area(void) {
  return rect(0, chrome_top(), (int)g_size.width, (int)g_size.height - chrome_top());
}

static umbra_chorium_types_size_t page_size(void) {
  rect_t a = page_area();
  umbra_chorium_types_size_t s = {a.width ? a.width : 1,
                                  a.height ? a.height : 1};
  return s;
}

/* Empty title-bar space drags the window: above the tabs and after "+". */
static void update_drag_areas(size_t count) {
  if (!g_has_title_bar) {
    return;
  }
  rect_t plus = new_tab_rect(count);
  int right = (int)g_size.width - g_buttons_w;
  rect_t areas[2] = {
      rect(0, 0, right, tab_top()),
      rect(plus.x + (int)plus.width + 4, 0,
           right - (plus.x + (int)plus.width + 4), g_strip),
  };
  umbra_chorium_surface_list_rect_t list = {areas, 2};
  umbra_chorium_surface_set_drag_areas(&list);
}

/* --- tabs ----------------------------------------------------------------- */

void request_draw(void) { g_draw_pending = true; }

static void open_tab(const char* text) {
  app_string_t t = str(text);
  umbra_chorium_types_size_t size = page_size();
  uint32_t id = umbra_browser_browser_open_tab(&t, &size);
  if (id) {
    g_active = id;
  }
  g_panel = PANEL_NONE;
}

void navigate_active(const char* text) {
  app_string_t t = str(text);
  umbra_browser_browser_navigate(g_active, &t);
  g_panel = PANEL_NONE;
  g_focus = FOCUS_PAGE;
}

static void focus_new_tab_address(void) {
  field_set(&g_address, "", false);
  g_focus = FOCUS_ADDRESS;
}

void new_tab(void) {
  open_tab("");
  focus_new_tab_address();
}

/* Makes another tab active after |closed| went away; opens one if none. */
static void pick_after_close(uint32_t closed) {
  if (g_active != closed) {
    return;
  }
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  g_active = tabs.len ? tabs.ptr[tabs.len - 1].id : 0;
  umbra_browser_browser_list_tab_info_free(&tabs);
  if (!g_active) {
    open_tab("");
    focus_new_tab_address();
  }
}

static void close_tab(uint32_t id) {
  umbra_browser_browser_close_tab(id);
  pick_after_close(id);
}

static void switch_tab(uint32_t id) {
  g_active = id;
  g_panel = PANEL_NONE;
  if (g_focus == FOCUS_ADDRESS) {
    g_focus = FOCUS_PAGE;
  }
}

static void next_tab(int step) {
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  for (size_t i = 0; i < tabs.len; ++i) {
    if (tabs.ptr[i].id == g_active) {
      switch_tab(tabs.ptr[(i + tabs.len + (size_t)step) % tabs.len].id);
      break;
    }
  }
  umbra_browser_browser_list_tab_info_free(&tabs);
}

static bool active_info(tab_info_t* info) {
  return umbra_browser_browser_info(g_active, info);
}

static void focus_address(void) {
  tab_info_t info;
  char url[2048] = "";
  if (active_info(&info)) {
    copy_str(url, sizeof url, &info.url);
    umbra_browser_browser_tab_info_free(&info);
  }
  field_set(&g_address, g_panel == PANEL_NONE ? url : "", true);
  g_focus = FOCUS_ADDRESS;
}

/* --- drawing -------------------------------------------------------------- */

static void draw_tab(const tab_info_t* t, size_t i, size_t count,
                     bool active) {
  rect_t r = tab_rect(i, count);
  bool hovered = g_hover == HIT_TAB + (int)i || g_hover == HIT_TAB_CLOSE + (int)i;
  if (active) {
    /* Rounded top, joined to the toolbar with Chrome's curved feet. */
    fill_round(rect(r.x, r.y, (int)r.width, (int)r.height + TAB_RADIUS),
               TAB_RADIUS, T->toolbar);
    int foot = TAB_RADIUS;
    fill(rect(r.x - foot, g_strip - foot, foot, foot), T->toolbar);
    circle(r.x - foot, g_strip - foot, foot, T->frame);
    fill(rect(r.x + (int)r.width, g_strip - foot, foot, foot), T->toolbar);
    circle(r.x + (int)r.width + foot, g_strip - foot, foot, T->frame);
  } else if (hovered) {
    fill_round(rect(r.x + 2, r.y + 2, (int)r.width - 4, (int)r.height - 6),
               TAB_RADIUS, T->tab_hover);
  }

  rect_t content = rect(r.x, r.y, (int)r.width, (int)r.height - 2);
  bool show_close = active || hovered || r.width >= 100;
  int title_x = r.x + 12;
  if (r.width >= 72) {
    icon_centered(t->loading ? ICON(RELOAD) : ICON(PAGE),
                  rect(r.x + 10, content.y, 18, (int)content.height), 16,
                  t->loading ? T->accent : T->icon);
    title_x = r.x + 34;
  }
  char title[512];
  if (t->title.len) {
    copy_str(title, sizeof title, &t->title);
  } else if (t->url.len) {
    copy_str(title, sizeof title, &t->url);
  } else {
    snprintf(title, sizeof title, "New Tab");
  }
  int title_w = r.x + (int)r.width - title_x - (show_close ? 32 : 10);
  text_in(rect(title_x, content.y, title_w, (int)content.height), title,
          12.5f, active ? T->text : T->text_dim);
  if (show_close) {
    rect_t x = tab_close_rect(r);
    if (g_hover == HIT_TAB_CLOSE + (int)i) {
      circle(x.x + 10, x.y + 10, 10, active ? T->hover : T->toolbar);
    }
    icon_centered(ICON(CLOSE), x, 10, T->icon);
  }
}

static void draw_strip(const tab_list_t* tabs) {
  fill(rect(0, 0, (int)g_size.width, g_strip), T->frame);
  size_t active = tabs->len;
  for (size_t i = 0; i < tabs->len; ++i) {
    if (tabs->ptr[i].id == g_active) {
      active = i;
      continue;
    }
    draw_tab(&tabs->ptr[i], i, tabs->len, false);
  }
  /* Dividers between inactive tabs that aren't highlighted. */
  for (size_t i = 0; i + 1 < tabs->len; ++i) {
    bool quiet = i != active && i + 1 != active &&
                 g_hover != HIT_TAB + (int)i && g_hover != HIT_TAB + (int)i + 1;
    if (quiet) {
      rect_t r = tab_rect(i, tabs->len);
      fill(rect(r.x + (int)r.width - 1, r.y + 9, 1, (int)r.height - 18),
           T->separator);
    }
  }
  if (active < tabs->len) {
    draw_tab(&tabs->ptr[active], active, tabs->len, true);
  }

  rect_t plus = new_tab_rect(tabs->len);
  if (g_hover == HIT_NEW_TAB) {
    circle(plus.x + 14, plus.y + 14, 14, T->tab_hover);
  }
  icon_centered(ICON(ADD), plus, 14, T->icon);

  if (g_has_title_bar) {
    static const icon_t icons[3] = {ICON(MINIMIZE), ICON(MAXIMIZE),
                                    ICON(CLOSE_WINDOW)};
    for (int i = 0; i < 3; ++i) {
      rect_t b = window_button(i);
      bool hover = g_hover == HIT_MINIMIZE + i;
      icon_t which = i == 1 && g_maximized ? ICON(RESTORE) : icons[i];
      uint32_t color = T->text;
      if (hover && i == 2) {
        fill(b, T->close_hover);
        color = 0xFFFFFFFF;
      } else if (hover) {
        fill(b, T->tab_hover);
      }
      icon_centered(which, b, 10, color);
    }
  }
}

static void toolbar_icon(int hit, rect_t r, icon_t which, bool enabled) {
  if (enabled && g_hover == hit) {
    circle(r.x + 17, r.y + 17, 17, T->hover);
  }
  icon_centered(which, r, 16, enabled ? T->icon : T->icon_off);
}

static const char* search_name(void) {
  char search[512] = "";
  get_setting("search", search, sizeof search);
  if (!search[0] || strstr(search, "duckduckgo")) return "DuckDuckGo";
  if (strstr(search, "startpage")) return "Startpage";
  if (strstr(search, "brave")) return "Brave Search";
  return "the web";
}

/* "https://example.com/" -> "example.com", the way Chrome shows it. */
static void display_url(char* out, size_t cap, const char* url) {
  const char* s = url;
  if (strncmp(s, "https://", 8) == 0) {
    s += 8;
  }
  snprintf(out, cap, "%s", s);
  size_t n = strlen(out);
  if (n && out[n - 1] == '/' && !strchr(out, '?') &&
      strchr(out, '/') == out + n - 1) {
    out[n - 1] = '\0';
  }
}

static void draw_toolbar(const tab_info_t* active) {
  fill(rect(0, g_strip, (int)g_size.width, TOOLBAR), T->toolbar);
  bool panel = g_panel != PANEL_NONE;
  toolbar_icon(HIT_BACK, toolbar_button(0), ICON(BACK),
               active && (active->can_go_back || panel));
  toolbar_icon(HIT_FORWARD, toolbar_button(1), ICON(FORWARD),
               active && active->can_go_forward && !panel);
  toolbar_icon(HIT_RELOAD, toolbar_button(2),
               active && active->loading ? ICON(STOP) : ICON(RELOAD),
               !panel);

  rect_t box = omnibox();
  bool editing = g_focus == FOCUS_ADDRESS;
  if (editing) {
    fill_round(box, (float)box.height / 2, T->accent);
    fill_round(rect(box.x + 2, box.y + 2, (int)box.width - 4,
                    (int)box.height - 4),
               (float)box.height / 2 - 2, T->toolbar);
  } else {
    fill_round(box, (float)box.height / 2,
               g_hover == HIT_ADDRESS ? T->hover : T->omnibox);
  }

  char url[2048] = "", shown[2048] = "", error[128] = "";
  if (active) {
    copy_str(url, sizeof url, &active->url);
    if (active->error.is_some) {
      copy_str(error, sizeof error, &active->error.val);
    }
  }
  int tor = umbra_browser_browser_tor_progress();
  icon_t which = editing ? ICON(SEARCH)
                 : panel ? ICON(SETTINGS)
                 : strncmp(url, "https://", 8) == 0 && !error[0] ? ICON(LOCK)
                                                                 : ICON(INFO);
  if (panel && g_panel == PANEL_HISTORY) {
    which = ICON(HISTORY);
  }
  icon_centered(which, rect(box.x + 8, box.y, 24, (int)box.height), 15,
                error[0] && !editing ? T->error : T->icon);
  rect_t text_box = rect(box.x + 38, box.y, (int)box.width - 50,
                         (int)box.height);
  char placeholder[128];
  snprintf(placeholder, sizeof placeholder, "Search %s or type a URL",
           search_name());
  if (editing) {
    field_draw(&g_address, text_box, text_box.x, 14, true, placeholder);
  } else if (panel) {
    text_in(text_box,
            g_panel == PANEL_SETTINGS ? "chorium://settings"
                                      : "chorium://history",
            14, T->text);
  } else if (tor >= 0 && tor < 100) {
    char line[64];
    snprintf(line, sizeof line, "Connecting to Tor... %d%%", tor);
    text_in(text_box, line, 14, T->text_dim);
  } else if (tor < 0) {
    text_in(text_box, "Tor failed to start: nothing will load", 14,
            T->error);
  } else if (error[0]) {
    char line[2300];
    display_url(shown, sizeof shown, url);
    snprintf(line, sizeof line, "%s  -  %s", shown, error);
    text_in(text_box, line, 14, T->error);
  } else if (url[0] && strncmp(url, "about:", 6) != 0) {
    display_url(shown, sizeof shown, url);
    text_in(text_box, shown, 14, T->text);
  } else {
    text_in(text_box, placeholder, 14, T->text_dim);
  }

  toolbar_icon(HIT_MENU, menu_button(), ICON(MENU), true);
  if (g_menu_open) {
    circle(menu_button().x + 17, menu_button().y + 17, 17, T->hover);
    icon_centered(ICON(MENU), menu_button(), 16, T->icon);
  }
  /* Hairline between toolbar and page. */
  fill(rect(0, chrome_top() - 1, (int)g_size.width, 1), T->frame);
}

static void show_title(const tab_info_t* active) {
  char title[512] = "Chorium";
  if (g_panel == PANEL_SETTINGS) {
    snprintf(title, sizeof title, "Settings");
  } else if (g_panel == PANEL_HISTORY) {
    snprintf(title, sizeof title, "History");
  } else if (active && active->title.len) {
    copy_str(title, sizeof title, &active->title);
  }
  app_string_t t = str(title);
  umbra_chorium_surface_set_title(&t);
}

static void draw(void) {
  g_draw_pending = false;
  update_layout();
  theme_update();
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  const tab_info_t* active = NULL;
  for (size_t i = 0; i < tabs.len; ++i) {
    if (tabs.ptr[i].id == g_active) {
      active = &tabs.ptr[i];
    }
  }
  update_drag_areas(tabs.len);

  umbra_chorium_surface_clear(T->toolbar);
  draw_strip(&tabs);
  draw_toolbar(active);

  rect_t area = page_area();
  if (g_panel != PANEL_NONE) {
    panel_draw(area);
  } else if (active && area.width && area.height) {
    umbra_chorium_types_size_t size = page_size();
    umbra_browser_browser_resize(g_active, &size);
    umbra_browser_browser_draw(g_active, &area);
  }
  if (g_menu_open) {
    menu_draw();
  }
  umbra_chorium_surface_present();
  show_title(active);
  umbra_browser_browser_list_tab_info_free(&tabs);
}

/* --- hit testing ---------------------------------------------------------- */

static int hit(int x, int y) {
  if (g_menu_open) {
    int code = panels_hit(x, y);
    if (code) {
      return code;
    }
  }
  if (y < g_strip) {
    if (g_has_title_bar && x >= (int)g_size.width - g_buttons_w) {
      return HIT_MINIMIZE + (x - ((int)g_size.width - g_buttons_w)) /
                                (g_buttons_w / 3);
    }
    tab_list_t tabs;
    umbra_browser_browser_tabs(&tabs);
    int code = HIT_DRAG;
    for (size_t i = 0; i < tabs.len; ++i) {
      rect_t r = tab_rect(i, tabs.len);
      if (inside(r, x, y)) {
        code = inside(tab_close_rect(r), x, y) &&
                       (tabs.ptr[i].id == g_active || r.width >= 100 ||
                        g_hover == HIT_TAB + (int)i ||
                        g_hover == HIT_TAB_CLOSE + (int)i)
                   ? HIT_TAB_CLOSE + (int)i
                   : HIT_TAB + (int)i;
      }
    }
    if (inside(new_tab_rect(tabs.len), x, y)) {
      code = HIT_NEW_TAB;
    }
    umbra_browser_browser_list_tab_info_free(&tabs);
    return code;
  }
  if (y < chrome_top()) {
    for (int i = 0; i < 3; ++i) {
      if (inside(toolbar_button(i), x, y)) {
        return HIT_BACK + i;
      }
    }
    if (inside(menu_button(), x, y)) return HIT_MENU;
    if (inside(omnibox(), x, y)) return HIT_ADDRESS;
    return HIT_NONE;
  }
  if (g_panel != PANEL_NONE) {
    return panels_hit(x, y);
  }
  return HIT_PAGE;
}

static uint32_t tab_id_at(int index) {
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  uint32_t id = index >= 0 && (size_t)index < tabs.len ? tabs.ptr[index].id : 0;
  umbra_browser_browser_list_tab_info_free(&tabs);
  return id;
}

static void click(int code, const pointer_t* p) {
  bool menu_was_open = g_menu_open;
  if (code < 400 || code >= 500) {
    g_menu_open = false; /* any click outside the menu closes it */
  }
  if (menu_was_open && code == HIT_MENU) {
    return;
  }
  if (code != HIT_ADDRESS && g_focus == FOCUS_ADDRESS) {
    g_focus = FOCUS_PAGE;
  }
  if (code >= HIT_TAB && code < HIT_TAB_CLOSE) {
    uint32_t id = tab_id_at(code - HIT_TAB);
    if (p->button == UMBRA_CHORIUM_TYPES_MOUSE_BUTTON_MIDDLE) {
      close_tab(id);
    } else if (id) {
      switch_tab(id);
    }
    return;
  }
  if (code >= HIT_TAB_CLOSE && code < HIT_NEW_TAB) {
    close_tab(tab_id_at(code - HIT_TAB_CLOSE));
    return;
  }
  switch (code) {
    case HIT_NEW_TAB:
      open_tab("");
      focus_new_tab_address();
      break;
    case HIT_BACK:
      if (g_panel != PANEL_NONE) {
        g_panel = PANEL_NONE;
      } else {
        umbra_browser_browser_back(g_active);
      }
      break;
    case HIT_FORWARD:
      umbra_browser_browser_forward(g_active);
      break;
    case HIT_RELOAD: {
      tab_info_t info;
      if (g_panel == PANEL_NONE && active_info(&info)) {
        if (info.loading) {
          umbra_browser_browser_stop(g_active);
        } else {
          umbra_browser_browser_reload(g_active);
        }
        umbra_browser_browser_tab_info_free(&info);
      }
      break;
    }
    case HIT_ADDRESS:
      if (g_focus != FOCUS_ADDRESS) {
        focus_address();
      } else {
        g_address.selected = false; /* second click: caret at the end */
      }
      break;
    case HIT_MENU:
      g_menu_open = !menu_was_open;
      break;
    case HIT_PAGE:
      g_focus = FOCUS_PAGE;
      break;
    default:
      if (code >= 400) {
        panels_click(code);
      }
      break;
  }
}

/* --- keys ----------------------------------------------------------------- */

/* Browser shortcuts and text fields. True if the page shouldn't see it. */
static bool on_key(const keypress_t* k) {
  bool ctrl = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_CTRL;
  bool alt = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_ALT;
  bool shift = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_SHIFT;
  if (k->down) {
    if (ctrl && k->code == 'T') {
      open_tab("");
      focus_new_tab_address();
      return true;
    }
    if (ctrl && k->code == 'W') {
      close_tab(g_active);
      return true;
    }
    if (ctrl && k->code == VK_TAB) {
      next_tab(shift ? -1 : 1);
      return true;
    }
    if ((ctrl && k->code == 'L') || (alt && k->code == 'D')) {
      focus_address(); /* an open panel stays until a URL is entered */
      return true;
    }
    if (ctrl && k->code == 'H') {
      panel_open(PANEL_HISTORY);
      return true;
    }
    if (k->code == VK_F5) {
      umbra_browser_browser_reload(g_active);
      return true;
    }
    if (alt && k->code == VK_LEFT) {
      umbra_browser_browser_back(g_active);
      return true;
    }
    if (alt && k->code == VK_RIGHT) {
      umbra_browser_browser_forward(g_active);
      return true;
    }
    if (k->code == VK_ESCAPE && g_menu_open) {
      g_menu_open = false;
      return true;
    }
  }
  if (g_focus == FOCUS_ADDRESS) {
    if (k->down && k->code == VK_ESCAPE) {
      g_focus = FOCUS_PAGE;
      return true;
    }
    bool enter;
    field_key(&g_address, k, &enter);
    if (enter) {
      navigate_active(g_address.buf);
    }
    return true;
  }
  if (g_panel != PANEL_NONE) {
    if (k->down && k->code == VK_ESCAPE && g_focus == FOCUS_PAGE) {
      g_panel = PANEL_NONE;
      return true;
    }
    panel_key(k);
    return true;
  }
  return false;
}

/* --- exports -------------------------------------------------------------- */

void exports_app_start(app_list_string_t* args) {
  /* Empty = chorium.wasm's "home" setting (DuckDuckGo unless changed). */
  char first[2048] = "";
  if (args->len > 1) {
    copy_str(first, sizeof first, &args->ptr[1]);
  }
  app_list_string_free(args);
  update_layout();
  open_tab(first);
  app_string_t msg = str("browser sample started");
  umbra_chorium_log_write(UMBRA_CHORIUM_LOG_LEVEL_INFO, &msg);
  draw();
}

void exports_app_on_input(app_input_t* event) {
  bool to_page = false;
  int x = 0, y = -1;
  switch (event->tag) {
    case UMBRA_CHORIUM_TYPES_INPUT_POINTER_MOVE: {
      x = event->val.pointer_move.x;
      y = event->val.pointer_move.y;
      int now = hit(x, y);
      if (now != g_hover) {
        g_hover = now;
        request_draw();
      }
      to_page = now == HIT_PAGE;
      break;
    }
    case UMBRA_CHORIUM_TYPES_INPUT_POINTER_DOWN: {
      x = event->val.pointer_down.x;
      y = event->val.pointer_down.y;
      int code = hit(x, y);
      click(code, &event->val.pointer_down);
      to_page = code == HIT_PAGE;
      request_draw();
      break;
    }
    case UMBRA_CHORIUM_TYPES_INPUT_POINTER_UP:
      x = event->val.pointer_up.x;
      y = event->val.pointer_up.y;
      to_page = hit(x, y) == HIT_PAGE;
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_POINTER_LEAVE:
      if (g_hover) {
        g_hover = HIT_NONE;
        request_draw();
      }
      to_page = true;
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_WHEEL:
      x = event->val.wheel.x;
      y = event->val.wheel.y;
      if (y >= chrome_top() && g_panel != PANEL_NONE) {
        panel_wheel(event->val.wheel.dy);
        request_draw();
      } else {
        to_page = y >= chrome_top();
      }
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_KEY:
      if (on_key(&event->val.key)) {
        request_draw();
      } else {
        to_page = g_focus == FOCUS_PAGE;
      }
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_TEXT:
      if (g_focus == FOCUS_ADDRESS) {
        field_text(&g_address, &event->val.text);
        request_draw();
      } else if (g_panel != PANEL_NONE) {
        panel_text(&event->val.text);
        request_draw();
      } else {
        to_page = true;
      }
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_FOCUS:
      to_page = true;
      break;
    case UMBRA_CHORIUM_TYPES_INPUT_BACK:
      if (g_panel != PANEL_NONE) {
        g_panel = PANEL_NONE;
        request_draw();
      } else {
        umbra_browser_browser_back(g_active);
      }
      break;
  }
  if (to_page && g_panel == PANEL_NONE) {
    switch (event->tag) {
      case UMBRA_CHORIUM_TYPES_INPUT_POINTER_MOVE:
      case UMBRA_CHORIUM_TYPES_INPUT_POINTER_DOWN:
      case UMBRA_CHORIUM_TYPES_INPUT_POINTER_UP:
        event->val.pointer_move.y -= chrome_top();
        break;
      case UMBRA_CHORIUM_TYPES_INPUT_WHEEL:
        event->val.wheel.y -= chrome_top();
        break;
    }
    umbra_browser_browser_send_input(g_active, event);
  }
  app_input_free(event);
  if (g_draw_pending) {
    draw();
  }
}

void exports_app_on_page_event(uint32_t page_id, app_page_event_t* event) {
  umbra_browser_browser_list_browser_event_t events;
  umbra_browser_browser_handle_page_event(page_id, event, &events);
  app_page_event_free(event);
  for (size_t i = 0; i < events.len; ++i) {
    umbra_browser_browser_browser_event_t* e = &events.ptr[i];
    switch (e->tag) {
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CHANGED:
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CRASHED:
        request_draw(); /* titles and spinners show for every tab */
        break;
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_OPENED:
        switch_tab(e->val.opened.id); /* follow links into new tabs */
        request_draw();
        break;
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CLOSED:
        pick_after_close(e->val.closed);
        request_draw();
        break;
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_FRAME:
        if (e->val.frame == g_active && g_panel == PANEL_NONE) {
          request_draw();
        }
        break;
      case UMBRA_BROWSER_BROWSER_BROWSER_EVENT_DIALOG:
        /* No dialog UI yet: accept, with the page's default prompt text. */
        umbra_browser_browser_dialog_reply(e->val.dialog.f0,
                                           e->val.dialog.f1.id, true, NULL);
        break;
    }
  }
  umbra_browser_browser_list_browser_event_free(&events);
  if (g_draw_pending) {
    draw();
  }
}

void exports_app_on_draw(void) { draw(); }
void exports_app_on_pause(void) {}
void exports_app_on_resume(void) { draw(); }

void exports_app_on_stop(void) {
  tab_list_t tabs;
  umbra_browser_browser_tabs(&tabs);
  for (size_t i = 0; i < tabs.len; ++i) {
    umbra_browser_browser_close_tab(tabs.ptr[i].id);
  }
  umbra_browser_browser_list_tab_info_free(&tabs);
}
