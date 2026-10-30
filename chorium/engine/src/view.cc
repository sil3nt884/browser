// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/view.h"

#include <cmath>
#include <set>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/public/browser/context_menu_params.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/viz/host/client_frame_sink_video_capturer.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "media/base/video_types.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom-shared.h"
#include "third_party/blink/public/mojom/peerconnection/webrtc_ip_handling_policy.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "umbra/chorium/engine/src/browser_context.h"
#include "umbra/chorium/engine/src/engine.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_ANDROID)
#include "umbra/chorium/engine/src/android/page_host.h"
#else
#include "ui/aura/window.h"
#include "umbra/chorium/engine/src/window_host.h"
#endif

namespace chorium {

namespace {

// Private JavaScript world for chorium_eval(): it sees the page's DOM but not
// the page's own scripts or variables, and the page can't see it. Embedder
// worlds must be in (CONTENT_END, MAX]; Chromium CHECKs this.
constexpr int32_t kEvalWorldId = content::ISOLATED_WORLD_ID_CONTENT_END + 1;

std::set<View*>& Views() {
  static base::NoDestructor<std::set<View*>> views;
  return *views;
}

int ToBlinkModifiers(int mods) {
  int out = 0;
  if (mods & CHORIUM_MOD_SHIFT) {
    out |= blink::WebInputEvent::kShiftKey;
  }
  if (mods & CHORIUM_MOD_CTRL) {
    out |= blink::WebInputEvent::kControlKey;
  }
  if (mods & CHORIUM_MOD_ALT) {
    out |= blink::WebInputEvent::kAltKey;
  }
  if (mods & CHORIUM_MOD_META) {
    out |= blink::WebInputEvent::kMetaKey;
  }
  return out;
}

blink::WebPointerProperties::Button ToBlinkButton(int button) {
  switch (button) {
    case CHORIUM_BUTTON_LEFT:
      return blink::WebPointerProperties::Button::kLeft;
    case CHORIUM_BUTTON_MIDDLE:
      return blink::WebPointerProperties::Button::kMiddle;
    case CHORIUM_BUTTON_RIGHT:
      return blink::WebPointerProperties::Button::kRight;
    default:
      return blink::WebPointerProperties::Button::kNoButton;
  }
}

int ButtonDownBit(int button) {
  switch (button) {
    case CHORIUM_BUTTON_LEFT:
      return blink::WebInputEvent::kLeftButtonDown;
    case CHORIUM_BUTTON_MIDDLE:
      return blink::WebInputEvent::kMiddleButtonDown;
    case CHORIUM_BUTTON_RIGHT:
      return blink::WebInputEvent::kRightButtonDown;
    default:
      return 0;
  }
}

gfx::Size ToPixels(const gfx::Size& css) {
  return gfx::ScaleToCeiledSize(css, State().scale);
}

}  // namespace

// static
void View::Create(chorium_view* handle, int width, int height) {
  View* view = new View(handle, width, height);
  handle->view = view;
  Views().insert(view);
}

// static
void View::Close(chorium_view* handle) {
  if (View* view = handle->view) {
    Views().erase(view);
    handle->view = nullptr;
    delete view;
  }
  delete handle;
}

// static
void View::PreferencesChanged() {
  for (View* view : Views()) {
    view->web_contents_->OnWebPreferencesChanged();
  }
}

// static
void View::CloseAll() {
  while (!Views().empty()) {
    Close((*Views().begin())->handle_);
  }
}

View::View(chorium_view* handle, int width, int height)
    : handle_(handle), size_(width, height) {
  content::WebContents::CreateParams params(State().context.get());
  web_contents_ = content::WebContents::Create(params);
  web_contents_->SetDelegate(this);
  Observe(web_contents_.get());

  // WebRTC may only use UDP through the proxy, which means not at all: Tor
  // carries TCP only. Without this a page could learn the real IP address.
  blink::RendererPreferences* prefs = web_contents_->GetMutableRendererPrefs();
  prefs->webrtc_ip_handling_policy =
      blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
  web_contents_->SyncRendererPrefs();

#if BUILDFLAG(IS_ANDROID)
  // A native view per page (touch, scrolling and the keyboard work as in any
  // Android browser); the runtime places it with chorium_android_show_page.
  android::AddPage(handle_, web_contents_.get());
#else
  host_ = std::make_unique<WindowHost>();
  host_->InitHost();
  aura::Window* root = host_->window();
  root->Show();
  gfx::NativeView native_view = web_contents_->GetNativeView();
  root->AddChild(native_view);
  native_view->Show();
  host_->Show();
  Resize(width, height);

  // Keep the page rendering at full rate even though nothing shows it.
  keep_rendering_ = web_contents_->IncrementCapturerCount(
      ToPixels(size_), /*stay_hidden=*/false, /*stay_awake=*/false,
      /*is_activity=*/false);
#endif

  web_contents_->GetController().LoadURL(GURL(url::kAboutBlankURL),
                                         content::Referrer(),
                                         ui::PAGE_TRANSITION_TYPED,
                                         std::string());
  StartCapture();
}

View::~View() {
  for (auto& [id, callback] : dialogs_) {
    std::move(callback).Run(false, std::u16string());
  }
  dialogs_.clear();
  if (capturer_) {
    capturer_->StopAndResetConsumer();
    capturer_.reset();
  }
  keep_rendering_.RunAndReset();
  Observe(nullptr);
#if BUILDFLAG(IS_ANDROID)
  android::RemovePage(handle_);
#endif
  web_contents_->SetDelegate(nullptr);
  web_contents_.reset();
#if !BUILDFLAG(IS_ANDROID)
  host_.reset();
#endif
}

void View::Emit(base::DictValue event) {
  if (!handle_->on_event) {
    return;
  }
  std::optional<std::string> json = base::WriteJson(event);
  if (json) {
    handle_->on_event(handle_, json->c_str(), handle_->user);
  }
}

content::RenderWidgetHost* View::Widget() {
  content::RenderWidgetHostView* rwhv =
      web_contents_->GetRenderWidgetHostView();
  return rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
}

void View::StartCapture() {
  if (!handle_->on_frame) {
    return;  // the runtime shows pages itself (Android)
  }
  content::RenderWidgetHostView* rwhv =
      web_contents_->GetRenderWidgetHostView();
  if (!rwhv) {
    return;  // Tried again when the page's renderer is ready.
  }
  if (capturer_) {
    capturer_->StopAndResetConsumer();
  }
  capturer_ = rwhv->CreateVideoCapturer();
  capturer_->SetFormat(media::PIXEL_FORMAT_ARGB);  // BGRA in memory
  capturer_->SetMinCapturePeriod(base::Seconds(1) / 60);
  capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  const gfx::Size px = ToPixels(size_);
  capturer_->SetResolutionConstraints(px, px, /*use_fixed_aspect_ratio=*/true);
  capturer_->SetAutoThrottlingEnabled(false);
  capturer_->Start(this, viz::mojom::BufferFormatPreference::kDefault);
}

int View::ButtonModifiers() const {
  return buttons_down_;
}

// Navigation ----------------------------------------------------------------

void View::LoadUrl(const std::string& url) {
  GURL gurl(url);
  if (!gurl.is_valid()) {
    Emit(base::DictValue()
             .Set("type", "loaded")
             .Set("url", url)
             .Set("ok", false)
             .Set("error", "invalid URL"));
    return;
  }
  web_contents_->GetController().LoadURL(gurl, content::Referrer(),
                                         ui::PAGE_TRANSITION_TYPED,
                                         std::string());
}

void View::LoadHtml(const std::string& html, const std::string& base_url) {
  content::NavigationController::LoadURLParams params(
      GURL("data:text/html;charset=utf-8;base64," + base::Base64Encode(html)));
  params.transition_type = ui::PAGE_TRANSITION_TYPED;
  if (!base_url.empty()) {
    params.load_type = content::NavigationController::LOAD_TYPE_DATA;
    params.base_url_for_data_url = GURL(base_url);
    params.virtual_url_for_special_cases = GURL(base_url);
  }
  web_contents_->GetController().LoadURLWithParams(params);
}

void View::Back() {
  if (web_contents_->GetController().CanGoBack()) {
    web_contents_->GetController().GoBack();
  }
}

void View::Forward() {
  if (web_contents_->GetController().CanGoForward()) {
    web_contents_->GetController().GoForward();
  }
}

void View::Reload() {
  web_contents_->GetController().Reload(content::ReloadType::NORMAL, false);
}

void View::Stop() {
  web_contents_->Stop();
}

void View::Resize(int width, int height) {
  size_ = gfx::Size(width, height);
#if !BUILDFLAG(IS_ANDROID)
  // On Android the page's native view gets its size from the layout.
  web_contents_->GetNativeView()->SetBounds(gfx::Rect(size_));
  host_->SetBoundsInPixels(gfx::Rect(ToPixels(size_)));
#endif
  if (capturer_) {
    const gfx::Size px = ToPixels(size_);
    capturer_->SetResolutionConstraints(px, px, true);
  }
}

void View::RefreshFrame() {
  if (capturer_) {
    capturer_->RequestRefreshFrame();
  }
}

// Input ---------------------------------------------------------------------

void View::Mouse(int type, int x, int y, int button, int click_count,
                 int mods) {
  content::RenderWidgetHost* widget = Widget();
  if (!widget) {
    return;
  }
  blink::WebInputEvent::Type blink_type;
  switch (type) {
    case CHORIUM_MOUSE_DOWN:
      blink_type = blink::WebInputEvent::Type::kMouseDown;
      buttons_down_ |= ButtonDownBit(button);
      break;
    case CHORIUM_MOUSE_UP:
      blink_type = blink::WebInputEvent::Type::kMouseUp;
      break;
    case CHORIUM_MOUSE_LEAVE:
      blink_type = blink::WebInputEvent::Type::kMouseLeave;
      break;
    default:
      blink_type = blink::WebInputEvent::Type::kMouseMove;
      break;
  }
  blink::WebMouseEvent event(blink_type,
                             ToBlinkModifiers(mods) | ButtonModifiers(),
                             ui::EventTimeForNow());
  event.SetPositionInWidget(x, y);
  event.SetPositionInScreen(x, y);
  event.button = ToBlinkButton(button);
  event.click_count = click_count;
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  if (type == CHORIUM_MOUSE_UP) {
    buttons_down_ &= ~ButtonDownBit(button);
  }
  widget->ForwardMouseEvent(event);
}

namespace {

// A gesture of wheel events ends this long after the last one, as Chrome's
// MouseWheelPhaseHandler does; events before then scroll the same thing.
constexpr base::TimeDelta kWheelGestureEnd = base::Milliseconds(250);

// Whole notches (the runtime sends 100 CSS pixels per notch) come from a
// mouse wheel; anything else from a touchpad.
bool IsNotch(float delta) {
  return delta != 0 && std::fabs(std::fmod(delta, 100.0f)) < 0.01f;
}

}  // namespace

void View::Wheel(int x, int y, float dx, float dy, int mods) {
  content::RenderWidgetHost* widget = Widget();
  if (!widget) {
    return;
  }
  blink::WebMouseWheelEvent event(blink::WebInputEvent::Type::kMouseWheel,
                                  ToBlinkModifiers(mods) | ButtonModifiers(),
                                  ui::EventTimeForNow());
  event.SetPositionInWidget(x, y);
  event.SetPositionInScreen(x, y);
  event.delta_x = -dx;
  event.delta_y = -dy;
  event.wheel_ticks_x = event.delta_x == 0 ? 0 : (event.delta_x > 0 ? 1 : -1);
  event.wheel_ticks_y = event.delta_y == 0 ? 0 : (event.delta_y > 0 ? 1 : -1);
  // Mouse notches as plain pixels: Chromium animates those (smooth
  // scrolling). Touchpad movement stays precise, following the fingers.
  event.delta_units = (IsNotch(dx) || IsNotch(dy))
                          ? ui::ScrollGranularity::kScrollByPixel
                          : ui::ScrollGranularity::kScrollByPrecisePixel;
  event.phase = wheel_active_ ? blink::WebMouseWheelEvent::kPhaseChanged
                              : blink::WebMouseWheelEvent::kPhaseBegan;
  event.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
  widget->ForwardWheelEvent(event);

  wheel_active_ = true;
  wheel_point_ = gfx::Point(x, y);
  wheel_modifiers_ = mods;
  wheel_end_.Start(FROM_HERE, kWheelGestureEnd,
                   base::BindOnce(&View::EndWheel, base::Unretained(this)));
}

void View::EndWheel() {
  wheel_active_ = false;
  content::RenderWidgetHost* widget = Widget();
  if (!widget) {
    return;
  }
  blink::WebMouseWheelEvent event(
      blink::WebInputEvent::Type::kMouseWheel,
      ToBlinkModifiers(wheel_modifiers_) | ButtonModifiers(),
      ui::EventTimeForNow());
  event.SetPositionInWidget(wheel_point_.x(), wheel_point_.y());
  event.SetPositionInScreen(wheel_point_.x(), wheel_point_.y());
  event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  event.dispatch_type = blink::WebInputEvent::DispatchType::kEventNonBlocking;
  widget->ForwardWheelEvent(event);
}

void View::Key(bool down, int key_code, int mods) {
  content::RenderWidgetHost* widget = Widget();
  if (!widget) {
    return;
  }
  const int flags = ToBlinkModifiers(mods);
  input::NativeWebKeyboardEvent event(
      down ? blink::WebInputEvent::Type::kRawKeyDown
           : blink::WebInputEvent::Type::kKeyUp,
      flags, ui::EventTimeForNow());
  const auto keyboard_code = static_cast<ui::KeyboardCode>(key_code);
  const ui::DomCode dom_code = ui::UsLayoutKeyboardCodeToDomCode(keyboard_code);
  ui::DomKey dom_key;
  ui::KeyboardCode ignored;
  if (!ui::DomCodeToUsLayoutDomKey(dom_code, 0, &dom_key, &ignored)) {
    dom_key = ui::DomKey::UNIDENTIFIED;
  }
  event.windows_key_code = key_code;
  event.dom_code = static_cast<int>(dom_code);
  event.dom_key = dom_key;
  widget->ForwardKeyboardEvent(event);

  // Enter submits forms through its character event, as on a real keyboard.
  if (down && keyboard_code == ui::VKEY_RETURN) {
    input::NativeWebKeyboardEvent ch(blink::WebInputEvent::Type::kChar, flags,
                                     ui::EventTimeForNow());
    ch.windows_key_code = ui::VKEY_RETURN;
    ch.text[0] = '\r';
    ch.unmodified_text[0] = '\r';
    ch.dom_code = event.dom_code;
    ch.dom_key = event.dom_key;
    widget->ForwardKeyboardEvent(ch);
  }
}

void View::Text(const std::u16string& text) {
  content::RenderWidgetHost* widget = Widget();
  if (!widget) {
    return;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    input::NativeWebKeyboardEvent ch(blink::WebInputEvent::Type::kChar, 0,
                                     ui::EventTimeForNow());
    ch.text[0] = text[i];
    ch.unmodified_text[0] = text[i];
    // Keep surrogate pairs (emoji etc.) in one event.
    if (i + 1 < text.size() && (text[i] & 0xFC00) == 0xD800 &&
        (text[i + 1] & 0xFC00) == 0xDC00) {
      ch.text[1] = text[i + 1];
      ch.unmodified_text[1] = text[i + 1];
      ++i;
    }
    ch.windows_key_code = ch.text[0];
    widget->ForwardKeyboardEvent(ch);
  }
}

void View::SetFocus(bool focused) {
#if BUILDFLAG(IS_ANDROID)
  // The page is a native view: Android moves focus to it when it's touched.
  // Focusing it from here would take the keyboard from the app's own fields.
#else
  content::RenderWidgetHost* widget = Widget();
  // Active as well as focused: Blink only draws the caret in a focused frame
  // of an active page, and a windowless host never gets window activation.
  if (focused) {
    web_contents_->Focus();
    if (widget) {
      widget->Focus();
      widget->SetActive(true);
    }
  } else if (widget) {
    widget->SetActive(false);
    widget->Blur();
  }
#endif
}

void View::Eval(int id, const std::u16string& js) {
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      js,
      base::BindOnce(
          [](base::WeakPtr<View> view, int id, base::Value result) {
            if (!view) {
              return;
            }
            view->Emit(base::DictValue()
                           .Set("type", "eval")
                           .Set("id", id)
                           .Set("ok", true)
                           .Set("result", std::move(result)));
          },
          weak_factory_.GetWeakPtr(), id),
      kEvalWorldId);
}

