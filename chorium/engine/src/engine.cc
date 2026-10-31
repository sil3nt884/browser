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
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_util.h"
#include "components/embedder_support/user_agent_utils.h"
#include "gpu/config/gpu_switches.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/cpp/transferable_directory.h"
#if BUILDFLAG(IS_WIN)
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#endif
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/display/screen.h"
#include "umbra/app/tor_library.h"
#include "umbra/app/umbra_startup.h"
#include "umbra/chorium/engine/src/browser_context.h"
#include "umbra/chorium/engine/src/renderer_client.h"
#include "umbra/chorium/engine/src/view.h"
#if !BUILDFLAG(IS_ANDROID)
#include "ui/events/devices/device_data_manager.h"
#include "umbra/chorium/engine/src/window_host.h"
#endif
#include "umbra/common/umbra_switches.h"

#if BUILDFLAG(IS_ANDROID)
#include <unistd.h>

#include <tuple>

#include "content/public/browser/browser_main_runner.h"

#include "base/android/apk_assets.h"
#include "base/files/file.h"
#include "base/files/memory_mapped_file.h"
#include "base/posix/global_descriptors.h"
#include "content/public/browser/android/compositor.h"
#include "content/public/common/result_codes.h"
#include "net/android/network_change_notifier_factory_android.h"
#include "content/public/browser/android/devtools_auth.h"
#include "net/base/net_errors.h"
#include "net/socket/unix_domain_server_socket_posix.h"
#include "content/public/browser/posix_file_descriptor_info.h"
#include "content/public/common/content_descriptors.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "content/child/font_warmup_win.h"
#endif

#if defined(CHORIUM_ANDROID_DEVTOOLS) || defined(CHORIUM_DESKTOP_DEVTOOLS)
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "net/base/net_errors.h"
#endif
#if defined(CHORIUM_DESKTOP_DEVTOOLS)
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#endif

namespace chorium {

namespace {

#if BUILDFLAG(IS_ANDROID)
// Where child processes find chorium.pak (see PreSandboxStartup).
constexpr int kChoriumPakDescriptor = kContentIPCDescriptorMax + 1;
#endif

#if defined(CHORIUM_DESKTOP_DEVTOOLS)
// DevTools on a loopback port (chorium_enable_devtools), as content shell's
// --remote-debugging-port.
class TcpDevToolsSocketFactory : public content::DevToolsSocketFactory {
 public:
  explicit TcpDevToolsSocketFactory(int port) : port_(port) {}

 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    if (socket->ListenWithAddressAndPort("127.0.0.1", port_, /*backlog=*/5) !=
        net::OK) {
      return nullptr;
    }
    return socket;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* name) override {
    return nullptr;
  }

  const int port_;
};
#endif

#if defined(CHORIUM_ANDROID_DEVTOOLS)
// DevTools for chrome://inspect over adb, as content shell has it: an
// abstract socket that only adb (shell) and this app may connect to.
class AdbDevToolsSocketFactory : public content::DevToolsSocketFactory {
 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket = std::make_unique<net::UnixDomainServerSocket>(
        base::BindRepeating(&content::CanUserConnectToDevTools),
        /*use_abstract_namespace=*/true);
    if (socket->BindAndListen("chorium_devtools_remote", /*backlog=*/5) !=
        net::OK) {
      return nullptr;
    }
    return socket;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* name) override {
    return nullptr;
  }
};
#endif

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

