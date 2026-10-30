// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_RENDERER_CLIENT_H_
#define UMBRA_CHORIUM_ENGINE_SRC_RENDERER_CLIENT_H_

#include "content/public/renderer/content_renderer_client.h"

namespace chorium {

// Runs in each page process.
class RendererClient : public content::ContentRendererClient {
 public:
  void RunScriptsAtDocumentStart(content::RenderFrame* render_frame) override;
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_RENDERER_CLIENT_H_
