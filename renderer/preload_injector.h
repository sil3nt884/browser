// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_RENDERER_PRELOAD_INJECTOR_H_
#define UMBRA_RENDERER_PRELOAD_INJECTOR_H_

#include "base/memory/weak_ptr.h"
#include "content/public/renderer/render_frame_observer.h"

namespace content {
class RenderFrame;
}

namespace umbra {

// Injects kPreloadScript into each top-level http(s) document once its DOM is
// ready. Owns itself; deleted when the frame goes away.
class PreloadInjector : public content::RenderFrameObserver {
 public:
  // Hooked into ChromeContentRendererClient::RenderFrameCreated().
  static void MaybeAttach(content::RenderFrame* render_frame);

  PreloadInjector(const PreloadInjector&) = delete;
  PreloadInjector& operator=(const PreloadInjector&) = delete;

 private:
  PreloadInjector(content::RenderFrame* render_frame,
                  bool enable_preload,
                  bool enable_video);
  ~PreloadInjector() override;

  // content::RenderFrameObserver:
  void DidDispatchDOMContentLoadedEvent() override;
  void OnDestruct() override;

  // Runs the script. Posted from DidDispatchDOMContentLoadedEvent(), which
  // Blink calls inside a ScriptForbiddenScope (FrameLoader::FinishedParsing):
  // script executed synchronously there is compiled but silently not run.
  void Inject();

  const bool enable_preload_;
  const bool enable_video_;
  base::WeakPtrFactory<PreloadInjector> weak_factory_{this};
};

}  // namespace umbra

#endif  // UMBRA_RENDERER_PRELOAD_INJECTOR_H_
