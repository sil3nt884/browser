/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Runs the browser app (browser.wasm: apps/browser plugged into
 * chorium.wasm) on the real runtime core with a fake engine and screen, and
 * checks what it does: the same code both runtimes run, without Chromium.
 *
 *   app_tests <browser.wasm> [scenario name filter]
 *
 * Every scenario runs in a desktop window (title bar drawn by the app) and
 * on a phone-sized screen. Exit code: the number of failed checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fakes.h"

#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B

static int g_failed, g_checks;
static const char* g_scenario;
static bool g_phone;
static const char* g_wasm;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failed;                                                       \
      fprintf(stderr, "  FAIL %s (%s): %s  [line %d]\n", g_scenario,     \
              g_phone ? "phone" : "desktop", #cond, __LINE__);          \
      dump_calls();                                                     \
    }                                                                   \
  } while (0)

static char g_dir[256];

/* A fresh app; |keep| keeps the app's files of the previous start. */
static bool start(bool keep, bool dark) {
  if (!keep) {
    snprintf(g_dir, sizeof g_dir, "/tmp/chorium-app-tests-%d-%d", getpid(),
             rand());
  }
  screen_t s = {g_phone ? 400 : 1280, g_phone ? 840 : 800, !g_phone, dark};
  fakes_reset(s, g_dir);
  if (!contract_load(g_wasm)) {
    fprintf(stderr, "  can't load %s: %s\n", g_wasm, contract_last_error());
    ++g_failed;
    return false;
  }
  char* argv[] = {"browser.wasm"};
  app_start(1, argv);
  pump();
  return true;
}

static void stop(void) {
  app_call("on-stop");
  pages_close_all();
  contract_unload();
}

/* --- scenarios ------------------------------------------------------------ */

static void starts_on_home_page(void) {
  start(false, false);
  CHECK(open_pages() == 1);
  CHECK(called("load_url") && strstr(last_call("load_url"), "duckduckgo.com"));
  CHECK(!called("crashed"));
  stop();
}

static void address_bar_navigates(void) {
  start(false, false);
  clear_calls();
  press_key('L', MOD_CTRL);
  type_text("example.com");
  press_key(VK_RETURN, 0);
  CHECK(called("load_url") && strstr(last_call("load_url"), "example.com"));
  /* Words go to the search engine. */
  press_key('L', MOD_CTRL);
  type_text("chorium tests");
  press_key(VK_RETURN, 0);
  CHECK(strstr(last_call("load_url"), "duckduckgo.com") &&
        strstr(last_call("load_url"), "chorium"));
  stop();
}

static void address_bar_clipboard(void) {
  start(false, false);
  /* Ctrl+L selects the address; Ctrl+C copies it. */
  press_key('L', MOD_CTRL);
  press_key('C', MOD_CTRL);
  CHECK(strstr(clipboard(), "duckduckgo.com") != NULL);
  /* Ctrl+V replaces the selection, on one line. */
  set_clipboard("pasted\nwords");
  press_key('L', MOD_CTRL);
  press_key('V', MOD_CTRL);
  clear_calls();
  press_key(VK_RETURN, 0);
  CHECK(called("load_url") && strstr(last_call("load_url"), "pasted") &&
        strstr(last_call("load_url"), "words") &&
        !strstr(last_call("load_url"), "\n"));
  /* Ctrl+X empties the field. */
  press_key('L', MOD_CTRL);
  press_key('X', MOD_CTRL);
  CHECK(strstr(clipboard(), "pasted") != NULL);
  stop();
}