void View::DialogReply(int id, bool accept, const std::u16string& text) {
  auto it = dialogs_.find(id);
  if (it == dialogs_.end()) {
    return;
  }
  DialogClosedCallback callback = std::move(it->second);
  dialogs_.erase(it);
  std::move(callback).Run(accept, text);
}

// content::WebContentsDelegate ---------------------------------------------

content::WebContents* View::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // Chorium never opens windows itself: the app decides, and opens its own
  // view for the URL if it wants one. The page's new window is dropped.
  Emit(base::DictValue()
           .Set("type", "new_window")
           .Set("url", target_url.spec())
           .Set("user_gesture", user_gesture));
  if (was_blocked) {
    *was_blocked = true;
  }
  return nullptr;
}

content::WebContents* View::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  if (params.disposition == WindowOpenDisposition::CURRENT_TAB) {
    base::WeakPtr<content::NavigationHandle> handle =
        source->GetController().LoadURLWithParams(
            content::NavigationController::LoadURLParams(params));
    if (handle && navigation_handle_callback) {
      std::move(navigation_handle_callback).Run(*handle);
    }
    return source;
  }
  Emit(base::DictValue()
           .Set("type", "new_window")
           .Set("url", params.url.spec())
           .Set("user_gesture", params.user_gesture));
  return nullptr;
}

