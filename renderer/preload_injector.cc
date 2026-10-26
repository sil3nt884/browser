// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/renderer/preload_injector.h"

#include <string>

#include "base/command_line.h"
#include "base/strings/string_util.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_isolated_world_info.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "umbra/common/umbra_switches.h"
#include "umbra/renderer/preload_script.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace umbra {

namespace {

// Added to chrome/common/chrome_isolated_world_ids.h by scripts/hooks.py
// (hook "isolated-world-id").
constexpr int32_t kWorldId = ISOLATED_WORLD_ID_UMBRA_PRELOAD;

void EnsureWorldInfo() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  blink::WebIsolatedWorldInfo info;
  info.security_origin = blink::WebSecurityOrigin::Create(
      GURL("chrome://umbra-preload/"));
  info.human_readable_name = blink::WebString::FromUTF8("Umbra preload");
  blink::SetIsolatedWorldInfo(kWorldId, info);
}

std::string BuildScript(bool preload, bool video) {
  std::string s(kPreloadScript);
  base::ReplaceFirstSubstringAfterOffset(&s, 0, "__PRELOAD__",
                                         preload ? "true" : "false");
  base::ReplaceFirstSubstringAfterOffset(&s, 0, "__VIDEO__",
                                         video ? "true" : "false");
  return s;
}

}  // namespace

// static
void PreloadInjector::MaybeAttach(content::RenderFrame* render_frame) {
  if (!render_frame || !render_frame->IsMainFrame()) {
    return;
  }
  const auto* cl = base::CommandLine::ForCurrentProcess();
  const bool preload = !cl->HasSwitch(switches::kDisablePreload);
  const bool video = !cl->HasSwitch(switches::kDisableVideoPreload);
  if (!preload && !video) {
    return;
  }
  new PreloadInjector(render_frame, preload, video);
}

PreloadInjector::PreloadInjector(content::RenderFrame* render_frame,
                                 bool enable_preload,
                                 bool enable_video)
    : content::RenderFrameObserver(render_frame),
      enable_preload_(enable_preload),
      enable_video_(enable_video) {}

PreloadInjector::~PreloadInjector() = default;

void PreloadInjector::DidDispatchDOMContentLoadedEvent() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame) {
    return;
  }
  const GURL url = frame->GetDocument().Url();
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;  // No chrome://, file://, data:, etc.
  }
  EnsureWorldInfo();
  const std::string script = BuildScript(enable_preload_, enable_video_);
  frame->ExecuteScriptInIsolatedWorld(
      kWorldId, blink::WebScriptSource(blink::WebString::FromUTF8(script)),
      blink::BackForwardCacheAware::kAllow);
}

void PreloadInjector::OnDestruct() {
  delete this;
}

}  // namespace umbra
