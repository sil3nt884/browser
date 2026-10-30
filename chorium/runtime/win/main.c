/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 *   Chorium.exe [app.wasm] [args for the app...]
 *
 * Opens a window, starts Chorium (Chromium + Tor) and runs the app in it.
 * Chromium's helper processes are this same .exe, so main() first hands
 * them to chorium_execute_process().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windowsx.h>

#include "host.h"

HWND g_hwnd;
float g_scale = 1.0f;
host_page g_pages[MAX_PAGES + 1];

static const char* const kModifiers[] = {"shift", "ctrl", "alt", "meta"};
static int g_last_tor = -2;
static uint32_t g_clicks; /* click count of the last button press */
static bool g_tracking_leave;
static bool g_minimized;
static wchar_t g_high_surrogate;

/* --- title bar ---------------------------------------------------------- */

/* The app draws the title bar (Chrome-style tabs in it); this file removes
 * the system one and makes the app's work: dragging, double-click to
 * maximise, the window buttons (with Windows 11 snap layouts on maximise)
 * and resizing from the top edge. Sizes in CSS pixels. */
#define TITLE_BAR_HEIGHT 40
#define WINDOW_BUTTON_WIDTH 46
#define MAX_DRAG_AREAS 16

static RECT g_drag[MAX_DRAG_AREAS]; /* CSS pixels, from the app */
static size_t g_drag_count;
static bool g_tracking_nc_leave;
static WPARAM g_nc_pressed; /* window button under a press, or 0 */

bool title_bar_layout(uint32_t* height, uint32_t* buttons_width,
                      bool* maximized) {
  *height = TITLE_BAR_HEIGHT;
  *buttons_width = 3 * WINDOW_BUTTON_WIDTH;
  *maximized = IsZoomed(g_hwnd) != 0;
  return true;
}

void title_bar_set_drag_areas(const int* xywh, size_t count) {
  g_drag_count = count < MAX_DRAG_AREAS ? count : MAX_DRAG_AREAS;
  for (size_t i = 0; i < g_drag_count; ++i) {
    g_drag[i].left = xywh[i * 4];
    g_drag[i].top = xywh[i * 4 + 1];
    g_drag[i].right = xywh[i * 4] + xywh[i * 4 + 2];
    g_drag[i].bottom = xywh[i * 4 + 1] + xywh[i * 4 + 3];
  }
}