#if BUILDFLAG(IS_ANDROID)
void View::SetOverlayMode(bool use_overlay_mode) {
  android::SetOverlayMode(use_overlay_mode);
}
#endif

void View::CloseContents(content::WebContents* source) {
  Emit(base::DictValue().Set("type", "closed"));
}

bool View::HandleContextMenu(content::RenderFrameHost& render_frame_host,
                             const content::ContextMenuParams& params) {
  bool image =
      params.media_type == blink::mojom::ContextMenuDataMediaType::kImage &&
      params.src_url.is_valid();
  bool link = params.link_url.is_valid();
  // Text fields keep their own menu (Android's paste bubble). On Android a
  // long press on plain text selects it with the system's selection menu.
#if BUILDFLAG(IS_ANDROID)
  bool selection = false;
#else
  bool selection = !params.selection_text.empty();
#endif
  if (params.is_editable || (!link && !image && !selection)) {
    return false;
  }
  Emit(base::DictValue()
           .Set("type", "context_menu")
           .Set("x", params.x)
           .Set("y", params.y)
           .Set("link_url", link ? params.link_url.spec() : std::string())
           .Set("link_text", link ? params.link_text : std::u16string())
           .Set("image_url", image ? params.src_url.spec() : std::string())
           .Set("selection", params.selection_text));
  return true;
}

