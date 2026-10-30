// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// libchorium.so's entry point. The same library runs in the app's process
// (the runtime, the engine's browser side) and in Chromium's page, GPU and
// network processes (Android services); content takes it from here.

#include "base/android/jni_android.h"
#include "base/android/library_loader/library_loader_hooks.h"
#include "content/public/app/content_jni_onload.h"
#include "content/public/app/content_main.h"
#include "umbra/chorium/engine/src/engine.h"

namespace {

bool InitializeChorium(
    base::android::LibraryProcessType library_process_type) {
  if (!content::android::OnJNIOnLoadInit()) {
    return false;
  }
  content::SetContentMainDelegate(chorium::GetMainDelegate());
  return true;
}

}  // namespace

JNI_EXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  base::android::InitVM(vm);
  base::android::SetNativeInitializationHook(InitializeChorium);
  return JNI_VERSION_1_4;
}