/* The resize frame Windows would put around the window, in device pixels. */
static int frame_thickness(void) {
  UINT dpi = GetDpiForWindow(g_hwnd);
  return GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

/* What a client-area point (device pixels) is, in title-bar terms. */
static LRESULT title_bar_hit(POINT pt) {
  RECT client;
  GetClientRect(g_hwnd, &client);
  if (!IsZoomed(g_hwnd) && pt.y < frame_thickness()) {
    int edge = frame_thickness() * 2;
    if (pt.x < edge) return HTTOPLEFT;
    if (pt.x >= client.right - edge) return HTTOPRIGHT;
    return HTTOP;
  }
  int x = (int)((float)pt.x / g_scale), y = (int)((float)pt.y / g_scale);
  int width = (int)((float)client.right / g_scale);
  if (y >= TITLE_BAR_HEIGHT) {
    return HTCLIENT;
  }
  int buttons_left = width - 3 * WINDOW_BUTTON_WIDTH;
  if (x >= buttons_left) {
    int i = (x - buttons_left) / WINDOW_BUTTON_WIDTH;
    return i == 0 ? HTMINBUTTON : i == 1 ? HTMAXBUTTON : HTCLOSE;
  }
  POINT css = {x, y};
  for (size_t i = 0; i < g_drag_count; ++i) {
    if (PtInRect(&g_drag[i], css)) {
      return HTCAPTION;
    }
  }
  return HTCLIENT;
}

static bool is_window_button(WPARAM hit) {
  return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

/* --- input -------------------------------------------------------------- */

static uint32_t modifier_bits(void) {
  uint32_t bits = 0;
  if (GetKeyState(VK_SHIFT) < 0) bits |= 1;
  if (GetKeyState(VK_CONTROL) < 0) bits |= 2;
  if (GetKeyState(VK_MENU) < 0) bits |= 4;
  if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) bits |= 8;
  return bits;
}

static val_t modifiers(void) { return v_flags(kModifiers, 4, modifier_bits()); }

static int css(int device_px) { return (int)((float)device_px / g_scale); }

static void send_pointer(const char* kind, LPARAM lp, const char* button,
                         uint32_t clicks) {
  static const char* const names[] = {"x", "y", "button", "click-count",
                                      "modifiers"};
  val_t fields[] = {v_s32(css(GET_X_LPARAM(lp))), v_s32(css(GET_Y_LPARAM(lp))),
                    v_enum(button), v_u32(clicks), modifiers()};
  val_t pointer = v_record(5, names, fields);
  app_input(v_variant(kind, &pointer));
}

static const char* held_button(WPARAM wp) {
  if (wp & MK_LBUTTON) return "left";
  if (wp & MK_RBUTTON) return "right";
  if (wp & MK_MBUTTON) return "middle";
  return "none";
}

static void send_wheel(HWND hwnd, WPARAM wp, LPARAM lp, bool horizontal) {
  static const char* const names[] = {"x", "y", "dx", "dy", "modifiers"};
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(hwnd, &pt);
  /* One notch = 100 CSS pixels, like Chrome on Windows. A wheel turned away
   * from the user (positive delta) scrolls up. */
  float notches = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
  float dx = horizontal ? notches * 100.0f : 0.0f;
  float dy = horizontal ? 0.0f : -notches * 100.0f;
  val_t fields[] = {v_s32(css(pt.x)), v_s32(css(pt.y)), v_f32(dx), v_f32(dy),
                    modifiers()};
  val_t wheel = v_record(5, names, fields);
  app_input(v_variant("wheel", &wheel));
}

static void send_key(bool down, WPARAM vk) {
  static const char* const names[] = {"down", "code", "modifiers"};
  val_t fields[] = {v_bool(down), v_u32((uint32_t)vk), modifiers()};
  val_t key = v_record(3, names, fields);
  app_input(v_variant("key", &key));
}

static void send_char(wchar_t unit) {
  wchar_t text[2];
  int n = 0;
  if (unit >= 0xD800 && unit < 0xDC00) {
    g_high_surrogate = unit;
    return;
  }
  if (unit >= 0xDC00 && unit < 0xE000) {
    if (!g_high_surrogate) {
      return;
    }
    text[n++] = g_high_surrogate;
  }
  g_high_surrogate = 0;
  /* Control characters (Enter, Backspace, Tab, ...) arrive as key events. */
  if (unit < 0x20 || unit == 0x7F) {
    return;
  }
  text[n++] = unit;
  char utf8[8];
  int len = WideCharToMultiByte(CP_UTF8, 0, text, n, utf8, sizeof utf8, NULL,
                                NULL);
  if (len > 0) {
    val_t s = v_string(utf8, (size_t)len);
    app_input(v_variant("text", &s));
  }
}

static void send_focus(bool focused) {
  val_t b = v_bool(focused);
  app_input(v_variant("focus", &b));
}

/* --- page events -------------------------------------------------------- */

static val_t string_field(const char* json, const char* key) {
  char* s = NULL;
  if (!json_string(json, key, &s)) {
    return v_cstring("");
  }
  val_t v = v_cstring(s);
  free(s);
  return v;
}

static bool bool_field(const char* json, const char* key) {
  bool b = false;
  json_bool(json, key, &b);
  return b;
}

static uint32_t u32_field(const char* json, const char* key) {
  long long n = 0;
  json_int(json, key, &n);
  return (uint32_t)n;
}

/* chorium.dll's JSON event -> WIT page-event. False for unknown types. */
static bool page_event_from_json(const char* json, val_t* out) {
  char* type = NULL;
  if (!json_string(json, "type", &type)) {
    return false;
  }
  bool ok = true;
  val_t payload;
  if (strcmp(type, "url") == 0) {
    static const char* const names[] = {"url", "can-go-back",
                                        "can-go-forward"};
    val_t fields[] = {string_field(json, "url"),
                      v_bool(bool_field(json, "can_go_back")),
                      v_bool(bool_field(json, "can_go_forward"))};
    payload = v_record(3, names, fields);
    *out = v_variant("navigated", &payload);
  } else if (strcmp(type, "title") == 0) {
    payload = string_field(json, "title");
    *out = v_variant("title", &payload);
  } else if (strcmp(type, "loading") == 0) {
    payload = v_bool(bool_field(json, "loading"));
    *out = v_variant("loading", &payload);
  } else if (strcmp(type, "loaded") == 0) {
    static const char* const names[] = {"url", "ok", "error"};
    val_t fields[] = {string_field(json, "url"), v_bool(bool_field(json, "ok")),
                      string_field(json, "error")};
    payload = v_record(3, names, fields);
    *out = v_variant("loaded", &payload);
  } else if (strcmp(type, "new_window") == 0) {
    payload = string_field(json, "url");
    *out = v_variant("new-window", &payload);
  } else if (strcmp(type, "fullscreen") == 0) {
    payload = v_bool(bool_field(json, "on"));
    *out = v_variant("fullscreen", &payload);
  } else if (strcmp(type, "dialog") == 0) {
    static const char* const names[] = {"id", "kind", "message",
                                        "default-text"};
    char* kind = NULL;
    json_string(json, "kind", &kind);
    val_t fields[] = {v_u32(u32_field(json, "id")),
                      v_enum(kind ? kind : "alert"),
                      string_field(json, "message"),
                      string_field(json, "default")};
    free(kind);
    payload = v_record(4, names, fields);
    *out = v_variant("dialog", &payload);
  } else if (strcmp(type, "eval") == 0) {
    static const char* const names[] = {"id", "ok", "json"};
    const char* raw = "null";
    size_t raw_len = 4;
    json_raw(json, "result", &raw, &raw_len);
    val_t fields[] = {v_u32(u32_field(json, "id")),
                      v_bool(bool_field(json, "ok")), v_string(raw, raw_len)};
    payload = v_record(3, names, fields);
    *out = v_variant("eval-result", &payload);
  } else if (strcmp(type, "crashed") == 0) {
    *out = v_variant("crashed", NULL);
  } else if (strcmp(type, "closed") == 0) {
    *out = v_variant("closed", NULL);
  } else {
    ok = false;
  }
  free(type);
  return ok;
}

/* --- window ------------------------------------------------------------- */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp,
                                    LPARAM lp) {
  switch (msg) {
    case WM_MOUSEMOVE:
      if (!g_tracking_leave) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        g_tracking_leave = TrackMouseEvent(&track);
      }
      send_pointer("pointer-move", lp, held_button(wp), 0);
      return 0;
    case WM_MOUSELEAVE:
      g_tracking_leave = false;
      app_input(v_variant("pointer-leave", NULL));
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: {
      bool dbl = msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                 msg == WM_MBUTTONDBLCLK;
      const char* button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
                               ? "left"
                           : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK)
                               ? "right"
                               : "middle";
      g_clicks = dbl ? 2 : 1;
      SetFocus(hwnd);
      SetCapture(hwnd);
      send_pointer("pointer-down", lp, button, g_clicks);
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      const char* button = msg == WM_LBUTTONUP   ? "left"
                           : msg == WM_RBUTTONUP ? "right"
                                                 : "middle";
      if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
        ReleaseCapture();
      }
      send_pointer("pointer-up", lp, button, g_clicks ? g_clicks : 1);
      return 0;
    }
    case WM_XBUTTONDOWN:
      if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) {
        app_input(v_variant("back", NULL));
      }
      return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      send_wheel(hwnd, wp, lp, msg == WM_MOUSEHWHEEL);
      return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
      send_key(msg == WM_KEYDOWN, wp);
      return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      send_key(msg == WM_SYSKEYDOWN, wp);
      break; /* keep Alt+F4 and the system menu working */
    case WM_CHAR:
      send_char((wchar_t)wp);
      return 0;
    case WM_SETFOCUS:
      send_focus(true);
      return 0;
    case WM_KILLFOCUS:
      send_focus(false);
      return 0;
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {
        g_minimized = true;
        app_call("on-pause");
        return 0;
      }
      surface_resize(LOWORD(lp), HIWORD(lp));
      if (g_minimized) {
        g_minimized = false;
        app_call("on-resume");
      }
      app_call("on-draw");
      return 0;
    case WM_DPICHANGED: {
      /* Pages keep rendering at the scale Chorium started with; the
       * surface scales them. */
      g_scale = HIWORD(wp) / 96.0f;
      surface_set_scale(g_scale);
      const RECT* r = (const RECT*)lp;
      SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_PAINT:
      /* Direct2D keeps no copy of the last frame: have the app draw. */
      ValidateRect(hwnd, NULL);
      app_call("on-draw");
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SETTINGCHANGE:
      if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
        app_call("on-draw"); /* light/dark mode changed */
      }
      break;

    /* The title bar: see title_bar_hit(). */
    case WM_NCCALCSIZE:
      if (wp) {
        /* Keep the system's side and bottom resize frame, drop its caption.
         * A maximised window hangs over the screen edge by the frame. */
        NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
        LONG top = p->rgrc[0].top;
        DefWindowProcW(hwnd, msg, wp, lp);
        p->rgrc[0].top = top + (IsZoomed(hwnd) ? frame_thickness() : 0);
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
      if (hit != HTCLIENT) {
        return hit; /* side and bottom frame */
      }
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      return title_bar_hit(pt);
    }
    case WM_NCMOUSEMOVE:
      if (is_window_button(wp) || wp == HTCAPTION) {
        /* Let the app show hover on the window buttons it draws. */
        if (!g_tracking_nc_leave) {
          TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                   hwnd, 0};
          g_tracking_nc_leave = TrackMouseEvent(&track);
        }
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        send_pointer("pointer-move", MAKELPARAM(pt.x, pt.y), "none", 0);
      }
      if (is_window_button(wp)) {
        return 0;
      }
      break;
    case WM_NCMOUSELEAVE:
      g_tracking_nc_leave = false;
      g_nc_pressed = 0;
      app_input(v_variant("pointer-leave", NULL));
      break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (is_window_button(wp)) {
        g_nc_pressed = wp; /* act on release, like real window buttons */
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (is_window_button(wp)) {
        if (wp == g_nc_pressed) {
          if (wp == HTMINBUTTON) {
            ShowWindow(hwnd, SW_MINIMIZE);
          } else if (wp == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
          } else {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
          }
        }
        g_nc_pressed = 0;
        return 0;
      }
      break;
    case WM_TIMER: {
      int tor = chorium_tor_progress();
      if (tor != g_last_tor) {
        g_last_tor = tor;
        app_call("on-draw");
      }
      return 0;
    }
    case WM_APP_FRAME: {
      uint32_t id = (uint32_t)wp;
      if (id >= 1 && id <= MAX_PAGES) {
        InterlockedExchange(&g_pages[id].frame_posted, 0);
        if (g_pages[id].used) {
          app_page_event(id, v_variant("frame", NULL));
        }
      }
      return 0;
    }
    case WM_APP_EVENT: {
      uint32_t id = (uint32_t)wp;
      char* json = (char*)lp;
      val_t event;
      if (id >= 1 && id <= MAX_PAGES && g_pages[id].used &&
          page_event_from_json(json, &event)) {
        app_page_event(id, event);
      }
      free(json);
      return 0;
    }
    case WM_APP_DRAW:
      app_call("on-draw");
      return 0;
    case WM_CLOSE:
      app_call("on-stop");
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char** argv) {
  int code = chorium_execute_process();
  if (code >= 0) {
    return code; /* a Chromium helper process */
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  /* Chorium.exe [app.wasm] [args for the app...]. Without an app (double-
   * clicked, or only a URL given) the app comes from the apps folder. */
  static char picked[MAX_PATH * 3];
  const char* wasm = NULL;
  int rest = 1; /* first argv entry that goes to the app */
  size_t n = argc > 1 ? strlen(argv[1]) : 0;
  if (n > 5 && _stricmp(argv[1] + n - 5, ".wasm") == 0) {
    wasm = argv[1];
    rest = 2;
  } else if (loader_pick_app(picked, sizeof picked)) {
    wasm = picked;
  } else {
    return 1;
  }

  g_scale = (float)GetDpiForSystem() / 96.0f;
  for (int i = 0; i <= MAX_PAGES; ++i) {
    InitializeCriticalSection(&g_pages[i].lock);
  }

  if (chorium_abi_version() != CHORIUM_ABI_VERSION) {
    loader_error("chorium.dll speaks ABI %d, this Chorium.exe needs %d.",
                 chorium_abi_version(), CHORIUM_ABI_VERSION);
    return 1;
  }
  if (chorium_init(g_scale) != 0) {
    loader_error("The engine (chorium.dll) didn't start.");
    return 1;
  }

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.lpszClassName = L"ChoriumHost";
  RegisterClassExW(&wc);
  /* The app's title bar is inside this; the system caption is removed. */
  RECT r = {0, 0, (LONG)(1280 * g_scale), (LONG)(880 * g_scale)};
  AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0,
                           GetDpiForSystem());
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Chorium",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL,
                           wc.hInstance, NULL);
  /* Apply WM_NCCALCSIZE (no system caption) to the new window. */
  SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  if (!surface_init(g_hwnd)) {
    loader_error("Direct2D isn't available.");
    chorium_shutdown();
    return 1;
  }
  RECT client;
  GetClientRect(g_hwnd, &client);
  surface_resize(client.right, client.bottom);

  if (!contract_load(wasm)) {
    loader_error("Couldn't load %s\n\n%s", wasm, contract_last_error());
    chorium_shutdown();
    return 1;
  }
  /* args[0] is the app's own path, like argv. Start before showing the
   * window, so the app has its pages before any input arrives. */
  char** app_args = malloc(sizeof(char*) * (size_t)(argc + 1));
  int app_argc = 0;
  app_args[app_argc++] = (char*)wasm;
  for (int i = rest; i < argc; ++i) {
    app_args[app_argc++] = argv[i];
  }
  app_start(app_argc, app_args);
  free(app_args);
  ShowWindow(g_hwnd, SW_SHOWDEFAULT);
  SetTimer(g_hwnd, 1, 250, NULL); /* Tor progress */

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  pages_close_all();
  contract_unload();
  chorium_shutdown();
  return 0;
}
