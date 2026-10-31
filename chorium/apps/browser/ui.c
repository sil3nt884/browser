/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define VK_BACK 0x08
#define VK_RETURN 0x0D
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_DELETE 0x2E

/* --- theme ---------------------------------------------------------------- */

/* Chrome's default light and dark colours. */
static const theme_t kLight = {
    .frame = 0xFFDEE1E6,
    .tab_hover = 0xFFE8EAED,
    .toolbar = 0xFFFFFFFF,
    .omnibox = 0xFFF1F3F4,
    .text = 0xFF202124,
    .text_dim = 0xFF5F6368,
    .icon = 0xFF5F6368,
    .icon_off = 0xFFBDC1C6,
    .hover = 0xFFE8EAED,
    .accent = 0xFF1A73E8,
    .selection = 0xFFAECBFA,
    .separator = 0xFFBDC1C6,
    .page_bg = 0xFFF8F9FA,
    .card = 0xFFFFFFFF,
    .shadow = 0x26000000,
    .error = 0xFFD93025,
    .close_hover = 0xFFC42B1C,
};

static const theme_t kDark = {
    .frame = 0xFF202124,
    .tab_hover = 0xFF2C2D30,
    .toolbar = 0xFF35363A,
    .omnibox = 0xFF202124,
    .text = 0xFFE8EAED,
    .text_dim = 0xFF9AA0A6,
    .icon = 0xFFC4C7C5,
    .icon_off = 0xFF5F6368,
    .hover = 0xFF4A4C50,
    .accent = 0xFF8AB4F8,
    .selection = 0xFF3B5CA8,
    .separator = 0xFF5F6368,
    .page_bg = 0xFF202124,
    .card = 0xFF292A2D,
    .shadow = 0x66000000,
    .error = 0xFFF28B82,
    .close_hover = 0xFFC42B1C,
};

const theme_t* T = &kLight;

void theme_update(void) {
  char mode[16] = "system";
  get_setting("theme", mode, sizeof mode);
  bool dark = strcmp(mode, "dark") == 0 ||
              (strcmp(mode, "light") != 0 && umbra_chorium_surface_dark_mode());
  T = dark ? &kDark : &kLight;
  /* Pages follow the same theme (prefers-color-scheme). */
  static int sent = -1;
  if ((int)dark != sent) {
    sent = dark;
    umbra_browser_browser_set_dark_mode(dark);
  }
}

/* --- geometry and drawing ------------------------------------------------- */

rect_t rect(int x, int y, int w, int h) {
  rect_t r = {x, y, (uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)};
  return r;
}

bool inside(rect_t r, int x, int y) {
  return x >= r.x && y >= r.y && x < r.x + (int)r.width &&
         y < r.y + (int)r.height;
}

void fill(rect_t r, uint32_t c) { umbra_chorium_surface_fill_rect(&r, c); }

void fill_round(rect_t r, float radius, uint32_t c) {
  umbra_chorium_surface_fill_round_rect(&r, radius, c);
}

void circle(int cx, int cy, int radius, uint32_t c) {
  fill_round(rect(cx - radius, cy - radius, radius * 2, radius * 2),
             (float)radius, c);
}

void stroke(rect_t r, uint32_t c, float width) {
  umbra_chorium_surface_stroke_rect(&r, c, width);
}

void text(int x, int baseline, const char* s, float size, uint32_t c) {
  app_string_t t = str(s);
  umbra_chorium_surface_draw_text(x, baseline, &t, size, c);
}

int text_width(const char* s, float size) {
  app_string_t t = str(s);
  umbra_chorium_types_size_t out;
  umbra_chorium_surface_measure_text(&t, size, &out);
  return (int)out.width;
}

void text_in(rect_t r, const char* s, float size, uint32_t c) {
  clip(r);
  /* Baseline a little below the middle: Segoe UI's ascent is ~0.75 em. */
  text(r.x, r.y + (int)r.height / 2 + (int)(size * 0.36f), s, size, c);
  unclip();
}

void icon(icon_t which, rect_t r, uint32_t c) {
  umbra_chorium_surface_draw_icon(which, &r, c);
}

void toggle_switch(rect_t r, bool on) {
  rect_t track = rect(r.x + (int)r.width - 36, r.y + ((int)r.height - 20) / 2,
                      36, 20);
  fill_round(track, 10, on ? T->accent : T->icon_off);
  circle(on ? track.x + 26 : track.x + 10, track.y + 10, 7, 0xFFFFFFFF);
}

void icon_centered(icon_t which, rect_t box, int size, uint32_t c) {
  icon(which,
       rect(box.x + ((int)box.width - size) / 2,
            box.y + ((int)box.height - size) / 2, size, size),
       c);
}

void clip(rect_t r) { umbra_chorium_surface_push_clip(&r); }
void unclip(void) { umbra_chorium_surface_pop_clip(); }

/* --- strings and settings ------------------------------------------------- */

app_string_t str(const char* s) {
  app_string_t r;
  app_string_set(&r, s);
  return r;
}

