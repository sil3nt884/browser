// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Chromium's sandbox, linked into Chorium.exe: the broker patches each
// helper it starts at this .exe's addresses, so it can't live in
// chorium.dll (see chorium_set_sandbox in chorium.h).

#include "chorium.h"
#include "host.h"
#include "sandbox/win/src/sandbox_factory.h"

extern "C" void host_set_sandbox(void) {
  chorium_set_sandbox(sandbox::SandboxFactory::GetBrokerServices(),
                      sandbox::SandboxFactory::GetTargetServices());
}
