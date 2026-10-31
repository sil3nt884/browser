/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Small helpers over Wasmtime's dynamic component values.
 */
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

/* Portable C shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

static void name_set(wasm_name_t* out, const char* s) {
  wasm_byte_vec_new(out, strlen(s), s);
}

val_t v_bool(bool b) {
  val_t v = {.kind = WASMTIME_COMPONENT_BOOL};
  v.of.boolean = b;
  return v;
}

val_t v_u32(uint32_t n) {
  val_t v = {.kind = WASMTIME_COMPONENT_U32};
  v.of.u32 = n;
  return v;
}

val_t v_u64(uint64_t n) {
  val_t v = {.kind = WASMTIME_COMPONENT_U64};
  v.of.u64 = n;
  return v;
}

val_t v_s32(int32_t n) {
  val_t v = {.kind = WASMTIME_COMPONENT_S32};
  v.of.s32 = n;
  return v;
}

val_t v_f32(float f) {
  val_t v = {.kind = WASMTIME_COMPONENT_F32};
  v.of.f32 = f;
  return v;
}

val_t v_string(const char* s, size_t len) {
  val_t v = {.kind = WASMTIME_COMPONENT_STRING};
  wasm_byte_vec_new(&v.of.string, len, s);
  return v;
}

val_t v_cstring(const char* s) { return v_string(s, strlen(s)); }

val_t v_enum(const char* name) {
  val_t v = {.kind = WASMTIME_COMPONENT_ENUM};
  name_set(&v.of.enumeration, name);
  return v;
}

val_t v_flags(const char* const* names, size_t count, uint32_t bits) {
  val_t v = {.kind = WASMTIME_COMPONENT_FLAGS};
  size_t set = 0;
  for (size_t i = 0; i < count; ++i) {
    set += (bits >> i) & 1;
  }
  wasmtime_component_valflags_new_uninit(&v.of.flags, set);
  size_t j = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((bits >> i) & 1) {
      name_set(&v.of.flags.data[j++], names[i]);
    }
  }
  return v;
}

val_t v_record(size_t count, const char* const* names, val_t* vals) {
  val_t v = {.kind = WASMTIME_COMPONENT_RECORD};
  wasmtime_component_valrecord_new_uninit(&v.of.record, count);
  for (size_t i = 0; i < count; ++i) {
    name_set(&v.of.record.data[i].name, names[i]);
    v.of.record.data[i].val = vals[i];
  }
  return v;
}

val_t v_variant(const char* name, val_t* payload) {
  val_t v = {.kind = WASMTIME_COMPONENT_VARIANT};
  name_set(&v.of.variant.discriminant, name);
  v.of.variant.val = payload ? wasmtime_component_val_new(payload) : NULL;
  return v;
}

val_t v_list(size_t count, val_t* items) {
  val_t v = {.kind = WASMTIME_COMPONENT_LIST};
  wasmtime_component_vallist_new_uninit(&v.of.list, count);
  for (size_t i = 0; i < count; ++i) {
    v.of.list.data[i] = items[i];
  }
  return v;
}

val_t v_option(val_t* some) {
  val_t v = {.kind = WASMTIME_COMPONENT_OPTION};
  v.of.option = some ? wasmtime_component_val_new(some) : NULL;
  return v;
}

val_t v_result(bool ok, val_t* payload) {
  val_t v = {.kind = WASMTIME_COMPONENT_RESULT};
  v.of.result.is_ok = ok;
  v.of.result.val = payload ? wasmtime_component_val_new(payload) : NULL;
  return v;
}

val_t v_bytes(const uint8_t* data, size_t len) {
  val_t v = {.kind = WASMTIME_COMPONENT_LIST};
  wasmtime_component_vallist_new_uninit(&v.of.list, len);
  for (size_t i = 0; i < len; ++i) {
    val_t b = {.kind = WASMTIME_COMPONENT_U8};
    b.of.u8 = data[i];
    v.of.list.data[i] = b;
  }
  return v;
}

bool r_name_is(const wasm_name_t* name, const char* s) {
  size_t n = strlen(s);
  return name->size == n && memcmp(name->data, s, n) == 0;
}

const val_t* r_field(const val_t* record, const char* name) {
  if (!record || record->kind != WASMTIME_COMPONENT_RECORD) {
    return NULL;
  }
  for (size_t i = 0; i < record->of.record.size; ++i) {
    if (r_name_is(&record->of.record.data[i].name, name)) {
      return &record->of.record.data[i].val;
    }
  }
  return NULL;
}

bool r_case(const val_t* variant, const char* name) {
  return variant && variant->kind == WASMTIME_COMPONENT_VARIANT &&
         r_name_is(&variant->of.variant.discriminant, name);
}

const val_t* r_payload(const val_t* variant) {
  return variant && variant->kind == WASMTIME_COMPONENT_VARIANT
             ? variant->of.variant.val
             : NULL;
}

char* r_cstring(const val_t* v) {
  size_t n = v && v->kind == WASMTIME_COMPONENT_STRING ? v->of.string.size : 0;
  char* s = malloc(n + 1);
  if (n) {
    memcpy(s, v->of.string.data, n);
  }
  s[n] = '\0';
  return s;
}

char* r_opt_cstring(const val_t* v) {
  if (!v || v->kind != WASMTIME_COMPONENT_OPTION || !v->of.option) {
    return NULL;
  }
  return r_cstring(v->of.option);
}

uint32_t r_flag_bits(const val_t* flags, const char* const* names,
                     size_t count) {
  uint32_t bits = 0;
  if (!flags || flags->kind != WASMTIME_COMPONENT_FLAGS) {
    return 0;
  }
  for (size_t i = 0; i < flags->of.flags.size; ++i) {
    for (size_t j = 0; j < count; ++j) {
      if (r_name_is(&flags->of.flags.data[i], names[j])) {
        bits |= 1u << j;
      }
    }
  }
  return bits;
}
