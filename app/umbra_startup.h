// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_APP_UMBRA_STARTUP_H_
#define UMBRA_APP_UMBRA_STARTUP_H_

namespace umbra {

// Hooked into ChromeMainDelegate::BasicStartupComplete(), which runs in every
// process before any networking. In the browser process this starts Tor and
// rewrites the command line so that all traffic goes through it. Other
// process types return immediately.
void OnBasicStartupComplete();

}  // namespace umbra

#endif  // UMBRA_APP_UMBRA_STARTUP_H_