namespace {

// Tor mode: one SOCKS5 proxy, no DIRECT fallback, no bypass (not even for
// localhost); if it's unreachable, requests fail. Off: direct.
net::ProxyConfigWithAnnotation CurrentProxyConfig() {
  EngineState& s = State();
  if (s.tor_proxy.empty()) {
    s.tor_proxy = TorProxy();
  }
  net::ProxyConfig config = net::ProxyConfig::CreateDirect();
  if (s.tor_on) {
    config.proxy_rules().ParseFromString(s.tor_proxy);
    config.proxy_rules().bypass_rules.ParseFromString("<-loopback>");
  }
  return net::ProxyConfigWithAnnotation(config, kTorProxyAnnotation);
}

// Pushes the current proxy to every network context and closes their open
// connections, so nothing made in one mode is reused in the other.
void ApplyProxy() {
  EngineState& s = State();
  std::erase_if(s.proxy_clients,
                [](const auto& client) { return !client.is_connected(); });
  for (auto& client : s.proxy_clients) {
    client->OnProxyConfigUpdated(CurrentProxyConfig());
  }
  if (s.context) {
    s.context->GetDefaultStoragePartition()
        ->GetNetworkContext()
        ->CloseAllConnections(base::DoNothing());
  }
}

// Off the engine thread: starts Tor again; its proxy, or the dead port.
std::string RestartTor() {
  uint16_t port = 0;
  std::string error;
  if (!umbra::TorLibrary::Restart(&port, &error)) {
    LOG(ERROR) << "[chorium] Tor failed to start again: " << error;
    return kFailClosedProxy;
  }
  return base::StringPrintf("socks5://127.0.0.1:%u", port);
}

}  // namespace

void SetTor(bool on) {
  EngineState& s = State();
  if (s.tor_on == on) {
    return;
  }
  if (!s.tor_runner) {
    s.tor_runner = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING});
  }
  s.tor_on = on;
  if (!on) {
    ApplyProxy();  // direct, then stop Tor
    s.tor_runner->PostTask(FROM_HERE, base::BindOnce([] {
                             if (!umbra::TorLibrary::Stop()) {
                               LOG(ERROR) << "[chorium] this Tor library "
                                             "can't stop; it keeps running";
                             }
                           }));
    return;
  }
  // Back on: fail closed (the dead port) until Tor has its new port.
  s.tor_proxy = kFailClosedProxy;
  ApplyProxy();
  s.tor_runner->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&RestartTor),
      base::BindOnce([](std::string proxy) {
        EngineState& state = State();
        state.tor_proxy = std::move(proxy);
        if (state.tor_on) {
          ApplyProxy();
        }
      }));
}

