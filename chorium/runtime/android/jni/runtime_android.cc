// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// The Android runtime's native side: the shared core (runtime/common) plus
// the platform functions of runtime.h, driven from Java (Runtime.java,
// AppView.java). Everything runs on Android's main thread, which is also
// Chromium's UI thread.
//
// Drawing: the app's surface calls are recorded into a display list (CSS
// pixels) and handed to AppView on present; AppView replays it on a Canvas.
// Pages are native views placed with chorium_android_show_page, under
// AppView: where a page shows, the display list clears a hole (kHole), and
// whatever the app draws after that (a menu) covers the page.

#ifdef UNSAFE_BUFFERS_BUILD
// The display list is plain bytes handed to Java.
#pragma allow_unsafe_buffers
#endif

#include <android/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "remote.h"
#include "runtime.h"
#include "umbra/chorium/runtime/android/runtime_jni/AppView_jni.h"
#include "umbra/chorium/runtime/android/runtime_jni/ChoriumNatives_jni.h"
#include "umbra/chorium/runtime/android/runtime_jni/RemoteControl_jni.h"

using base::android::AttachCurrentThread;
using base::android::ConvertJavaStringToUTF8;
using base::android::ConvertUTF8ToJavaString;
using base::android::JavaRef;

namespace {

// Display list operations; AppView.java reads the same numbers.
enum Op : int32_t {
  kClear = 1,
  kFillRect = 2,
  kStrokeRect = 3,
  kRoundRect = 4,
  kText = 5,
  kIcon = 6,
  kClip = 7,
  kUnclip = 8,
  kHole = 9,
};

float g_density = 1.0f;
int g_width_px = 1, g_height_px = 1;
bool g_dark = false;
std::string& FilesDir() {
  static base::NoDestructor<std::string> dir;
  return *dir;
}

// The frame being recorded.
std::vector<uint8_t>& Ops() {
  static base::NoDestructor<std::vector<uint8_t>> ops;
  return *ops;
}
bool g_frame_open = false;  // ops recorded since the last present
uint32_t g_page_shown = 0;  // the page drawn this frame, or 0
int g_page_rect[4];         // its rect, CSS pixels

const char* const kModifiers[] = {"shift", "ctrl", "alt", "meta"};

void Put(int32_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  Ops().insert(Ops().end(), p, p + 4);
}

void Put(float v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  Ops().insert(Ops().end(), p, p + 4);
}

void Put(const char* utf8, size_t len) {
  Put(static_cast<int32_t>(len));
  Ops().insert(Ops().end(), reinterpret_cast<const uint8_t*>(utf8),
               reinterpret_cast<const uint8_t*>(utf8) + len);
}

// Starts a frame on the first drawing call after a present.
void Begin() {
  if (!g_frame_open) {
    Ops().clear();
    g_page_shown = 0;
    g_frame_open = true;
  }
}

void Rect(int32_t op, int x, int y, int w, int h) {
  Begin();
  Put(op);
  Put(x);
  Put(y);
  Put(w);
  Put(h);
}

// The main thread's task runner, for the remote control's server thread.
scoped_refptr<base::SingleThreadTaskRunner>& UiRunner() {
  static base::NoDestructor<scoped_refptr<base::SingleThreadTaskRunner>> ui;
  return *ui;
}

void PostToUi(base::OnceClosure task) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                              std::move(task));
}

void MakeDir(const std::string& path) {
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "[runtime] can't create %s\n", path.c_str());
  }
}

void SendPointer(const char* kind, float x, float y, int button,
                 int clicks) {
  static const char* const names[] = {"x", "y", "button", "click-count",
                                      "modifiers"};
  static const char* const buttons[] = {"none", "left", "middle", "right"};
  val_t fields[] = {v_s32(static_cast<int32_t>(x)),
                    v_s32(static_cast<int32_t>(y)),
                    v_enum(buttons[button & 3]),
                    v_u32(static_cast<uint32_t>(clicks)),
                    v_flags(kModifiers, 4, 0)};
  val_t pointer = v_record(5, names, fields);
  app_input(v_variant(kind, &pointer));
}

}  // namespace

// --- runtime.h: surface ---------------------------------------------------

void surface_dimensions(uint32_t* width, uint32_t* height) {
  *width = static_cast<uint32_t>(g_width_px / g_density + 0.5f);
  *height = static_cast<uint32_t>(g_height_px / g_density + 0.5f);
}

float surface_scale(void) {
  return g_density;
}

void surface_clear(uint32_t argb) {
  Begin();
  Put(kClear);
  Put(static_cast<int32_t>(argb));
}

