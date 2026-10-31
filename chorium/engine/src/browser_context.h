// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_
#define UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_

#include <memory>

#include "base/files/file_path.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#if BUILDFLAG(IS_ANDROID)
#include "base/android/application_status_listener.h"
#endif

#if BUILDFLAG(IS_WIN)
class PrefService;
class CookieEncryptionProviderImpl;
namespace os_crypt_async {
class OSCryptAsync;
}
#endif

namespace chorium {

class DownloadDelegate;

// The one profile all views share. Persistent only when the host supplies a
// profile directory; the page cache always stays in RAM. Anything that would
// need a settings UI (permissions, certificate exceptions) is absent, which
// means "deny". Downloads go straight to the Downloads folder.
class BrowserContext : public content::BrowserContext {
 public:
  BrowserContext();
  BrowserContext(const BrowserContext&) = delete;
  BrowserContext& operator=(const BrowserContext&) = delete;
  ~BrowserContext() override;
#if BUILDFLAG(IS_WIN)
  CookieEncryptionProviderImpl* cookie_encryption() {
    return cookie_encryption_.get();
  }
#endif

  // content::BrowserContext:
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  base::FilePath GetPath() const override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PlatformNotificationService* GetPlatformNotificationService()
      override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService()
      override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::ReduceAcceptLanguageControllerDelegate*
  GetReduceAcceptLanguageControllerDelegate() override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;

 private:
#if BUILDFLAG(IS_ANDROID)
  void OnApplicationStateChanged(base::android::ApplicationState state);
  std::unique_ptr<base::android::ApplicationStatusListener> app_state_listener_;
#endif
  base::FilePath path_;
  bool off_the_record_ = true;
#if BUILDFLAG(IS_WIN)
  // Destruction order: receivers, key providers, then the prefs they use.
  std::unique_ptr<PrefService> local_state_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_;
  std::unique_ptr<CookieEncryptionProviderImpl> cookie_encryption_;
#endif
  std::unique_ptr<DownloadDelegate> download_delegate_;
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_
