/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * chorium.wasm: the portable browser layer. Implements the `browser`
 * interface (interfaces/browser/browser.wit) on top of the runtime's pages,
 * net, files and log. Owns the pages; apps see tabs by id.
 *
 * Ownership follows wit-bindgen's C bindings: arguments of an export belong
 * to the export (freed here), anything returned is freed by the bindings
 * after the call, and values returned by imports belong to the caller.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "portable.h"

typedef portable_string_t str_t;
typedef exports_umbra_browser_browser_tab_info_t tab_info_t;
typedef exports_umbra_browser_browser_browser_event_t event_t;
typedef exports_umbra_browser_browser_download_t download_t;
typedef umbra_chorium_types_size_t size_t2;

#define MAX_TABS 64
#define MAX_HISTORY 1000
#define MAX_DOWNLOADS 100
#define SETTINGS_FILE "settings.txt"
#define HISTORY_FILE "history.tsv"
/* The open tabs: "active\t<index>" and then one URL per line, oldest tab
 * first. Saved as they change, read by restore-session. */
#define SESSION_FILE "session.txt"
#define DEFAULT_SEARCH "https://html.duckduckgo.com/html/?q=%s"
#define DEFAULT_HOME "https://html.duckduckgo.com/html/"

typedef struct {
  bool used;
  uint32_t id;
  uint32_t page_id; /* the runtime's id, as in on-page-event */
  umbra_chorium_pages_own_page_t page;
  size_t2 size;
  char* url;
  char* title;
  char* error; /* NULL if the last load worked */
  bool loading, can_go_back, can_go_forward;
} tab_t;

typedef struct {
  char* url;
  char* title;
  uint32_t visits;
  uint64_t last_visit;
} visit_t;

typedef struct {
  char* key;
  char* value;
} setting_t;

static tab_t g_tabs[MAX_TABS];

/* This run's downloads, newest first, and the tab each came from. */
typedef struct {
  download_t d;
  uint32_t tab_id;
} dl_t;
static dl_t g_downloads[MAX_DOWNLOADS];
static size_t g_download_count;
static uint32_t g_next_tab = 1;
static bool g_ready;
static umbra_chorium_files_own_dir_t g_dir;
static visit_t g_history[MAX_HISTORY];
static size_t g_history_len;
static setting_t* g_settings;
static size_t g_settings_len;
static uint32_t g_active_tab; /* set-active-tab, for the session */

/* --- strings -------------------------------------------------------------- */

static char* dup_n(const uint8_t* s, size_t n) {
  char* out = malloc(n + 1);
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char* from_str(const str_t* s) { return dup_n(s->ptr, s->len); }

/* Takes ownership of |s|. */
static char* take_str(str_t* s) {
  char* out = from_str(s);
  portable_string_free(s);
  return out;
}

static str_t to_str(const char* s) {
  str_t out;
  portable_string_dup(&out, s ? s : "");
  return out;
}

static void set(char** field, const char* value) {
  free(*field);
  *field = value ? strdup(value) : NULL;
}

static void log_msg(umbra_chorium_log_level_t level, const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  str_t s;
  portable_string_set(&s, buf);
  umbra_chorium_log_write(level, &s);
}

/* --- files ---------------------------------------------------------------- */

static umbra_chorium_files_borrow_dir_t dir(void) {
  return umbra_chorium_files_borrow_dir(g_dir);
}

/* The whole file as a NUL-terminated string, or NULL. */
static char* read_file(const char* name) {
  str_t n;
  portable_string_set(&n, name);
  portable_list_u8_t data;
  str_t err;
  if (!umbra_chorium_files_method_dir_read(dir(), &n, &data, &err)) {
    portable_string_free(&err);
    return NULL;
  }
  char* text = dup_n(data.ptr, data.len);
  portable_list_u8_free(&data);
  return text;
}

static void write_file(const char* name, const char* text, size_t len) {
  str_t n;
  portable_string_set(&n, name);
  portable_list_u8_t data = {(uint8_t*)text, len};
  str_t err;
  if (!umbra_chorium_files_method_dir_write(dir(), &n, &data, &err)) {
    char* e = take_str(&err);
    log_msg(UMBRA_CHORIUM_LOG_LEVEL_WARN, "can't save %s: %s", name, e);
    free(e);
  }
}

/* Grows a text buffer for saving. */
typedef struct {
  char* p;
  size_t len, cap;
} buf_t;

static void buf_add(buf_t* b, const char* s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n + 1);
  b->len += n;
}