void surface_fill_rect(int x, int y, int w, int h, uint32_t argb) {
  Rect(kFillRect, x, y, w, h);
  Put(static_cast<int32_t>(argb));
}

void surface_stroke_rect(int x, int y, int w, int h, uint32_t argb,
                         float width) {
  Rect(kStrokeRect, x, y, w, h);
  Put(static_cast<int32_t>(argb));
  Put(width);
}

void surface_fill_round_rect(int x, int y, int w, int h, float radius,
                             uint32_t argb) {
  Rect(kRoundRect, x, y, w, h);
  Put(radius);
  Put(static_cast<int32_t>(argb));
}

void surface_draw_text(int x, int y, const char* utf8, size_t len,
                       float font_size, uint32_t argb) {
  Begin();
  Put(kText);
  Put(x);
  Put(y);
  Put(font_size);
  Put(static_cast<int32_t>(argb));
  Put(utf8, len);
}

void surface_measure_text(const char* utf8, size_t len, float font_size,
                          uint32_t* width, uint32_t* height) {
  JNIEnv* env = AttachCurrentThread();
  int64_t packed = Java_AppView_measureText(
      env, ConvertUTF8ToJavaString(env, std::string_view(utf8, len)),
      font_size);
  *width = static_cast<uint32_t>(packed >> 32);
  *height = static_cast<uint32_t>(packed & 0xFFFFFFFF);
}

void surface_draw_icon(int icon, int x, int y, int w, int h, uint32_t argb) {
  Rect(kIcon, x, y, w, h);
  Put(static_cast<int32_t>(icon));
  Put(static_cast<int32_t>(argb));
}

void surface_draw_page(uint32_t page_id, int x, int y, int w, int h) {
  Rect(kHole, x, y, w, h);
  g_page_shown = page_id;
  g_page_rect[0] = x;
  g_page_rect[1] = y;
  g_page_rect[2] = w;
  g_page_rect[3] = h;
}

void surface_push_clip(int x, int y, int w, int h) {
  Rect(kClip, x, y, w, h);
}

void surface_pop_clip(void) {
  Begin();
  Put(kUnclip);
}

void surface_present(void) {
  Begin();
  JNIEnv* env = AttachCurrentThread();
  Java_AppView_present(env, base::android::ToJavaByteArray(env, Ops()));
  chorium_view* view = page_view(g_page_shown);
  if (view) {
    auto px = [](int css) { return static_cast<int>(css * g_density + 0.5f); };
    chorium_android_show_page(view, px(g_page_rect[0]), px(g_page_rect[1]),
                              px(g_page_rect[0] + g_page_rect[2]) -
                                  px(g_page_rect[0]),
                              px(g_page_rect[1] + g_page_rect[3]) -
                                  px(g_page_rect[1]));
  } else {
    chorium_android_hide_pages();
  }
  g_frame_open = false;
}

bool surface_dark_mode(void) {
  return g_dark;
}

void surface_show_keyboard(bool visible) {
  Java_AppView_showKeyboard(AttachCurrentThread(), visible);
}

void surface_set_title(const char* utf8, size_t len) {
  JNIEnv* env = AttachCurrentThread();
  Java_AppView_setTitle(
      env, ConvertUTF8ToJavaString(env, std::string_view(utf8, len)));
}

void platform_log(const char* level, const char* text, size_t len) {
  int priority = strcmp(level, "error") == 0   ? ANDROID_LOG_ERROR
                 : strcmp(level, "warn") == 0  ? ANDROID_LOG_WARN
                 : strcmp(level, "debug") == 0 ? ANDROID_LOG_DEBUG
                                               : ANDROID_LOG_INFO;
  __android_log_print(priority, "chorium", "[app] %.*s",
                      static_cast<int>(len), text);
}

void surface_set_fullscreen(bool on) {
  Java_AppView_setFullscreen(AttachCurrentThread(), on);
}

void surface_set_back_enabled(bool on) {
  Java_AppView_setBackEnabled(AttachCurrentThread(), on);
}

void surface_copy_text(const char* utf8, size_t len) {
  JNIEnv* env = AttachCurrentThread();
  Java_AppView_copyText(
      env, ConvertUTF8ToJavaString(env, std::string_view(utf8, len)));
}

char* surface_read_clipboard(void) {
  JNIEnv* env = AttachCurrentThread();
  std::string text = ConvertJavaStringToUTF8(env, Java_AppView_readClipboard(env));
  return strdup(text.c_str());
}

