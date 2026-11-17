// Append-only, power-cut-safe blob store on raw NOR flash (clean room: standard log-structured design, no vendor code).
//
// Region = two banks of kBankSectors x 4 KiB (kRegionBytes in total). One bank is active; the other is erased and used when the active one fills up.
//   bank    = 16-byte header {'PDB2', seq, crc32(magic,seq), 0xFFFFFFFF} + records. The bank with the highest valid seq is active.
//   record  = 12-byte header {'PDR2', len u16, klen u8, flags u8, crc32} + key (klen, no NUL) + data (len), padded to 4 bytes.
//             crc32 covers len, klen, flags, key and data. flags bit0 = tombstone (key erased). The LAST valid record of a key wins.
// Write  : program ONE record at the tail of the active bank. A power cut leaves a torn tail (bad CRC): readers stop there and the NEXT write
//          compacts into the other bank instead of appending after the garbage. Old values are never touched, so a cut keeps old OR new.
// Compact: erase the other bank, copy the live records, program the bank header LAST (commit), then append. A cut before the commit leaves the
//          old bank active; after it the new bank is active and already complete.
// Wear   : a 4 KiB sector is erased only when a bank is recycled, i.e. once per ~(bank size / record size) writes.
// Host-tested in tests/test_log_store.cpp with a power cut at every programmed byte.
#pragma once
#include <stdint.h>
#include <string.h>
#include "pd/crc.h"
#include "pd/store.h"
#include "flash_store.h"   // FlashDev
namespace tg {
class LogStore : public pd::IBlobStore {
 public:
  static constexpr uint32_t kSector = 4096, kBankSectors = 3, kBank = kBankSectors * kSector, kRegionBytes = 2 * kBank;
  static constexpr uint32_t kBankMagic = 0x32424450u /*'PDB2'*/, kRecMagic = 0x32524450u /*'PDR2'*/, kBankHdr = 16, kRecHdr = 12;
  static constexpr uint32_t kMaxKey = 15, kMaxData = 4096;   // presets ~300 B, settings ~1 KB, IR <= 1032 B, captures 1..4 KB
  LogStore(FlashDev& d, uint32_t base) : d_(d), base_(base) {}

  bool read(const char* key, uint8_t* buf, size_t cap, size_t& len) override {
    if (!mount()) return false;
    Rec r; if (!find(key, r) || r.tomb || r.len > cap) return false;
    if (!d_.read(r.pos + kRecHdr + r.klen, buf, r.len)) return false;
    len = r.len; return true;
  }
  bool exists(const char* key) override { Rec r; return mount() && find(key, r) && !r.tomb; }
  bool write(const char* key, const uint8_t* data, size_t len) override { return put(key, data, len, false); }
  bool erase(const char* key) override { Rec r; if (!mount()) return false; if (!find(key, r) || r.tomb) return true; return put(key, nullptr, 0, true); }
  const char* backend() const override { return "flash-log"; }

  // introspection for tests/diagnostics
  uint32_t activeBank() { mount(); return act_; }
  uint32_t used() { mount(); return tail_ - bankOff(act_); }
  bool dirty() { mount(); return dirty_; }
  void remount() { mounted_ = false; }

 private:
  struct Rec { uint32_t pos = 0, size = 0; uint16_t len = 0; uint8_t klen = 0; bool tomb = false; };
  static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }
  uint32_t bankOff(uint32_t b) const { return base_ + b * kBank; }
  static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
  static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

  // Reads the record at pos (inside bank b). 1 = valid (r filled), 0 = erased space (end of log), -1 = garbage/torn/unreadable.
  int rec(uint32_t b, uint32_t pos, Rec& r, uint8_t* body) {
    uint32_t end = bankOff(b) + kBank; uint8_t h[kRecHdr];
    if (pos + kRecHdr > end) return pos == end ? 0 : -1;
    if (!d_.read(pos, h, kRecHdr)) return -1;
    uint32_t m = rd32(h);
    if (m == 0xFFFFFFFFu) { for (int i = 4; i < (int)kRecHdr; i++) if (h[i] != 0xFF) return -1; return 0; }
    if (m != kRecMagic) return -1;
    uint16_t len = (uint16_t)(h[4] | h[5] << 8); uint8_t klen = h[6], fl = h[7];
    if (klen == 0 || klen > kMaxKey || len > kMaxData || (fl & ~1u) || pos + kRecHdr + pad4((uint32_t)klen + len) > end) return -1;
    uint32_t c = pd::kCrc32Init; c = pd::crc32_update(c, h + 4, 4);
    uint8_t t[32]; uint32_t off = 0, n = (uint32_t)klen + len;
    while (off < n) {
      uint32_t k = n - off < sizeof t ? n - off : (uint32_t)sizeof t;
      if (!d_.read(pos + kRecHdr + off, t, k)) return -1;
      c = pd::crc32_update(c, t, k);
      if (body) memcpy(body + off, t, k);
      off += k;
    }
    if (pd::crc32_final(c) != rd32(h + 8)) return -1;
    r.pos = pos; r.len = len; r.klen = klen; r.tomb = fl & 1; r.size = kRecHdr + pad4(n); return 1;
  }
  bool bankSeq(uint32_t b, uint32_t& seq) {
    uint8_t h[kBankHdr]; if (!d_.read(bankOff(b), h, kBankHdr)) return false;
    if (rd32(h) != kBankMagic || pd::crc32(h, 8) != rd32(h + 8)) return false;
    seq = rd32(h + 4); return true;
  }
  bool commitBank(uint32_t b, uint32_t seq) {
    uint8_t h[kBankHdr]; wr32(h, kBankMagic); wr32(h + 4, seq); wr32(h + 8, pd::crc32(h, 8)); wr32(h + 12, 0xFFFFFFFFu);
    return d_.program(bankOff(b), h, kBankHdr);
  }
  bool eraseBank(uint32_t b) { for (uint32_t s = 0; s < kBankSectors; s++) if (!d_.erase4k(bankOff(b) + s * kSector)) return false; return true; }

