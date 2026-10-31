/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Files in and out: the download notice at the bottom of the page (with
 * Open / Show in folder), and pages' file pickers (<input type=file>),
 * which go to the system's picker.
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

typedef umbra_chorium_types_download_t download_t;

/* --- sizes ---------------------------------------------------------------- */

void format_size(char* out, size_t cap, uint64_t bytes) {
  if (bytes < 1024) {
    snprintf(out, cap, "%u B", (unsigned)bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(out, cap, "%.0f KB", (double)bytes / 1024);
  } else if (bytes < 1024ull * 1024 * 1024) {
    snprintf(out, cap, "%.1f MB", (double)bytes / (1024 * 1024));
  } else {
    snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
  }
}

void download_status(char* out, size_t cap, const download_t* d) {
  char got[32], total[32];
  format_size(got, sizeof got, d->received);
  format_size(total, sizeof total, d->total);
  switch (d->state) {
    case UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_IN_PROGRESS:
      if (d->total) {
        snprintf(out, cap, "%s of %s", got, total);
      } else {
        snprintf(out, cap, "%s", got);
      }
      break;
    case UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_DONE:
      snprintf(out, cap, "%s, done", d->total ? total : got);
      break;
    case UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_FAILED:
      snprintf(out, cap, "Failed");
      break;
    case UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_CANCELLED:
      snprintf(out, cap, "Cancelled");
      break;
  }
}

void open_download(const char* path, bool in_folder) {
  app_string_t p = str(path);
  umbra_chorium_surface_open_file(&p, in_folder);
}

/* --- the notice ----------------------------------------------------------- */

static struct {
  bool open;
  uint32_t id;
  char name[256];
  char path[2048];
  char status[64];
  umbra_chorium_types_download_state_t state;
  uint64_t received, total;
} g_notice;

void downloads_event(const download_t* d) {
  if (d->id != g_notice.id) {
    /* A new download takes the notice over (and opens it again). */
    if (d->state != UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_IN_PROGRESS &&
        g_notice.open && g_notice.id) {
      return; /* old news about another download */
    }
    g_notice.id = d->id;
    g_notice.open = true;
  }
  copy_str(g_notice.name, sizeof g_notice.name, &d->file_name);
  copy_str(g_notice.path, sizeof g_notice.path, &d->path);
  download_status(g_notice.status, sizeof g_notice.status, d);
  g_notice.state = d->state;
  g_notice.received = d->received;
  g_notice.total = d->total;
  request_draw();
}

void notice_close(void) { g_notice.open = false; }

static rect_t notice_box(rect_t page) {
  int w = g_phone ? (int)page.width - 16 : 420;
  int h = 64;
  return rect(page.x + (g_phone ? 8 : 16),
              page.y + (int)page.height - h - (g_phone ? 8 : 16), w, h);
}

/* The notice's buttons, right to left: close, then Open / Show in folder. */
static rect_t notice_button(rect_t box, int i) {
  int size = 40;
  return rect(box.x + (int)box.width - 8 - size * (i + 1) - 4 * i,
              box.y + ((int)box.height - size) / 2, size, size);
}

static rect_t g_page; /* page area at the last draw */

void notice_draw(rect_t page) {
  g_page = page;
  if (!g_notice.open) {
    return;
  }
  rect_t box = notice_box(page);
  fill_round(rect(box.x - 1, box.y + 1, (int)box.width + 2, (int)box.height + 3),
             11, T->shadow);
  fill_round(box, 10, T->card);
  bool done = g_notice.state == UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_DONE;
  bool running = g_notice.state == UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_IN_PROGRESS;
  icon_centered(done ? ICON(CHECK) : ICON(DOWNLOAD),
                rect(box.x + 12, box.y, 28, (int)box.height), 22,
                g_notice.state == UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_FAILED
                    ? T->error
                    : T->accent);
  int buttons = done ? 3 : 1;
  int text_w = (int)box.width - 52 - buttons * 44 - 8;
  text_in(rect(box.x + 52, box.y + 10, text_w, 22), g_notice.name, 14, T->text);
  text_in(rect(box.x + 52, box.y + 32, text_w, 20), g_notice.status, 12.5f,
          T->text_dim);
  if (running && g_notice.total) {
    /* A progress line along the bottom edge. */
    int w = (int)((double)((int)box.width - 24) * (double)g_notice.received /
                  (double)g_notice.total);
    fill(rect(box.x + 12, box.y + (int)box.height - 5, (int)box.width - 24, 2),
         T->separator);
    fill(rect(box.x + 12, box.y + (int)box.height - 5, w, 2), T->accent);
  }
  static const icon_t kIcons[] = {ICON(CLOSE), ICON(FOLDER), ICON(PAGE)};
  for (int i = 0; i < buttons; ++i) {
    rect_t b = notice_button(box, i);
    if (g_hover == HIT_NOTICE + i) {
      circle(b.x + 20, b.y + 20, 18, T->hover);
    }
    icon_centered(kIcons[i], b, 20, T->icon);
  }
}

int notice_hit(int x, int y) {
  if (!g_notice.open || !g_page.width) {
    return 0;
  }
  rect_t box = notice_box(g_page);
  if (!inside(box, x, y)) {
    return 0;
  }
  int buttons = g_notice.state == UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_DONE ? 3 : 1;
  for (int i = 0; i < buttons; ++i) {
    if (inside(notice_button(box, i), x, y)) {
      return HIT_NOTICE + i;
    }
  }
  return HIT_NOTICE_BODY;
}

void notice_click(int code) {
  bool done = g_notice.state == UMBRA_CHORIUM_TYPES_DOWNLOAD_STATE_DONE;
  switch (code) {
    case HIT_NOTICE: /* close */
      g_notice.open = false;
      break;
    case HIT_NOTICE + 1:
      open_download(g_notice.path, true);
      break;
    case HIT_NOTICE + 2:
      open_download(g_notice.path, false);
      break;
    case HIT_NOTICE_BODY:
      if (done) {
        open_download(g_notice.path, false);
      } else {
        g_notice.open = false;
        panel_open(PANEL_DOWNLOADS);
      }
      break;
  }
}

/* --- file pickers --------------------------------------------------------- */

/* Pickers waiting for the system's answer: the tab and its chooser id. */
#define MAX_PICKS 8
static struct {
  uint32_t request; /* 0 = free */
  uint32_t tab;
  uint32_t chooser;
} g_picks[MAX_PICKS];
static uint32_t g_next_request = 1;

void file_chooser_event(uint32_t tab, umbra_chorium_types_file_chooser_t* c) {
  int slot = -1;
  for (int i = 0; i < MAX_PICKS && slot < 0; ++i) {
    if (!g_picks[i].request) {
      slot = i;
    }
  }
  if (slot < 0) {
    app_list_string_t none = {NULL, 0};
    umbra_browser_browser_file_chooser_reply(tab, c->id, &none);
    return;
  }
  g_picks[slot].request = g_next_request++;
  g_picks[slot].tab = tab;
  g_picks[slot].chooser = c->id;
  umbra_chorium_surface_pick_files(g_picks[slot].request, c->mode, &c->accept,
                                   &c->default_name);
}

void files_picked(umbra_chorium_types_files_picked_t* p) {
  for (int i = 0; i < MAX_PICKS; ++i) {
    if (g_picks[i].request && g_picks[i].request == p->request) {
      g_picks[i].request = 0;
      umbra_browser_browser_file_chooser_reply(g_picks[i].tab,
                                               g_picks[i].chooser, &p->paths);
    }
  }
}