void surface_pick_files(uint32_t request, picker_mode_t mode,
                        const char* const* accept, size_t count,
                        const char* default_name) {
  JNIEnv* env = AttachCurrentThread();
  std::vector<std::string> types(accept, accept + count);
  // The picker is another activity: its answer comes later, through
  // JNI_ChoriumNatives_FilesPicked.
  Java_AppView_pickFiles(env, static_cast<int>(request), static_cast<int>(mode),
                         base::android::ToJavaArrayOfStrings(env, types),
                         ConvertUTF8ToJavaString(env, default_name));
}

void surface_open_file(const char* path, bool in_folder) {
  JNIEnv* env = AttachCurrentThread();
  Java_AppView_openFile(env, ConvertUTF8ToJavaString(env, path), in_folder);
}

bool title_bar_layout(uint32_t* height, uint32_t* buttons_width,
                      bool* maximized) {
  return false;  // the system bars belong to Android
}

void title_bar_set_drag_areas(const int* xywh, size_t count) {}

// --- runtime.h: platform ---------------------------------------------------

chorium_frame_fn platform_frame_callback(void) {
  return nullptr;  // pages are native views; no frames to capture
}

void platform_page_event(chorium_view* view, const char* json, void* user) {
  // Already on the main thread, but inside the engine's own task: deliver
  // after it, so the app is never entered from inside a call it made.
  uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
  PostToUi(base::BindOnce(
      [](uint32_t id, std::string json) { app_engine_event(id, json.c_str()); },
      id, std::string(json)));
}

void platform_page_closed(uint32_t page_id) {
  if (g_page_shown == page_id) {
    g_page_shown = 0;
  }
}

bool platform_app_data_dir(const char* app_name, char* out, size_t cap) {
  std::string dir = FilesDir() + "/apps";
  MakeDir(dir);
  dir += "/";
  dir += app_name;
  MakeDir(dir);
  return snprintf(out, cap, "%s", dir.c_str()) < static_cast<int>(cap);
}

void platform_app_crashed(const char* message) {
  JNIEnv* env = AttachCurrentThread();
  Java_AppView_crashed(env, ConvertUTF8ToJavaString(env, message));
}

void platform_request_draw(void) {
  PostToUi(base::BindOnce([] { app_call("on-draw"); }));
}

// Tor's bootstrap progress comes with no event: poll it, as the Windows host
// does, and redraw when it moves. Stops once Tor is up or has failed. Logs
// progress and Arti's errors to logcat (tag "chorium"): Arti's own log goes
// to stderr, which Android drops.
static int g_tor_last = -2;  // last progress reported, or -2

static void PollTorProgress() {
  int& last = g_tor_last;
  static char last_error[256];
  int tor = chorium_tor_progress();
  char error[256];
  chorium_tor_last_error(error, sizeof error);
  if (strcmp(error, last_error) != 0) {
    snprintf(last_error, sizeof last_error, "%s", error);
    __android_log_print(ANDROID_LOG_WARN, "chorium", "tor error: %s", error);
  }
  if (tor != last) {
    last = tor;
    __android_log_print(ANDROID_LOG_INFO, "chorium", "tor %d%%", tor);
    app_call("on-draw");
  }
  if (tor >= 0 && tor < 100) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&PollTorProgress), base::Milliseconds(250));
  }
}

// Tor was switched back on: poll its progress again until it's up.
void platform_tor_restarted(void) {
  bool polling = g_tor_last >= 0 && g_tor_last < 100;
  g_tor_last = -2;
  if (!polling) {
    PollTorProgress();
  }
}

// --- remote.h: the remote control ------------------------------------------

void platform_post_to_ui(void (*fn)(void*), void* arg) {
  UiRunner()->PostTask(
      FROM_HERE, base::BindOnce([](void (*fn)(void*), void* arg) { fn(arg); },
                                fn, base::Unretained(arg)));
}

void platform_remote_input(const remote_input_t* in) {
  JNIEnv* env = AttachCurrentThread();
  Java_RemoteControl_input(env, static_cast<int>(in->kind), in->x, in->y,
                           in->code, in->mods,
                           ConvertUTF8ToJavaString(env, in->text ? in->text : ""));
}

// The page is its own surface, which no in-app capture includes: the tests
// take screenshots with adb (screencap) instead.
void platform_screenshot(void* token) {
  remote_screenshot_done(token, nullptr, nullptr, 0);
}

bool platform_answer_picker(const char* const* paths, size_t count) {
  JNIEnv* env = AttachCurrentThread();
  std::vector<std::string> list(paths, paths + count);
  return Java_RemoteControl_answerPicker(
      env, base::android::ToJavaArrayOfStrings(env, list));
}

// --- JNI: ChoriumNatives.java -----------------------------------------------------

