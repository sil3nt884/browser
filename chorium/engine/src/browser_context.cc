// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/browser_context.h"

#include "base/files/file_util.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/zoom_level_delegate.h"
#include "umbra/chorium/engine/src/downloads.h"
#include "umbra/chorium/engine/src/engine.h"
#include "base/logging.h"
#if BUILDFLAG(IS_WIN)
#include "components/os_crypt/async/browser/dpapi_key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/os_crypt_win.h"
#include "components/prefs/json_pref_store.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#endif

namespace chorium {

BrowserContext::BrowserContext() {
  if (!State().profile_dir.empty()) {
    path_ = State().profile_dir;
    // A marker outside the profile also covers force-stop and power loss.
    // Only the engine-owned profile is removed; app settings/history stay put.
    const auto marker = path_.AddExtensionASCII("clear-on-close");
    State().clear_site_data_on_close = base::PathExists(marker);
    if (State().clear_site_data_on_close) {
      CHECK(base::DeletePathRecursively(path_)) << "Cannot clear Chorium profile";
    }
    CHECK(base::CreateDirectory(path_)) << "Cannot create Chorium profile";
    off_the_record_ = false;
#if BUILDFLAG(IS_WIN)
    auto registry = base::MakeRefCounted<PrefRegistrySimple>();
    os_crypt_async::RegisterLocalPrefs(registry.get());
    PrefServiceFactory factory;
    factory.set_user_prefs(base::MakeRefCounted<JsonPrefStore>(
        path_.AppendASCII("Local State")));
    local_state_ = factory.Create(registry);
    CHECK(os_crypt_async::Init(local_state_.get()))
        << "Cannot initialize Windows cookie encryption";
    local_state_->CommitPendingWrite();
    std::vector<std::pair<os_crypt_async::OSCryptAsync::Precedence,
                          std::unique_ptr<os_crypt_async::KeyProvider>>> providers;
    providers.emplace_back(0, std::make_unique<os_crypt_async::DPAPIKeyProvider>(
                                  local_state_.get()));
    os_crypt_ = std::make_unique<os_crypt_async::OSCryptAsync>(std::move(providers));
    cookie_encryption_ =
        std::make_unique<CookieEncryptionProviderImpl>(os_crypt_.get());
#endif
    return;
  }
  base::FilePath temp;
  base::GetTempDir(&temp);
  path_ = temp.AppendASCII(
      "chorium-" + base::NumberToString(base::GetCurrentProcId()));
}

BrowserContext::~BrowserContext() {
  NotifyWillBeDestroyed();
  ShutdownStoragePartitions();
}

std::unique_ptr<content::ZoomLevelDelegate>
BrowserContext::CreateZoomLevelDelegate(const base::FilePath&) {
  return nullptr;
}

base::FilePath BrowserContext::GetPath() const {
  return path_;
}

bool BrowserContext::IsOffTheRecord() {
  return off_the_record_;
}

content::DownloadManagerDelegate*
BrowserContext::GetDownloadManagerDelegate() {
  if (!download_delegate_) {
    download_delegate_ = std::make_unique<DownloadDelegate>();
    download_delegate_->Attach(GetDownloadManager());
  }
  return download_delegate_.get();
}

content::BrowserPluginGuestManager* BrowserContext::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy* BrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PlatformNotificationService*
BrowserContext::GetPlatformNotificationService() {
  return nullptr;
}

content::PushMessagingService* BrowserContext::GetPushMessagingService() {
  return nullptr;
}

content::StorageNotificationService*
BrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate* BrowserContext::GetSSLHostStateDelegate() {
  return nullptr;
}

content::PermissionControllerDelegate*
BrowserContext::GetPermissionControllerDelegate() {
  return nullptr;
}

content::ReduceAcceptLanguageControllerDelegate*
BrowserContext::GetReduceAcceptLanguageControllerDelegate() {
  return nullptr;
}

content::ClientHintsControllerDelegate*
BrowserContext::GetClientHintsControllerDelegate() {
  return nullptr;
}

content::BackgroundFetchDelegate* BrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
BrowserContext::GetBackgroundSyncController() {
  return nullptr;
}

content::BrowsingDataRemoverDelegate*
BrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

}  // namespace chorium