void View::Overscroll(const char* kind, const char* phase, float dx,
                      float dy) {
  Emit(base::DictValue()
           .Set("type", "overscroll")
           .Set("kind", kind)
           .Set("phase", phase)
           .Set("dx", dx)
           .Set("dy", dy));
}

content::JavaScriptDialogManager* View::GetJavaScriptDialogManager(
    content::WebContents* source) {
  return this;
}

bool View::CanEnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame) {
  return true;
}

void View::EnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {
  fullscreen_ = true;
  Emit(base::DictValue().Set("type", "fullscreen").Set("on", true));
  // As content shell: the page learns its new state (and size) at once.
  if (content::RenderWidgetHost* widget = Widget()) {
    widget->SynchronizeVisualProperties();
  }
}

void View::ExitFullscreenModeForTab(content::WebContents* source) {
  fullscreen_ = false;
  Emit(base::DictValue().Set("type", "fullscreen").Set("on", false));
  if (content::RenderWidgetHost* widget = Widget()) {
    widget->SynchronizeVisualProperties();
  }
}

bool View::IsFullscreenForTabOrPending(
    const content::WebContents* web_contents) {
  return fullscreen_;
}

// content::WebContentsObserver ---------------------------------------------

void View::RenderFrameHostChanged(content::RenderFrameHost* old_host,
                                  content::RenderFrameHost* new_host) {
  if (new_host && new_host->IsInPrimaryMainFrame()) {
    StartCapture();
  }
}

