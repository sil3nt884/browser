// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// The surface on Windows: Direct2D for shapes and page frames, DirectWrite
// for text and icons (Segoe Fluent Icons). Anti-aliased, with real alpha.
// The render target's DPI is 96 x g_scale, so Direct2D's units are CSS
// pixels and the app's coordinates go through unchanged.
//
// C++ only because DirectWrite has no C headers; everything else in the
// runtime is C and calls the extern "C" functions in host.h.

#include <d2d1.h>
#include <dwrite.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

namespace {

ID2D1Factory* g_d2d;
IDWriteFactory* g_dwrite;
ID2D1HwndRenderTarget* g_target;
ID2D1SolidColorBrush* g_brush;
HWND g_window;
int g_w = 1, g_h = 1;  // device pixels
bool g_drawing;
int g_clips;  // pushed and not yet popped this frame
const wchar_t* g_text_family = L"Segoe UI";
const wchar_t* g_icon_family = L"Segoe MDL2 Assets";

struct Format {
  float size;
  bool icon;
  IDWriteTextFormat* format;
};
constexpr int kFormats = 16;
Format g_formats[kFormats];
int g_next_format;

// Code points in Segoe Fluent Icons / Segoe MDL2 Assets, in the order of
// chorium.wit's `icon` enum.
const wchar_t kIcons[] = {
    0xE72B,  // back
    0xE72A,  // forward
    0xE72C,  // reload
    0xE711,  // stop
    0xE711,  // close
    0xE710,  // add
    0xE712,  // menu (drawn turned upright, like Chrome's)
    0xE721,  // search
    0xE72E,  // lock
    0xE946,  // info
    0xE774,  // page (globe)
    0xE713,  // settings
    0xE81C,  // history
    0xE80F,  // home
    0xE73E,  // check
    0xE921,  // minimize
    0xE922,  // maximize
    0xE923,  // restore
    0xE8BB,  // close-window
};
constexpr int kMenuIcon = 6;

template <typename T>
void release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

bool family_exists(const wchar_t* name) {
  IDWriteFontCollection* fonts = nullptr;
  if (FAILED(g_dwrite->GetSystemFontCollection(&fonts, FALSE))) {
    return false;
  }
  UINT32 index = 0;
  BOOL exists = FALSE;
  fonts->FindFamilyName(name, &index, &exists);
  fonts->Release();
  return exists;
}

D2D1_COLOR_F color(uint32_t argb) {
  return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f,
                      ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                      ((argb >> 24) & 0xFF) / 255.0f);
}

D2D1_RECT_F rect(int x, int y, int w, int h) {
  return D2D1::RectF((float)x, (float)y, (float)(x + w), (float)(y + h));
}

// UTF-8 -> UTF-16, malloc'd; |out_len| in UTF-16 units.
wchar_t* wide(const char* utf8, size_t len, UINT32* out_len) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, nullptr, 0);
  wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)(n + 1));
  MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, w, n);
  w[n] = 0;
  *out_len = (UINT32)n;
  return w;
}

IDWriteTextFormat* format(float size, bool icon) {
  if (size < 1) {
    size = 1;
  }
  for (Format& f : g_formats) {
    if (f.format && f.icon == icon && f.size == size) {
      return f.format;
    }
  }
  Format& slot = g_formats[g_next_format++ % kFormats];
  release(slot.format);
  slot.size = size;
  slot.icon = icon;
  if (FAILED(g_dwrite->CreateTextFormat(
          icon ? g_icon_family : g_text_family, nullptr,
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &slot.format))) {
    return nullptr;
  }
  slot.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (icon) {
    slot.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    slot.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  return slot.format;
}

void drop_page_bitmaps() {
  for (int i = 0; i <= MAX_PAGES; ++i) {
    ID2D1Bitmap* b = (ID2D1Bitmap*)g_pages[i].bitmap;
    release(b);
    g_pages[i].bitmap = nullptr;
  }
}

void drop_target() {
  drop_page_bitmaps();
  release(g_brush);
  release(g_target);
}

bool ensure_target() {
  if (g_target) {
    return true;
  }
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
      96.0f * g_scale, 96.0f * g_scale);
  D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
      D2D1::HwndRenderTargetProperties(
          g_window, D2D1::SizeU((UINT32)g_w, (UINT32)g_h),
          D2D1_PRESENT_OPTIONS_IMMEDIATELY);
  if (FAILED(g_d2d->CreateHwndRenderTarget(props, hwnd_props, &g_target))) {
    return false;
  }
  g_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  if (FAILED(g_target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0),
                                             &g_brush))) {
    drop_target();
    return false;
  }
  return true;
}

