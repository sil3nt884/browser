/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * The Windows side of runtime.h's platform section that isn't drawing:
 * page frames and events from chorium.dll's thread to the UI thread, the
 * app's data folder, and errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shlobj.h>

#include "host.h"

win_page g_win_pages[MAX_PAGES + 1];

/* The window's timer polls Tor's progress all the time (main.c). */
void platform_tor_restarted(void) {}

void platform_log(const char* level, const char* text, size_t len) {
  fprintf(stderr, "[app %s] %.*s\n", level, (int)len, text);
}

void win_pages_init(void) {
  for (int i = 0; i <= MAX_PAGES; ++i) {
    InitializeCriticalSection(&g_win_pages[i].lock);
  }
}

/* chorium.dll's frame callback (its thread): keep the newest frame. */
static void page_frame(chorium_view* view, const uint8_t* bgra, int width,
                       int height, int stride, void* user) {
  uint32_t id = (uint32_t)(uintptr_t)user;
  win_page* page = &g_win_pages[id];
  size_t row = (size_t)width * 4;
  size_t need = row * (size_t)height;
  EnterCriticalSection(&page->lock);
  if (page->closing) {
    LeaveCriticalSection(&page->lock);
    return;
  }
  if (page->frame_cap < need) {
    free(page->frame);
    page->frame = malloc(need);
    page->frame_cap = page->frame ? need : 0;
  }
  if (page->frame) {
    for (int y = 0; y < height; ++y) {
      memcpy(page->frame + row * (size_t)y, bgra + (size_t)stride * y, row);
    }
    page->frame_w = width;
    page->frame_h = height;
    ++page->frame_seq;
  }
  LeaveCriticalSection(&page->lock);
  /* One message at a time; the app redraws with the newest frame anyway. */
  if (InterlockedExchange(&page->frame_posted, 1) == 0) {
    PostMessageW(g_hwnd, WM_APP_FRAME, id, 0);
  }
}

chorium_frame_fn platform_frame_callback(void) { return page_frame; }

void platform_page_event(chorium_view* view, const char* json, void* user) {
  char* copy = _strdup(json);
  if (copy && !PostMessageW(g_hwnd, WM_APP_EVENT, (WPARAM)(uintptr_t)user,
                            (LPARAM)copy)) {
    free(copy);
  }
}

void platform_page_closed(uint32_t page_id) {
  win_page* page = &g_win_pages[page_id];
  surface_forget_page(page_id);
  EnterCriticalSection(&page->lock);
  page->closing = true;
  free(page->frame);
  page->frame = NULL;
  page->frame_cap = 0;
  page->frame_w = page->frame_h = 0;
  LeaveCriticalSection(&page->lock);
}

/* %LOCALAPPDATA%\Chorium\apps\<app name> */
bool platform_app_data_dir(const char* app_name, char* out, size_t cap) {
  wchar_t dir[MAX_PATH];
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                              NULL, 0, dir))) {
    GetTempPathW(MAX_PATH, dir);
  }
  wchar_t name[128];
  if (!MultiByteToWideChar(CP_UTF8, 0, app_name, -1, name, 128)) {
    return false;
  }
  wcscat_s(dir, MAX_PATH, L"\\Chorium");
  CreateDirectoryW(dir, NULL);
  wcscat_s(dir, MAX_PATH, L"\\apps");
  CreateDirectoryW(dir, NULL);
  wcscat_s(dir, MAX_PATH, L"\\");
  wcscat_s(dir, MAX_PATH, name);
  CreateDirectoryW(dir, NULL);
  return WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, (int)cap, NULL,
                             NULL) > 0;
}

void platform_app_crashed(const char* message) {
  loader_error("The app stopped with an error.\n\n%s", message);
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

void platform_request_draw(void) { PostMessageW(g_hwnd, WM_APP_DRAW, 0, 0); }