static void address_bar_edits_characters(void) {
  start(false, false);
  press_key('L', MOD_CTRL);
  type_text("https://example.com/inZZdex");
  press_key(0x23, 0); /* End */
  for (int i = 0; i < 3; ++i) press_key(0x25, 0);
  press_key(0x25, MOD_SHIFT);
  press_key(0x25, MOD_SHIFT);
  press_key('C', MOD_CTRL);
  CHECK(strcmp(clipboard(), "ZZ") == 0);
  type_text("de");
  press_key('A', MOD_CTRL);
  press_key('C', MOD_CTRL);
  CHECK(strcmp(clipboard(), "https://example.com/indedex") == 0);
  /* Home/End and Delete operate at the caret, not at the end. */
  press_key(0x24, 0);
  press_key(0x2E, MOD_CTRL);
  type_text("http://");
  press_key('A', MOD_CTRL);
  press_key('C', MOD_CTRL);
  CHECK(strcmp(clipboard(), "http://example.com/indedex") == 0);
  /* UTF-8 movement and deletion must preserve complete characters. */
  type_text("a😀éz");
  press_key(0x25, 0);
  press_key(0x25, 0);
  press_key(0x08, 0);
  type_text("中");
  press_key('A', MOD_CTRL);
  press_key('C', MOD_CTRL);
  CHECK(strcmp(clipboard(), "a中éz") == 0);
  press_key(0x24, 0);
  press_key(0x27, MOD_SHIFT);
  press_key('X', MOD_CTRL);
  CHECK(strcmp(clipboard(), "a") == 0);
  set_clipboard("start\n");
  press_key('V', MOD_CTRL);
  press_key('A', MOD_CTRL);
  press_key('C', MOD_CTRL);
  CHECK(strcmp(clipboard(), "start 中éz") == 0);
  stop();
}

/* chorium.h's CHORIUM_EDIT_FLAG bits. */
#define EDIT_CUT 1
#define EDIT_COPY 2
#define EDIT_PASTE 4
#define EDIT_SELECT_ALL 8

static void context_menu_text_field(void) {
  if (g_phone) {
    return; /* Android keeps the system's own text menu */
  }
  start(false, false);
  uint32_t page = newest_page();
  char json[512];
  snprintf(json, sizeof json,
           "{\"type\":\"context_menu\",\"x\":100,\"y\":120,\"link_url\":\"\","
           "\"link_text\":\"\",\"image_url\":\"\",\"selection\":\"\","
           "\"editable\":true,\"edit\":%d}",
           EDIT_CUT | EDIT_COPY | EDIT_PASTE | EDIT_SELECT_ALL);
  engine_event(page, json);
  CHECK(shown_exactly("Cut") && shown_exactly("Copy") &&
        shown_exactly("Paste") && shown_exactly("Select all"));
  CHECK(!shown("Open in new tab"));
  clear_calls();
  CHECK(click_text("Paste"));
  char want[64];
  snprintf(want, sizeof want, "edit %u 2", page);
  CHECK(called(want));
  CHECK(!shown_exactly("Paste")); /* the menu closed */

  /* Only what the field allows right now: nothing selected, nothing to
   * paste. */
  snprintf(json, sizeof json,
           "{\"type\":\"context_menu\",\"x\":100,\"y\":120,\"link_url\":\"\","
           "\"link_text\":\"\",\"image_url\":\"\",\"selection\":\"\","
           "\"editable\":true,\"edit\":%d}",
           EDIT_SELECT_ALL);
  engine_event(page, json);
  CHECK(shown_exactly("Select all") && !shown_exactly("Cut") &&
        !shown_exactly("Paste"));
  clear_calls();
  CHECK(click_text("Select all"));
  snprintf(want, sizeof want, "edit %u 3", page);
  CHECK(called(want));
  stop();
}

