// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_APP_TOR_LIBRARY_H_
#define UMBRA_APP_TOR_LIBRARY_H_

#include <cstdint>
#include <string>

#include "base/files/file_path.h"

namespace umbra {

// Loads the umbra_tor shared library (Rust/Arti) and starts the engine.
// Browser process only. All functions are safe to call more than once.
class TorLibrary {
 public:
  enum class Phase { kFailed, kStarting, kBootstrapping, kReady, kNotLoaded };

  TorLibrary() = delete;

  // Loads the library and starts Tor. On success returns true and writes the
  // loopback SOCKS port. On failure returns false and fills |error|.
  static bool Start(const base::FilePath& state_dir,
                    const base::FilePath& cache_dir,
                    uint16_t* socks_port,
                    std::string* error);

  // Current lifecycle phase and bootstrap progress (0-100).
  static Phase GetStatus(int* percent);

  // Most recent error reported by the engine, if any.
  static std::string GetLastError();
};

}  // namespace umbra

#endif  // UMBRA_APP_TOR_LIBRARY_H_
