/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The remote control's Windows side (see runtime/common/remote.h): input
 * goes to the window as the same messages a mouse and keyboard send, and
 * screenshots are taken from the window itself.
 */
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "remote.h"

#include <windowsx.h>

/* The native remote uses explicitly sized Win32 buffers, as the host does. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

void platform_post_to_ui(void (*fn)(void*), void* arg) {
  PostMessageW(g_hwnd, WM_APP_CALL, (WPARAM)fn, (LPARAM)arg);
}

static LPARAM device_point(int x, int y) {
  return MAKELPARAM((int)((float)x * g_scale + 0.5f),
                    (int)((float)y * g_scale + 0.5f));
}

/* The window reads modifiers with GetKeyState: set them in this thread's
 * keyboard state for the duration of the key. */
static void set_modifiers(int mods) {
  BYTE keys[256];
  GetKeyboardState(keys);
  keys[VK_SHIFT] = (mods & 1) ? 0x80 : 0;
  keys[VK_CONTROL] = (mods & 2) ? 0x80 : 0;
  keys[VK_MENU] = (mods & 4) ? 0x80 : 0;
  SetKeyboardState(keys);
}

static WPARAM g_buttons; /* MK_* held by remote input */

void platform_remote_input(const remote_input_t* in) {
  LPARAM pt = device_point(in->x, in->y);
  switch (in->kind) {
    case RI_DOWN:
      g_buttons |= MK_LBUTTON;
      SendMessageW(g_hwnd, WM_MOUSEMOVE, g_buttons & ~MK_LBUTTON, pt);
      SendMessageW(g_hwnd, WM_LBUTTONDOWN, g_buttons, pt);
      break;
    case RI_MOVE:
      SendMessageW(g_hwnd, WM_MOUSEMOVE, g_buttons, pt);
      break;
    case RI_UP:
      g_buttons &= ~MK_LBUTTON;
      SendMessageW(g_hwnd, WM_LBUTTONUP, g_buttons, pt);
      break;
    case RI_RIGHT_DOWN:
      g_buttons |= MK_RBUTTON;
      SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, pt);
      SendMessageW(g_hwnd, WM_RBUTTONDOWN, g_buttons, pt);
      break;
    case RI_RIGHT_UP:
      g_buttons &= ~MK_RBUTTON;
      SendMessageW(g_hwnd, WM_RBUTTONUP, g_buttons, pt);
      break;
    case RI_WHEEL: {
      /* WM_MOUSEWHEEL carries screen coordinates; one notch is 100 CSS px. */
      POINT screen = {GET_X_LPARAM(pt), GET_Y_LPARAM(pt)};
      ClientToScreen(g_hwnd, &screen);
      int delta = (int)(-in->dy / 100.0f * WHEEL_DELTA);
      SendMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta),
                   MAKELPARAM(screen.x, screen.y));
      break;
    }
    case RI_KEY_DOWN:
    case RI_KEY_UP: {
      bool down = in->kind == RI_KEY_DOWN;
      set_modifiers(down ? in->mods : 0);
      SendMessageW(g_hwnd, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)in->code,
                   down ? 1 : (LPARAM)0xC0000001);
      if (!down) {
        set_modifiers(0);
      }
      break;
    }
    case RI_TEXT: {
      wchar_t wide[1024];
      int n = MultiByteToWideChar(CP_UTF8, 0, in->text, -1, wide, 1024);
      for (int i = 0; i + 1 < n; ++i) {
        SendMessageW(g_hwnd, WM_CHAR, wide[i], 1);
      }
      break;
    }
    case RI_BACK:
      SendMessageW(g_hwnd, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), pt);
      break;
  }
}

void platform_screenshot(void* token) {
  RECT client;
  GetClientRect(g_hwnd, &client);
  int w = client.right, h = client.bottom;
  HDC window_dc = GetDC(g_hwnd);
  HDC dc = CreateCompatibleDC(window_dc);
  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; /* top-down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = NULL;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  HGDIOBJ old = SelectObject(dc, bmp);
  /* PW_RENDERFULLCONTENT (2): Direct2D content too. */
  PrintWindow(g_hwnd, dc, PW_CLIENTONLY | 2);
  size_t image = (size_t)w * 4 * (size_t)h;
  size_t total = 54 + image;
  uint8_t* file = malloc(total);
  memset(file, 0, 54);
  file[0] = 'B';
  file[1] = 'M';
  *(uint32_t*)(file + 2) = (uint32_t)total;
  *(uint32_t*)(file + 10) = 54;
  *(uint32_t*)(file + 14) = 40;
  *(int32_t*)(file + 18) = w;
  *(int32_t*)(file + 22) = -h;
  *(uint16_t*)(file + 26) = 1;
  *(uint16_t*)(file + 28) = 32;
  *(uint32_t*)(file + 34) = (uint32_t)image;
  memcpy(file + 54, bits, image);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  ReleaseDC(g_hwnd, window_dc);
  remote_screenshot_done(token, "bmp", file, total);
  free(file);
}

/* The tests drive the real Open dialog on Windows. */
bool platform_answer_picker(const char* const* paths, size_t count) {
  return false;
}
