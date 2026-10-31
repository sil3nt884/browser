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
  size_t n = strlen(value);
  if (n >= sizeof f->buf) {
    n = sizeof f->buf - 1;
    while (n && ((unsigned char)value[n] & 0xC0) == 0x80) --n;
  }
  memmove(f->buf, value, n);
  f->buf[n] = '\0';
  f->caret = strlen(f->buf);
  f->anchor = select_all ? 0 : f->caret;
  f->scroll = 0;
}

static size_t previous_char(const char* s, size_t at) {
  if (!at) return 0;
  do { --at; } while (at && ((unsigned char)s[at] & 0xC0) == 0x80);
  return at;
}

static size_t next_char(const char* s, size_t at) {
  if (!s[at]) return at;
  do { ++at; } while (((unsigned char)s[at] & 0xC0) == 0x80);
  return at;
}

static bool separator(char c) {
  return c && strchr(" /.:?&=#-_", c);
}

static size_t previous_word(const char* s, size_t at) {
  while (at && separator(s[previous_char(s, at)])) at = previous_char(s, at);
  while (at && !separator(s[previous_char(s, at)])) at = previous_char(s, at);
  return at;
}

static size_t next_word(const char* s, size_t at) {
  while (s[at] && !separator(s[at])) at = next_char(s, at);
  while (s[at] && separator(s[at])) at = next_char(s, at);
  return at;
}

static size_t selection_start(const field_t* f) {
  return f->caret < f->anchor ? f->caret : f->anchor;
}

static size_t selection_end(const field_t* f) {
  return f->caret > f->anchor ? f->caret : f->anchor;
}

static void erase_range(field_t* f, size_t start, size_t end) {
  memmove(f->buf + start, f->buf + end, strlen(f->buf + end) + 1);
  f->anchor = f->caret = start;
}