void View::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) {
    return;
  }
  committed_url_ = navigation->GetURL();
  committed_error_ =
      navigation->IsErrorPage() ? navigation->GetNetErrorCode() : net::OK;
  content::NavigationController& controller = web_contents_->GetController();
  Emit(base::DictValue()
           .Set("type", "url")
           .Set("url", web_contents_->GetLastCommittedURL().spec())
           .Set("can_go_back", controller.CanGoBack())
           .Set("can_go_forward", controller.CanGoForward()));
  // A page restored from the back/forward cache gets no TitleWasSet, so
  // always report the title that goes with the new URL.
  Emit(base::DictValue()
           .Set("type", "title")
           .Set("title", base::UTF16ToUTF8(web_contents_->GetTitle())));
  if (!capturer_) {
    StartCapture();
  }
}

void View::TitleWasSet(content::NavigationEntry* entry) {
  Emit(base::DictValue()
           .Set("type", "title")
           .Set("title", base::UTF16ToUTF8(web_contents_->GetTitle())));
}

void View::DidStartLoading() {
  Emit(base::DictValue().Set("type", "loading").Set("loading", true));
}

void View::DidStopLoading() {
  Emit(base::DictValue().Set("type", "loading").Set("loading", false));
}

void View::DidFinishLoad(content::RenderFrameHost* render_frame_host,
                         const GURL& validated_url) {
  if (!render_frame_host->IsInPrimaryMainFrame()) {
    return;
  }
  if (committed_error_ != net::OK) {
    // Report the URL that failed, not chrome-error://chromewebdata/.
    Emit(base::DictValue()
             .Set("type", "loaded")
             .Set("url", committed_url_.spec())
             .Set("ok", false)
             .Set("error", net::ErrorToShortString(committed_error_)));
    return;
  }
  Emit(base::DictValue()
           .Set("type", "loaded")
           .Set("url", validated_url.spec())
           .Set("ok", true)
           .Set("error", ""));
}

