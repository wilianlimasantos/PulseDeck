// Volatile IBlobStore in RAM (lost at power off; persistence still needs the flash store). No heap, fixed pool.
#pragma once
#include <stdint.h>
#include <string.h>
#include "pd/store.h"
namespace tg {
template <int kEntries, int kCap>
struct RamBlobStore : pd::IBlobStore {
  struct E { char k[16]; uint16_t n; bool used; uint8_t d[kCap]; };
  E e_[kEntries] = {};
  E* find(const char* k) { for (auto& x : e_) if (x.used && !strncmp(x.k, k, sizeof x.k)) return &x; return nullptr; }
  bool read(const char* k, uint8_t* buf, size_t cap, size_t& l) override { E* x = find(k); if (!x || x->n > cap) return false; memcpy(buf, x->d, x->n); l = x->n; return true; }
  bool write(const char* k, const uint8_t* d, size_t n) override {
    if (n > (size_t)kCap || strlen(k) >= sizeof(E::k)) return false;
    E* x = find(k); if (!x) for (auto& y : e_) if (!y.used) { x = &y; break; }
    if (!x) return false;
    memset(x->k, 0, sizeof x->k); memcpy(x->k, k, strlen(k)); memcpy(x->d, d, n); x->n = (uint16_t)n; x->used = true; return true;
  }
  bool erase(const char* k) override { E* x = find(k); if (x) x->used = false; return true; }
  bool exists(const char* k) override { return find(k) != nullptr; }
  const char* backend() const override { return "ram"; }
};
}  // namespace tg
