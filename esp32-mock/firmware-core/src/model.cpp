#include "pd/model.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pd/store.h"

namespace pd {

void copyName(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < cap; i++) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = (c >= 0x20 && c < 0x7f && c != '\'') ? (char)c : '_';
  }
  memset(dst + i, 0, cap - i);
}

void defaultFsa(Settings& s, Layout l) {
  static const uint8_t longType[4] = {4, 9, 7, 3};  // prev preset, tuner, tap, next preset
  for (int f = 0; f < 4; f++) {
    s.fsa[f][0] = {1, (uint8_t)f, 255};
    s.fsa[f][1] = {l == Layout::Scenes || l == Layout::Custom ? longType[f] : (uint8_t)0, 0, 255};
    s.fsa[f][2] = {0, 0, 255};
  }
}

void say(ILineSink& o, const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  o.line(buf);
}

}  // namespace pd