void View::DidFailLoad(content::RenderFrameHost* render_frame_host,
                       const GURL& validated_url,
                       int error_code) {
  if (!render_frame_host->IsInPrimaryMainFrame()) {
    return;
  }
  Emit(base::DictValue()
           .Set("type", "loaded")
           .Set("url", validated_url.spec())
           .Set("ok", false)
           .Set("error", net::ErrorToShortString(error_code)));
}

void View::PrimaryMainFrameRenderProcessGone(base::TerminationStatus status) {
  capturer_.reset();
  Emit(base::DictValue().Set("type", "crashed"));
}

// content::JavaScriptDialogManager -----------------------------------------

void View::RunJavaScriptDialog(content::WebContents* web_contents,
                               content::RenderFrameHost* render_frame_host,
                               content::JavaScriptDialogType dialog_type,
                               const std::u16string& message_text,
                               const std::u16string& default_prompt_text,
                               DialogClosedCallback callback,
                               bool* did_suppress_message) {
  const char* kind = "alert";
  if (dialog_type == content::JAVASCRIPT_DIALOG_TYPE_CONFIRM) {
    kind = "confirm";
  } else if (dialog_type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT) {
    kind = "prompt";
  }
  const int id = next_dialog_id_++;
  dialogs_[id] = std::move(callback);
  Emit(base::DictValue()
           .Set("type", "dialog")
           .Set("id", id)
           .Set("kind", kind)
           .Set("message", base::UTF16ToUTF8(message_text))
           .Set("default", base::UTF16ToUTF8(default_prompt_text)));
}

