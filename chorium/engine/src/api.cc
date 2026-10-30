// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// The C functions in chorium.h. Each one copies its arguments and queues the
// work for the engine thread, which runs Chromium's browser main loop.

#include <atomic>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "content/public/app/content_main.h"
#include "sandbox/policy/switches.h"
#include "umbra/app/tor_library.h"
#include "umbra/chorium/interfaces/native/chorium.h"
#include "umbra/chorium/engine/src/engine.h"
#include "umbra/chorium/engine/src/view.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "content/public/app/sandbox_helper_win.h"
#include "sandbox/win/src/sandbox_types.h"
#endif

namespace {

using chorium::View;

std::atomic<int> g_phase{0};  // 0 not started, 1 running, 2 stopped

int RunContentMain(chorium::MainDelegate* delegate) {
  content::ContentMainParams params(delegate);
#if BUILDFLAG(IS_WIN)
  params.instance = ::GetModuleHandle(nullptr);
  static base::NoDestructor<sandbox::SandboxInterfaceInfo> sandbox_info;
  content::InitializeSandboxInfo(sandbox_info.get());
  params.sandbox_info = sandbox_info.get();
#endif
  return content::ContentMain(std::move(params));
}

chorium::MainDelegate* Delegate() {
  static base::NoDestructor<chorium::MainDelegate> delegate;
  return delegate.get();
}

#if BUILDFLAG(IS_WIN)
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
  // InitializeSandboxInfo() reads the command line before ContentMain()
  // would set it up. A helper keeps the switches the browser gave it.
  base::CommandLine::Init(0, nullptr);
  return RunContentMain(Delegate());
#else
  return -1;
#endif
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
  // TODO(chorium): ship chorium_sandbox.lib so apps can link Chromium's
  // sandbox into their .exe; until then page processes run unsandboxed.
  command_line->AppendSwitch(sandbox::policy::switches::kNoSandbox);

#if BUILDFLAG(IS_WIN)
  g_engine_thread =
      ::CreateThread(nullptr, 0, &EngineThreadMain, nullptr, 0, nullptr);
  if (!g_engine_thread) {
    g_phase = 2;
    return -1;
  }
#else
  return -1;
#endif
  chorium::State().ready.Wait();
  return chorium::State().started ? 0 : -1;
}

void chorium_shutdown(void) {
  int expected = 1;
  if (!g_phase.compare_exchange_strong(expected, 2)) {
    return;
  }
  chorium::PostToEngine(base::BindOnce([] {
    chorium::EngineState& s = chorium::State();
    s.started = false;
    if (s.quit) {
      std::move(s.quit).Run();
    }
  }));
#if BUILDFLAG(IS_WIN)
  ::WaitForSingleObject(g_engine_thread, INFINITE);
  ::CloseHandle(g_engine_thread);
  g_engine_thread = nullptr;
#endif
}

int chorium_tor_progress(void) {
  int percent = 0;
  switch (umbra::TorLibrary::GetStatus(&percent)) {
    case umbra::TorLibrary::Phase::kReady:
      return 100;
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
  if (g_phase != 1 || width <= 0 || height <= 0) {
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

}  // extern "C"
