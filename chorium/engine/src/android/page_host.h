// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_ANDROID_PAGE_HOST_H_
#define UMBRA_CHORIUM_ENGINE_SRC_ANDROID_PAGE_HOST_H_

#include <string>
#include <utility>
#include "base/files/file_path.h"

struct chorium_view;

namespace content {
class WebContents;
}

namespace chorium::android {

// Pages on Android are native views owned by the Java PageHost
// (engine/android/java/.../PageHost.java), one ContentView per page drawn
// through a single ContentViewRenderView. UI thread only.

void AddPage(chorium_view* handle, content::WebContents* web_contents);
void RemovePage(chorium_view* handle);
// Pixels of the page host view.
void ShowPage(chorium_view* handle, int x, int y, int width, int height);
void HidePages();
// Video in an overlay under the page (WebContentsDelegate::SetOverlayMode).
void SetOverlayMode(bool use_overlay_mode);

// Worker thread: MediaStore URI and the reserved display name.
std::pair<base::FilePath, base::FilePath> CreatePublicDownload(
    const std::string& name, const std::string& mime, const std::string& url);

}  // namespace chorium::android

#endif  // UMBRA_CHORIUM_ENGINE_SRC_ANDROID_PAGE_HOST_H_
