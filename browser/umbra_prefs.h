// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_BROWSER_UMBRA_PREFS_H_
#define UMBRA_BROWSER_UMBRA_PREFS_H_

class PrefRegistry;

namespace umbra {

// Called right after Chromium registers its profile prefs. Changes defaults
// (never user-set values) so a fresh profile behaves like Umbra:
//   - every window is incognito: cache, cookies and history live in RAM only
//   - link preloading stays on
//   - no keystroke-leaking search suggestions or error-page lookups
//   - WebRTC never leaves the proxy
void OverrideProfilePrefDefaults(PrefRegistry* registry);

}  // namespace umbra

#endif  // UMBRA_BROWSER_UMBRA_PREFS_H_