void ClearSiteData(base::OnceClosure done) {
  if (auto* context = State().context.get()) {
    context->GetDefaultStoragePartition()->ClearData(
        content::StoragePartition::REMOVE_DATA_MASK_ALL, blink::StorageKey(),
        base::Time(), base::Time::Max(), std::move(done));
  } else {
    std::move(done).Run();
  }
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

#if BUILDFLAG(IS_ANDROID)
// As content shell: the network state comes from Android (the Java
// NetworkChangeNotifier that ChoriumApplication starts). Without it Chromium
// sees no connection, and pages see navigator.onLine == false.
int BrowserMainParts::PreEarlyInitialization() {
  net::NetworkChangeNotifier::SetFactory(
      new net::NetworkChangeNotifierFactoryAndroid());
  return content::RESULT_CODE_NORMAL_EXIT;
}
#endif

int BrowserMainParts::PreMainMessageLoopRun() {
#if !BUILDFLAG(IS_ANDROID)
  // Android has its own screen (the device's display).
  screen_ = std::make_unique<Screen>(State().scale);
  display::Screen::SetScreenInstance(screen_.get());
  if (!ui::DeviceDataManager::HasInstance()) {
    ui::DeviceDataManager::CreateInstance();
  }
#endif
  context_ = std::make_unique<BrowserContext>();
  State().context = context_.get();
#if BUILDFLAG(IS_ANDROID)
  // The message loop is Android's main looper, already running, and
  // WillRunMainMessageLoop is never called: the engine is ready now.
  EngineState& s = State();
  s.ui = base::SingleThreadTaskRunner::GetCurrentDefault();
  s.started = true;
  s.ready.Signal();
#endif
#if defined(CHORIUM_ANDROID_DEVTOOLS)
  content::DevToolsAgentHost::StartRemoteDebuggingServer(
      std::make_unique<AdbDevToolsSocketFactory>(), base::FilePath(),
      base::FilePath());
#endif
#if defined(CHORIUM_DESKTOP_DEVTOOLS)
  if (State().devtools_port > 0) {
    content::DevToolsAgentHost::StartRemoteDebuggingServer(
        std::make_unique<TcpDevToolsSocketFactory>(State().devtools_port),
        base::FilePath(), base::FilePath());
  }
#endif
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

  // Persist site data independently of the HTTP cache. An absent cache
  // directory means Chromium uses an in-memory cache.
  auto& params = *network_context_params;
  params.http_cache_enabled = true;
  if (!in_memory && !context->IsOffTheRecord()) {
    params.file_paths = network::mojom::NetworkContextFilePaths::New();
    params.file_paths->data_directory = network::TransferableDirectory(
        context->GetPath().Append(relative_partition_path).AppendASCII("Network"));
    params.file_paths->cookie_database_name = base::FilePath(FILE_PATH_LITERAL("Cookies"));
    params.persist_session_cookies = true;
    params.restore_old_session_cookies = true;
#if BUILDFLAG(IS_WIN)
    params.cookie_encryption_provider =
        static_cast<BrowserContext*>(context)->cookie_encryption()->BindNewRemote();
    params.enable_encrypted_cookies = true;
#else
    // Android protects this directory with the app's private UID/storage.
    params.enable_encrypted_cookies = false;
#endif
  }

  // The kill switch, set in code for every network context: one SOCKS5
  // proxy, no DIRECT fallback, no bypass (not even for localhost). SOCKS5
  // sends host names, so DNS is resolved by the Tor exit. If the proxy is
  // unreachable, requests fail. The Tor switch changes it live (SetTor).
  network_context_params->initial_proxy_config = CurrentProxyConfig();
  mojo::Remote<network::mojom::ProxyConfigClient> client;
  network_context_params->proxy_config_client_receiver =
      client.BindNewPipeAndPassReceiver();
  State().proxy_clients.push_back(std::move(client));
}

// The same user agent and client hints as a stock Chrome of this version.
std::string BrowserClient::GetUserAgent() {
  return embedder_support::GetUserAgent();
}

bool BrowserClient::AllowCompressionDictionaryTransport(
    content::BrowserContext* context) {
  // Content otherwise adds a persistent Shared Dictionary cache behind our
  // RAM-only HTTP cache. Sites retain normal gzip/Brotli transport support.
  return false;
}

blink::UserAgentMetadata BrowserClient::GetUserAgentMetadata() {
  return embedder_support::GetUserAgentMetadata();
}

// The app's theme, not the system's: prefers-color-scheme, and the colors of
// form controls and scrollbars.
void BrowserClient::OverrideWebPreferences(
    content::WebContents* web_contents,
    content::SiteInstance& main_frame_site,
    blink::web_pref::WebPreferences* prefs) {
  auto scheme = State().dark ? blink::mojom::PreferredColorScheme::kDark
                             : blink::mojom::PreferredColorScheme::kLight;
  prefs->preferred_color_scheme = scheme;
  prefs->preferred_root_scrollbar_color_scheme = scheme;
}

#if defined(CHORIUM_ANDROID_DEVTOOLS) || defined(CHORIUM_DESKTOP_DEVTOOLS)
std::unique_ptr<content::DevToolsManagerDelegate>
BrowserClient::CreateDevToolsManagerDelegate() {
  return std::make_unique<content::DevToolsManagerDelegate>();
}
#endif

#if BUILDFLAG(IS_ANDROID)
void BrowserClient::GetAdditionalMappedFilesForChildProcess(
    const base::CommandLine& command_line,
    int child_process_id,
    content::PosixFileDescriptorInfo* mappings) {
  auto* descriptors = base::GlobalDescriptors::GetInstance();
  mappings->ShareWithRegion(kChoriumPakDescriptor,
                            descriptors->Get(kChoriumPakDescriptor),
                            descriptors->GetRegion(kChoriumPakDescriptor));
}
#endif

// MainDelegate -------------------------------------------------------------

MainDelegate::MainDelegate() = default;
MainDelegate::~MainDelegate() = default;

std::optional<int> MainDelegate::BasicStartupComplete() {
  logging::LoggingSettings settings;
#if BUILDFLAG(IS_ANDROID)
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;  // logcat
#else
  settings.logging_dest = logging::LOG_TO_STDERR;
#endif
  logging::InitLogging(settings);
#if BUILDFLAG(IS_ANDROID)
  // As content shell: pages use the browser's compositor. Without this
  // every page view takes WebView's path (no text selection handles, so
  // touching text in a field crashes; no overscroll).
  content::Compositor::Initialize();
#endif

  // Browser process only: starts Tor (Arti) and records its SOCKS port, or
  // the dead port if it failed. Shared with the Umbra browser.
  umbra::OnBasicStartupComplete();
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      switches::kDisableGpuShaderDiskCache);
  // Umbra also makes every local DNS lookup fail. The network service reads
  // that once at start, and with the Tor switch off pages need DNS, so
  // Chorium goes without it. With Tor on, names are still resolved by the
  // Tor exit: SOCKS5 sends host names, and there is no direct fallback.
  base::CommandLine::ForCurrentProcess()->RemoveSwitch(
      network::switches::kHostResolverRules);
  return std::nullopt;
}

