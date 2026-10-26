#include "chrome/renderer/chrome_content_renderer_client.h"

#include <memory>

void ChromeContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  ChromeRenderFrameObserver* render_frame_observer =
      new ChromeRenderFrameObserver(render_frame, web_cache_impl_.get());
}