bool field_key(field_t* f, const keypress_t* k, bool* enter) {
  bool ctrl = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_CTRL;
  bool shift = k->modifiers & UMBRA_CHORIUM_TYPES_MODIFIERS_SHIFT;
  *enter = false;
  if (!k->down) {
    return true;
  }
  if (ctrl && k->code == 'A') {
    f->anchor = 0;
    f->caret = strlen(f->buf);
    return true;
  }
  if (ctrl && (k->code == 'C' || k->code == 'X')) {
    size_t start = selection_start(f), end = selection_end(f);
    if (start != end) {
      app_string_t s = {(uint8_t*)f->buf + start, end - start};
      umbra_chorium_surface_copy_text(&s);
      if (k->code == 'X') {
        erase_range(f, start, end);
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
      f->anchor = f->caret;
      *enter = true;
      return true;
    case VK_BACK:
      if (f->caret != f->anchor) {
        erase_range(f, selection_start(f), selection_end(f));
      } else {
        erase_range(f, ctrl ? previous_word(f->buf, f->caret)
                            : previous_char(f->buf, f->caret), f->caret);
      }
      return true;
    case VK_DELETE:
      if (f->caret != f->anchor) {
        erase_range(f, selection_start(f), selection_end(f));
      } else {
        erase_range(f, f->caret, ctrl ? next_word(f->buf, f->caret)
                                    : next_char(f->buf, f->caret));
      }
      return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END: {
      size_t at = f->caret;
      if (!shift && f->caret != f->anchor &&
          (k->code == VK_LEFT || k->code == VK_RIGHT)) {
        at = k->code == VK_LEFT ? selection_start(f) : selection_end(f);
      } else if (k->code == VK_LEFT) {
        at = ctrl ? previous_word(f->buf, at) : previous_char(f->buf, at);
      } else if (k->code == VK_RIGHT) {
        at = ctrl ? next_word(f->buf, at) : next_char(f->buf, at);
      } else {
        at = k->code == VK_HOME ? 0 : strlen(f->buf);
      }
      f->caret = at;
      if (!shift) f->anchor = at;
      return true;
    }
  }
  return false;
}

void field_paste(field_t* f) {
  app_string_t clip;
  umbra_chorium_surface_read_clipboard(&clip);
  field_text(f, &clip);
  app_string_free(&clip);
}

void field_text(field_t* f, const app_string_t* s) {
  size_t start = selection_start(f), end = selection_end(f);
  size_t available = sizeof f->buf - 1 - (strlen(f->buf) - (end - start));
  char add[sizeof f->buf];
  size_t n = 0;
  for (size_t i = 0; i < s->len;) {
    size_t bytes = 1;
    while (i + bytes < s->len && (s->ptr[i + bytes] & 0xC0) == 0x80) ++bytes;
    if (n + bytes > available) break;
    if (s->ptr[i] != '\r' && s->ptr[i]) {
      if (s->ptr[i] == '\n' || s->ptr[i] == '\t') add[n++] = ' ';
      else { memcpy(add + n, s->ptr + i, bytes); n += bytes; }
    }
    i += bytes;
  }
  erase_range(f, start, end);
  memmove(f->buf + start + n, f->buf + start, strlen(f->buf + start) + 1);
  memcpy(f->buf + start, add, n);
  f->anchor = f->caret = start + n;
}

static int prefix_width(const field_t* f, size_t end, float size) {
  char prefix[sizeof f->buf];
  memcpy(prefix, f->buf, end);
  prefix[end] = '\0';
  return text_width(prefix, size);
}

void field_place(field_t* f, int x, bool extend) {
  int width = (int)f->viewport.width;
  if (extend) {
    if (x < f->viewport.x) f->scroll -= (f->viewport.x - x) / 2 + 1;
    if (x > f->viewport.x + width) f->scroll += (x - f->viewport.x - width) / 2 + 1;
    int maximum = text_width(f->buf, f->font_size) - width + 2;
    if (f->scroll > maximum) f->scroll = maximum;
    if (f->scroll < 0) f->scroll = 0;
  }
  int target = x - f->viewport.x + f->scroll;
  size_t lo = 0, hi = strlen(f->buf);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && ((unsigned char)f->buf[mid] & 0xC0) == 0x80) --mid;
    size_t next = next_char(f->buf, mid);
    int middle = (prefix_width(f, mid, f->font_size) +
                  prefix_width(f, next, f->font_size)) / 2;
    if (target < middle) hi = mid;
    else lo = next;
  }
  f->caret = lo;
  if (!extend) f->anchor = lo;
}

void field_draw(field_t* f, rect_t r, int x, float size, bool focused,
                const char* placeholder) {
  rect_t box = rect(x, r.y, (int)r.width - (x - r.x), (int)r.height);
  f->viewport = box;
  f->font_size = size;
  int caret = prefix_width(f, f->caret, size);
  int width = (int)box.width;
  bool selected = f->caret != f->anchor;
  if (!focused) f->scroll = 0;
  else if (!(selected && selection_start(f) == 0 &&
             selection_end(f) == strlen(f->buf))) {
    if (caret - f->scroll > width - 2) f->scroll = caret - width + 2;
    if (caret - f->scroll < 0) f->scroll = caret;
  }
  int maximum = text_width(f->buf, size) - width + 2;
  if (f->scroll > maximum) f->scroll = maximum;
  if (f->scroll < 0) f->scroll = 0;
  clip(box);
  if (!f->buf[0]) {
    text_in(box, placeholder, size, T->text_dim);
  } else {
    if (focused && selected) {
      int left = prefix_width(f, selection_start(f), size);
      int right = prefix_width(f, selection_end(f), size);
      int h = (int)(size * 1.4f);
      fill(rect(x + left - f->scroll, r.y + ((int)r.height - h) / 2, right - left, h),
           T->selection);
    }
    text(x - f->scroll, r.y + (int)r.height / 2 + (int)(size * 0.36f),
         f->buf, size, T->text);
  }
  if (focused && !selected) {
    int h = (int)(size * 1.3f);
    fill(rect(x + caret - f->scroll, r.y + ((int)r.height - h) / 2, 1, h), T->text);
  }
  unclip();
}
