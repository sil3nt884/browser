/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Starting without a command line: Chorium.exe is a windowed program, so
 * double-clicking it shows no console. With no app named on the command
 * line it runs the app in the "apps" folder next to it, or lets the person
 * pick when there are several. Problems are shown in message boxes, since
 * there is usually no console to print to.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#include <commctrl.h> /* after windows.h, which host.h includes */

#define MAX_APPS 16

void loader_error(const char* fmt, ...) {
  char message[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof message, fmt, args);
  va_end(args);
  fprintf(stderr, "[host] %s\n", message);
  wchar_t wide[1024];
  MultiByteToWideChar(CP_UTF8, 0, message, -1, wide, 1024);
  MessageBoxW(g_hwnd, wide, L"Chorium", MB_ICONERROR | MB_OK);
}

/* <folder of Chorium.exe>\apps */
static void apps_folder(wchar_t* out, size_t cap) {
  GetModuleFileNameW(NULL, out, (DWORD)cap);
  wchar_t* slash = wcsrchr(out, L'\\');
  if (slash) {
    *slash = 0;
  }
  wcscat_s(out, cap, L"\\apps");
}

bool loader_pick_app(char* out, size_t cap) {
  wchar_t folder[MAX_PATH];
  apps_folder(folder, MAX_PATH);
  wchar_t pattern[MAX_PATH + 8];
  swprintf(pattern, MAX_PATH + 8, L"%ls\\*.wasm", folder);

  wchar_t names[MAX_APPS][MAX_PATH];
  int count = 0;
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileW(pattern, &found);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          count < MAX_APPS) {
        wcscpy_s(names[count++], MAX_PATH, found.cFileName);
      }
    } while (FindNextFileW(h, &found));
    FindClose(h);
  }

  int chosen = 0;
  if (count == 0) {
    char folder8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, folder, -1, folder8, sizeof folder8, NULL,
                        NULL);
    loader_error("No app to run.\n\nPut an app (.wasm) in\n%s\nor open one "
                 "with Chorium.",
                 folder8);
    return false;
  }
  if (count > 1) {
    /* One button per app, named after the file without ".wasm". */
    TASKDIALOG_BUTTON buttons[MAX_APPS];
    wchar_t labels[MAX_APPS][MAX_PATH];
    for (int i = 0; i < count; ++i) {
      wcscpy_s(labels[i], MAX_PATH, names[i]);
      wchar_t* dot = wcsrchr(labels[i], L'.');
      if (dot) {
        *dot = 0;
      }
      buttons[i].nButtonID = 1000 + i;
      buttons[i].pszButtonText = labels[i];
    }
    TASKDIALOGCONFIG config = {sizeof(config)};
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Chorium";
    config.pszMainInstruction = L"Which app?";
    config.cButtons = (UINT)count;
    config.pButtons = buttons;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&config, &pressed, NULL, NULL)) ||
        pressed < 1000 || pressed >= 1000 + count) {
      return false;
    }
    chosen = pressed - 1000;
  }

  wchar_t path[MAX_PATH * 2];
  swprintf(path, MAX_PATH * 2, L"%ls\\%ls", folder, names[chosen]);
  return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL,
                             NULL) > 0;
}
