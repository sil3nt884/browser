// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_CHORIUM_ENGINE_SRC_ENGINE_H_
#define UMBRA_CHORIUM_ENGINE_SRC_ENGINE_H_

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/proxy_config_with_annotation.mojom.h"
#include "build/build_config.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"

namespace display {
class Screen;
}

namespace chorium {

class BrowserContext;

// State shared by the C API (any thread) and the engine thread.
struct EngineState {
  float scale = 1.0f;
  // Signalled once the engine thread is about to run its message loop, or
  // has failed to start.
  base::WaitableEvent ready;
  bool started = false;
  // Engine thread's task runner; set before |ready| is signalled.
  scoped_refptr<base::SingleThreadTaskRunner> ui;
  // Engine thread only.
  base::OnceClosure quit;
  raw_ptr<BrowserContext> context = nullptr;
  // Pages' preferred color scheme (chorium_set_dark_mode). Engine thread.
  bool dark = false;
  // The Tor switch (chorium_set_tor); read from any thread.
  std::atomic<bool> tor_on{true};
  // Tor mode's proxy: Tor's SOCKS port, or the dead port while Tor is down.
  // Engine thread.
  std::string tor_proxy;
  // Every network context's live proxy settings. Engine thread.
  std::vector<mojo::Remote<network::mojom::ProxyConfigClient>> proxy_clients;
  // Stops and starts Tor off the engine thread, one at a time.
  scoped_refptr<base::SequencedTaskRunner> tor_runner;
};

EngineState& State();

// Posts |task| to the engine thread. Drops it if the engine isn't running.
void PostToEngine(base::OnceClosure task);

// The Tor switch (see chorium_set_tor). Engine thread.
void SetTor(bool on);

// The Tor proxy every network context must use, e.g. "socks5://127.0.0.1:N".
// Points at a dead loopback port if Tor did not start (fail closed).
std::string TorProxy();

class ContentClient : public content::ContentClient {
 public:
  std::u16string GetLocalizedString(int message_id) override;
  std::string_view GetDataResource(int resource_id,
                                   ui::ResourceScaleFactor scale) override;
  scoped_refptr<base::RefCountedMemory> GetDataResourceBytes(
      int resource_id) override;
  std::string GetDataResourceString(int resource_id) override;
  gfx::Image& GetNativeImageNamed(int resource_id) override;
};

class BrowserMainParts : public content::BrowserMainParts {
 public:
  BrowserMainParts();
  ~BrowserMainParts() override;

#if BUILDFLAG(IS_ANDROID)
  int PreEarlyInitialization() override;
#endif
  int PreMainMessageLoopRun() override;
  void WillRunMainMessageLoop(
      std::unique_ptr<base::RunLoop>& run_loop) override;
  void PostMainMessageLoopRun() override;

 private:
  std::unique_ptr<display::Screen> screen_;
  std::unique_ptr<BrowserContext> context_;
};

class BrowserClient : public content::ContentBrowserClient {
 public:
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  void ConfigureNetworkContextParams(
      content::BrowserContext* context,
      bool in_memory,
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* network_context_params,
      cert_verifier::mojom::CertVerifierCreationParams*
          cert_verifier_creation_params) override;
  std::string GetUserAgent() override;
  blink::UserAgentMetadata GetUserAgentMetadata() override;
  void OverrideWebPreferences(content::WebContents* web_contents,
                              content::SiteInstance& main_frame_site,
                              blink::web_pref::WebPreferences* prefs) override;
#if defined(CHORIUM_ANDROID_DEVTOOLS)
  // The DevTools server (engine.cc) needs one; the defaults do.
  std::unique_ptr<content::DevToolsManagerDelegate>
  CreateDevToolsManagerDelegate() override;
#endif
#if BUILDFLAG(IS_ANDROID)
  // Child processes can't open the APK's assets: hand them chorium.pak.
  void GetAdditionalMappedFilesForChildProcess(
      const base::CommandLine& command_line,
      int child_process_id,
      content::PosixFileDescriptorInfo* mappings) override;
#endif
};

// The one MainDelegate of the process (the Android runtime hands it to
// content::SetContentMainDelegate from JNI_OnLoad).
class MainDelegate;
MainDelegate* GetMainDelegate();

class MainDelegate : public content::ContentMainDelegate {
 public:
  MainDelegate();
  ~MainDelegate() override;

  std::optional<int> BasicStartupComplete() override;
  void PreSandboxStartup() override;
#if BUILDFLAG(IS_ANDROID)
  // Android's main loop is Java's: start the browser and return.
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;
#endif
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  content::ContentRendererClient* CreateContentRendererClient() override;

 private:
  std::unique_ptr<ContentClient> content_client_;
  std::unique_ptr<BrowserClient> browser_client_;
  std::unique_ptr<content::ContentRendererClient> renderer_client_;
};

}  // namespace chorium

#endif  // UMBRA_CHORIUM_ENGINE_SRC_ENGINE_H_
