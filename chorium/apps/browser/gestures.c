/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Chrome's page gestures, drawn by the app: pull to refresh and swipe back or
 * forward (Android reports them as overscroll events), and the menu for a
 * long press or right click on a link, an image or selected text.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* --- pull to refresh, swipe back/forward ----------------------------------- */

#define PULL_TO_ACT 80 /* CSS pixels pulled before release acts */

static struct {
  bool active;
  umbra_chorium_types_overscroll_kind_t kind;
  float pulled; /* along the gesture: down for refresh, away from the edge */
} g_over;

static bool armed(void) { return g_over.pulled >= PULL_TO_ACT; }

void overscroll_event(const umbra_browser_browser_overscroll_t* o) {
  switch (o->phase) {
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_PHASE_START:
      g_over.active = true;
      g_over.kind = o->kind;
      g_over.pulled = 0;
      break;
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_PHASE_PULL:
      if (!g_over.active) {
        return;
      }
      g_over.pulled +=
          g_over.kind == UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_REFRESH ? o->dy
          : g_over.kind == UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_BACK  ? o->dx
                                                                     : -o->dx;
      if (g_over.pulled < 0) {
        g_over.pulled = 0;
      }
      break;
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_PHASE_RELEASE:
      if (g_over.active && armed()) {
        switch (g_over.kind) {
          case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_REFRESH:
            umbra_browser_browser_reload(g_active);
            break;
          case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_BACK:
            umbra_browser_browser_back(g_active);
            break;
          case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_FORWARD:
            umbra_browser_browser_forward(g_active);
            break;
        }
      }
      g_over.active = false;
      break;
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_PHASE_CANCEL:
      g_over.active = false;
      break;
  }
  request_draw();
}

/* A round button with |which| in it, the way Chrome draws these. */
static void bubble(int cx, int cy, icon_t which) {
  circle(cx, cy + 1, 21, T->shadow);
  circle(cx, cy, 20, T->card);
  icon_centered(which, rect(cx - 20, cy - 20, 40, 40), 22,
                armed() ? T->accent : T->icon);
}

void overscroll_draw(rect_t page) {
  if (!g_over.active) {
    return;
  }
  float travel = g_over.pulled < 180 ? g_over.pulled : 180;
  switch (g_over.kind) {
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_REFRESH:
      bubble(page.x + (int)page.width / 2, page.y - 20 + (int)(travel * 0.6f),
             ICON(RELOAD));
      break;
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_BACK:
      bubble(page.x - 20 + (int)(travel * 0.5f),
             page.y + (int)page.height / 2, ICON(BACK));
      break;
    case UMBRA_CHORIUM_TYPES_OVERSCROLL_KIND_FORWARD:
      bubble(page.x + (int)page.width + 20 - (int)(travel * 0.5f),
             page.y + (int)page.height / 2, ICON(FORWARD));
      break;
  }
}

/* --- the long-press / right-click menu ------------------------------------- */

typedef enum {
  EDIT_CUT,
  EDIT_COPY,
  EDIT_PASTE,
  EDIT_SELECT_ALL,
  OPEN_LINK,
  COPY_LINK,
  COPY_LINK_TEXT,
  SAVE_LINK,
  OPEN_IMAGE,
  COPY_IMAGE,
  COPY_IMAGE_ADDRESS,
  SAVE_IMAGE,
  COPY_SELECTION,
  SEARCH_SELECTION,
} action_t;

#define CTX_MAX 10 /* hit codes HIT_CTX_ITEM .. HIT_CTX_CARD - 1 */

static struct {
  bool open;
  int x, y;           /* where it was asked for, surface pixels */
  int page_x, page_y; /* the same, page pixels (Copy image) */
  char link[2048], link_text[512], image[2048], selection[2048];
  int count;
  action_t actions[CTX_MAX];
  char search_label[96];
} g_ctx;

bool context_menu_is_open(void) { return g_ctx.open; }
void context_menu_close(void) { g_ctx.open = false; }

static void add(action_t a) {
  if (g_ctx.count < CTX_MAX) {
    g_ctx.actions[g_ctx.count++] = a;
  }
}

/* "Search for “…”" with at most ~28 bytes of the selection, cut between
 * UTF-8 characters. */