// Every drawing call starts the frame if it hasn't started yet.
bool begin() {
  if (g_drawing) {
    return true;
  }
  if (!ensure_target()) {
    return false;
  }
  g_target->BeginDraw();
  g_target->SetTransform(D2D1::Matrix3x2F::Identity());
  g_drawing = true;
  g_clips = 0;
  return true;
}

ID2D1SolidColorBrush* brush(uint32_t argb) {
  g_brush->SetColor(color(argb));
  return g_brush;
}

}  // namespace

extern "C" {

bool surface_init(HWND hwnd) {
  g_window = hwnd;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 (IUnknown**)&g_dwrite))) {
    return false;
  }
  if (family_exists(L"Segoe UI Variable Text")) {
    g_text_family = L"Segoe UI Variable Text";  // Windows 11
  }
  if (family_exists(L"Segoe Fluent Icons")) {
    g_icon_family = L"Segoe Fluent Icons";  // Windows 11
  }
  return true;
}

void surface_resize(int width_px, int height_px) {
  g_w = width_px > 0 ? width_px : 1;
  g_h = height_px > 0 ? height_px : 1;
  if (g_target) {
    g_target->Resize(D2D1::SizeU((UINT32)g_w, (UINT32)g_h));
  }
}

void surface_set_scale(float scale) {
  if (g_target) {
    g_target->SetDpi(96.0f * scale, 96.0f * scale);
  }
  drop_page_bitmaps();  // their DPI came from the old scale
}

void surface_dimensions(uint32_t* width, uint32_t* height) {
  *width = (uint32_t)lroundf((float)g_w / g_scale);
  *height = (uint32_t)lroundf((float)g_h / g_scale);
}

void surface_clear(uint32_t argb) {
  if (begin()) {
    g_target->Clear(color(argb));
  }
}

void surface_fill_rect(int x, int y, int w, int h, uint32_t argb) {
  if (begin()) {
    g_target->FillRectangle(rect(x, y, w, h), brush(argb));
  }
}

void surface_stroke_rect(int x, int y, int w, int h, uint32_t argb,
                         float width) {
  if (!begin()) {
    return;
  }
  float half = width / 2;
  D2D1_RECT_F r = rect(x, y, w, h);
  r.left += half;
  r.top += half;
  r.right -= half;
  r.bottom -= half;
  g_target->DrawRectangle(r, brush(argb), width);
}

void surface_fill_round_rect(int x, int y, int w, int h, float radius,
                             uint32_t argb) {
  if (begin()) {
    g_target->FillRoundedRectangle(
        D2D1::RoundedRect(rect(x, y, w, h), radius, radius), brush(argb));
  }
}

void surface_draw_text(int x, int y, const char* utf8, size_t len,
                       float font_size, uint32_t argb) {
  IDWriteTextFormat* f = format(font_size, false);
  if (!f || !begin()) {
    return;
  }
  UINT32 n;
  wchar_t* w = wide(utf8, len, &n);
  IDWriteTextLayout* layout = nullptr;
  if (SUCCEEDED(g_dwrite->CreateTextLayout(w, n, f, 100000.0f, 1000.0f,
                                           &layout))) {
    DWRITE_LINE_METRICS line;
    UINT32 lines = 0;
    layout->GetLineMetrics(&line, 1, &lines);
    float baseline = lines ? line.baseline : font_size;
    g_target->DrawTextLayout(D2D1::Point2F((float)x, (float)y - baseline),
                             layout, brush(argb));
    layout->Release();
  }
  free(w);
}