/* Tabs and newlines would break the line format. */
static void buf_add_clean(buf_t* b, const char* s) {
  char tmp[2] = {0, 0};
  for (; *s; ++s) {
    tmp[0] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    buf_add(b, tmp);
  }
}

/* --- settings ------------------------------------------------------------- */

static setting_t* find_setting(const char* key) {
  for (size_t i = 0; i < g_settings_len; ++i) {
    if (strcmp(g_settings[i].key, key) == 0) {
      return &g_settings[i];
    }
  }
  return NULL;
}

static void put_setting(const char* key, const char* value) {
  setting_t* s = find_setting(key);
  if (!s) {
    g_settings = realloc(g_settings, sizeof(setting_t) * (g_settings_len + 1));
    s = &g_settings[g_settings_len++];
    s->key = strdup(key);
    s->value = NULL;
  }
  set(&s->value, value);
}

static const char* setting(const char* key, const char* fallback) {
  setting_t* s = find_setting(key);
  return s && s->value[0] ? s->value : fallback;
}

static void load_settings(void) {
  char* text = read_file(SETTINGS_FILE);
  if (!text) {
    return;
  }
  for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    char* eq = strchr(line, '=');
    if (eq) {
      *eq = '\0';
      put_setting(line, eq + 1);
    }
  }
  free(text);
}

static void save_settings(void) {
  buf_t b = {0};
  buf_add(&b, "");
  for (size_t i = 0; i < g_settings_len; ++i) {
    buf_add_clean(&b, g_settings[i].key);
    buf_add(&b, "=");
    buf_add_clean(&b, g_settings[i].value);
    buf_add(&b, "\n");
  }
  write_file(SETTINGS_FILE, b.p, b.len);
  free(b.p);
}

/* --- history -------------------------------------------------------------- */

static bool worth_remembering(const char* url) {
  return url && url[0] && strncmp(url, "about:", 6) != 0 &&
         strncmp(url, "chrome-error:", 13) != 0 &&
         strncmp(url, "data:", 5) != 0;
}

static visit_t* find_visit(const char* url) {
  for (size_t i = 0; i < g_history_len; ++i) {
    if (strcmp(g_history[i].url, url) == 0) {
      return &g_history[i];
    }
  }
  return NULL;
}

static void forget(size_t i) {
  free(g_history[i].url);
  free(g_history[i].title);
  memmove(&g_history[i], &g_history[i + 1],
          sizeof(visit_t) * (g_history_len - i - 1));
  --g_history_len;
}

static visit_t* remember(const char* url, const char* title,
                         uint32_t visits, uint64_t when) {
  visit_t* v = find_visit(url);
  if (!v) {
    if (g_history_len == MAX_HISTORY) {
      size_t oldest = 0;
      for (size_t i = 1; i < g_history_len; ++i) {
        if (g_history[i].last_visit < g_history[oldest].last_visit) {
          oldest = i;
        }
      }
      forget(oldest);
    }
    v = &g_history[g_history_len++];
    v->url = strdup(url);
    v->title = strdup("");
    v->visits = 0;
    v->last_visit = 0;
  }
  if (title && title[0]) {
    set(&v->title, title);
  }
  v->visits += visits;
  if (when > v->last_visit) {
    v->last_visit = when;
  }
  return v;
}

static void load_history(void) {
  char* text = read_file(HISTORY_FILE);
  if (!text) {
    return;
  }
  char* save = NULL;
  for (char* line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char* f[4] = {line, NULL, NULL, NULL};
    for (int i = 1; i < 4; ++i) {
      f[i] = f[i - 1] ? strchr(f[i - 1], '\t') : NULL;
      if (f[i]) {
        *f[i]++ = '\0';
      }
    }
    if (f[3] && worth_remembering(f[0])) {
      remember(f[0], f[1], (uint32_t)strtoul(f[2], NULL, 10),
               strtoull(f[3], NULL, 10));
    }
  }
  free(text);
}

