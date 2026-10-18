// util.h - tiny header-only helpers shared by several modules.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bounded string copy that always NUL-terminates (never overflows dst).
static inline void fr_copy(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  if (!src) src = "";
  size_t i = 0;
  for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
}

template <typename T>
static inline T fr_clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
