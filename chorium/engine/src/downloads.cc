// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/downloads.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_item_utils.h"
#include "net/base/filename_util.h"
#include "umbra/chorium/engine/src/browser_context.h"
#include "umbra/chorium/engine/src/engine.h"
#include "umbra/chorium/engine/src/view.h"

#if BUILDFLAG(IS_ANDROID)
#include "base/android/path_utils.h"
#elif BUILDFLAG(IS_WIN)
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "base/win/scoped_co_mem.h"
#endif

namespace chorium {

namespace {

// Progress events at most this often per download.
constexpr base::TimeDelta kProgressInterval = base::Milliseconds(250);

base::FilePath DefaultDownloadDir() {
  base::FilePath dir;
#if BUILDFLAG(IS_ANDROID)
  // The shared Download folder: since Android 11 an app may create files
  // there by path, and they show up in the Files app and other apps.
  base::android::GetDownloadsDirectory(&dir);
#elif BUILDFLAG(IS_WIN)
  base::win::ScopedCoMem<wchar_t> path;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr,
                                       &path))) {
    dir = base::FilePath(path.get());
  }
#endif
  if (dir.empty()) {
    base::PathService::Get(base::DIR_HOME, &dir);
    dir = dir.AppendASCII("Downloads");
  }
  return dir;
}

// Off the engine thread: a free name in |dir| for what |url| sent.
base::FilePath ChooseTarget(base::FilePath dir,
                            GURL url,
                            std::string content_disposition,
                            std::string suggested_name,
                            std::string mime_type) {
  if (dir.empty()) {
    dir = DefaultDownloadDir();
  }
  base::CreateDirectory(dir);
  base::FilePath name =
      net::GenerateFileName(url, content_disposition, std::string(),
                            suggested_name, mime_type, "download");
  return base::GetUniquePath(dir.Append(name));
}

const char* StateName(download::DownloadItem::DownloadState state) {
  switch (state) {
    case download::DownloadItem::IN_PROGRESS:
      return "in_progress";
    case download::DownloadItem::COMPLETE:
      return "done";
    case download::DownloadItem::CANCELLED:
      return "cancelled";
    case download::DownloadItem::INTERRUPTED:
    case download::DownloadItem::MAX_DOWNLOAD_STATE:
      break;
  }
  return "failed";
}

DownloadDelegate* g_delegate = nullptr;

}  // namespace

DownloadDelegate::DownloadDelegate() {
  g_delegate = this;
}

void DownloadDelegate::Attach(content::DownloadManager* manager) {
  manager_ = manager;
  manager_->AddObserver(this);
}

DownloadDelegate::~DownloadDelegate() {
  Shutdown();
  if (g_delegate == this) {
    g_delegate = nullptr;
  }
}

void DownloadDelegate::Shutdown() {
  if (manager_) {
    manager_->RemoveObserver(this);
    manager_ = nullptr;
  }
  weak_factory_.InvalidateWeakPtrs();
}

void DownloadDelegate::ManagerGoingDown(content::DownloadManager* manager) {
  Shutdown();
}

bool DownloadDelegate::DetermineDownloadTarget(
    download::DownloadItem* item,
    download::DownloadTargetCallback* callback) {
  if (!item->GetForcedFilePath().empty()) {
    download::DownloadTargetInfo target;
    target.target_path = item->GetForcedFilePath();
    target.intermediate_path = item->GetForcedFilePath();
    std::move(*callback).Run(std::move(target));
    return true;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN,
       base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ChooseTarget, State().download_dir, item->GetURL(),
                     item->GetContentDisposition(),
                     item->GetSuggestedFilename(), item->GetMimeType()),
      base::BindOnce(&DownloadDelegate::OnTargetChosen,
                     weak_factory_.GetWeakPtr(), item->GetId(),
                     std::move(*callback)));
  return true;
}

void DownloadDelegate::OnTargetChosen(uint32_t id,
                                      download::DownloadTargetCallback callback,
                                      const base::FilePath& path) {
  download::DownloadTargetInfo target;
  target.target_path = path;
  target.intermediate_path = path.AddExtension(FILE_PATH_LITERAL("crdownload"));
  std::move(callback).Run(std::move(target));
}

void DownloadDelegate::OnDownloadCreated(content::DownloadManager* manager,
                                         download::DownloadItem* item) {
  item->AddObserver(this);
}

void DownloadDelegate::OnDownloadUpdated(download::DownloadItem* item) {
  if (item->GetTargetFilePath().empty()) {
    return;  // no name yet
  }
  auto it = last_.find(item->GetId());
  base::TimeTicks now = base::TimeTicks::Now();
  if (it != last_.end() && it->second.state == item->GetState() &&
      now - it->second.time < kProgressInterval) {
    return;
  }
  last_[item->GetId()] = {now, item->GetState()};
  Report(item);
}

void DownloadDelegate::OnDownloadDestroyed(download::DownloadItem* item) {
  item->RemoveObserver(this);
  last_.erase(item->GetId());
}

void DownloadDelegate::Report(download::DownloadItem* item) {
  View* view =
      View::ForWebContentsOrAny(content::DownloadItemUtils::GetWebContents(item));
  if (!view) {
    return;
  }
  base::FilePath path = item->GetTargetFilePath();
  view->Emit(base::DictValue()
                 .Set("type", "download")
                 .Set("id", static_cast<int>(item->GetId()))
                 .Set("file_name", path.BaseName().AsUTF8Unsafe())
                 .Set("path", path.AsUTF8Unsafe())
                 .Set("url", item->GetURL().spec())
                 .Set("received", static_cast<double>(item->GetReceivedBytes()))
                 .Set("total", static_cast<double>(
                                   std::max<int64_t>(item->GetTotalBytes(), 0)))
                 .Set("state", StateName(item->GetState())));
}

void DownloadDelegate::Cancel(uint32_t id) {
  if (!manager_) {
    return;
  }
  if (download::DownloadItem* item = manager_->GetDownload(id)) {
    item->Cancel(/*user_cancel=*/true);
  }
}

void CancelDownload(uint32_t id) {
  if (g_delegate) {
    g_delegate->Cancel(id);
  }
}

}  // namespace chorium
