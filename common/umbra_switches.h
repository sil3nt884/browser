// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_COMMON_UMBRA_SWITCHES_H_
#define UMBRA_COMMON_UMBRA_SWITCHES_H_

namespace umbra::switches {

// Use an already-running Tor SOCKS port (e.g. "127.0.0.1:9150") instead of the
// built-in engine. Handy while developing; traffic still fails closed.
inline constexpr char kExternalSocks[] = "umbra-external-socks";

// Turn off hover/tap preloading of same-site links.
inline constexpr char kDisablePreload[] = "umbra-disable-preload";

// Turn off forcing <video preload="auto"> on pages.
inline constexpr char kDisableVideoPreload[] = "umbra-disable-video-preload";

// Chromium switch names we set ourselves (spelled out so //umbra does not need
// to depend on the components that own them).
inline constexpr char kProcessType[] = "type";
inline constexpr char kProxyServer[] = "proxy-server";
inline constexpr char kProxyBypassList[] = "proxy-bypass-list";
inline constexpr char kHostResolverRules[] = "host-resolver-rules";
inline constexpr char kDisableQuic[] = "disable-quic";
inline constexpr char kForceWebRtcIPHandlingPolicy[] =
    "force-webrtc-ip-handling-policy";

}  // namespace umbra::switches

#endif  // UMBRA_COMMON_UMBRA_SWITCHES_H_