static void context_menu_link(void) {
  start(false, false);
  uint32_t page = newest_page();
  engine_event(page,
               "{\"type\":\"context_menu\",\"x\":50,\"y\":60,"
               "\"link_url\":\"https://example.com/file.zip\","
               "\"link_text\":\"the file\",\"image_url\":\"\","
               "\"selection\":\"\",\"editable\":false,\"edit\":0}");
  CHECK(shown("Open in new tab") && shown("Copy link address") &&
        shown("Copy link text") && shown("Save link as file"));
  clear_calls();
  CHECK(click_text("Save link as file"));
  CHECK(called("save_url") &&
        strstr(last_call("save_url"), "https://example.com/file.zip"));

  engine_event(page,
               "{\"type\":\"context_menu\",\"x\":50,\"y\":60,"
               "\"link_url\":\"https://example.com/a\",\"link_text\":\"A\","
               "\"image_url\":\"\",\"selection\":\"\",\"editable\":false,"
               "\"edit\":0}");
  CHECK(click_text("Copy link address"));
  CHECK(strcmp(clipboard(), "https://example.com/a") == 0);

  int pages = open_pages();
  engine_event(page,
               "{\"type\":\"context_menu\",\"x\":50,\"y\":60,"
               "\"link_url\":\"https://example.com/b\",\"link_text\":\"B\","
               "\"image_url\":\"\",\"selection\":\"\",\"editable\":false,"
               "\"edit\":0}");
  CHECK(click_text("Open in new tab"));
  CHECK(open_pages() == pages + 1);
  CHECK(strstr(last_call("load_url"), "https://example.com/b") != NULL);
  stop();
}

static void context_menu_image(void) {
  start(false, false);
  uint32_t page = newest_page();
  const char* json =
      "{\"type\":\"context_menu\",\"x\":210,\"y\":330,\"link_url\":\"\","
      "\"link_text\":\"\",\"image_url\":\"https://example.com/cat.png\","
      "\"selection\":\"\",\"editable\":false,\"edit\":0}";
  engine_event(page, json);
  CHECK(shown_exactly("Copy image") && shown("Copy image address") &&
        shown("Save image") && shown("Open image in new tab"));
  clear_calls();
  CHECK(click_text_exactly("Copy image"));
  /* Copy image asks the engine, at the page point the menu was for. */
  char want[64];
  snprintf(want, sizeof want, "copy_image_at %u 210 330", page);
  CHECK(called(want));

  engine_event(page, json);
  CHECK(click_text("Save image"));
  CHECK(called("save_url") &&
        strstr(last_call("save_url"), "https://example.com/cat.png"));

  engine_event(page, json);
  CHECK(click_text("Copy image address"));
  CHECK(strcmp(clipboard(), "https://example.com/cat.png") == 0);
  stop();
}

static void context_menu_selection(void) {
  if (g_phone) {
    return; /* Android's own selection menu */
  }
  start(false, false);
  engine_event(newest_page(),
               "{\"type\":\"context_menu\",\"x\":5,\"y\":5,\"link_url\":\"\","
               "\"link_text\":\"\",\"image_url\":\"\","
               "\"selection\":\"some words\",\"editable\":false,\"edit\":0}");
  CHECK(shown("Copy") && shown("Search"));
  CHECK(click_text("Copy"));
  CHECK(strcmp(clipboard(), "some words") == 0);
  stop();
}

static void download_notice(void) {
  start(false, false);
  uint32_t page = newest_page();
  engine_event(page,
               "{\"type\":\"download\",\"id\":4,\"file_name\":\"big.zip\","
               "\"path\":\"/dl/big.zip\",\"url\":\"https://e.com/big.zip\","
               "\"received\":524288,\"total\":1048576,"
               "\"state\":\"in_progress\"}");
  CHECK(shown("big.zip"));
  CHECK(shown("512 KB of 1.0 MB"));
  engine_event(page,
               "{\"type\":\"download\",\"id\":4,\"file_name\":\"big.zip\","
               "\"path\":\"/dl/big.zip\",\"url\":\"https://e.com/big.zip\","
               "\"received\":1048576,\"total\":1048576,\"state\":\"done\"}");
  CHECK(shown("1.0 MB, done"));
  clear_calls();
  CHECK(click_icon(ICON_FOLDER));
  CHECK(called("open_file 1 /dl/big.zip"));
  CHECK(click_text("big.zip"));
  CHECK(called("open_file 0 /dl/big.zip"));
  /* The notice's close button is the last X drawn. */
  CHECK(click_icon(ICON_CLOSE));
  CHECK(!shown("big.zip"));
  stop();
}