static void make_search_label(void) {
  char quoted[40];
  size_t n = strlen(g_ctx.selection);
  if (n > 28) {
    n = 28;
    while (n > 0 && (g_ctx.selection[n] & 0xC0) == 0x80) {
      --n;
    }
  }
  snprintf(quoted, sizeof quoted, "%.*s%s", (int)n, g_ctx.selection,
           n < strlen(g_ctx.selection) ? "..." : "");
  snprintf(g_ctx.search_label, sizeof g_ctx.search_label,
           "Search the web for \"%s\"", quoted);
}

void context_menu_open(const umbra_browser_browser_context_menu_t* m,
                       rect_t page) {
  copy_str(g_ctx.link, sizeof g_ctx.link, &m->link_url);
  copy_str(g_ctx.link_text, sizeof g_ctx.link_text, &m->link_text);
  copy_str(g_ctx.image, sizeof g_ctx.image, &m->image_url);
  copy_str(g_ctx.selection, sizeof g_ctx.selection, &m->selection);
  g_ctx.x = page.x + m->x;
  g_ctx.y = page.y + m->y;
  g_ctx.page_x = m->x;
  g_ctx.page_y = m->y;
  g_ctx.count = 0;
  if (m->editable) {
    /* A text field: only what it can do right now. */
    if (m->edit & UMBRA_CHORIUM_TYPES_EDIT_FLAGS_CUT) add(EDIT_CUT);
    if (m->edit & UMBRA_CHORIUM_TYPES_EDIT_FLAGS_COPY) add(EDIT_COPY);
    if (m->edit & UMBRA_CHORIUM_TYPES_EDIT_FLAGS_PASTE) add(EDIT_PASTE);
    if (m->edit & UMBRA_CHORIUM_TYPES_EDIT_FLAGS_SELECT_ALL) {
      add(EDIT_SELECT_ALL);
    }
  }
  if (g_ctx.link[0]) {
    add(OPEN_LINK);
    add(COPY_LINK);
    if (g_ctx.link_text[0]) {
      add(COPY_LINK_TEXT);
    }
    add(SAVE_LINK);
  }
  if (g_ctx.image[0]) {
    add(OPEN_IMAGE);
    add(COPY_IMAGE);
    add(COPY_IMAGE_ADDRESS);
    add(SAVE_IMAGE);
  }
  if (g_ctx.selection[0] && !g_ctx.link[0] && !m->editable) {
    add(COPY_SELECTION);
    add(SEARCH_SELECTION);
    make_search_label();
  }
  g_ctx.open = g_ctx.count > 0;
  g_menu_open = false;
  request_draw();
}

static const char* label(action_t a) {
  switch (a) {
    case EDIT_CUT: return "Cut";
    case EDIT_COPY: return "Copy";
    case EDIT_PASTE: return "Paste";
    case EDIT_SELECT_ALL: return "Select all";
    case OPEN_LINK: return "Open in new tab";
    case COPY_LINK: return "Copy link address";
    case COPY_LINK_TEXT: return "Copy link text";
    case SAVE_LINK: return "Save link as file";
    case OPEN_IMAGE: return "Open image in new tab";
    case COPY_IMAGE: return "Copy image";
    case COPY_IMAGE_ADDRESS: return "Copy image address";
    case SAVE_IMAGE: return "Save image";
    case COPY_SELECTION: return "Copy";
    case SEARCH_SELECTION: return g_ctx.search_label;
  }
  return "";
}

/* Phones: a card in the middle with the link as its title (as Chrome on
 * Android). Desktop: a small menu at the pointer. */
#define CTX_ITEM_H (g_phone ? 48 : 32)
#define CTX_TITLE_H (g_phone ? 52 : 0)

static rect_t card(void) {
  int w = g_phone ? ((int)g_size.width - 32 < 360 ? (int)g_size.width - 32
                                                  : 360)
                  : 240;
  int h = CTX_TITLE_H + g_ctx.count * CTX_ITEM_H + 16;
  if (g_phone) {
    return rect(((int)g_size.width - w) / 2, ((int)g_size.height - h) / 2, w,
                h);
  }
  int x = g_ctx.x + w > (int)g_size.width ? (int)g_size.width - w - 4
                                           : g_ctx.x;
  int y = g_ctx.y + h > (int)g_size.height ? g_ctx.y - h : g_ctx.y;
  return rect(x < 0 ? 0 : x, y < 0 ? 0 : y, w, h);
}

