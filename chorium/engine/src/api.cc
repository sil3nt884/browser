// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// The C functions in chorium.h. Each one copies its arguments and queues the
// work for the engine thread, which runs Chromium's browser main loop.

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/storage_partition.h"
#include "umbra/chorium/engine/src/browser_context.h"
#include "sandbox/policy/switches.h"
#include "umbra/app/tor_library.h"
#include "umbra/chorium/interfaces/native/chorium.h"
#include "umbra/chorium/engine/src/downloads.h"
#include "umbra/chorium/engine/src/engine.h"
#include "umbra/chorium/engine/src/view.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "sandbox/policy/mojom/sandbox.mojom-shared.h"
#include "sandbox/policy/sandbox_type.h"
#include "sandbox/win/src/sandbox.h"
#include "sandbox/win/src/sandbox_types.h"
#endif

#if BUILDFLAG(IS_ANDROID)
#include "umbra/chorium/engine/src/android/page_host.h"
#endif

namespace {

using chorium::View;

std::atomic<int> g_phase{0};  // 0 not started, 1 running, 2 stopped

#if BUILDFLAG(IS_WIN)
// The app's sandbox services (chorium_set_sandbox), or null: helpers run
// unsandboxed.
sandbox::BrokerServices* g_broker_services = nullptr;
sandbox::TargetServices* g_target_services = nullptr;
#endif

chorium::MainDelegate* Delegate() {
  static base::NoDestructor<chorium::MainDelegate> delegate;
  return delegate.get();
}

#if BUILDFLAG(IS_WIN)
int RunContentMain(chorium::MainDelegate* delegate) {
  content::ContentMainParams params(delegate);
  params.instance = ::GetModuleHandle(nullptr);
  static base::NoDestructor<sandbox::SandboxInterfaceInfo> info;
  *info = {};
  if (g_broker_services || g_target_services) {
    // As content::InitializeSandboxInfo, with the .exe's services.
    const base::CommandLine& command_line =
        *base::CommandLine::ForCurrentProcess();
    const bool is_browser =
        command_line.GetSwitchValueASCII(sandbox::policy::switches::kProcessType)
            .empty();
    const bool is_sandboxed =
        sandbox::policy::SandboxTypeFromCommandLine(command_line) !=
        sandbox::mojom::Sandbox::kNoSandbox;
    if (is_browser) {
      info->broker_services = g_broker_services;
      info->broker_services->RatchetDownSecurityMitigations(
          sandbox::MITIGATION_DEP | sandbox::MITIGATION_DEP_NO_ATL_THUNK |
          sandbox::MITIGATION_HARDEN_TOKEN_IL_POLICY);
    } else if (is_sandboxed) {
      info->target_services = g_target_services;
    }
    // Unsandboxed helpers get neither (they would act as brokers).
  }
  params.sandbox_info = info.get();
  return content::ContentMain(std::move(params));
}

HANDLE g_engine_thread = nullptr;

DWORD WINAPI EngineThreadMain(void*) {
  RunContentMain(Delegate());
  // Wakes chorium_init() if Chromium stopped before its loop ever ran.
  chorium::State().ready.Signal();
  return 0;
}

// Chromium starts its helpers with --type=renderer, --type=gpu-process, ...
bool IsHelperProcess() {
  return base::CommandLine::FromString(::GetCommandLineW())
      .HasSwitch("type");
}
#endif

// Runs |f| with the handle's View on the engine thread, if it still exists.
void OnView(chorium_view* handle,
            base::OnceCallback<void(chorium::View*)> f) {
  if (!handle) {
    return;
  }
  chorium::PostToEngine(base::BindOnce(
      [](chorium_view* h, base::OnceCallback<void(chorium::View*)> f) {
        if (h->view) {
          std::move(f).Run(h->view.get());
        }
      },
      base::Unretained(handle), std::move(f)));
}

std::string Str(const char* s) {
  return s ? std::string(s) : std::string();
}

}  // namespace

