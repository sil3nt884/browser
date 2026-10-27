// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/browser/umbra_prefs.h"

#include <string_view>

#include "base/logging.h"
#include "base/values.h"
#include "components/prefs/pref_registry.h"
#include "components/prefs/pref_store.h"

namespace umbra {

namespace {

// Pref names are spelled out rather than included so //umbra does not depend
// on chrome/browser. If Chromium renames one, the override is skipped and a
// warning is logged (see SetIfRegistered) instead of crashing.

// policy::policy_prefs::kIncognitoModeAvailability.
// IncognitoModePrefs::Availability::kForced == 2.
constexpr char kIncognitoModeAvailability[] = "incognito.mode_availability";
constexpr int kIncognitoForced = 2;

// prefetch::prefs::kNetworkPredictionOptions. kStandard == 0.
constexpr char kNetworkPredictionOptions[] = "net.network_prediction_options";
constexpr int kNetworkPredictionStandard = 0;

// prefs::kSearchSuggestEnabled
constexpr char kSearchSuggestEnabled[] = "search.suggest_enabled";
// prefs::kAlternateErrorPagesEnabled (embedder_support)
constexpr char kAlternateErrorPagesEnabled[] = "alternate_error_pages.enabled";
// prefs::kWebRTCIPHandlingPolicy
constexpr char kWebRtcIpHandlingPolicy[] = "webrtc.ip_handling_policy";

// PDF and printing are switched off here rather than with enable_pdf /
// enable_printing: since M157 desktop Chrome's UI depends on both
// unconditionally, so the GN args no longer build on Windows.
// prefs::kPluginsAlwaysOpenPdfExternally: PDFs download, never render.
constexpr char kAlwaysOpenPdfExternally[] =
    "plugins.always_open_pdf_externally";
// prefs::kPrintingEnabled
constexpr char kPrintingEnabled[] = "printing.enabled";

void SetIfRegistered(PrefRegistry* registry,
                     std::string_view name,
                     base::Value value) {
  const base::Value* existing = nullptr;
  if (!registry->defaults()->GetValue(name, &existing)) {
    LOG(WARNING) << "[umbra] pref not registered, override skipped: " << name;
    return;
  }
  if (existing->type() != value.type()) {
    LOG(WARNING) << "[umbra] pref type changed, override skipped: " << name;
    return;
  }
  registry->SetDefaultPrefValue(name, std::move(value));
}

}  // namespace

void OverrideProfilePrefDefaults(PrefRegistry* registry) {
  SetIfRegistered(registry, kIncognitoModeAvailability,
                  base::Value(kIncognitoForced));
  SetIfRegistered(registry, kNetworkPredictionOptions,
                  base::Value(kNetworkPredictionStandard));
  SetIfRegistered(registry, kSearchSuggestEnabled, base::Value(false));
  SetIfRegistered(registry, kAlternateErrorPagesEnabled, base::Value(false));
  SetIfRegistered(registry, kWebRtcIpHandlingPolicy,
                  base::Value("disable_non_proxied_udp"));
  SetIfRegistered(registry, kAlwaysOpenPdfExternally, base::Value(true));
  SetIfRegistered(registry, kPrintingEnabled, base::Value(false));
}

}  // namespace umbra