static rect_t item(rect_t c, int i) {
  return rect(c.x, c.y + 8 + CTX_TITLE_H + i * CTX_ITEM_H, (int)c.width,
              CTX_ITEM_H);
}

void context_menu_draw(void) {
  if (!g_ctx.open) {
    return;
  }
  if (g_phone) {
    fill(rect(0, 0, (int)g_size.width, (int)g_size.height), 0x66000000);
  }
  rect_t c = card();
  fill_round(rect(c.x - 1, c.y + 1, (int)c.width + 2, (int)c.height + 3), 9,
             T->shadow);
  fill_round(c, 8, T->card);
  if (g_phone) {
    const char* title = g_ctx.link[0]      ? g_ctx.link
                        : g_ctx.image[0]     ? g_ctx.image
                        : g_ctx.selection[0] ? g_ctx.selection
                                             : "Text";
    text_in(rect(c.x + 20, c.y + 8, (int)c.width - 40, CTX_TITLE_H - 8),
            title, 13, T->text_dim);
    fill(rect(c.x, c.y + CTX_TITLE_H + 4, (int)c.width, 1), T->separator);
  }
  for (int i = 0; i < g_ctx.count; ++i) {
    rect_t r = item(c, i);
    if (g_hover == HIT_CTX_ITEM + i) {
      fill(r, T->hover);
    }
    text_in(rect(r.x + (g_phone ? 20 : 16), r.y, (int)r.width - 32,
                 (int)r.height),
            label(g_ctx.actions[i]), g_phone ? 15 : 13.5f, T->text);
  }
}

int context_menu_hit(int x, int y) {
  rect_t c = card();
  if (!inside(c, x, y)) {
    return HIT_CTX_DISMISS;
  }
  for (int i = 0; i < g_ctx.count; ++i) {
    if (inside(item(c, i), x, y)) {
      return HIT_CTX_ITEM + i;
    }
  }
  return HIT_CTX_CARD;
}

static void copy(const char* s) {
  app_string_t t = str(s);
  umbra_chorium_surface_copy_text(&t);
}

void context_menu_click(int code) {
  if (code == HIT_CTX_CARD) {
    return; /* the card's padding or title: stay open */
  }
  g_ctx.open = false;
  if (code < HIT_CTX_ITEM || code >= HIT_CTX_ITEM + g_ctx.count) {
    return; /* outside: just close */
  }
  switch (g_ctx.actions[code - HIT_CTX_ITEM]) {
    case EDIT_CUT:
      umbra_browser_browser_edit(g_active, UMBRA_CHORIUM_TYPES_EDIT_COMMAND_CUT);
      break;
    case EDIT_COPY:
      umbra_browser_browser_edit(g_active,
                                 UMBRA_CHORIUM_TYPES_EDIT_COMMAND_COPY);
      break;
    case EDIT_PASTE:
      umbra_browser_browser_edit(g_active,
                                 UMBRA_CHORIUM_TYPES_EDIT_COMMAND_PASTE);
      break;
    case EDIT_SELECT_ALL:
      umbra_browser_browser_edit(g_active,
                                 UMBRA_CHORIUM_TYPES_EDIT_COMMAND_SELECT_ALL);
      break;
    case SAVE_LINK: {
      app_string_t url = str(g_ctx.link);
      umbra_browser_browser_save_url(g_active, &url);
      break;
    }
    case SAVE_IMAGE: {
      app_string_t url = str(g_ctx.image);
      umbra_browser_browser_save_url(g_active, &url);
      break;
    }
    case COPY_IMAGE:
      umbra_browser_browser_copy_image_at(g_active, g_ctx.page_x,
                                          g_ctx.page_y);
      break;
    case COPY_IMAGE_ADDRESS:
      copy(g_ctx.image);
      break;
    case OPEN_LINK:
      open_tab_with(g_ctx.link, false);
      break;
    case COPY_LINK:
      copy(g_ctx.link);
      break;
    case COPY_LINK_TEXT:
      copy(g_ctx.link_text);
      break;
    case OPEN_IMAGE:
      open_tab_with(g_ctx.image, false);
      break;
    case COPY_SELECTION:
      copy(g_ctx.selection);
      break;
    case SEARCH_SELECTION:
      open_tab_with(g_ctx.selection, true);
      break;
  }
}
