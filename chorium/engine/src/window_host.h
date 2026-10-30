// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_WINDOW_HOST_H_
#define UMBRA_CHORIUM_ENGINE_SRC_WINDOW_HOST_H_

#include <memory>
#include <optional>
#include <string>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/window_observer.h"
#include "ui/aura/window_tree_host.h"
#include "ui/display/screen_base.h"
#include "ui/events/event_source.h"
#include "ui/events/platform/platform_event_dispatcher.h"

namespace chorium {

// The root of a view's window tree. It has no native window: pages are
// composited into it and captured from there, never drawn on a screen.
// Same approach as Chromium's own //headless layer.
class WindowHost : public aura::WindowTreeHost,
                   public ui::PlatformEventDispatcher {
 public:
  WindowHost();
  WindowHost(const WindowHost&) = delete;
  WindowHost& operator=(const WindowHost&) = delete;
  ~WindowHost() override;

  // ui::PlatformEventDispatcher:
  bool CanDispatchEvent(const ui::PlatformEvent& event) override;
  uint32_t DispatchEvent(const ui::PlatformEvent& event) override;

  // aura::WindowTreeHost:
  ui::EventSource* GetEventSource() override;
  gfx::AcceleratedWidget GetAcceleratedWidget() override;
  void ShowImpl() override;
  void HideImpl() override;
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  gfx::Point GetLocationOnScreenInPixels() const override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool CaptureSystemKeyEventsImpl(
      std::optional<base::flat_set<ui::DomCode>> codes) override;
  void ReleaseSystemKeyEventCapture() override;
  bool IsKeyLocked(ui::DomCode dom_code) override;
  base::flat_map<std::string, std::string> GetKeyboardLayoutMap() override;
  void SetCursorNative(gfx::NativeCursor cursor_type) override;
  void MoveCursorToScreenLocationInPixels(const gfx::Point& location) override;
  void OnCursorVisibilityChangedNative(bool show) override;

 private:
  class FocusClient;

  gfx::Rect bounds_;
  std::unique_ptr<FocusClient> focus_client_;
};

// The only display Chorium's pages ever see. Its scale is the device pixel
// ratio chosen in chorium_init().
class Screen : public display::ScreenBase {
 public:
  explicit Screen(float scale);
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;
  ~Screen() override;

  // display::Screen:
  gfx::Point GetCursorScreenPoint() override;
  bool IsWindowUnderCursor(gfx::NativeWindow window) override;
  gfx::NativeWindow GetWindowAtScreenPoint(const gfx::Point& point) override;
  gfx::NativeWindow GetLocalProcessWindowAtPoint(
      const gfx::Point& point,
      const std::set<gfx::NativeWindow>& ignore) override;
  display::Display GetDisplayNearestWindow(
      gfx::NativeWindow window) const override;
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_WINDOW_HOST_H_
