// Host-side IBlobStore: RAM map with fault injection (tests only).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "pd/store.h"

struct MemStore : pd::IBlobStore {
  std::map<std::string, std::vector<uint8_t>> m;
  bool failWrites = false;
  bool read(const char* k, uint8_t* buf, size_t cap, size_t& len) override {
    auto it = m.find(k);
    if (it == m.end() || it->second.size() > cap) return false;
    memcpy(buf, it->second.data(), it->second.size());
    len = it->second.size();
    return true;
  }
  bool write(const char* k, const uint8_t* d, size_t n) override {
    if (failWrites) return false;
    m[k].assign(d, d + n);
    return true;
  }
  bool erase(const char* k) override { return m.erase(k) > 0; }
  bool exists(const char* k) override { return m.count(k) > 0; }
  const char* backend() const override { return "mem"; }
};

struct Capture : pd::ILineSink {
  std::vector<std::string> lines;
  void line(const char* s) override { lines.emplace_back(s); }
  bool has(const std::string& prefix) const {
    for (auto& l : lines) if (l.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
  }
  std::string find(const std::string& prefix) const {
    for (auto& l : lines) if (l.compare(0, prefix.size(), prefix) == 0) return l;
    return "";
  }
  std::string last() const { return lines.empty() ? "" : lines.back(); }
  void clear() { lines.clear(); }
};
