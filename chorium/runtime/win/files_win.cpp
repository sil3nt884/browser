/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Files and the clipboard on Windows: the system file picker (for pages'
 * <input type=file>), opening downloads, reading pasted text.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "host.h"

/* Portable C++ shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {

std::wstring Wide(const char* utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], n);
  }
  return out;
}

std::string Utf8(const wchar_t* wide) {
  int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr,
                              nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], n, nullptr, nullptr);
  }
  return out;
}

/* A picker request, from the app's call to when the picker can show. */
struct Pick {
  uint32_t request;
  picker_mode_t mode;
  std::vector<std::string> accept;
  std::string default_name;
};

/* The extensions Windows has registered for a MIME type, e.g. ".png". */
void ExtensionsForMime(const std::string& mime, std::vector<std::wstring>* out) {
  struct Wildcard {
    const char* prefix;
    const wchar_t* patterns[10];
  };
  static const Wildcard kWildcards[] = {
      {"image/",
       {L"*.png", L"*.jpg", L"*.jpeg", L"*.gif", L"*.webp", L"*.bmp", L"*.svg",
        L"*.avif", L"*.ico", L"*.heic"}},
      {"video/",
       {L"*.mp4", L"*.webm", L"*.mkv", L"*.mov", L"*.avi", L"*.m4v", L"*.ogv"}},
      {"audio/",
       {L"*.mp3", L"*.wav", L"*.ogg", L"*.m4a", L"*.flac", L"*.aac", L"*.opus",
        L"*.weba"}},
      {"text/", {L"*.txt", L"*.csv", L"*.html", L"*.css", L"*.md"}},
  };
  if (mime.size() > 2 && mime.compare(mime.size() - 2, 2, "/*") == 0) {
    for (const Wildcard& w : kWildcards) {
      if (mime.compare(0, mime.size() - 1, w.prefix) == 0) {
        for (const wchar_t* p : w.patterns) {
          if (p) {
            out->push_back(p);
          }
        }
      }
    }
    return;
  }
  std::wstring key = L"MIME\\Database\\Content Type\\" + Wide(mime.c_str());
  wchar_t ext[32];
  DWORD size = sizeof ext;
  if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), L"Extension",
                   RRF_RT_REG_SZ, nullptr, ext, &size) == ERROR_SUCCESS) {
    out->push_back(std::wstring(L"*") + ext);
  }
}

/* "*.png;*.jpg" for a page's accept list; empty if it accepts anything. */
std::wstring FilterSpec(const std::vector<std::string>& accept) {
  std::vector<std::wstring> patterns;
  for (const std::string& type : accept) {
    if (type.empty()) {
      continue;
    }
    if (type[0] == '.') {
      patterns.push_back(L"*" + Wide(type.c_str()));
    } else {
      ExtensionsForMime(type, &patterns);
    }
  }
  std::wstring spec;
  for (const std::wstring& p : patterns) {
    if (!spec.empty()) {
      spec += L";";
    }
    spec += p;
  }
  return spec;
}

std::vector<std::string> RunPicker(const Pick& pick) {
  std::vector<std::string> paths;
  bool save = pick.mode == PICKER_SAVE;
  IFileDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(
      save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
      CLSCTX_INPROC_SERVER,
      save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
      reinterpret_cast<void**>(&dialog));
  if (FAILED(hr)) {
    return paths;
  }
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
  if (pick.mode == PICKER_OPEN_MULTIPLE) {
    options |= FOS_ALLOWMULTISELECT;
  } else if (pick.mode == PICKER_OPEN_FOLDER) {
    options |= FOS_PICKFOLDERS;
  }
  dialog->SetOptions(options);

  std::wstring spec = FilterSpec(pick.accept);
  if (!spec.empty() && pick.mode != PICKER_OPEN_FOLDER) {
    COMDLG_FILTERSPEC filters[] = {{L"Accepted files", spec.c_str()},
                                   {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
  }
  if (!pick.default_name.empty()) {
    dialog->SetFileName(Wide(pick.default_name.c_str()).c_str());
  }

  if (SUCCEEDED(dialog->Show(g_hwnd))) {
    if (pick.mode == PICKER_OPEN_MULTIPLE) {
      IShellItemArray* items = nullptr;
      if (SUCCEEDED(static_cast<IFileOpenDialog*>(dialog)->GetResults(
              &items))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
          IShellItem* item = nullptr;
          wchar_t* path = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item)) &&
              SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.push_back(Utf8(path));
            CoTaskMemFree(path);
          }
          if (item) {
            item->Release();
          }
        }
        items->Release();
      }
    } else {
      IShellItem* item = nullptr;
      wchar_t* path = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) &&
          SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        paths.push_back(Utf8(path));
        CoTaskMemFree(path);
      }
      if (item) {
        item->Release();
      }
    }
  }
  dialog->Release();
  return paths;
}

}  // namespace

extern "C" {

/* The picker is modal: show it from the message loop, never from inside the
 * app's call (main.c's WM_APP_PICK runs this). */
void surface_pick_files(uint32_t request, picker_mode_t mode,
                        const char* const* accept, size_t count,
                        const char* default_name) {
  Pick* pick = new Pick;
  pick->request = request;
  pick->mode = mode;
  for (size_t i = 0; i < count; ++i) {
    pick->accept.push_back(accept[i]);
  }
  pick->default_name = default_name ? default_name : "";
  if (!PostMessageW(g_hwnd, WM_APP_PICK, 0, reinterpret_cast<LPARAM>(pick))) {
    delete pick;
  }
}

void win_run_picker(void* p) {
  Pick* pick = static_cast<Pick*>(p);
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::vector<std::string> paths = RunPicker(*pick);
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  std::vector<const char*> c_paths;
  for (const std::string& path : paths) {
    c_paths.push_back(path.c_str());
  }
  app_files_picked(pick->request, c_paths.data(), c_paths.size());
  delete pick;
}

void surface_open_file(const char* path, bool in_folder) {
  std::wstring wide = Wide(path);
  if (wide.empty()) {
    return;
  }
  if (!in_folder) {
    ShellExecuteW(g_hwnd, L"open", wide.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
    return;
  }
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  PIDLIST_ABSOLUTE item = ILCreateFromPathW(wide.c_str());
  if (item) {
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
  }
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
}

char* surface_read_clipboard(void) {
  if (!OpenClipboard(g_hwnd)) {
    return nullptr;
  }
  char* out = nullptr;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data) {
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text) {
      out = _strdup(Utf8(text).c_str());
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return out;
}

}  // extern "C"