static void downloads_panel(void) {
  start(false, false);
  uint32_t page = newest_page();
  engine_event(page,
               "{\"type\":\"download\",\"id\":1,\"file_name\":\"done.pdf\","
               "\"path\":\"/dl/done.pdf\",\"url\":\"https://e.com/d\","
               "\"received\":2048,\"total\":2048,\"state\":\"done\"}");
  engine_event(page,
               "{\"type\":\"download\",\"id\":2,\"file_name\":\"slow.iso\","
               "\"path\":\"/dl/slow.iso\",\"url\":\"https://e.com/s\","
               "\"received\":10,\"total\":0,\"state\":\"in_progress\"}");
  engine_event(page,
               "{\"type\":\"download\",\"id\":3,\"file_name\":\"bad.bin\","
               "\"path\":\"/dl/bad.bin\",\"url\":\"https://e.com/b\","
               "\"received\":0,\"total\":0,\"state\":\"failed\"}");
  /* From the menu. */
  CHECK(click_icon(ICON_MENU));
  CHECK(click_text("Downloads"));
  CHECK(shown_exactly("Downloads"));
  CHECK(shown("done.pdf") && shown("slow.iso") && shown("bad.bin"));
  CHECK(shown("Failed"));
  /* Newest first: the running one has a cancel button (an X). */
  clear_calls();
  CHECK(click_icon(ICON_CLOSE));
  CHECK(called("cancel_download 2"));
  CHECK(click_text("done.pdf"));
  CHECK(called("open_file 0 /dl/done.pdf"));
  CHECK(click_icon(ICON_FOLDER));
  CHECK(called("open_file 1 /dl/done.pdf"));
  stop();

  if (!g_phone) {
    start(false, false);
    press_key('J', MOD_CTRL);
    CHECK(shown_exactly("Downloads"));
    CHECK(shown("Files you download will show here."));
    stop();
  }
}

static void file_chooser(void) {
  start(false, false);
  uint32_t page = newest_page();
  clear_calls();
  engine_event(page,
               "{\"type\":\"file_chooser\",\"id\":5,\"mode\":\"open_multiple\","
               "\"accept\":\".png\\nimage/*\",\"default_name\":\"\"}");
  CHECK(called("pick_files"));
  const char* pick = last_call("pick_files");
  unsigned request = 0;
  int mode = -1;
  char types[256] = "";
  if (pick) {
    sscanf(pick, "pick_files %u %d %255s", &request, &mode, types);
  }
  CHECK(mode == 1 && strcmp(types, ".png|image/*") == 0);

  const char* paths[] = {"/home/a.png", "/home/b.png"};
  app_files_picked(request, paths, 2);
  pump();
  char want[128];
  snprintf(want, sizeof want, "file_chooser_reply %u 5 2 /home/a.png|/home/b.png",
           page);
  CHECK(called(want));

  /* A late or unknown answer goes nowhere; a cancel answers with nothing. */
  clear_calls();
  app_files_picked(request, paths, 1);
  pump();
  CHECK(!called("file_chooser_reply"));
  engine_event(page,
               "{\"type\":\"file_chooser\",\"id\":6,\"mode\":\"save\","
               "\"accept\":\"\",\"default_name\":\"notes.txt\"}");
  pick = last_call("pick_files");
  if (pick) {
    sscanf(pick, "pick_files %u %d", &request, &mode);
  }
  CHECK(mode == 3);
  app_files_picked(request, NULL, 0);
  pump();
  snprintf(want, sizeof want, "file_chooser_reply %u 6 0", page);
  CHECK(called(want));
  stop();
}

