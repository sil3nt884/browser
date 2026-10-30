// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/renderer_client.h"

#include "build/build_config.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_css_origin.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace chorium {

namespace {

#if !BUILDFLAG(IS_ANDROID)
// A normal <select> opens its list in a separate popup widget, which the
// desktop engine doesn't capture or send input to: the list never showed and
// clicks went to the page. A customizable select (appearance: base-select)
// draws its list inside the page, in the top layer, so it is part of the
// page's frames and gets its clicks. User origin with !important outranks
// the page's own styles. Android shows the native list instead.
constexpr char kSelectInPage[] =
    "select:not([multiple]), ::picker(select) {"
    "  appearance: base-select !important;"
    "}";
#endif

}  // namespace

void RendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
#if !BUILDFLAG(IS_ANDROID)
  render_frame->GetWebFrame()->GetDocument().InsertStyleSheet(
      blink::WebString::FromUtf8(kSelectInPage), nullptr,
      blink::WebCssOrigin::kUser);
#endif
}

}  // namespace chorium
