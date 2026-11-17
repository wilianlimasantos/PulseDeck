#include "flash_store.h"

#include <LittleFS.h>

static String path(const char* key) { return String("/") + key; }

bool LittleFsStore::begin() {
  if (!LittleFS.begin(true)) return false;  // true = format if the mount fails (first boot)
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String n = f.name();
    f.close();
    if (n.endsWith(".tmp")) LittleFS.remove(n.startsWith("/") ? n : "/" + n);  // interrupted write
  }
  return true;
}

bool LittleFsStore::read(const char* key, uint8_t* buf, size_t cap, size_t& len) {
  if (!LittleFS.exists(path(key))) return false;  // quiet: a missing slot is normal
  File f = LittleFS.open(path(key), "r");
  if (!f) return false;
  size_t n = f.size();
  if (n > cap) { f.close(); return false; }
  size_t got = f.read(buf, n);
  f.close();
  if (got != n) return false;
  len = n;
  return true;
}

bool LittleFsStore::write(const char* key, const uint8_t* data, size_t len) {
  String tmp = path(key) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t w = f.write(data, len);
  f.close();
  if (w != len) { LittleFS.remove(tmp); return false; }
  // rename() replaces the target atomically in LittleFS: after a power cut the old or the new file survives
  if (!LittleFS.rename(tmp, path(key))) { LittleFS.remove(tmp); return false; }
  return true;
}

bool LittleFsStore::erase(const char* key) { return LittleFS.exists(path(key)) && LittleFS.remove(path(key)); }
bool LittleFsStore::exists(const char* key) { return LittleFS.exists(path(key)); }
size_t LittleFsStore::totalBytes() const { return LittleFS.totalBytes(); }
size_t LittleFsStore::usedBytes() const { return LittleFS.usedBytes(); }

// ---------------------------------------------------------------------------

bool NvsStore::begin() { return p_.begin("pulsedeck", false); }

bool NvsStore::read(const char* key, uint8_t* buf, size_t cap, size_t& len) {
  if (!p_.isKey(key)) return false;
  size_t n = p_.getBytesLength(key);
  if (n == 0 || n > cap) return false;
  if (p_.getBytes(key, buf, n) != n) return false;
  len = n;
  return true;
}

bool NvsStore::write(const char* key, const uint8_t* data, size_t len) { return p_.putBytes(key, data, len) == len; }
bool NvsStore::erase(const char* key) { return p_.isKey(key) && p_.remove(key); }
bool NvsStore::exists(const char* key) { return p_.isKey(key); }