static void tor_switch(void) {
  start(false, false);
  CHECK(shown_exactly("Tor"));
  CHECK(click_icon(ICON_MENU));
  clear_calls();
  /* The menu's Tor row has a switch: the last "Tor" drawn. */
  CHECK(click_text("Tor"));
  CHECK(called("set_tor 0"));
  pump();
  app_call("on-draw");
  CHECK(shown_exactly("Direct"));
  CHECK(click_icon(ICON_MENU));
  CHECK(click_text("Tor"));
  CHECK(called("set_tor 1"));
  stop();
}

static void dark_mode_follows_system(void) {
  start(false, true);
  CHECK(called("set_dark_mode 1"));
  stop();
  start(false, false);
  CHECK(!called("set_dark_mode 1"));
  stop();
}

static void site_data_settings(void) {
  start(false, false);
  CHECK(called("set_clear_on_close 0"));
  CHECK(click_icon(ICON_MENU));
  CHECK(click_text_exactly("Settings"));
  static const char* const fields[] = {"x", "y", "dx", "dy", "modifiers"};
  static const char* const modifiers[] = {"shift", "ctrl", "alt", "meta"};
  val_t values[] = {v_s32(250), v_s32(500), v_f32(0), v_f32(2000),
                    v_flags(modifiers, 4, 0)};
  val_t wheel = v_record(5, fields, values);
  app_input(v_variant("wheel", &wheel));
  pump();
  int x, y;
  CHECK(find_text("Clear browsing history", &x, &y));
  click(x + (g_phone ? 376 : 680) - 81, y);
  CHECK(find_text("Clear cookies and site data", &x, &y));
  clear_calls();
  click(x + (g_phone ? 376 : 680) - 81, y);
  CHECK(called("clear_site_data"));
  CHECK(click_text_exactly("Clear when Chorium closes"));
  CHECK(called("set_clear_on_close 1"));
  clear_calls();
  stop();
  CHECK(called("clear_site_data"));
  start(true, false);
  CHECK(called("set_clear_on_close 1"));
  stop();
}

static void session_restore(void) {
  start(false, false);
  press_key('L', MOD_CTRL);
  type_text("first.example");
  press_key(VK_RETURN, 0);
  /* A second tab, from a link. */
  engine_event(newest_page(),
               "{\"type\":\"new_window\",\"url\":\"https://second.example/\","
               "\"user_gesture\":true}");
  CHECK(open_pages() == 2);
  stop();

  start(true, false); /* same app files: a restart */
  CHECK(open_pages() == 2);
  CHECK(count_calls("load_url") == 2);
  CHECK(any_call_contains("load_url", "first.example"));
  CHECK(any_call_contains("load_url", "https://second.example/"));
  stop();
}

static void new_window_opens_tab(void) {
  start(false, false);
  int before = open_pages();
  clear_calls();
  engine_event(newest_page(),
               "{\"type\":\"new_window\",\"url\":\"https://popup.example/\","
               "\"user_gesture\":true}");
  CHECK(open_pages() == before + 1);
  CHECK(called("load_url") &&
        strstr(last_call("load_url"), "https://popup.example/"));
  CHECK(shown("popup.example")); /* the app follows the requested tab */
  before = open_pages();
  clear_calls();
  engine_event(newest_page(), "{\"type\":\"new_window\",\"url\":\"\"}");
  CHECK(open_pages() == before + 1);
  CHECK(!called("load_url")); /* window.open() stays blank */
  stop();
}

static void fullscreen_hides_ui(void) {
  start(false, false);
  uint32_t page = newest_page();
  CHECK(shown("duckduckgo"));
  engine_event(page, "{\"type\":\"fullscreen\",\"on\":true}");
  CHECK(fullscreen());
  CHECK(!shown("duckduckgo")); /* no address bar */
  engine_event(page, "{\"type\":\"fullscreen\",\"on\":false}");
  CHECK(!fullscreen());
  CHECK(shown("duckduckgo"));
  stop();
}

static void dialogs_answered(void) {
  start(false, false);
  uint32_t page = newest_page();
  clear_calls();
  engine_event(page,
               "{\"type\":\"dialog\",\"id\":7,\"kind\":\"alert\","
               "\"message\":\"hi\",\"default\":\"\"}");
  char want[64];
  snprintf(want, sizeof want, "dialog_reply %u 7 1", page);
  CHECK(called(want));
  stop();
}