static void JNI_ChoriumNatives_Profile(JNIEnv* env,
                                      const JavaRef<jstring>& path) {
  std::string directory = ConvertJavaStringToUTF8(env, path);
  chorium_set_profile_dir(directory.c_str());
}

static jboolean JNI_ChoriumNatives_Start(JNIEnv* env,
                                  const JavaRef<jbyteArray>& wasm,
                                  const JavaRef<jstring>& app_name,
                                  const JavaRef<jstring>& files_dir,
                                  const JavaRef<jobjectArray>& args,
                                  jboolean remote_control) {
  FilesDir() = ConvertJavaStringToUTF8(env, files_dir);
  UiRunner() = base::SingleThreadTaskRunner::GetCurrentDefault();
  if (remote_control && !remote_start()) {
    __android_log_print(ANDROID_LOG_ERROR, "chorium",
                        "remote control: can't listen");
  }
  std::vector<uint8_t> bytes;
  base::android::JavaByteArrayToByteVector(env, wasm, &bytes);
  std::string name = ConvertJavaStringToUTF8(env, app_name);
  if (!contract_load_bytes(bytes.data(), bytes.size(), name.c_str())) {
    platform_app_crashed(contract_last_error());
    return false;
  }
  std::vector<std::string> strings;
  base::android::AppendJavaStringArrayToStringVector(env, args, &strings);
  std::vector<char*> argv;
  for (std::string& s : strings) {
    argv.push_back(s.data());
  }
  app_start(static_cast<int>(argv.size()), argv.data());
  PollTorProgress();
  return true;
}

static void JNI_ChoriumNatives_Surface(JNIEnv* env,
                                jint width_px,
                                jint height_px,
                                jfloat density,
                                jboolean dark) {
  g_width_px = width_px > 0 ? width_px : 1;
  g_height_px = height_px > 0 ? height_px : 1;
  g_density = density > 0 ? density : 1.0f;
  g_dark = dark;
  app_call("on-draw");
}

static void JNI_ChoriumNatives_Draw(JNIEnv* env) {
  app_call("on-draw");
}

static void JNI_ChoriumNatives_Pointer(JNIEnv* env, jint type, jfloat x, jfloat y) {
  // x, y in CSS pixels. 0 down, 1 move, 2 up, 3 cancel.
  switch (type) {
    case 0:
      SendPointer("pointer-down", x, y, 1, 1);
      break;
    case 1:
      SendPointer("pointer-move", x, y, 1, 0);
      break;
    case 2:
      SendPointer("pointer-up", x, y, 1, 1);
      break;
    default:
      app_input(v_variant("pointer-leave", nullptr));
      break;
  }
}

static void JNI_ChoriumNatives_Key(JNIEnv* env, jboolean down, jint code,
                            jint modifiers) {
  static const char* const names[] = {"down", "code", "modifiers"};
  val_t fields[] = {v_bool(down), v_u32(static_cast<uint32_t>(code)),
                    v_flags(kModifiers, 4, static_cast<uint32_t>(modifiers))};
  val_t key = v_record(3, names, fields);
  app_input(v_variant("key", &key));
}

static void JNI_ChoriumNatives_Text(JNIEnv* env, const JavaRef<jstring>& text) {
  std::string s = ConvertJavaStringToUTF8(env, text);
  val_t v = v_string(s.data(), s.size());
  app_input(v_variant("text", &v));
}

static void JNI_ChoriumNatives_Back(JNIEnv* env) {
  app_input(v_variant("back", nullptr));
}

static void JNI_ChoriumNatives_FilesPicked(JNIEnv* env,
                                           jint request,
                                           const JavaRef<jobjectArray>& paths) {
  std::vector<std::string> strings;
  base::android::AppendJavaStringArrayToStringVector(env, paths, &strings);
  std::vector<const char*> c_paths;
  for (const std::string& s : strings) {
    c_paths.push_back(s.c_str());
  }
  app_files_picked(static_cast<uint32_t>(request), c_paths.data(),
                   c_paths.size());
}

static void JNI_ChoriumNatives_Focus(JNIEnv* env, jboolean focused) {
  val_t b = v_bool(focused);
  app_input(v_variant("focus", &b));
}

static void JNI_ChoriumNatives_Pause(JNIEnv* env) {
  app_call("on-pause");
}

static void JNI_ChoriumNatives_Resume(JNIEnv* env) {
  app_call("on-resume");
}

static void JNI_ChoriumNatives_Stop(JNIEnv* env) {
  app_call("on-stop");
  pages_close_all();
  // The process may live on and open the activity again: start clean then.
  contract_unload();
}

DEFINE_JNI(ChoriumNatives)
DEFINE_JNI(AppView)
DEFINE_JNI(RemoteControl)