  bool mount() {
    if (mounted_) return true;
    uint32_t s0 = 0, s1 = 0; bool v0 = bankSeq(0, s0), v1 = bankSeq(1, s1);
    if (!v0 && !v1) {   // blank (or both destroyed): format bank 0
      if (!eraseBank(0) || !commitBank(0, 1)) return false;
      act_ = 0; seq_ = 1;
    } else if (v0 && (!v1 || (int32_t)(s0 - s1) > 0)) { act_ = 0; seq_ = s0; }
    else { act_ = 1; seq_ = s1; }
    uint32_t pos = bankOff(act_) + kBankHdr; dirty_ = false; Rec r;
    for (;;) {
      int k = rec(act_, pos, r, nullptr);
      if (k == 0) break;
      if (k < 0) { dirty_ = true; break; }
      pos += r.size;
    }
    tail_ = pos; mounted_ = true; return true;
  }
  bool find(const char* key, Rec& out) {   // last record of `key` in the active bank
    uint32_t kl = (uint32_t)strlen(key); if (kl == 0 || kl > kMaxKey) return false;
    bool got = false; uint32_t pos = bankOff(act_) + kBankHdr; Rec r; uint8_t kb[kMaxKey];
    while (pos < tail_ && rec(act_, pos, r, nullptr) == 1) {
      if (r.klen == kl && d_.read(pos + kRecHdr, kb, kl) && !memcmp(kb, key, kl)) { out = r; got = true; }
      pos += r.size;
    }
    return got;
  }
  bool append(uint32_t at, const char* key, uint32_t kl, const uint8_t* data, uint32_t len, bool tomb) {
    uint8_t h[kRecHdr]; wr32(h, kRecMagic); h[4] = (uint8_t)len; h[5] = (uint8_t)(len >> 8); h[6] = (uint8_t)kl; h[7] = tomb ? 1 : 0;
    uint32_t c = pd::kCrc32Init; c = pd::crc32_update(c, h + 4, 4); c = pd::crc32_update(c, (const uint8_t*)key, kl); if (len) c = pd::crc32_update(c, data, len);
    wr32(h + 8, pd::crc32_final(c));
    uint32_t n = pad4(kl + len);
    memcpy(pkt_, h, kRecHdr); memset(pkt_ + kRecHdr, 0xFF, n); memcpy(pkt_ + kRecHdr, key, kl); if (len) memcpy(pkt_ + kRecHdr + kl, data, len);
    return d_.program(at, pkt_, kRecHdr + n);   // one program: header + body; a cut leaves a record with a bad CRC
  }
  bool compact() {   // copy live records to the other bank and make it active
    uint32_t nb = 1 - act_; if (!eraseBank(nb)) return false;
    uint32_t src = bankOff(act_) + kBankHdr, dst = bankOff(nb) + kBankHdr; Rec r, q;
    while (src < tail_ && rec(act_, src, r, bodyBuf_) == 1) {
      bool live = !r.tomb;
      for (uint32_t p = src + r.size; live && p < tail_ && rec(act_, p, q, nullptr) == 1; p += q.size) {   // a later record of the same key supersedes this one
        uint8_t kb[kMaxKey]; if (q.klen == r.klen && d_.read(p + kRecHdr, kb, q.klen) && !memcmp(kb, bodyBuf_, r.klen)) live = false;
      }
      if (live) {
        if (!append(dst, (const char*)bodyBuf_, r.klen, bodyBuf_ + r.klen, r.len, false)) return false;
        dst += r.size;
      }
      src += r.size;
    }
    if (!commitBank(nb, seq_ + 1)) return false;   // commit point
    act_ = nb; seq_++; tail_ = dst; dirty_ = false; return true;
  }
  bool put(const char* key, const uint8_t* data, size_t len, bool tomb) {
    uint32_t kl = (uint32_t)strlen(key);
    if (kl == 0 || kl > kMaxKey || len > kMaxData || (len && !data)) return false;
    if (!mount()) return false;
    uint32_t size = kRecHdr + pad4(kl + (uint32_t)len);
    if (dirty_ || tail_ + size > bankOff(act_) + kBank) {
      if (!compact()) { mounted_ = false; return false; }
      if (tail_ + size > bankOff(act_) + kBank) return false;   // live data + this record do not fit
    }
    if (!append(tail_, key, kl, data, (uint32_t)len, tomb)) { mounted_ = false; return false; }
    tail_ += size; return true;
  }

  FlashDev& d_; uint32_t base_;
  bool mounted_ = false, dirty_ = false; uint32_t act_ = 0, seq_ = 0, tail_ = 0;
  uint8_t bodyBuf_[kMaxKey + kMaxData], pkt_[kRecHdr + ((kMaxKey + kMaxData + 3u) & ~3u)];
};
}  // namespace tg
