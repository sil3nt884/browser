/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Engine events (JSON from chorium.h) to WIT page-events, for every
 * platform.
 */
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

/* Portable C shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

/* --- page events -------------------------------------------------------- */

static val_t string_field(const char* json, const char* key) {
  char* s = NULL;
  if (!json_string(json, key, &s)) {
    return v_cstring("");
  }
  val_t v = v_cstring(s);
  free(s);
  return v;
}

static bool bool_field(const char* json, const char* key) {
  bool b = false;
  json_bool(json, key, &b);
  return b;
}

static uint32_t u32_field(const char* json, const char* key) {
  long long n = 0;
  json_int(json, key, &n);
  return (uint32_t)n;
}

/* chorium.dll's JSON event -> WIT page-event. False for unknown types. */
static float f32_field(const char* json, const char* key) {
  const char* raw = NULL;
  size_t len = 0;
  if (!json_raw(json, key, &raw, &len)) {
    return 0;
  }
  return strtof(raw, NULL); /* stops at the , or } after the number */
}

/* |json|'s |key| if it is one of |names| (an enum case), else NULL. */
static const char* enum_field(const char* json, const char* key,
                              const char* const* names, size_t count,
                              char** owned) {
  *owned = NULL;
  if (!json_string(json, key, owned)) {
    return NULL;
  }
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(*owned, names[i]) == 0) {
      return names[i];
    }
  }
  return NULL;
}

static bool page_event_from_json(const char* json, val_t* out) {
  char* type = NULL;
  if (!json_string(json, "type", &type)) {
    return false;
  }
  bool ok = true;
  val_t payload;
  if (strcmp(type, "url") == 0) {
    static const char* const names[] = {"url", "can-go-back",
                                        "can-go-forward"};
    val_t fields[] = {string_field(json, "url"),
                      v_bool(bool_field(json, "can_go_back")),
                      v_bool(bool_field(json, "can_go_forward"))};
    payload = v_record(3, names, fields);
    *out = v_variant("navigated", &payload);
  } else if (strcmp(type, "title") == 0) {
    payload = string_field(json, "title");
    *out = v_variant("title", &payload);
  } else if (strcmp(type, "loading") == 0) {
    payload = v_bool(bool_field(json, "loading"));
    *out = v_variant("loading", &payload);
  } else if (strcmp(type, "loaded") == 0) {
    static const char* const names[] = {"url", "ok", "error"};
    val_t fields[] = {string_field(json, "url"), v_bool(bool_field(json, "ok")),
                      string_field(json, "error")};
    payload = v_record(3, names, fields);
    *out = v_variant("loaded", &payload);
  } else if (strcmp(type, "new_window") == 0) {
    payload = string_field(json, "url");
    *out = v_variant("new-window", &payload);
  } else if (strcmp(type, "fullscreen") == 0) {
    payload = v_bool(bool_field(json, "on"));
    *out = v_variant("fullscreen", &payload);
  } else if (strcmp(type, "dialog") == 0) {
    static const char* const names[] = {"id", "kind", "message",
                                        "default-text"};
    char* kind = NULL;
    json_string(json, "kind", &kind);
    val_t fields[] = {v_u32(u32_field(json, "id")),
                      v_enum(kind ? kind : "alert"),
                      string_field(json, "message"),
                      string_field(json, "default")};
    free(kind);
    payload = v_record(4, names, fields);
    *out = v_variant("dialog", &payload);
  } else if (strcmp(type, "eval") == 0) {
    static const char* const names[] = {"id", "ok", "json"};
    const char* raw = "null";
    size_t raw_len = 4;
    json_raw(json, "result", &raw, &raw_len);
    val_t fields[] = {v_u32(u32_field(json, "id")),
                      v_bool(bool_field(json, "ok")), v_string(raw, raw_len)};
    payload = v_record(3, names, fields);
    *out = v_variant("eval-result", &payload);
  } else if (strcmp(type, "crashed") == 0) {
    *out = v_variant("crashed", NULL);
  } else if (strcmp(type, "closed") == 0) {
    *out = v_variant("closed", NULL);
  } else if (strcmp(type, "overscroll") == 0) {
    static const char* const kinds[] = {"refresh", "back", "forward"};
    static const char* const phases[] = {"start", "pull", "release",
                                         "cancel"};
    static const char* const names[] = {"kind", "phase", "dx", "dy"};
    char *kind_s, *phase_s;
    const char* kind = enum_field(json, "kind", kinds, 3, &kind_s);
    const char* phase = enum_field(json, "phase", phases, 4, &phase_s);
    ok = kind && phase;
    if (ok) {
      val_t fields[] = {v_enum(kind), v_enum(phase), v_f32(f32_field(json, "dx")),
                        v_f32(f32_field(json, "dy"))};
      payload = v_record(4, names, fields);
      *out = v_variant("overscroll", &payload);
    }
    free(kind_s);
    free(phase_s);
  } else if (strcmp(type, "context_menu") == 0) {
    static const char* const names[] = {"x", "y", "link-url", "link-text",
                                        "image-url", "selection"};
    long long x = 0, y = 0;
    json_int(json, "x", &x);
    json_int(json, "y", &y);
    val_t fields[] = {v_s32((int32_t)x), v_s32((int32_t)y),
                      string_field(json, "link_url"),
                      string_field(json, "link_text"),
                      string_field(json, "image_url"),
                      string_field(json, "selection")};
    payload = v_record(6, names, fields);
    *out = v_variant("context-menu", &payload);
  } else {
    ok = false;
  }
  free(type);
  return ok;
}

void app_engine_event(uint32_t page_id, const char* json) {
  val_t event;
  if (page_alive(page_id) && page_event_from_json(json, &event)) {
    app_page_event(page_id, event);
  }
}