extern "C" {

int chorium_abi_version(void) {
  return CHORIUM_ABI_VERSION;
}

int chorium_execute_process(void) {
#if BUILDFLAG(IS_WIN)
  if (!IsHelperProcess()) {
    return -1;
  }
  // RunContentMain() reads the command line before ContentMain() would set
  // it up. A helper keeps the switches the browser gave it.
  base::CommandLine::Init(0, nullptr);
  return RunContentMain(Delegate());
#else
  return -1;
#endif
}

void chorium_set_profile_dir(const char* path) {
  if (!chorium::State().started && g_phase == 0) {
    chorium::State().profile_dir = base::FilePath::FromUTF8Unsafe(path ? path : "");
  }
}

void chorium_clear_site_data(void) {
  chorium::PostToEngine(base::BindOnce([] {
    chorium::ClearSiteData(base::DoNothing());
  }));
}

void chorium_set_clear_site_data_on_close(int on) {
  chorium::PostToEngine(base::BindOnce([](bool enabled) {
    auto& state = chorium::State();
    state.clear_site_data_on_close = enabled;
    if (!state.profile_dir.empty()) {
      const auto marker = state.profile_dir.AddExtensionASCII("clear-on-close");
      if (enabled) {
        if (!base::WriteFile(marker, "1")) {
          LOG(ERROR) << "Cannot save clear-on-close preference";
        }
      } else if (!base::DeleteFile(marker)) {
        LOG(ERROR) << "Cannot remove clear-on-close marker";
      }
    }
  }, on != 0));
}

int chorium_init(float scale) {
  int expected = 0;
  if (!g_phase.compare_exchange_strong(expected, 1)) {
    return -1;
  }
  chorium::State().scale = scale > 0 ? scale : 1.0f;

  // Chorium owns this process's command line: the app's own arguments never
  // reach Chromium, so nothing can slip in a switch (a proxy, say).
  base::CommandLine::Init(0, nullptr);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  *command_line = base::CommandLine(command_line->GetProgram());
#if BUILDFLAG(IS_WIN)
  // The sandbox has to come from the app's .exe (chorium_set_sandbox).
  if (!g_broker_services) {
    command_line->AppendSwitch(sandbox::policy::switches::kNoSandbox);
  }
#endif

#if BUILDFLAG(IS_WIN)
  g_engine_thread =
      ::CreateThread(nullptr, 0, &EngineThreadMain, nullptr, 0, nullptr);
  if (!g_engine_thread) {
    g_phase = 2;
    return -1;
  }
  chorium::State().ready.Wait();
  return chorium::State().started ? 0 : -1;
#else
  return -1;  // Android: Java starts the engine (see chorium.h)
#endif
}

void chorium_shutdown(void) {
  int expected = 1;
  if (!g_phase.compare_exchange_strong(expected, 2)) {
    return;
  }
  chorium::PostToEngine(base::BindOnce([] {
    chorium::EngineState& s = chorium::State();
    auto quit = base::BindOnce([] {
      auto& state = chorium::State();
      state.started = false;
      if (state.quit) {
        std::move(state.quit).Run();
      }
    });
    if (s.clear_site_data_on_close) {
      chorium::ClearSiteData(std::move(quit));
    } else {
      std::move(quit).Run();
    }
  }));
#if BUILDFLAG(IS_WIN)
  ::WaitForSingleObject(g_engine_thread, INFINITE);
  ::CloseHandle(g_engine_thread);
  g_engine_thread = nullptr;
#endif
}

int chorium_tor_last_error(char* buf, int size) {
  if (!buf || size <= 0) {
    return 0;
  }
  // SAFETY: the C ABI promises |buf| holds |size| bytes.
  auto out = UNSAFE_BUFFERS(base::span(buf, static_cast<size_t>(size)));
  std::string error = umbra::TorLibrary::GetLastError();
  size_t n = std::min(error.size(), out.size() - 1);
  out.first(n).copy_from(base::span(error).first(n));
  out[n] = '\0';
  return static_cast<int>(n);
}

int chorium_tor_progress(void) {
  int percent = 0;
  switch (umbra::TorLibrary::GetStatus(&percent)) {
    case umbra::TorLibrary::Phase::kReady:
      return 100;
    case umbra::TorLibrary::Phase::kStopped:  // switched off, or restarting
    case umbra::TorLibrary::Phase::kStarting:
      return 0;
    case umbra::TorLibrary::Phase::kBootstrapping:
      return percent < 100 ? percent : 99;
    case umbra::TorLibrary::Phase::kFailed:
    case umbra::TorLibrary::Phase::kNotLoaded:
      return -1;
  }
  return -1;
}

chorium_view* chorium_open(int width,
                           int height,
                           chorium_frame_fn on_frame,
                           chorium_event_fn on_event,
                           void* user) {
  /* chorium_init has run (desktop), or Java started the browser (Android). */
  if (!chorium::State().started || width <= 0 || height <= 0) {
    return nullptr;
  }
  auto* handle = new chorium_view;
  handle->on_frame = on_frame;
  handle->on_event = on_event;
  handle->user = user;
  chorium::PostToEngine(base::BindOnce(&View::Create, base::Unretained(handle),
                                       width, height));
  return handle;
}

void chorium_close(chorium_view* view) {
  if (view) {
    chorium::PostToEngine(
        base::BindOnce(&View::Close, base::Unretained(view)));
  }
}

void chorium_load_url(chorium_view* view, const char* url) {
  OnView(view, base::BindOnce([](std::string url,
                                 View* v) { v->LoadUrl(url); },
                              Str(url)));
}

void chorium_load_html(chorium_view* view,
                       const char* html,
                       const char* base_url) {
  OnView(view, base::BindOnce(
                   [](std::string html, std::string base, View* v) {
                     v->LoadHtml(html, base);
                   },
                   Str(html), Str(base_url)));
}

void chorium_back(chorium_view* view) {
  OnView(view, base::BindOnce([](View* v) { v->Back(); }));
}

void chorium_forward(chorium_view* view) {
  OnView(view, base::BindOnce([](View* v) { v->Forward(); }));
}

void chorium_reload(chorium_view* view) {
  OnView(view, base::BindOnce([](View* v) { v->Reload(); }));
}

void chorium_stop(chorium_view* view) {
  OnView(view, base::BindOnce([](View* v) { v->Stop(); }));
}

void chorium_resize(chorium_view* view, int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  OnView(view, base::BindOnce([](int w, int h, View* v) { v->Resize(w, h); },
                              width, height));
}

void chorium_refresh_frame(chorium_view* view) {
  OnView(view, base::BindOnce([](View* v) { v->RefreshFrame(); }));
}

void chorium_mouse(chorium_view* view,
                   int type,
                   int x,
                   int y,
                   int button,
                   int click_count,
                   int modifiers) {
  OnView(view, base::BindOnce(
                   [](int type, int x, int y, int button, int clicks, int mods,
                      View* v) { v->Mouse(type, x, y, button, clicks, mods); },
                   type, x, y, button, click_count, modifiers));
}

void chorium_wheel(chorium_view* view,
                   int x,
                   int y,
                   float dx,
                   float dy,
                   int modifiers) {
  OnView(view, base::BindOnce([](int x, int y, float dx, float dy, int mods,
                                 View* v) { v->Wheel(x, y, dx, dy, mods); },
                              x, y, dx, dy, modifiers));
}

void chorium_key(chorium_view* view, int down, int key_code, int modifiers) {
  OnView(view, base::BindOnce([](bool down, int code, int mods,
                                 View* v) { v->Key(down, code, mods); },
                              down != 0, key_code, modifiers));
}

void chorium_text(chorium_view* view, const char* utf8) {
  OnView(view, base::BindOnce([](std::u16string text,
                                 View* v) { v->Text(text); },
                              base::UTF8ToUTF16(Str(utf8))));
}

void chorium_set_tor(int on) {
  chorium::PostToEngine(base::BindOnce(&chorium::SetTor, on != 0));
}

int chorium_tor_enabled(void) {
  return chorium::State().tor_on ? 1 : 0;
}

void chorium_set_dark_mode(int dark) {
  chorium::PostToEngine(base::BindOnce(
      [](bool on) {
        if (chorium::State().dark != on) {
          chorium::State().dark = on;
          chorium::View::PreferencesChanged();
        }
      },
      dark != 0));
}

void chorium_set_frame_rate(int hz) {
  if (hz < 24 || hz > 1000) {
    return;
  }
  chorium::PostToEngine(base::BindOnce(
      [](int hz) {
        if (chorium::State().frame_hz != hz) {
          chorium::State().frame_hz = hz;
          chorium::View::FrameRateChanged();
        }
      },
      hz));
}

void chorium_enable_devtools(int port) {
  // Read once, when the engine starts.
  if (g_phase == 0 && port > 0 && port < 65536) {
    chorium::State().devtools_port = port;
  }
}

void chorium_set_download_dir(const char* utf8) {
  chorium::PostToEngine(base::BindOnce(
      [](std::string dir) {
        chorium::State().download_dir = base::FilePath::FromUTF8Unsafe(dir);
      },
      Str(utf8)));
}

void chorium_cancel_download(int id) {
  chorium::PostToEngine(base::BindOnce(&chorium::CancelDownload,
                                       static_cast<uint32_t>(id)));
}

#if BUILDFLAG(IS_WIN)
void chorium_set_sandbox(void* broker_services, void* target_services) {
  g_broker_services = static_cast<sandbox::BrokerServices*>(broker_services);
  g_target_services = static_cast<sandbox::TargetServices*>(target_services);
}
#endif

void chorium_edit(chorium_view* view, int command) {
  OnView(view, base::BindOnce([](int command, View* v) { v->Edit(command); },
                              command));
}

void chorium_save_url(chorium_view* view, const char* url) {
  OnView(view, base::BindOnce([](std::string url,
                                 View* v) { v->SaveUrl(url); },
                              Str(url)));
}

void chorium_copy_image_at(chorium_view* view, int x, int y) {
  OnView(view, base::BindOnce([](int x, int y, View* v) { v->CopyImageAt(x, y); },
                              x, y));
}

void chorium_file_chooser_reply(chorium_view* view,
                                int id,
                                const char* const* paths,
                                int count) {
  std::vector<std::string> files;
  for (int i = 0; paths && i < count; ++i) {
    // SAFETY: the C ABI promises |paths| holds |count| strings.
    files.push_back(Str(UNSAFE_BUFFERS(paths[i])));
  }
  OnView(view, base::BindOnce(
                   [](int id, std::vector<std::string> files, View* v) {
                     v->FileChooserReply(id, files);
                   },
                   id, std::move(files)));
}

void chorium_focus(chorium_view* view, int focused) {
  OnView(view, base::BindOnce([](bool focused, View* v) { v->SetFocus(focused); },
                              focused != 0));
}

void chorium_eval(chorium_view* view, int id, const char* js) {
  OnView(view, base::BindOnce([](int id, std::u16string js,
                                 View* v) { v->Eval(id, js); },
                              id, base::UTF8ToUTF16(Str(js))));
}

void chorium_dialog_reply(chorium_view* view,
                          int id,
                          int accept,
                          const char* text) {
  OnView(view, base::BindOnce(
                   [](int id, bool accept, std::u16string text, View* v) {
                     v->DialogReply(id, accept, text);
                   },
                   id, accept != 0, base::UTF8ToUTF16(Str(text))));
}

#if BUILDFLAG(IS_ANDROID)
void chorium_android_show_page(chorium_view* view,
                               int x,
                               int y,
                               int width,
                               int height) {
  if (view) {
    chorium::PostToEngine(base::BindOnce(
        [](chorium_view* h, int x, int y, int w, int height) {
          chorium::android::ShowPage(h, x, y, w, height);
        },
        base::Unretained(view), x, y, width, height));
  }
}

void chorium_android_hide_pages(void) {
  chorium::PostToEngine(base::BindOnce(&chorium::android::HidePages));
}
#endif

}  // extern "C"

namespace chorium {

MainDelegate* GetMainDelegate() {
  return Delegate();
}

}  // namespace chorium
