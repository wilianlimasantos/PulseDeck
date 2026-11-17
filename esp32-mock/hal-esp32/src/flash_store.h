// ESP32 flash backends for the core's IBlobStore.
//  - LittleFsStore: one file per key (presets + scenes), atomic replace via temp file + rename
//  - NvsStore:      small settings blob in NVS (GSAVE)
#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "pd/store.h"

class LittleFsStore : public pd::IBlobStore {
 public:
  bool begin();  // mounts (formats on first use) and removes stale temp files
  bool read(const char* key, uint8_t* buf, size_t cap, size_t& len) override;
  bool write(const char* key, const uint8_t* data, size_t len) override;
  bool erase(const char* key) override;
  bool exists(const char* key) override;
  const char* backend() const override { return "littlefs"; }
  size_t totalBytes() const;
  size_t usedBytes() const;
};

class NvsStore : public pd::IBlobStore {
 public:
  bool begin();
  bool read(const char* key, uint8_t* buf, size_t cap, size_t& len) override;
  bool write(const char* key, const uint8_t* data, size_t len) override;
  bool erase(const char* key) override;
  bool exists(const char* key) override;
  const char* backend() const override { return "nvs"; }

 private:
  Preferences p_;
};
