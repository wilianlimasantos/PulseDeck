// Power-fail-safe blob store on raw NOR flash. One A/B pair of 4 KiB sectors per key id (0..32: p00..p31, gset).
// Sector = 16-byte header {magic 'PDK1', seq, len, key, crc32(data)} + data (<= 4080). A write goes to the sector that is
// NOT the newest valid one: erase, program data, program header LAST. A cut leaves the other sector (old value) valid.
// Region is a build-time choice (TG_STORE_OFF): it must be flash that the Tank-G stock layout leaves free. NOT VERIFIED.
#pragma once
#include <stdint.h>
#include <string.h>
#include "pd/crc.h"
#include "pd/store.h"
namespace tg {
struct FlashDev {  // implemented by the BSP glue (fl_*) or by a host simulator
  virtual bool read(uint32_t off, uint8_t* dst, uint32_t n) = 0;
  virtual bool erase4k(uint32_t off) = 0;
  virtual bool program(uint32_t off, const uint8_t* src, uint32_t n) = 0;  // bits only 1->0
};
class FlashStore : public pd::IBlobStore {
 public:
  static constexpr uint32_t kSector = 4096, kKeys = 33, kRegionBytes = kKeys * 2 * kSector;
  FlashStore(FlashDev& d, uint32_t base) : d_(d), base_(base) {}
  bool read(const char* key, uint8_t* buf, size_t cap, size_t& len) override {
    int k = keyId(key); if (k < 0) return false;
    int s = newest(k, nullptr); if (s < 0) return false;
    Hdr h; if (!hdr(k, s, h) || h.len > cap) return false;
    if (!d_.read(addr(k, s) + sizeof(Hdr), buf, h.len)) return false;
    len = h.len; return true;
  }
  bool write(const char* key, const uint8_t* data, size_t len) override {
    int k = keyId(key); if (k < 0 || len == 0 || len > kSector - sizeof(Hdr)) return false;
    uint32_t seq = 0; int cur = newest(k, &seq);
    int tgt = cur < 0 ? 0 : 1 - cur;
    uint32_t a = addr(k, tgt);
    if (!d_.erase4k(a)) return false;
    if (!d_.program(a + sizeof(Hdr), data, (uint32_t)len)) return false;
    Hdr h; h.magic = kMagic; h.seq = seq + 1; h.len = (uint16_t)len; h.key = (uint8_t)k; h.rsv = 0xFF;
    h.crc = crc(data, len);
    return d_.program(a, (const uint8_t*)&h, sizeof h);  // commit record last
  }
  bool erase(const char* key) override {
    int k = keyId(key); if (k < 0) return false;
    for (int s = 0; s < 2; s++) if (!d_.erase4k(addr(k, s))) return false;
    return true;
  }
  bool exists(const char* key) override { int k = keyId(key); return k >= 0 && newest(k, nullptr) >= 0; }
  const char* backend() const override { return "nor-ab"; }
 private:
  struct Hdr { uint32_t magic, seq; uint16_t len; uint8_t key, rsv; uint32_t crc; };
  static constexpr uint32_t kMagic = 0x314B4450;  // "PDK1"
  FlashDev& d_; uint32_t base_;
  uint32_t addr(int k, int s) const { return base_ + ((uint32_t)k * 2 + (uint32_t)s) * kSector; }
  static uint32_t crc(const uint8_t* p, size_t n) { return pd::crc32_final(pd::crc32_update(pd::kCrc32Init, p, n)); }
  static int keyId(const char* k) {
    if (!strcmp(k, "gset")) return 32;
    if (k[0] == 'p' && k[1] >= '0' && k[1] <= '9' && k[2] >= '0' && k[2] <= '9' && !k[3]) { int n = (k[1] - '0') * 10 + (k[2] - '0'); return n < 32 ? n : -1; }
    return -1;
  }
  bool hdr(int k, int s, Hdr& h) {
    if (!d_.read(addr(k, s), (uint8_t*)&h, sizeof h) || h.magic != kMagic || h.key != k || h.len == 0 || h.len > kSector - sizeof h) return false;
    uint8_t t[64]; uint32_t c = pd::kCrc32Init; uint32_t off = addr(k, s) + sizeof h;
    for (uint32_t i = 0; i < h.len; i += sizeof t) { uint32_t n = h.len - i < sizeof t ? h.len - i : sizeof t; if (!d_.read(off + i, t, n)) return false; c = pd::crc32_update(c, t, n); }
    return pd::crc32_final(c) == h.crc;
  }
  int newest(int k, uint32_t* seq) {
    Hdr h[2]; bool ok[2] = {hdr(k, 0, h[0]), hdr(k, 1, h[1])};
    int best = -1;
    for (int s = 0; s < 2; s++) if (ok[s] && (best < 0 || (int32_t)(h[s].seq - h[best].seq) > 0)) best = s;
    if (best >= 0 && seq) *seq = h[best].seq;
    return best;
  }
};
}  // namespace tg