static void back_and_gestures(void) {
  start(false, false);
  uint32_t page = newest_page();
  press_key('L', MOD_CTRL);
  type_text("a.example");
  press_key(VK_RETURN, 0);
  clear_calls();
  press_back();
  char want[64];
  snprintf(want, sizeof want, "back %u", page);
  CHECK(called(want));
  if (g_phone) {
    /* Pull to refresh. */
    clear_calls();
    engine_event(page, "{\"type\":\"overscroll\",\"kind\":\"refresh\","
                       "\"phase\":\"start\",\"dx\":0,\"dy\":0}");
    engine_event(page, "{\"type\":\"overscroll\",\"kind\":\"refresh\","
                       "\"phase\":\"pull\",\"dx\":0,\"dy\":400}");
    engine_event(page, "{\"type\":\"overscroll\",\"kind\":\"refresh\","
                       "\"phase\":\"release\",\"dx\":0,\"dy\":0}");
    snprintf(want, sizeof want, "reload %u", page);
    CHECK(called(want));
  }
  stop();
}

static void history_lists_visits(void) {
  start(false, false);
  press_key('L', MOD_CTRL);
  type_text("visited.example");
  press_key(VK_RETURN, 0);
  CHECK(click_icon(ICON_MENU));
  CHECK(click_text("History"));
  CHECK(shown("visited.example"));
  stop();
}

static void crash_is_survived(void) {
  start(false, false);
  engine_event(newest_page(), "{\"type\":\"crashed\"}");
  app_call("on-draw");
  CHECK(!called("crashed")); /* the app itself keeps running */
  stop();
}

/* --- runner --------------------------------------------------------------- */

typedef struct {
  const char* name;
  void (*run)(void);
} scenario_t;

static const scenario_t kScenarios[] = {
    {"starts_on_home_page", starts_on_home_page},
    {"address_bar_navigates", address_bar_navigates},
    {"address_bar_clipboard", address_bar_clipboard},
    {"address_bar_edits_characters", address_bar_edits_characters},
    {"context_menu_text_field", context_menu_text_field},
    {"context_menu_link", context_menu_link},
    {"context_menu_image", context_menu_image},
    {"context_menu_selection", context_menu_selection},
    {"download_notice", download_notice},
    {"downloads_panel", downloads_panel},
    {"file_chooser", file_chooser},
    {"tor_switch", tor_switch},
    {"dark_mode_follows_system", dark_mode_follows_system},
    {"site_data_settings", site_data_settings},
    {"session_restore", session_restore},
    {"new_window_opens_tab", new_window_opens_tab},
    {"fullscreen_hides_ui", fullscreen_hides_ui},
    {"dialogs_answered", dialogs_answered},
    {"back_and_gestures", back_and_gestures},
    {"history_lists_visits", history_lists_visits},
    {"crash_is_survived", crash_is_survived},
};

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: app_tests <browser.wasm> [filter]\n");
    return 2;
  }
  g_wasm = argv[1];
  const char* filter = argc > 2 ? argv[2] : NULL;
  srand((unsigned)getpid());
  int ran = 0;
  for (size_t i = 0; i < sizeof kScenarios / sizeof kScenarios[0]; ++i) {
    if (filter && !strstr(kScenarios[i].name, filter)) {
      continue;
    }
    g_scenario = kScenarios[i].name;
    for (int phone = 0; phone < 2; ++phone) {
      g_phone = phone;
      int before = g_failed;
      kScenarios[i].run();
      printf("%s %s (%s)\n", g_failed == before ? "ok  " : "FAIL", g_scenario,
             phone ? "phone" : "desktop");
    }
    ++ran;
  }
  printf("\n%d scenarios, %d checks, %d failed\n", ran, g_checks, g_failed);
  return g_failed ? 1 : 0;
}
