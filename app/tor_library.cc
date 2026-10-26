// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/app/tor_library.h"

#include <array>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "umbra/tor_core/include/umbra_tor.h"

#if BUILDFLAG(IS_ANDROID)
#include "base/android/apk_assets.h"
#include "base/files/file.h"
#include "base/files/memory_mapped_file.h"
#include "base/hash/hash.h"
#endif

namespace umbra {

namespace {

struct Api {
  bool loaded = false;
  std::string load_error;
  umbra_tor_abi_version_fn abi_version = nullptr;
  umbra_tor_start_fn start = nullptr;
  umbra_tor_status_fn status = nullptr;
  umbra_tor_last_error_fn last_error = nullptr;
};

Api& GetApi() {
  static base::NoDestructor<Api> api;
  return *api;
}

#if BUILDFLAG(IS_ANDROID)
// The library ships uncompressed inside the APK as an asset. Copy it into the
// app's private data dir (once per build, keyed by content hash) and load it
// from there. This avoids having to touch the APK's native-library packaging.
constexpr char kAndroidAssetPath[] = "assets/umbra/libumbra_tor.so";

base::FilePath ExtractAndroidLibrary(std::string* error) {
  base::MemoryMappedFile::Region region;
  int fd = base::android::OpenApkAsset(kAndroidAssetPath, &region);
  if (fd < 0) {
    *error = "asset not found in APK: " + std::string(kAndroidAssetPath);
    return {};
  }
  base::File asset(fd);
  std::vector<uint8_t> bytes(static_cast<size_t>(region.size));
  if (!asset.ReadAndCheck(region.offset, bytes)) {
    *error = "could not read library asset";
    return {};
  }

  base::FilePath data_dir;
  if (!base::PathService::Get(base::DIR_ANDROID_APP_DATA, &data_dir)) {
    *error = "no app data dir";
    return {};
  }
  base::FilePath lib_dir = data_dir.Append("umbra_lib");
  if (!base::CreateDirectory(lib_dir)) {
    *error = "could not create " + lib_dir.value();
    return {};
  }
  base::FilePath dest = lib_dir.Append(
      base::StringPrintf("libumbra_tor-%08x.so", base::FastHash(bytes)));
  if (base::PathExists(dest)) {
    return dest;
  }

  base::FilePath tmp = dest.AddExtension(".tmp");
  if (!base::WriteFile(tmp, bytes)) {
    *error = "could not write " + tmp.value();
    return {};
  }
  // Read-only + executable for the app user only.
  base::SetPosixFilePermissions(tmp, 0500);
  if (!base::ReplaceFile(tmp, dest, nullptr)) {
    *error = "could not move library into place";
    return {};
  }
  return dest;
}
#endif  // BUILDFLAG(IS_ANDROID)

base::FilePath LibraryPath(std::string* error) {
#if BUILDFLAG(IS_WIN)
  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_MODULE, &dir)) {
    *error = "no module dir";
    return {};
  }
  return dir.Append(FILE_PATH_LITERAL("umbra_tor.dll"));
#elif BUILDFLAG(IS_ANDROID)
  return ExtractAndroidLibrary(error);
#elif BUILDFLAG(IS_MAC)
  base::FilePath dir;
  base::PathService::Get(base::DIR_MODULE, &dir);
  return dir.Append("libumbra_tor.dylib");
#else
  base::FilePath dir;
  base::PathService::Get(base::DIR_MODULE, &dir);
  return dir.Append("libumbra_tor.so");
#endif
}

bool EnsureLoaded() {
  Api& api = GetApi();
  if (api.loaded) {
    return true;
  }
  if (!api.load_error.empty()) {
    return false;  // Already failed once; don't retry on every call.
  }

  std::string error;
  base::FilePath path = LibraryPath(&error);
  if (path.empty()) {
    api.load_error = error;
    return false;
  }

  base::NativeLibraryLoadError load_error;
  base::NativeLibrary lib = base::LoadNativeLibrary(path, &load_error);
  if (!lib) {
    api.load_error = "LoadNativeLibrary(" + path.AsUTF8Unsafe() +
                     ") failed: " + load_error.ToString();
    return false;
  }

  api.abi_version = reinterpret_cast<umbra_tor_abi_version_fn>(
      base::GetFunctionPointerFromNativeLibrary(lib, "umbra_tor_abi_version"));
  api.start = reinterpret_cast<umbra_tor_start_fn>(
      base::GetFunctionPointerFromNativeLibrary(lib, "umbra_tor_start"));
  api.status = reinterpret_cast<umbra_tor_status_fn>(
      base::GetFunctionPointerFromNativeLibrary(lib, "umbra_tor_status"));
  api.last_error = reinterpret_cast<umbra_tor_last_error_fn>(
      base::GetFunctionPointerFromNativeLibrary(lib, "umbra_tor_last_error"));

  if (!api.abi_version || !api.start || !api.status || !api.last_error) {
    api.load_error = "umbra_tor library is missing exports";
    return false;
  }
  if (api.abi_version() != UMBRA_TOR_ABI_VERSION) {
    api.load_error = base::StringPrintf(
        "umbra_tor ABI mismatch: library %u, browser %u", api.abi_version(),
        UMBRA_TOR_ABI_VERSION);
    return false;
  }
  // Intentionally never unloaded: Tor runs for the life of the process.
  api.loaded = true;
  return true;
}

}  // namespace

// static
bool TorLibrary::Start(const base::FilePath& state_dir,
                       const base::FilePath& cache_dir,
                       uint16_t* socks_port,
                       std::string* error) {
  if (!EnsureLoaded()) {
    *error = GetApi().load_error;
    return false;
  }
  if (!base::CreateDirectory(state_dir) || !base::CreateDirectory(cache_dir)) {
    *error = "could not create Tor directories";
    return false;
  }
  const std::string state = state_dir.AsUTF8Unsafe();
  const std::string cache = cache_dir.AsUTF8Unsafe();
  uint16_t port = 0;
  int32_t rc = GetApi().start(state.c_str(), cache.c_str(), &port);
  if (rc != UMBRA_TOR_OK || port == 0) {
    *error = base::StringPrintf("umbra_tor_start failed (%d): ", rc) +
             GetLastError();
    return false;
  }
  *socks_port = port;
  return true;
}

// static
TorLibrary::Phase TorLibrary::GetStatus(int* percent) {
  if (!GetApi().loaded) {
    if (percent) {
      *percent = 0;
    }
    return Phase::kNotLoaded;
  }
  int32_t pct = 0;
  int32_t phase = GetApi().status(&pct);
  if (percent) {
    *percent = pct;
  }
  switch (phase) {
    case UMBRA_TOR_PHASE_READY:
      return Phase::kReady;
    case UMBRA_TOR_PHASE_BOOTSTRAPPING:
      return Phase::kBootstrapping;
    case UMBRA_TOR_PHASE_STARTING:
      return Phase::kStarting;
    default:
      return Phase::kFailed;
  }
}

// static
std::string TorLibrary::GetLastError() {
  if (!GetApi().loaded) {
    return GetApi().load_error;
  }
  std::array<char, 512> buf{};
  GetApi().last_error(buf.data(), buf.size());
  return std::string(buf.data());
}

}  // namespace umbra