static void save_history(void) {
  buf_t b = {0};
  buf_add(&b, "");
  char num[48];
  for (size_t i = 0; i < g_history_len; ++i) {
    buf_add_clean(&b, g_history[i].url);
    buf_add(&b, "\t");
    buf_add_clean(&b, g_history[i].title);
    snprintf(num, sizeof num, "\t%u\t%llu\n", g_history[i].visits,
             (unsigned long long)g_history[i].last_visit);
    buf_add(&b, num);
  }
  write_file(HISTORY_FILE, b.p, b.len);
  free(b.p);
}

/* --- setup ---------------------------------------------------------------- */

static void ready(void) {
  if (g_ready) {
    return;
  }
  g_ready = true;
  g_dir = umbra_chorium_files_app_data();
  load_settings();
  setting_t* clear = find_setting("clear-site-data-on-close");
  umbra_chorium_net_set_clear_site_data_on_close(
      clear && strcmp(clear->value, "true") == 0);
  load_history();
}

/* --- URLs ----------------------------------------------------------------- */

static bool has_scheme(const char* s) {
  const char* colon = strchr(s, ':');
  if (!colon || colon == s) {
    return false;
  }
  for (const char* p = s; p < colon; ++p) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (p > s && ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
                         c == '.'));
    if (!ok) {
      return false;
    }
  }
  /* "localhost:8080" and "example.com:443/x" are hosts with ports. */
  if (colon[1] >= '0' && colon[1] <= '9') {
    return false;
  }
  return strncmp(colon, "://", 3) == 0 || strncmp(s, "about:", 6) == 0 ||
         strncmp(s, "data:", 5) == 0 || strncmp(s, "javascript:", 11) == 0 ||
         strncmp(s, "mailto:", 7) == 0;
}

static bool looks_like_host(const char* s) {
  if (strchr(s, ' ') || s[0] == '.' || s[0] == '\0') {
    return false;
  }
  if (strncmp(s, "localhost", 9) == 0 &&
      (s[9] == '\0' || s[9] == ':' || s[9] == '/')) {
    return true;
  }
  if (s[0] == '[') {
    return true; /* IPv6 literal */
  }
  const char* end = s + strcspn(s, "/?#");
  const char* dot = memchr(s, '.', (size_t)(end - s));
  return dot && dot + 1 < end && dot[1] != ':';
}

static void percent_encode(buf_t* b, const char* s) {
  static const char hex[] = "0123456789ABCDEF";
  char tmp[4];
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      tmp[0] = (char)c;
      tmp[1] = '\0';
    } else if (c == ' ') {
      tmp[0] = '+';
      tmp[1] = '\0';
    } else {
      tmp[0] = '%';
      tmp[1] = hex[c >> 4];
      tmp[2] = hex[c & 15];
      tmp[3] = '\0';
    }
    buf_add(b, tmp);
  }
}

/* What a person typed -> the URL to load. Returns a malloc'd string. */
static char* resolve(const char* typed) {
  while (*typed == ' ' || *typed == '\t') {
    ++typed;
  }
  char* text = strdup(typed);
  size_t n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\t')) {
    text[--n] = '\0';
  }
  char* out;
  if (!n) {
    out = strdup(setting("home", DEFAULT_HOME));
  } else if (has_scheme(text)) {
    out = strdup(text);
  } else if (looks_like_host(text)) {
    out = malloc(n + 9);
    snprintf(out, n + 9, "https://%s", text);
  } else {
    const char* tmpl = setting("search", DEFAULT_SEARCH);
    const char* mark = strstr(tmpl, "%s");
    buf_t b = {0};
    buf_add(&b, "");
    if (mark) {
      char* head = dup_n((const uint8_t*)tmpl, (size_t)(mark - tmpl));
      buf_add(&b, head);
      free(head);
      percent_encode(&b, text);
      buf_add(&b, mark + 2);
    } else {
      buf_add(&b, tmpl);
      percent_encode(&b, text);
    }
    out = b.p;
  }
  free(text);
  return out;
}

