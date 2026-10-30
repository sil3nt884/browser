/* Copyright 2026 The Umbra Authors. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 *
 * Just enough JSON to read chorium.dll's events: one object, top-level keys
 * looked up by name. Values may nest (eval results); they are skipped over.
 */
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

/* Portable C shared with MSVC builds, not Chromium code: Chromium's C++
 * buffer-safety warnings don't apply. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

static const char* skip_ws(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
    ++p;
  }
  return p;
}

/* |p| at an opening quote; returns just past the closing one, or NULL. */
static const char* skip_string(const char* p) {
  for (++p; *p; ++p) {
    if (*p == '\\') {
      if (!*++p) {
        return NULL;
      }
    } else if (*p == '"') {
      return p + 1;
    }
  }
  return NULL;
}

static const char* skip_value(const char* p) {
  p = skip_ws(p);
  if (*p == '"') {
    return skip_string(p);
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = skip_string(p);
        if (!p) {
          return NULL;
        }
        continue;
      }
      if (*p == '{' || *p == '[') {
        ++depth;
      } else if (*p == '}' || *p == ']') {
        if (--depth == 0) {
          return p + 1;
        }
      }
      ++p;
    }
    return NULL;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ') {
    ++p;
  }
  return p;
}

/* Finds |key| at the top level; returns its value's start (whitespace
 * skipped) and end. */
static bool find(const char* json, const char* key, const char** start,
                 const char** end) {
  size_t key_len = strlen(key);
  const char* p = skip_ws(json);
  if (*p != '{') {
    return false;
  }
  p = skip_ws(p + 1);
  while (*p == '"') {
    const char* k = p + 1;
    const char* after = skip_string(p);
    if (!after) {
      return false;
    }
    bool match = (size_t)(after - 1 - k) == key_len &&
                 memcmp(k, key, key_len) == 0;
    p = skip_ws(after);
    if (*p != ':') {
      return false;
    }
    const char* v = skip_ws(p + 1);
    const char* v_end = skip_value(v);
    if (!v_end) {
      return false;
    }
    if (match) {
      *start = v;
      *end = v_end;
      return true;
    }
    p = skip_ws(v_end);
    if (*p != ',') {
      return false;
    }
    p = skip_ws(p + 1);
  }
  return false;
}

static int hex4(const char* p) {
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    char c = p[i];
    n <<= 4;
    if (c >= '0' && c <= '9') {
      n |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      n |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      n |= c - 'A' + 10;
    } else {
      return -1;
    }
  }
  return n;
}

static char* put_utf8(char* o, unsigned cp) {
  if (cp < 0x80) {
    *o++ = (char)cp;
  } else if (cp < 0x800) {
    *o++ = (char)(0xC0 | (cp >> 6));
    *o++ = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *o++ = (char)(0xE0 | (cp >> 12));
    *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *o++ = (char)(0x80 | (cp & 0x3F));
  } else {
    *o++ = (char)(0xF0 | (cp >> 18));
    *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
    *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *o++ = (char)(0x80 | (cp & 0x3F));
  }
  return o;
}

bool json_string(const char* json, const char* key, char** out) {
  const char *p, *end;
  if (!find(json, key, &p, &end) || *p != '"') {
    return false;
  }
  /* Unescaping never grows the text (\uXXXX is 6 bytes, at most 4 out). */
  char* s = malloc((size_t)(end - p) + 1);
  char* o = s;
  for (++p; p < end - 1; ++p) {
    if (*p != '\\') {
      *o++ = *p;
      continue;
    }
    switch (*++p) {
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'n': *o++ = '\n'; break;
      case 'r': *o++ = '\r'; break;
      case 't': *o++ = '\t'; break;
      case 'u': {
        int cp = hex4(p + 1);
        if (cp < 0) {
          free(s);
          return false;
        }
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
          int lo = hex4(p + 3);
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        o = put_utf8(o, (unsigned)cp);
        break;
      }
      default: *o++ = *p; break; /* \" \\ \/ */
    }
  }
  *o = '\0';
  *out = s;
  return true;
}

bool json_bool(const char* json, const char* key, bool* out) {
  const char *p, *end;
  if (!find(json, key, &p, &end)) {
    return false;
  }
  if (strncmp(p, "true", 4) == 0) {
    *out = true;
  } else if (strncmp(p, "false", 5) == 0) {
    *out = false;
  } else {
    return false;
  }
  return true;
}

bool json_int(const char* json, const char* key, long long* out) {
  const char *p, *end;
  if (!find(json, key, &p, &end)) {
    return false;
  }
  char* stop;
  long long n = strtoll(p, &stop, 10);
  if (stop == p) {
    return false;
  }
  *out = n;
  return true;
}

bool json_raw(const char* json, const char* key, const char** start,
              size_t* len) {
  const char *p, *end;
  if (!find(json, key, &p, &end)) {
    return false;
  }
  *start = p;
  *len = (size_t)(end - p);
  return true;
}
