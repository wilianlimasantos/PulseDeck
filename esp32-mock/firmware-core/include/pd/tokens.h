// Allocation-free line tokenizer (whitespace separated, max 24 tokens, line <= 255 chars).
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace pd {

class Tokens {
 public:
  static constexpr int kMax = 24;
  static constexpr size_t kLine = 256;

  explicit Tokens(const char* line) {
    size_t n = strlen(line);
    if (n >= kLine) n = kLine - 1;
    memcpy(buf_, line, n);
    buf_[n] = 0;
    char* s = buf_;
    while (*s && count_ < kMax) {
      while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') *s++ = 0;
      if (!*s) break;
      p_[count_++] = s;
      while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
    }
    if (*s) *s = 0;
  }
  int size() const { return count_; }
  bool empty() const { return count_ == 0; }
  // NUL-terminated token (empty string when absent).
  const char* c(int i) const { return (i >= 0 && i < count_) ? p_[i] : ""; }
  bool is(int i, const char* s) const { return strcmp(c(i), s) == 0; }
  long num(int i, long def = 0) const { return (i >= 0 && i < count_) ? strtol(p_[i], nullptr, 10) : def; }
  // Tokens i.. joined with single spaces ("PSAVE 3 My Tone" -> "My Tone"); truncated to cap-1.
  void rest(int i, char* dst, size_t cap) const {
    size_t n = 0;
    for (int k = i; k < count_ && n + 1 < cap; k++) {
      if (k > i && n + 1 < cap) dst[n++] = ' ';
      for (const char* s = p_[k]; *s && n + 1 < cap; s++) dst[n++] = *s;
    }
    dst[n] = 0;
  }

 private:
  char buf_[kLine];
  const char* p_[kMax];
  int count_ = 0;
};

}  // namespace pd