/* --- tabs ----------------------------------------------------------------- */

static tab_t* tab(uint32_t id) {
  for (int i = 0; i < MAX_TABS; ++i) {
    if (g_tabs[i].used && g_tabs[i].id == id) {
      return &g_tabs[i];
    }
  }
  return NULL;
}

static tab_t* tab_for_page(uint32_t page_id) {
  for (int i = 0; i < MAX_TABS; ++i) {
    if (g_tabs[i].used && g_tabs[i].page_id == page_id) {
      return &g_tabs[i];
    }
  }
  return NULL;
}

static str_t copy_str(const str_t* s) {
  str_t out;
  out.len = s->len;
  out.ptr = malloc(s->len ? s->len : 1);
  if (s->len) {
    memcpy(out.ptr, s->ptr, s->len);
  }
  return out;
}

static download_t copy_download(const download_t* d) {
  download_t out = *d;
  out.file_name = copy_str(&d->file_name);
  out.path = copy_str(&d->path);
  out.url = copy_str(&d->url);
  return out;
}

static void free_download(download_t* d) {
  portable_string_free(&d->file_name);
  portable_string_free(&d->path);
  portable_string_free(&d->url);
}

/* Records |d| (new, or news about a known one) from tab |tab_id|. */
static void track_download(const download_t* d, uint32_t tab_id) {
  for (size_t i = 0; i < g_download_count; ++i) {
    if (g_downloads[i].d.id == d->id) {
      free_download(&g_downloads[i].d);
      g_downloads[i].d = copy_download(d);
      return;
    }
  }
  if (g_download_count == MAX_DOWNLOADS) {
    free_download(&g_downloads[--g_download_count].d);
  }
  memmove(&g_downloads[1], &g_downloads[0], sizeof(dl_t) * g_download_count);
  g_downloads[0].d = copy_download(d);
  g_downloads[0].tab_id = tab_id;
  ++g_download_count;
}

static umbra_chorium_pages_borrow_page_t page(tab_t* t) {
  return umbra_chorium_pages_borrow_page(t->page);
}

static void load(tab_t* t, const char* url) {
  str_t s;
  portable_string_set(&s, url);
  umbra_chorium_pages_method_page_load_url(page(t), &s);
}

static void fill_info(const tab_t* t, tab_info_t* info) {
  info->id = t->id;
  info->url = to_str(t->url);
  info->title = to_str(t->title);
  info->loading = t->loading;
  info->can_go_back = t->can_go_back;
  info->can_go_forward = t->can_go_forward;
  info->error.is_some = t->error != NULL;
  if (t->error) {
    info->error.val = to_str(t->error);
  }
}

static tab_t* new_tab(const char* typed, size_t2 size) {
  tab_t* t = NULL;
  for (int i = 0; i < MAX_TABS; ++i) {
    if (!g_tabs[i].used) {
      t = &g_tabs[i];
      break;
    }
  }
  if (!t) {
    log_msg(UMBRA_CHORIUM_LOG_LEVEL_WARN, "too many tabs");
    return NULL;
  }
  memset(t, 0, sizeof *t);
  t->used = true;
  t->id = g_next_tab++;
  t->size.width = size.width ? size.width : 1;
  t->size.height = size.height ? size.height : 1;
  t->page = umbra_chorium_pages_constructor_page(&t->size);
  t->page_id = umbra_chorium_pages_method_page_id(page(t));
  t->url = strdup("");
  t->title = strdup("");
  char* url = resolve(typed);
  if (strcmp(url, "about:blank") != 0) {
    load(t, url);
  }
  free(url);
  return t;
}

static void close_tab(tab_t* t) {
  umbra_chorium_pages_page_drop_own(t->page);
  free(t->url);
  free(t->title);
  free(t->error);
  memset(t, 0, sizeof *t);
}

