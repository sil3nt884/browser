// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_
#define UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_

#include <memory>

#include "base/files/file_path.h"
#include "content/public/browser/browser_context.h"

namespace chorium {

// The one profile all views share. Off the record: cookies, cache and storage
// live in memory only and are gone when the engine stops. Anything that would
// need a settings UI (permissions, downloads, certificate exceptions) is
// absent, which means "deny".
class BrowserContext : public content::BrowserContext {
 public:
  BrowserContext();
  BrowserContext(const BrowserContext&) = delete;
  BrowserContext& operator=(const BrowserContext&) = delete;
  ~BrowserContext() override;

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
  // Chromium wants a path even for in-memory profiles. Nothing is written
  // there, so it is never created.
  base::FilePath path_;
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_BROWSER_CONTEXT_H_
