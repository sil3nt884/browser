// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_VIEW_H_
#define UMBRA_CHORIUM_ENGINE_SRC_VIEW_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/size.h"
#include "umbra/chorium/interfaces/native/chorium.h"
#include "url/gurl.h"

namespace viz {
class ClientFrameSinkVideoCapturer;
}

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace chorium {
class View;
class WindowHost;
}  // namespace chorium

// The handle the C API hands out. Created on the caller's thread; |view| is
// only touched on the engine thread.
struct chorium_view {
  chorium_frame_fn on_frame = nullptr;
  chorium_event_fn on_event = nullptr;
  // The app's own pointer, only handed back to its callbacks.
  RAW_PTR_EXCLUSION void* user = nullptr;
  raw_ptr<chorium::View> view = nullptr;
};

namespace chorium {

// One page: a WebContents in a windowless WindowHost (desktop) or a native
// view (Android), with its frames captured and handed to the C callbacks
// when the caller wants them. Engine thread only.
class View : public content::WebContentsDelegate,
             public content::WebContentsObserver,
             public content::JavaScriptDialogManager,
             public viz::mojom::FrameSinkVideoConsumer {
 public:
  // Creates the view for |handle| and sets handle->view.
  static void Create(chorium_view* handle, int width, int height);
  // Destroys the view and frees |handle|.
  static void Close(chorium_view* handle);
  // Destroys every view (engine shutdown).
  static void CloseAll();
  // Applies changed preferences (the color scheme) to every page.
  static void PreferencesChanged();
  // Applies State().frame_hz to every page.
  static void FrameRateChanged();
  // The view showing |web_contents|, else any view; null if none is open.
  static View* ForWebContentsOrAny(content::WebContents* web_contents);

  View(const View&) = delete;
  View& operator=(const View&) = delete;
  ~View() override;

  void LoadUrl(const std::string& url);
  void LoadHtml(const std::string& html, const std::string& base_url);
  void Back();
  void Forward();
  void Reload();
  void Stop();
  void Resize(int width, int height);
  void RefreshFrame();

  void Mouse(int type, int x, int y, int button, int click_count, int mods);
  void Wheel(int x, int y, float dx, float dy, int mods);
  void Key(bool down, int key_code, int mods);
  void Text(const std::u16string& text);
  void SetFocus(bool focused);
  void Eval(int id, const std::u16string& js);
  void DialogReply(int id, bool accept, const std::u16string& text);
  // Android: the page was pulled past its edge (see "overscroll" in
  // chorium.h). |dx| and |dy| are CSS pixels.
  void Overscroll(const char* kind, const char* phase, float dx, float dy);
  // CHORIUM_EDIT_* on the focused text field.
  void Edit(int command);
  void SaveUrl(const std::string& url);
  void CopyImageAt(int x, int y);
  void FileChooserReply(int id, const std::vector<std::string>& paths);

  // Sends |event| (JSON, see chorium.h) to the app.
  void Emit(base::DictValue event);

 private:
  View(chorium_view* handle, int width, int height);

  content::RenderWidgetHost* Widget();
  void StartCapture();
  void EndWheel();
  int ButtonModifiers() const;

  // content::WebContentsDelegate:
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  void CloseContents(content::WebContents* source) override;
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
#if BUILDFLAG(IS_ANDROID)
  void SetOverlayMode(bool use_overlay_mode) override;
#endif
  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;
  bool CanEnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame) override;
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* source) override;
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) override;

  // content::WebContentsObserver:
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void TitleWasSet(content::NavigationEntry* entry) override;
  void DidStartLoading() override;
  void DidStopLoading() override;
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void DidFailLoad(content::RenderFrameHost* render_frame_host,
                   const GURL& validated_url,
                   int error_code) override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

  // viz::mojom::FrameSinkVideoConsumer:
  void OnFrameCaptured(
      media::mojom::VideoBufferHandlePtr data,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override;
  void OnNewCaptureVersion(
      const media::CaptureVersion& capture_version) override {}
  void OnFrameWithEmptyRegionCapture() override {}
  void OnStopped() override {}
  void OnLog(const std::string& message) override {}

  const raw_ptr<chorium_view> handle_;
  gfx::Size size_;  // CSS pixels
  bool fullscreen_ = false;
  // A run of wheel events is one scroll gesture, ended a moment after the
  // last event (see Wheel).
  bool wheel_active_ = false;
  gfx::Point wheel_point_;
  int wheel_modifiers_ = 0;
  base::OneShotTimer wheel_end_;
  int buttons_down_ = 0;  // blink::WebInputEvent button modifier bits
  int next_dialog_id_ = 1;
  std::map<int, DialogClosedCallback> dialogs_;
  // Open file choosers, by the id the app answers with, and their mode.
  struct Chooser {
    scoped_refptr<content::FileSelectListener> listener;
    blink::mojom::FileChooserParams::Mode mode;
  };
  int next_chooser_id_ = 1;
  std::map<int, Chooser> choosers_;
  // The last committed main-frame navigation: an error page commits as
  // chrome-error:// and then "finishes loading" like any other page.
  GURL committed_url_;
  int committed_error_ = 0;  // net::Error, 0 = OK

#if !BUILDFLAG(IS_ANDROID)
  std::unique_ptr<WindowHost> host_;  // the page's windowless aura root
#endif
  std::unique_ptr<content::WebContents> web_contents_;
  std::unique_ptr<viz::ClientFrameSinkVideoCapturer> capturer_;
  base::ScopedClosureRunner keep_rendering_;

  base::WeakPtrFactory<View> weak_factory_{this};
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_VIEW_H_
