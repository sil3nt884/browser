// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_DOWNLOADS_H_
#define UMBRA_CHORIUM_ENGINE_SRC_DOWNLOADS_H_

#include <map>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_manager_delegate.h"

namespace chorium {

// Saves every download straight into the Downloads folder (or
// State().download_dir) under a name that doesn't overwrite anything, and
// reports each one's progress to the app as "download" events. Engine
// thread only.
class DownloadDelegate : public content::DownloadManagerDelegate,
                         public content::DownloadManager::Observer,
                         public download::DownloadItem::Observer {
 public:
  DownloadDelegate();
  DownloadDelegate(const DownloadDelegate&) = delete;
  DownloadDelegate& operator=(const DownloadDelegate&) = delete;
  ~DownloadDelegate() override;

  // Separate from the constructor: the manager asks its browser context for
  // the delegate while it is being created.
  void Attach(content::DownloadManager* manager);
  void Cancel(uint32_t id);

  // content::DownloadManagerDelegate:
  void Shutdown() override;
  bool DetermineDownloadTarget(
      download::DownloadItem* item,
      download::DownloadTargetCallback* callback) override;

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void ManagerGoingDown(content::DownloadManager* manager) override;

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

 private:
  void Report(download::DownloadItem* item);

  raw_ptr<content::DownloadManager> manager_;
  // When each download last reported progress, and the state it reported.
  struct Last {
    base::TimeTicks time;
    download::DownloadItem::DownloadState state;
  };
  std::map<uint32_t, Last> last_;
  base::WeakPtrFactory<DownloadDelegate> weak_factory_{this};
};

// chorium_cancel_download. Engine thread.
void CancelDownload(uint32_t id);

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_DOWNLOADS_H_