/* Tabs in the order they were opened (ids grow; slots get reused). */
static size_t ordered_tabs(tab_t** order) {
  size_t n = 0;
  for (int i = 0; i < MAX_TABS; ++i) {
    if (g_tabs[i].used) {
      size_t j = n++;
      while (j > 0 && order[j - 1]->id > g_tabs[i].id) {
        order[j] = order[j - 1];
        --j;
      }
      order[j] = &g_tabs[i];
    }
  }
  return n;
}

static void save_session(void) {
  tab_t* order[MAX_TABS];
  size_t n = ordered_tabs(order);
  size_t active = 0;
  buf_t b = {0};
  buf_add(&b, "");
  size_t saved = 0;
  char line[48];
  for (size_t i = 0; i < n; ++i) {
    if (!order[i]->url[0] || strncmp(order[i]->url, "about:", 6) == 0) {
      continue; /* nothing to come back to */
    }
    if (order[i]->id == g_active_tab) {
      active = saved;
    }
    buf_add_clean(&b, order[i]->url);
    buf_add(&b, "\n");
    ++saved;
  }
  snprintf(line, sizeof line, "active\t%zu\n", active);
  buf_t out = {0};
  buf_add(&out, line);
  buf_add(&out, b.p);
  write_file(SESSION_FILE, out.p, out.len);
  free(b.p);
  free(out.p);
}

/* --- exports: tabs -------------------------------------------------------- */

exports_umbra_browser_browser_tab_id_t exports_umbra_browser_browser_open_tab(
    str_t* text, exports_umbra_browser_browser_size_t* size) {
  ready();
  char* typed = take_str(text);
  tab_t* t = new_tab(typed, *size);
  free(typed);
  return t ? t->id : 0;
}

void exports_umbra_browser_browser_close_tab(
    exports_umbra_browser_browser_tab_id_t id) {
  tab_t* t = tab(id);
  if (t) {
    close_tab(t);
    save_session();
  }
}

bool exports_umbra_browser_browser_restore_session(
    exports_umbra_browser_browser_size_t* size, uint32_t* ret) {
  ready();
  char* text = read_file(SESSION_FILE);
  if (!text) {
    return false;
  }
  size_t active = 0, i = 0;
  uint32_t shown = 0;
  char* save = NULL;
  for (char* line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (strncmp(line, "active\t", 7) == 0) {
      active = (size_t)strtoul(line + 7, NULL, 10);
      continue;
    }
    tab_t* t = new_tab(line, *size);
    if (t && (i == active || !shown)) {
      shown = t->id;
    }
    ++i;
  }
  free(text);
  if (!shown) {
    return false;
  }
  g_active_tab = shown;
  *ret = shown;
  return true;
}

void exports_umbra_browser_browser_set_active_tab(
    exports_umbra_browser_browser_tab_id_t id) {
  if (g_active_tab != id) {
    g_active_tab = id;
    save_session();
  }
}

void exports_umbra_browser_browser_edit(
    exports_umbra_browser_browser_tab_id_t id,
    umbra_chorium_types_edit_command_t command) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_edit(page(t), command);
  }
}

void exports_umbra_browser_browser_save_url(
    exports_umbra_browser_browser_tab_id_t id, str_t* url) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_save_url(page(t), url);
  }
  portable_string_free(url);
}

void exports_umbra_browser_browser_copy_image_at(
    exports_umbra_browser_browser_tab_id_t id, int32_t x, int32_t y) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_copy_image_at(page(t), x, y);
  }
}

void exports_umbra_browser_browser_file_chooser_reply(
    exports_umbra_browser_browser_tab_id_t id, uint32_t chooser,
    portable_list_string_t* paths) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_file_chooser_reply(page(t), chooser,
                                                       paths);
  }
  portable_list_string_free(paths);
}

void exports_umbra_browser_browser_downloads(
    exports_umbra_browser_browser_list_download_t* ret) {
  ret->len = g_download_count;
  ret->ptr = g_download_count ? malloc(sizeof(download_t) * g_download_count)
                              : NULL;
  for (size_t i = 0; i < g_download_count; ++i) {
    ret->ptr[i] = copy_download(&g_downloads[i].d);
  }
}

