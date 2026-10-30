/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Chorium.exe, the Chorium runtime for Windows: the shared core
 * (runtime/common) plus a Win32 window, Direct2D drawing and page frames
 * captured from chorium.dll.
 */
#ifndef CHORIUM_RUNTIME_WIN_HOST_H_
#define CHORIUM_RUNTIME_WIN_HOST_H_

#include <windows.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" { /* surface_d2d.cpp is C++ (DirectWrite has no C headers) */
#endif

/* Window messages posted from Chromium's thread to the UI thread. */
#define WM_APP_FRAME (WM_APP + 1) /* wParam = page id */
#define WM_APP_EVENT (WM_APP + 2) /* wParam = page id, lParam = char* JSON */
#define WM_APP_DRAW (WM_APP + 3)

/* The latest frame of each page, by page id (0 unused). */
typedef struct win_page {
  /* Guards the frame fields; Chromium's thread writes, the UI thread draws. */
  CRITICAL_SECTION lock;
  bool closing;
  uint8_t* frame; /* BGRA, rows packed */
  int frame_w, frame_h;
  size_t frame_cap;
  unsigned frame_seq; /* bumped with every new frame */
  volatile LONG frame_posted;
  /* UI thread only: the frame as a Direct2D bitmap, and which frame it is. */
  void* bitmap;
  unsigned bitmap_seq;
} win_page;

extern HWND g_hwnd;
extern float g_scale; /* device pixels per CSS pixel */
extern win_page g_win_pages[MAX_PAGES + 1];

/* --- platform_win.c ------------------------------------------------------ */
void win_pages_init(void);

/* --- surface_d2d.cpp: Direct2D + DirectWrite ----------------------------- */
bool surface_init(HWND hwnd);
void surface_resize(int width_px, int height_px);
void surface_set_scale(float scale);
/* A page is closing: drop its bitmap (UI thread). */
void surface_forget_page(uint32_t page_id);

/* --- loader.c: starting without a command line --------------------------- */
/* The app (.wasm) in the apps folder next to Chorium.exe, as a UTF-8 path;
 * asks when there are several. False (after telling the person) if none. */
bool loader_pick_app(char* out, size_t cap);
/* Shows a problem in a message box (and on stderr, if there is one). */
void loader_error(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* CHORIUM_RUNTIME_WIN_HOST_H_ */
