// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/window_host.h"

#include "base/check_op.h"
#include "ui/aura/client/focus_change_observer.h"
#include "ui/aura/window.h"
#include "ui/display/display.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_conversions.h"

namespace chorium {

// Keeps track of which window has keyboard focus inside one WindowHost.
class WindowHost::FocusClient : public aura::client::FocusClient,
                                public aura::WindowObserver {
 public:
  FocusClient() = default;
  ~FocusClient() override = default;

  void AddObserver(aura::client::FocusChangeObserver* observer) override {
    observers_.AddObserver(observer);
  }
  void RemoveObserver(aura::client::FocusChangeObserver* observer) override {
    observers_.RemoveObserver(observer);
  }
  void FocusWindow(aura::Window* window) override {
    if (window && !window->CanFocus()) {
      return;
    }
    if (focused_) {
      observation_.Reset();
    }
    aura::Window* old = focused_;
    focused_ = window;
    if (focused_) {
      observation_.Observe(focused_.get());
    }
    for (aura::client::FocusChangeObserver& observer : observers_) {
      observer.OnWindowFocused(focused_, old);
    }
    if (auto* o = aura::client::GetFocusChangeObserver(old)) {
      o->OnWindowFocused(focused_, old);
    }
    if (auto* o = aura::client::GetFocusChangeObserver(focused_)) {
      o->OnWindowFocused(focused_, old);
    }
  }
  void ResetFocusWithinActiveWindow(aura::Window* window) override {
    if (!window->Contains(focused_)) {
      FocusWindow(window);
    }
  }
  aura::Window* GetFocusedWindow() override { return focused_; }

  void OnWindowDestroying(aura::Window* window) override {
    DCHECK_EQ(window, focused_);
    FocusWindow(nullptr);
  }

 private:
  raw_ptr<aura::Window> focused_ = nullptr;
  base::ScopedObservation<aura::Window, aura::WindowObserver> observation_{
      this};
  base::ObserverList<aura::client::FocusChangeObserver> observers_;
};

WindowHost::WindowHost() {
  CreateCompositor();
  OnAcceleratedWidgetAvailable();
  focus_client_ = std::make_unique<FocusClient>();
  aura::client::SetFocusClient(window(), focus_client_.get());
  aura::client::SetWindowParentingClient(window(), this);
}

WindowHost::~WindowHost() {
  aura::client::SetWindowParentingClient(window(), nullptr);
  aura::client::SetFocusClient(window(), nullptr);
  DestroyCompositor();
  DestroyDispatcher();
}

aura::Window* WindowHost::GetDefaultParent(aura::Window*,
                                          const gfx::Rect&,
                                          int64_t) {
  return window();
}

bool WindowHost::CanDispatchEvent(const ui::PlatformEvent& event) {
  return false;
}

uint32_t WindowHost::DispatchEvent(const ui::PlatformEvent& event) {
  return 0;
}

ui::EventSource* WindowHost::GetEventSource() {
  return this;
}

gfx::AcceleratedWidget WindowHost::GetAcceleratedWidget() {
  return gfx::AcceleratedWidget();
}

void WindowHost::ShowImpl() {}

void WindowHost::HideImpl() {}

gfx::Rect WindowHost::GetBoundsInPixels() const {
  return bounds_;
}

void WindowHost::SetBoundsInPixels(const gfx::Rect& bounds) {
  window()->SetBounds(bounds);
  if (bounds_ == bounds) {
    return;
  }
  const bool moved = bounds_.origin() != bounds.origin();
  bounds_ = bounds;
  if (moved) {
    auto weak = GetWeakPtr();
    OnHostMovedInPixels();
    if (!weak) {
      return;
    }
  }
  // Always report the size, even if unchanged: the compositor's layers are
  // only updated from here (see headless_window_tree_host.cc).
  OnHostResizedInPixels(bounds.size());
}

gfx::Point WindowHost::GetLocationOnScreenInPixels() const {
  return bounds_.origin();
}

void WindowHost::SetCapture() {}

void WindowHost::ReleaseCapture() {}

bool WindowHost::CaptureSystemKeyEventsImpl(
    std::optional<base::flat_set<ui::DomCode>> codes) {
  return false;
}

void WindowHost::ReleaseSystemKeyEventCapture() {}

bool WindowHost::IsKeyLocked(ui::DomCode dom_code) {
  return false;
}

base::flat_map<std::string, std::string> WindowHost::GetKeyboardLayoutMap() {
  return {};
}

void WindowHost::SetCursorNative(gfx::NativeCursor cursor_type) {}

void WindowHost::MoveCursorToScreenLocationInPixels(
    const gfx::Point& location) {}

void WindowHost::OnCursorVisibilityChangedNative(bool show) {}

Screen::Screen(float scale) {
  // A large virtual monitor so pages never think they are cramped.
  constexpr gfx::Size kSizeDip(3840, 2160);
  display::Display display(1);
  display.SetScaleAndBounds(
      scale, gfx::Rect(gfx::ScaleToCeiledSize(kSizeDip, scale)));
  ProcessDisplayChanged(display, /*is_primary=*/true);
}

Screen::~Screen() = default;

gfx::Point Screen::GetCursorScreenPoint() {
  return gfx::Point();
}

bool Screen::IsWindowUnderCursor(gfx::NativeWindow window) {
  return false;
}

gfx::NativeWindow Screen::GetWindowAtScreenPoint(const gfx::Point& point) {
  return gfx::NativeWindow();
}

gfx::NativeWindow Screen::GetLocalProcessWindowAtPoint(
    const gfx::Point& point,
    const std::set<gfx::NativeWindow>& ignore) {
  return gfx::NativeWindow();
}

display::Display Screen::GetDisplayNearestWindow(
    gfx::NativeWindow window) const {
  return GetPrimaryDisplay();
}

}  // namespace chorium