void copy_str(char* out, size_t cap, const app_string_t* s) {
  size_t n = s->len < cap - 1 ? s->len : cap - 1;
  memcpy(out, s->ptr, n);
  out[n] = '\0';
}

bool get_setting(const char* key, char* out, size_t cap) {
  app_string_t k = str(key);
  app_string_t v;
  if (!umbra_browser_browser_get_setting(&k, &v)) {
    return false;
  }
  copy_str(out, cap, &v);
  app_string_free(&v);
  return true;
}

void set_setting(const char* key, const char* value) {
  app_string_t k = str(key);
  app_string_t v = str(value);
  umbra_browser_browser_set_setting(&k, &v);
}

/* --- text fields ---------------------------------------------------------- */

void field_set(field_t* f, const char* value, bool select_all) {
  snprintf(f->buf, sizeof f->buf, "%s", value);
  f->selected = select_all && f->buf[0];
}

static void backspace(char* s) {
  size_t n = strlen(s);
  while (n > 0) {
    --n;
    if (((unsigned char)s[n] & 0xC0) != 0x80) {
      break;
    }
  }
  s[n] = '\0';
}

/* Ctrl+Backspace: trailing separators, then the word before them. */
static void backspace_word(char* s) {
  static const char* separators = " /.:?&=#-_";
  size_t n = strlen(s);
  while (n > 0 && strchr(separators, s[n - 1])) {
    --n;
  }
  while (n > 0 && !strchr(separators, s[n - 1])) {
    --n;
  }
  s[n] = '\0';
}

bool field_key(field_t* f, const keypress_t* k, bool* enter) {
  bool ctrl = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_CTRL;
  *enter = false;
  if (!k->down) {
    return true;
  }
  if (ctrl && k->code == 'A') {
    f->selected = f->buf[0] != '\0';
    return true;
  }
  /* The caret is always at the end, so copy and cut take the whole text
   * once it's selected. */
  if (ctrl && (k->code == 'C' || k->code == 'X')) {
    if (f->selected) {
      app_string_t s = str(f->buf);
      umbra_chorium_surface_copy_text(&s);
      if (k->code == 'X') {
        f->buf[0] = '\0';
        f->selected = false;
      }
    }
    return true;
  }
  if (ctrl && k->code == 'V') {
    field_paste(f);
    return true;
  }
  switch (k->code) {
    case VK_RETURN:
      f->selected = false;
      *enter = true;
      return true;
    case VK_BACK:
      if (f->selected) {
        f->buf[0] = '\0';
      } else if (ctrl) {
        backspace_word(f->buf);
      } else {
        backspace(f->buf);
      }
      f->selected = false;
      return true;
    case VK_DELETE:
      /* The caret is always at the end: Delete only clears a selection. */
      if (f->selected) {
        f->buf[0] = '\0';
        f->selected = false;
      }
      return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
      f->selected = false;
      return true;
  }
  return false;
}

void field_paste(field_t* f) {
  app_string_t clip;
  umbra_chorium_surface_read_clipboard(&clip);
  if (f->selected) {
    f->buf[0] = '\0';
    f->selected = false;
  }
  /* One line: line breaks become spaces, as in Chrome's address bar. */
  size_t n = strlen(f->buf);
  for (size_t i = 0; i < clip.len && n + 1 < sizeof f->buf; ++i) {
    char c = (char)clip.ptr[i];
    if (c == '\r') {
      continue;
    }
    f->buf[n++] = c == '\n' || c == '\t' ? ' ' : c;
  }
  f->buf[n] = '\0';
  app_string_free(&clip);
}

void field_text(field_t* f, const app_string_t* s) {
  char add[256];
  copy_str(add, sizeof add, s);
  if (f->selected) {
    f->buf[0] = '\0';
    f->selected = false;
  }
  if (strlen(f->buf) + strlen(add) < sizeof f->buf) {
    strcat(f->buf, add);
  }
}

void field_draw(const field_t* f, rect_t r, int x, float size, bool focused,
                const char* placeholder) {
  rect_t box = rect(x, r.y, (int)r.width - (x - r.x), (int)r.height);
  if (!f->buf[0]) {
    text_in(box, placeholder, size, T->text_dim);
  } else {
    if (focused && f->selected) {
      int w = text_width(f->buf, size);
      int h = (int)(size * 1.4f);
      if (w + 2 > (int)box.width) {
        w = (int)box.width - 2; /* long text is clipped; so is its highlight */
      }
      fill(rect(x - 1, r.y + ((int)r.height - h) / 2, w + 2, h),
           T->selection);
    }
    text_in(box, f->buf, size, T->text);
  }
  if (focused && !f->selected) {
    int w = f->buf[0] ? text_width(f->buf, size) : 0;
    int h = (int)(size * 1.3f);
    if (x + w < r.x + (int)r.width) {
      fill(rect(x + w, r.y + ((int)r.height - h) / 2, 1, h), T->text);
    }
  }
}