void exports_umbra_browser_browser_cancel_download(uint32_t id) {
  /* The engine's downloads are global: any page can cancel one, but the
   * one it came from is the natural choice. */
  tab_t* t = NULL;
  for (size_t i = 0; i < g_download_count; ++i) {
    if (g_downloads[i].d.id == id) {
      t = tab(g_downloads[i].tab_id);
    }
  }
  for (int i = 0; !t && i < MAX_TABS; ++i) {
    if (g_tabs[i].used) {
      t = &g_tabs[i];
    }
  }
  if (t) {
    umbra_chorium_pages_method_page_cancel_download(page(t), id);
  }
}

void exports_umbra_browser_browser_set_dark_mode(bool dark) {
  umbra_chorium_pages_set_dark_mode(dark);
}

void exports_umbra_browser_browser_exit_fullscreen(
    exports_umbra_browser_browser_tab_id_t id) {
  tab_t* t = tab(id);
  if (t) {
    str_t js = to_str("document.fullscreenElement && document.exitFullscreen()");
    umbra_chorium_pages_method_page_eval(page(t), 0, &js);
  }
}

void exports_umbra_browser_browser_tabs(
    exports_umbra_browser_browser_list_tab_info_t* ret) {
  /* Slots get reused, ids don't: sort by id for "oldest first". */
  tab_t* order[MAX_TABS];
  size_t n = ordered_tabs(order);
  ret->len = n;
  ret->ptr = n ? malloc(sizeof(tab_info_t) * n) : NULL;
  for (size_t i = 0; i < n; ++i) {
    fill_info(order[i], &ret->ptr[i]);
  }
}

bool exports_umbra_browser_browser_info(
    exports_umbra_browser_browser_tab_id_t id, tab_info_t* ret) {
  tab_t* t = tab(id);
  if (!t) {
    return false;
  }
  fill_info(t, ret);
  return true;
}

void exports_umbra_browser_browser_navigate(
    exports_umbra_browser_browser_tab_id_t id, str_t* text) {
  ready();
  char* typed = take_str(text);
  tab_t* t = tab(id);
  if (t) {
    char* url = resolve(typed);
    load(t, url);
    free(url);
  }
  free(typed);
}

void exports_umbra_browser_browser_resolve(str_t* text, str_t* ret) {
  ready();
  char* typed = take_str(text);
  char* url = resolve(typed);
  *ret = to_str(url);
  free(url);
  free(typed);
}

#define TAB_OR_RETURN(id)  \
  tab_t* t = tab(id);      \
  if (!t) {                \
    return;                \
  }

void exports_umbra_browser_browser_back(
    exports_umbra_browser_browser_tab_id_t id) {
  TAB_OR_RETURN(id);
  umbra_chorium_pages_method_page_back(page(t));
}

void exports_umbra_browser_browser_forward(
    exports_umbra_browser_browser_tab_id_t id) {
  TAB_OR_RETURN(id);
  umbra_chorium_pages_method_page_forward(page(t));
}

void exports_umbra_browser_browser_reload(
    exports_umbra_browser_browser_tab_id_t id) {
  TAB_OR_RETURN(id);
  umbra_chorium_pages_method_page_reload(page(t));
}

void exports_umbra_browser_browser_stop(
    exports_umbra_browser_browser_tab_id_t id) {
  TAB_OR_RETURN(id);
  umbra_chorium_pages_method_page_stop(page(t));
}

void exports_umbra_browser_browser_resize(
    exports_umbra_browser_browser_tab_id_t id,
    exports_umbra_browser_browser_size_t* size) {
  TAB_OR_RETURN(id);
  if (!size->width || !size->height ||
      (size->width == t->size.width && size->height == t->size.height)) {
    return;
  }
  t->size = *size;
  umbra_chorium_pages_method_page_resize(page(t), &t->size);
}

void exports_umbra_browser_browser_send_input(
    exports_umbra_browser_browser_tab_id_t id,
    exports_umbra_browser_browser_input_t* event) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_send_input(page(t), event);
  }
  exports_umbra_browser_browser_input_free(event);
}