void surface_measure_text(const char* utf8, size_t len, float font_size,
                          uint32_t* width, uint32_t* height) {
  *width = *height = 0;
  IDWriteTextFormat* f = format(font_size, false);
  if (!f) {
    return;
  }
  UINT32 n;
  wchar_t* w = wide(utf8, len, &n);
  IDWriteTextLayout* layout = nullptr;
  if (SUCCEEDED(g_dwrite->CreateTextLayout(w, n, f, 100000.0f, 1000.0f,
                                           &layout))) {
    DWRITE_TEXT_METRICS m;
    if (SUCCEEDED(layout->GetMetrics(&m))) {
      *width = (uint32_t)ceilf(m.widthIncludingTrailingWhitespace);
      *height = (uint32_t)ceilf(m.height);
    }
    layout->Release();
  }
  free(w);
}

void surface_draw_icon(int icon, int x, int y, int w, int h, uint32_t argb) {
  if (icon < 0 || icon >= (int)(sizeof kIcons / sizeof kIcons[0])) {
    return;
  }
  IDWriteTextFormat* f = format((float)(w < h ? w : h), true);
  if (!f || !begin()) {
    return;
  }
  D2D1_RECT_F r = rect(x, y, w, h);
  if (icon == kMenuIcon) {
    g_target->SetTransform(D2D1::Matrix3x2F::Rotation(
        90.0f, D2D1::Point2F(x + w / 2.0f, y + h / 2.0f)));
  }
  g_target->DrawText(&kIcons[icon], 1, f, r, brush(argb));
  g_target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void surface_draw_page(host_page* page, int x, int y, int w, int h) {
  if (!begin()) {
    return;
  }
  ID2D1Bitmap* bitmap = (ID2D1Bitmap*)page->bitmap;
  EnterCriticalSection(&page->lock);
  if (page->frame && page->frame_w > 0 && page->frame_h > 0 &&
      (!bitmap || page->bitmap_seq != page->frame_seq)) {
    D2D1_SIZE_U size = D2D1::SizeU((UINT32)page->frame_w,
                                   (UINT32)page->frame_h);
    if (bitmap && (bitmap->GetPixelSize().width != size.width ||
                   bitmap->GetPixelSize().height != size.height)) {
      release(bitmap);
    }
    if (bitmap) {
      bitmap->CopyFromMemory(nullptr, page->frame, size.width * 4);
    } else {
      D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_IGNORE),
          96.0f * g_scale, 96.0f * g_scale);
      g_target->CreateBitmap(size, page->frame, size.width * 4, props,
                             &bitmap);
    }
    page->bitmap_seq = page->frame_seq;
  }
  LeaveCriticalSection(&page->lock);
  page->bitmap = bitmap;
  if (bitmap) {
    g_target->DrawBitmap(bitmap, rect(x, y, w, h), 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  } else {
    g_target->FillRectangle(rect(x, y, w, h), brush(0xFFFFFFFF));
  }
}

void surface_forget_page(host_page* page) {
  ID2D1Bitmap* bitmap = (ID2D1Bitmap*)page->bitmap;
  release(bitmap);
  page->bitmap = nullptr;
}

void surface_push_clip(int x, int y, int w, int h) {
  if (begin()) {
    g_target->PushAxisAlignedClip(rect(x, y, w, h), D2D1_ANTIALIAS_MODE_ALIASED);
    ++g_clips;
  }
}

void surface_pop_clip(void) {
  if (g_drawing && g_clips > 0) {
    g_target->PopAxisAlignedClip();
    --g_clips;
  }
}

void surface_present(void) {
  if (!g_drawing) {
    return;
  }
  while (g_clips > 0) {  // an app that forgot a pop-clip
    g_target->PopAxisAlignedClip();
    --g_clips;
  }
  g_drawing = false;
  if (g_target->EndDraw() == D2DERR_RECREATE_TARGET) {
    drop_target();  // the GPU went away; the next frame makes a new one
    PostMessageW(g_window, WM_APP_DRAW, 0, 0);
  }
}

bool surface_dark_mode(void) {
  DWORD light = 1;
  DWORD size = sizeof light;
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
               L"Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  return light == 0;
}

}  // extern "C"