#if BUILDFLAG(IS_ANDROID)
std::variant<int, content::MainFunctionParams> MainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  if (!process_type.empty()) {
    return std::move(main_function_params);  // child processes: as usual
  }
  // As content_shell does: the browser's message loop is Android's, already
  // running, so only start (and leak) the runner; content must not run a
  // loop of its own (it aborts if it tries).
  std::unique_ptr<content::BrowserMainRunner> runner =
      content::BrowserMainRunner::Create();
  int code = runner->Initialize(std::move(main_function_params));
  DCHECK_LT(code, 0) << "BrowserMainRunner::Initialize failed";
  std::ignore = runner.release();
  return 0;
}
#endif

void MainDelegate::PreSandboxStartup() {
#if BUILDFLAG(IS_WIN)
  if (base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII("type") ==
      "renderer") {
    // DirectWrite must not try to contact the system font-cache service
    // after token lockdown. FontDataService supplies font bytes instead.
    content::PatchServiceManagerCalls();
  }
#endif
#if BUILDFLAG(IS_ANDROID)
  // chorium.pak is an APK asset. Child processes can't open the APK (another
  // UID), so the browser passes them the file (see BrowserClient).
  auto* descriptors = base::GlobalDescriptors::GetInstance();
  int fd = descriptors->MaybeGet(kChoriumPakDescriptor);
  base::MemoryMappedFile::Region region;
  if (fd >= 0) {
    region = descriptors->GetRegion(kChoriumPakDescriptor);
  } else {
    fd = base::android::OpenApkAsset("assets/chorium.pak", &region);
    CHECK_GE(fd, 0) << "chorium.pak missing from the APK";
    descriptors->Set(kChoriumPakDescriptor, fd, region);
  }
  // As content shell: InitSharedInstanceWithPakFileRegion only loads the pak
  // as the locale pack (strings); the data resources (Blink's default
  // stylesheets among them) need it added as a data pack too.
  ui::ResourceBundle::InitSharedInstanceWithPakFileRegion(
      base::File(dup(fd)), region);
  ui::ResourceBundle::GetSharedInstance().AddDataPackFromFileRegion(
      base::File(dup(fd)), region, ui::k100Percent);
#else
  // chorium.pak sits next to chorium.dll.
  base::FilePath pak;
  CHECK(base::PathService::Get(base::DIR_MODULE, &pak));
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      pak.Append(FILE_PATH_LITERAL("chorium.pak")));
#endif
}

content::ContentClient* MainDelegate::CreateContentClient() {
  content_client_ = std::make_unique<ContentClient>();
  return content_client_.get();
}

content::ContentBrowserClient* MainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<BrowserClient>();
  return browser_client_.get();
}

content::ContentRendererClient* MainDelegate::CreateContentRendererClient() {
  renderer_client_ = std::make_unique<RendererClient>();
  return renderer_client_.get();
}

}  // namespace chorium