void exports_umbra_browser_browser_draw(
    exports_umbra_browser_browser_tab_id_t id,
    exports_umbra_browser_browser_rect_t* dest) {
  TAB_OR_RETURN(id);
  umbra_chorium_pages_method_page_draw(page(t), dest);
}

void exports_umbra_browser_browser_dialog_reply(
    exports_umbra_browser_browser_tab_id_t id, uint32_t dialog_id,
    bool accept, str_t* maybe_text) {
  tab_t* t = tab(id);
  if (t) {
    umbra_chorium_pages_method_page_dialog_reply(page(t), dialog_id, accept,
                                                 maybe_text);
  }
  if (maybe_text) {
    portable_string_free(maybe_text);
  }
}

/* --- exports: events ------------------------------------------------------ */

static void add_event(exports_umbra_browser_browser_list_browser_event_t* list,
                      event_t e) {
  list->ptr = realloc(list->ptr, sizeof(event_t) * (list->len + 1));
  list->ptr[list->len++] = e;
}

static event_t changed(const tab_t* t) {
  event_t e = {.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CHANGED};
  fill_info(t, &e.val.changed);
  return e;
}

void exports_umbra_browser_browser_handle_page_event(
    uint32_t page_id, exports_umbra_browser_browser_page_event_t* ev,
    exports_umbra_browser_browser_list_browser_event_t* ret) {
  ret->ptr = NULL;
  ret->len = 0;
  tab_t* t = tab_for_page(page_id);
  if (!t) {
    exports_umbra_browser_browser_page_event_free(ev);
    return;
  }
  event_t e = {0};
  switch (ev->tag) {
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_NAVIGATED: {
      char* url = from_str(&ev->val.navigated.url);
      set(&t->url, url);
      free(url);
      set(&t->error, NULL);
      t->can_go_back = ev->val.navigated.can_go_back;
      t->can_go_forward = ev->val.navigated.can_go_forward;
      add_event(ret, changed(t));
      save_session();
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_TITLE: {
      char* title = from_str(&ev->val.title);
      set(&t->title, title);
      free(title);
      visit_t* v = worth_remembering(t->url) ? find_visit(t->url) : NULL;
      if (v) {
        set(&v->title, t->title);
      }
      add_event(ret, changed(t));
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_LOADING:
      t->loading = ev->val.loading;
      add_event(ret, changed(t));
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_LOADED: {
      char* url = from_str(&ev->val.loaded.url);
      if (ev->val.loaded.ok) {
        set(&t->error, NULL);
        if (worth_remembering(url)) {
          remember(url, t->title, 1, (uint64_t)time(NULL));
          save_history();
        }
      } else {
        char* error = from_str(&ev->val.loaded.error);
        set(&t->error, error);
        set(&t->url, url); /* show what failed, not chrome-error:// */
        free(error);
      }
      free(url);
      add_event(ret, changed(t));
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_NEW_WINDOW: {
      char* url = from_str(&ev->val.new_window);
      tab_t* opened = new_tab(url, t->size);
      free(url);
      if (opened) {
        e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_OPENED;
        fill_info(opened, &e.val.opened);
        add_event(ret, e);
      }
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_DIALOG: {
      /* Hand the dialog's strings over instead of copying them. */
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_DIALOG;
      e.val.dialog.f0 = t->id;
      e.val.dialog.f1 = ev->val.dialog;
      memset(&ev->val.dialog, 0, sizeof ev->val.dialog);
      add_event(ret, e);
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_CRASHED:
      umbra_chorium_pages_method_page_reload(page(t));
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CRASHED;
      e.val.crashed = t->id;
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_CLOSED: {
      uint32_t id = t->id;
      close_tab(t);
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CLOSED;
      e.val.closed = id;
      add_event(ret, e);
      break;
    }
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_FRAME:
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_FRAME;
      e.val.frame = t->id;
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_FULLSCREEN:
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_FULLSCREEN;
      e.val.fullscreen.f0 = t->id;
      e.val.fullscreen.f1 = ev->val.fullscreen;
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_OVERSCROLL:
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_OVERSCROLL;
      e.val.overscroll.f0 = t->id;
      e.val.overscroll.f1 = ev->val.overscroll;
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_CONTEXT_MENU:
      /* Hand the strings over, as for dialogs. */
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_CONTEXT_MENU;
      e.val.context_menu.f0 = t->id;
      e.val.context_menu.f1 = ev->val.context_menu;
      memset(&ev->val.context_menu, 0, sizeof ev->val.context_menu);
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_DOWNLOAD:
      track_download(&ev->val.download, t->id);
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_DOWNLOAD;
      e.val.download = copy_download(&ev->val.download);
      add_event(ret, e);
      break;
    case UMBRA_CHORIUM_TYPES_PAGE_EVENT_FILE_CHOOSER:
      e.tag = EXPORTS_UMBRA_BROWSER_BROWSER_BROWSER_EVENT_FILE_CHOOSER;
      e.val.file_chooser.f0 = t->id;
      e.val.file_chooser.f1 = ev->val.file_chooser;
      memset(&ev->val.file_chooser, 0, sizeof ev->val.file_chooser);
      add_event(ret, e);
      break;
    default:
      /* eval-result: nothing in the browser interface yet. */
      break;
  }
  exports_umbra_browser_browser_page_event_free(ev);
}

/* --- exports: history, settings, Tor -------------------------------------- */

void exports_umbra_browser_browser_history(
    uint32_t limit, exports_umbra_browser_browser_list_history_entry_t* ret) {
  ready();
  size_t n = g_history_len < limit ? g_history_len : limit;
  /* Most recent first: pick the n newest by insertion sort on an index. */
  size_t* order = malloc(sizeof(size_t) * (g_history_len ? g_history_len : 1));
  for (size_t i = 0; i < g_history_len; ++i) {
    size_t j = i;
    while (j > 0 && g_history[order[j - 1]].last_visit <
                        g_history[i].last_visit) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = i;
  }
  ret->len = n;
  ret->ptr = n ? malloc(sizeof(*ret->ptr) * n) : NULL;
  for (size_t i = 0; i < n; ++i) {
    const visit_t* v = &g_history[order[i]];
    ret->ptr[i].url = to_str(v->url);
    ret->ptr[i].title = to_str(v->title);
    ret->ptr[i].visits = v->visits;
    ret->ptr[i].last_visit = v->last_visit;
  }
  free(order);
}

void exports_umbra_browser_browser_clear_history(void) {
  ready();
  while (g_history_len) {
    forget(g_history_len - 1);
  }
  str_t n;
  portable_string_set(&n, HISTORY_FILE);
  str_t err;
  if (!umbra_chorium_files_method_dir_remove(dir(), &n, &err)) {
    portable_string_free(&err); /* nothing saved yet */
  }
}

void exports_umbra_browser_browser_clear_site_data(void) {
  umbra_chorium_net_clear_site_data();
}

bool exports_umbra_browser_browser_get_setting(str_t* key, str_t* ret) {
  ready();
  char* k = take_str(key);
  setting_t* s = find_setting(k);
  free(k);
  if (!s) {
    return false;
  }
  *ret = to_str(s->value);
  return true;
}

void exports_umbra_browser_browser_set_setting(str_t* key, str_t* value) {
  ready();
  char* k = take_str(key);
  char* v = take_str(value);
  if (k[0] && !strchr(k, '=')) {
    put_setting(k, v);
    save_settings();
    if (strcmp(k, "clear-site-data-on-close") == 0) {
      umbra_chorium_net_set_clear_site_data_on_close(strcmp(v, "true") == 0);
    }
  }
  free(k);
  free(v);
}

int32_t exports_umbra_browser_browser_tor_progress(void) {
  return umbra_chorium_net_tor_progress();
}

void exports_umbra_browser_browser_set_tor(bool on) {
  umbra_chorium_net_set_tor(on);
}

bool exports_umbra_browser_browser_tor_enabled(void) {
  return umbra_chorium_net_tor_enabled();
}
