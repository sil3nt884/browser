// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/app/umbra_startup.h"

#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "umbra/app/tor_library.h"
#include "umbra/common/umbra_switches.h"

namespace umbra {

namespace {

// Where traffic goes if Tor could not be started: a loopback port nothing
// listens on. Every request fails; nothing is ever sent in the clear.
constexpr char kFailClosedProxy[] = "socks5://127.0.0.1:1";

bool GetTorDirs(base::FilePath* state, base::FilePath* cache) {
  base::FilePath root;
#if BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &root)) {
    return false;
  }
  root = root.Append(FILE_PATH_LITERAL("Umbra")).Append(FILE_PATH_LITERAL("Tor"));
#elif BUILDFLAG(IS_ANDROID)
  if (!base::PathService::Get(base::DIR_ANDROID_APP_DATA, &root)) {
    return false;
  }
  root = root.Append("umbra_tor");
#else
  if (!base::PathService::Get(base::DIR_CACHE, &root)) {
    return false;
  }
  root = root.Append("umbra").Append("tor");
#endif
  // state/: guard nodes and keys. Kept between runs on purpose: rotating
  // guards every launch makes you *easier* to deanonymise, not harder.
  // cache/: directory consensus, kept so cold start takes seconds, not a minute.
  *state = root.Append(FILE_PATH_LITERAL("state"));
  *cache = root.Append(FILE_PATH_LITERAL("cache"));
  return true;
}

// Accepts "host:port" where host is a loopback address.
bool ParseExternalSocks(const std::string& value, std::string* proxy) {
  const size_t colon = value.rfind(':');
  if (colon == std::string::npos) {
    return false;
  }
  const std::string host = value.substr(0, colon);
  unsigned port = 0;
  if (!base::StringToUint(value.substr(colon + 1), &port) || port == 0 ||
      port > 65535) {
    return false;
  }
  if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") {
    return false;
  }
  *proxy = base::StringPrintf("socks5://%s:%u", host.c_str(), port);
  return true;
}

void LockDownNetworking(base::CommandLine* cl, const std::string& proxy) {
  // All HTTP(S)/WS traffic through Tor. Chromium's SOCKS5 client sends
  // hostnames, so DNS is resolved by the Tor exit.
  cl->AppendSwitchASCII(switches::kProxyServer, proxy);
  // Don't let localhost/127.x skip the proxy (blocks local-network probing
  // from web pages too).
  cl->AppendSwitchASCII(switches::kProxyBypassList, "<-loopback>");
  // Belt and braces: any code path that tries a local DNS lookup gets
  // NXDOMAIN, except for the proxy address itself.
  cl->AppendSwitchASCII(switches::kHostResolverRules,
                        "MAP * ~NOTFOUND , EXCLUDE 127.0.0.1");
  // QUIC is UDP; Tor carries TCP only. Without this Chromium may try QUIC
  // directly.
  cl->AppendSwitch(switches::kDisableQuic);
  // WebRTC must not open UDP sockets outside the proxy (classic IP leak).
  cl->AppendSwitchASCII(switches::kForceWebRtcIPHandlingPolicy,
                        "disable_non_proxied_udp");
}

}  // namespace

void OnBasicStartupComplete() {
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();
  if (!cl->GetSwitchValueASCII(switches::kProcessType).empty()) {
    return;  // Renderer, GPU, utility...: inherit the browser's settings.
  }

  std::string proxy;
  if (cl->HasSwitch(switches::kExternalSocks)) {
    if (!ParseExternalSocks(cl->GetSwitchValueASCII(switches::kExternalSocks),
                            &proxy)) {
      LOG(ERROR) << "[umbra] bad --" << switches::kExternalSocks
                 << " value; failing closed";
      proxy = kFailClosedProxy;
    }
    LockDownNetworking(cl, proxy);
    return;
  }

  base::FilePath state_dir, cache_dir;
  uint16_t port = 0;
  std::string error;
  if (!GetTorDirs(&state_dir, &cache_dir)) {
    error = "could not determine Tor data directories";
  } else if (TorLibrary::Start(state_dir, cache_dir, &port, &error)) {
    proxy = base::StringPrintf("socks5://127.0.0.1:%u", port);
    VLOG(1) << "[umbra] Tor SOCKS on " << proxy;
  }

  if (proxy.empty()) {
    LOG(ERROR) << "[umbra] Tor failed to start: " << error
               << " -- all network requests will fail (fail closed).";
    proxy = kFailClosedProxy;
  }
  LockDownNetworking(cl, proxy);
}

}  // namespace umbra
