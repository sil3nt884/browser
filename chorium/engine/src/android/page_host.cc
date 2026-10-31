// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/android/page_host.h"

#include <stdint.h>

#include <array>
#include <vector>

#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "content/public/browser/web_contents.h"
#include "umbra/chorium/engine/chorium_engine_jni/PageHost_jni.h"
#include "umbra/chorium/engine/src/view.h"

namespace chorium::android {

std::pair<base::FilePath, base::FilePath> CreatePublicDownload(
    const std::string& name, const std::string& mime, const std::string& url) {
  JNIEnv* env = base::android::AttachCurrentThread();
  auto result = Java_PageHost_createDownloadTarget(
      env, base::android::ConvertUTF8ToJavaString(env, name),
      base::android::ConvertUTF8ToJavaString(env, mime),
      base::android::ConvertUTF8ToJavaString(env, url));
  std::vector<std::string> parts;
  base::android::AppendJavaStringArrayToStringVector(env, result, &parts);
  return {base::FilePath(parts.at(0)), base::FilePath(parts.at(1))};
}

namespace {

// The handle's address identifies the page on the Java side.
int64_t Key(chorium_view* handle) {
  return static_cast<int64_t>(reinterpret_cast<intptr_t>(handle));
}

}  // namespace

void AddPage(chorium_view* handle, content::WebContents* web_contents) {
  JNIEnv* env = base::android::AttachCurrentThread();
  Java_PageHost_addPage(env, Key(handle), web_contents->GetJavaWebContents());
}

void RemovePage(chorium_view* handle) {
  Java_PageHost_removePage(base::android::AttachCurrentThread(), Key(handle));
}

void ShowPage(chorium_view* handle, int x, int y, int width, int height) {
  Java_PageHost_showPage(base::android::AttachCurrentThread(), Key(handle), x,
                         y, width, height);
}

void HidePages() {
  Java_PageHost_hidePages(base::android::AttachCurrentThread());
}

void SetOverlayMode(bool use_overlay_mode) {
  Java_PageHost_setOverlayMode(base::android::AttachCurrentThread(),
                               use_overlay_mode);
}

// PageHost.java's overscroll handler; |key| is the page's handle.
static void JNI_PageHost_Overscroll(JNIEnv* env,
                                    jlong key,
                                    jint kind,
                                    jint phase,
                                    jfloat dx,
                                    jfloat dy) {
  static constexpr std::array<const char*, 3> kKinds = {"refresh", "back",
                                                        "forward"};
  static constexpr std::array<const char*, 4> kPhases = {"start", "pull",
                                                         "release", "cancel"};
  auto* handle = reinterpret_cast<chorium_view*>(static_cast<intptr_t>(key));
  if (kind < 0 || static_cast<size_t>(kind) >= kKinds.size() || phase < 0 ||
      static_cast<size_t>(phase) >= kPhases.size() || !handle->view) {
    return;
  }
  handle->view->Overscroll(kKinds[kind], kPhases[phase], dx, dy);
}

}  // namespace chorium::android

DEFINE_JNI(PageHost)