void View::RunBeforeUnloadDialog(content::WebContents* web_contents,
                                 content::RenderFrameHost* render_frame_host,
                                 bool is_reload,
                                 DialogClosedCallback callback) {
  const int id = next_dialog_id_++;
  dialogs_[id] = std::move(callback);
  Emit(base::DictValue()
           .Set("type", "dialog")
           .Set("id", id)
           .Set("kind", "beforeunload")
           .Set("message", "")
           .Set("default", ""));
}

void View::CancelDialogs(content::WebContents* web_contents,
                         bool reset_state) {
  std::map<int, DialogClosedCallback> dialogs = std::move(dialogs_);
  dialogs_.clear();
  for (auto& [id, callback] : dialogs) {
    std::move(callback).Run(false, std::u16string());
  }
}

// viz::mojom::FrameSinkVideoConsumer ---------------------------------------

void View::OnFrameCaptured(
    media::mojom::VideoBufferHandlePtr data,
    media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        callbacks) {
  mojo::Remote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks> done(
      std::move(callbacks));
  if (!handle_->on_frame || !data->is_read_only_shmem_region() ||
      info->pixel_format != media::PIXEL_FORMAT_ARGB) {
    done->Done();
    return;
  }
  base::ReadOnlySharedMemoryMapping mapping =
      data->get_read_only_shmem_region().Map();
  if (!mapping.IsValid()) {
    done->Done();
    return;
  }
  const int stride = info->strides
                         ? static_cast<int>(info->strides->stride_by_plane[0])
                         : info->coded_size.width() * 4;
  const gfx::Rect rect = content_rect.IsEmpty() ? info->visible_rect
                                                : content_rect;
  const size_t offset =
      static_cast<size_t>(rect.y()) * stride + static_cast<size_t>(rect.x()) * 4;
  const size_t needed = offset + static_cast<size_t>(rect.height() - 1) * stride +
                        static_cast<size_t>(rect.width()) * 4;
  base::span<const uint8_t> bytes = mapping.GetMemoryAsSpan<uint8_t>();
  if (rect.IsEmpty() || bytes.size() < needed) {
    done->Done();
    return;
  }
  // The app gets |rect|'s pixels: |needed| bytes from |offset| on.
  base::span<const uint8_t> pixels = bytes.subspan(offset, needed - offset);
  handle_->on_frame(handle_, pixels.data(), rect.width(), rect.height(),
                    stride, handle_->user);
  done->Done();
}

}  // namespace chorium
