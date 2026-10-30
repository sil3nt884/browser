/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * End-to-end check for Chorium: loads a page through Tor and saves what it
 * rendered as a .bmp.
 *
 *   chorium_test [url] [out.bmp]
 *   (defaults: https://check.torproject.org/  chorium_test.bmp)
 *
 * Prints every event, waits for the page to finish loading, then writes the
 * next frame. Exit code 0 means a frame was saved.
 */

/* A host app in plain C, not Chromium code: Chromium's C++ span rules don't
 * apply here. */
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "chorium.h"

static HANDLE g_loaded;   /* page finished loading */
static HANDLE g_saved;    /* a frame was written */
static volatile LONG g_want_frame;
static const char* g_out = "chorium_test.bmp";

static void write_bmp(const char* path, const uint8_t* bgra, int w, int h,
                      int stride) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "can't write %s\n", path);
    return;
  }
  const uint32_t row = (uint32_t)w * 4;
  const uint32_t image = row * (uint32_t)h;
  uint8_t header[54] = {'B', 'M'};
  *(uint32_t*)(header + 2) = 54 + image;
  *(uint32_t*)(header + 10) = 54;
  *(uint32_t*)(header + 14) = 40;
  *(int32_t*)(header + 18) = w;
  *(int32_t*)(header + 22) = -h; /* top-down rows */
  *(uint16_t*)(header + 26) = 1;
  *(uint16_t*)(header + 28) = 32;
  *(uint32_t*)(header + 34) = image;
  fwrite(header, 1, sizeof(header), f);
  for (int y = 0; y < h; ++y) {
    fwrite(bgra + (size_t)y * stride, 1, row, f);
  }
  fclose(f);
}

static void on_frame(chorium_view* view, const uint8_t* bgra, int w, int h,
                     int stride, void* user) {
  if (InterlockedCompareExchange(&g_want_frame, 0, 1) == 1) {
    write_bmp(g_out, bgra, w, h, stride);
    printf("frame %dx%d saved to %s\n", w, h, g_out);
    SetEvent(g_saved);
  }
}

static void on_event(chorium_view* view, const char* json, void* user) {
  printf("event %s\n", json);
  /* A new view starts on about:blank; wait for the page we asked for. */
  if (strstr(json, "\"type\":\"loaded\"") &&
      !strstr(json, "\"url\":\"about:")) {
    SetEvent(g_loaded);
  }
  /* Answer page dialogs so they never block the test. */
  if (strstr(json, "\"type\":\"dialog\"")) {
    const char* id = strstr(json, "\"id\":");
    if (id) {
      chorium_dialog_reply(view, atoi(id + 5), 1, NULL);
    }
  }
}

int main(int argc, char** argv) {
  int code = chorium_execute_process();
  if (code >= 0) {
    return code; /* a Chromium helper process */
  }

  const char* url = argc > 1 ? argv[1] : "https://check.torproject.org/";
  if (argc > 2) {
    g_out = argv[2];
  }
  setvbuf(stdout, NULL, _IONBF, 0); /* keep output if the engine aborts */
  g_loaded = CreateEventA(NULL, TRUE, FALSE, NULL);
  g_saved = CreateEventA(NULL, TRUE, FALSE, NULL);

  const DWORD start = GetTickCount();
  if (chorium_init(1.0f) != 0) {
    fprintf(stderr, "chorium_init failed\n");
    return 1;
  }
  printf("engine up in %lu ms\n", GetTickCount() - start);

  chorium_view* view = chorium_open(1280, 800, on_frame, on_event, NULL);
  chorium_load_url(view, url);

  /* Tor bootstraps in the background; the first page waits for it. */
  int last = -2;
  while (WaitForSingleObject(g_loaded, 500) == WAIT_TIMEOUT) {
    int p = chorium_tor_progress();
    if (p != last) {
      printf("tor %d%%\n", p);
      last = p;
    }
    if (GetTickCount() - start > 180000) {
      fprintf(stderr, "timed out waiting for the page\n");
      chorium_shutdown();
      return 2;
    }
  }
  printf("page loaded after %lu ms\n", GetTickCount() - start);

  /* Give late layout a moment, then take the next frame. */
  Sleep(1000);
  InterlockedExchange(&g_want_frame, 1);
  chorium_refresh_frame(view);
  const DWORD got = WaitForSingleObject(g_saved, 10000);

  chorium_eval(view, 1, "document.title");
  Sleep(500);

  chorium_close(view);
  chorium_shutdown();
  if (got != WAIT_OBJECT_0) {
    fprintf(stderr, "no frame arrived\n");
    return 3;
  }
  return 0;
}
