// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "umbra/chorium/engine/src/engine.h"

#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "components/embedder_support/user_agent_utils.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/display/screen.h"
#include "ui/events/devices/device_data_manager.h"
#include "umbra/app/umbra_startup.h"
#include "umbra/chorium/engine/src/browser_context.h"
#include "umbra/chorium/engine/src/view.h"
#include "umbra/chorium/engine/src/window_host.h"
#include "umbra/common/umbra_switches.h"

namespace chorium {

namespace {

// Same dead port as umbra/app/umbra_startup.cc: if Tor is not up, requests
// go nowhere instead of going out directly.
constexpr char kFailClosedProxy[] = "socks5://127.0.0.1:1";

constexpr net::NetworkTrafficAnnotationTag kTorProxyAnnotation =
    net::DefineNetworkTrafficAnnotation("chorium_tor_proxy", R"(
      semantics {
        sender: "Chorium"
        description: "All page traffic is sent through the in-process Tor "
                     "client's local SOCKS5 port."
        trigger: "Any network request made by a page."
        data: "Whatever the page sends."
        destination: OTHER
      }
      policy {
        cookies_allowed: YES
        setting: "None; Tor routing can't be switched off."
        policy_exception_justification: "Not configurable."
      })");

}  // namespace

EngineState& State() {
  static base::NoDestructor<EngineState> state;
  return *state;
}

void PostToEngine(base::OnceClosure task) {
  EngineState& s = State();
  if (!s.started || !s.ui) {
    return;
  }
  s.ui->PostTask(FROM_HERE, std::move(task));
}

std::string TorProxy() {
  // umbra::OnBasicStartupComplete() records the Tor port here (or the dead
  // port if Tor failed). Chorium owns this process's command line, so nothing
  // from outside can put a different value in.
  const std::string proxy =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          umbra::switches::kProxyServer);
  if (!base::StartsWith(proxy, "socks5://127.0.0.1:")) {
    LOG(ERROR) << "[chorium] no Tor proxy recorded; failing closed";
    return kFailClosedProxy;
  }
  return proxy;
}

// ContentClient ------------------------------------------------------------

std::u16string ContentClient::GetLocalizedString(int message_id) {
  return l10n_util::GetStringUTF16(message_id);
}

std::string_view ContentClient::GetDataResource(int resource_id,
                                                ui::ResourceScaleFactor scale) {
  return ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
      resource_id, scale);
}

scoped_refptr<base::RefCountedMemory> ContentClient::GetDataResourceBytes(
    int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
      resource_id);
}

std::string ContentClient::GetDataResourceString(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
      resource_id);
}

gfx::Image& ContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(
      resource_id);
}

// BrowserMainParts ---------------------------------------------------------

BrowserMainParts::BrowserMainParts() = default;
BrowserMainParts::~BrowserMainParts() = default;

int BrowserMainParts::PreMainMessageLoopRun() {
  screen_ = std::make_unique<Screen>(State().scale);
  display::Screen::SetScreenInstance(screen_.get());
  if (!ui::DeviceDataManager::HasInstance()) {
    ui::DeviceDataManager::CreateInstance();
  }
  context_ = std::make_unique<BrowserContext>();
  State().context = context_.get();
  return 0;
}

void BrowserMainParts::WillRunMainMessageLoop(
    std::unique_ptr<base::RunLoop>& run_loop) {
  EngineState& s = State();
  s.quit = run_loop->QuitClosure();
  s.ui = base::SingleThreadTaskRunner::GetCurrentDefault();
  s.started = true;
  s.ready.Signal();
}

void BrowserMainParts::PostMainMessageLoopRun() {
  View::CloseAll();
  State().context = nullptr;
  context_.reset();
  display::Screen::SetScreenInstance(nullptr);
  screen_.reset();
}

// BrowserClient ------------------------------------------------------------

std::unique_ptr<content::BrowserMainParts>
BrowserClient::CreateBrowserMainParts(bool is_integration_test) {
  return std::make_unique<BrowserMainParts>();
}

void BrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* network_context_params,
    cert_verifier::mojom::CertVerifierCreationParams*
        cert_verifier_creation_params) {
  content::ContentBrowserClient::ConfigureNetworkContextParams(
      context, in_memory, relative_partition_path, network_context_params,
      cert_verifier_creation_params);

  // The kill switch, set in code for every network context: one SOCKS5
  // proxy, no DIRECT fallback, no bypass (not even for localhost). SOCKS5
  // sends host names, so DNS is resolved by the Tor exit. If the proxy is
  // unreachable, requests fail.
  net::ProxyConfig config;
  config.proxy_rules().ParseFromString(TorProxy());
  config.proxy_rules().bypass_rules.ParseFromString("<-loopback>");
  network_context_params->initial_proxy_config =
      net::ProxyConfigWithAnnotation(config, kTorProxyAnnotation);
}

// The same user agent and client hints as a stock Chrome of this version.
std::string BrowserClient::GetUserAgent() {
  return embedder_support::GetUserAgent();
}

blink::UserAgentMetadata BrowserClient::GetUserAgentMetadata() {
  return embedder_support::GetUserAgentMetadata();
}

// MainDelegate -------------------------------------------------------------

MainDelegate::MainDelegate() = default;
MainDelegate::~MainDelegate() = default;

std::optional<int> MainDelegate::BasicStartupComplete() {
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_STDERR;
  logging::InitLogging(settings);

  // Browser process only: starts Tor (Arti) and records its SOCKS port, or
  // the dead port if it failed. Shared with the Umbra browser.
  umbra::OnBasicStartupComplete();
  return std::nullopt;
}

void MainDelegate::PreSandboxStartup() {
  // chorium.pak sits next to chorium.dll.
  base::FilePath pak;
  CHECK(base::PathService::Get(base::DIR_MODULE, &pak));
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      pak.Append(FILE_PATH_LITERAL("chorium.pak")));
}

content::ContentClient* MainDelegate::CreateContentClient() {
  content_client_ = std::make_unique<ContentClient>();
  return content_client_.get();
}

content::ContentBrowserClient* MainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<BrowserClient>();
  return browser_client_.get();
}

}  // namespace chorium
