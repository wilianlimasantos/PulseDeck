#include "pd/codec.h"

#include <string.h>

#include "pd/catalog.h"
#include "pd/crc.h"

namespace pd {

namespace {

// Bounded writer: never writes past cap, remembers overflow. No heap.
struct W {
  uint8_t* d;
  size_t cap;
  size_t n = 0;
  bool over = false;
  W(uint8_t* buf, size_t c) : d(buf), cap(c) {}
  void u8(uint32_t x) { if (n < cap) d[n] = (uint8_t)x; else over = true; n++; }
  void u16(uint32_t x) { u8(x); u8(x >> 8); }
  void u32(uint32_t x) { u16(x); u16(x >> 16); }
  void raw(const void* p, size_t k) { for (size_t i = 0; i < k; i++) u8(((const uint8_t*)p)[i]); }
  size_t tlvBegin(uint8_t key) { u8(key); size_t at = n; u16(0); return at; }
  void tlvEnd(size_t at) { size_t len = n - at - 2; if (at + 1 < cap) { d[at] = (uint8_t)len; d[at + 1] = (uint8_t)(len >> 8); } }
};

struct R {
  const uint8_t* d;
  size_t n, i = 0;
  bool ok = true;
  uint8_t u8() { if (i + 1 > n) { ok = false; return 0; } return d[i++]; }
  uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
  uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
  void raw(void* p, size_t k) { if (i + k > n) { ok = false; memset(p, 0, k); return; } memcpy(p, d + i, k); i += k; }
};

constexpr uint8_t kPresetMagic[4] = {'P', 'D', 'P', '1'};
constexpr uint8_t kSetMagic[4] = {'P', 'D', 'S', '1'};
constexpr uint16_t kVersion = 1;

// Record = magic[4] ver u16 len u32 | payload | crc32.  The payload is written at out+10 by the caller.
constexpr size_t kHeader = 10, kTrailer = 4;

size_t seal(uint8_t* out, size_t cap, const uint8_t magic[4], const W& pl) {
  if (pl.over) return 0;
  W w(out, cap);
  w.raw(magic, 4);
  w.u16(kVersion);
  w.u32((uint32_t)pl.n);
  w.n += pl.n;  // payload already in place
  w.u32(crc32(out, w.n));
  return w.over ? 0 : w.n;
}

CodecResult unseal(const uint8_t* d, size_t n, const uint8_t magic[4], const uint8_t*& pl, size_t& pn) {
  if (n < 14) return CodecResult::Truncated;
  if (memcmp(d, magic, 4) != 0) return CodecResult::BadMagic;
  R r{d + 4, n - 4};
  uint16_t ver = r.u16();
  uint32_t len = r.u32();
  if (ver != kVersion) return CodecResult::BadVersion;
  if ((size_t)len + 14 != n) return CodecResult::Truncated;
  uint32_t want = (uint32_t)d[n - 4] | ((uint32_t)d[n - 3] << 8) | ((uint32_t)d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
  if (crc32(d, n - 4) != want) return CodecResult::BadCrc;
  pl = d + 10;
  pn = len;
  return CodecResult::Ok;
}

bool strOk(const char* s, size_t cap) { return memchr(s, 0, cap) != nullptr; }

}  // namespace

const char* codecResultName(CodecResult r) {
  switch (r) {
    case CodecResult::Ok: return "ok";
    case CodecResult::Truncated: return "truncated";
    case CodecResult::BadMagic: return "bad magic";
    case CodecResult::BadVersion: return "bad version";
    case CodecResult::BadCrc: return "bad crc";
    case CodecResult::BadField: return "bad field";
  }
  return "?";
}

size_t encodePreset(const Preset& p, uint8_t* out, size_t cap) {
  if (cap < kHeader + kTrailer) return 0;
  W w(out + kHeader, cap - kHeader - kTrailer);
  w.raw(p.name, kNameLen);
  w.raw(p.color, 8);
  w.u8((p.amp_present ? 1 : 0) | (p.amp_on ? 2 : 0));
  for (int i = 0; i < 6; i++) w.u16((uint16_t)p.ampeq[i]);
  w.u8((uint8_t)p.gain_in);
  w.u8((uint8_t)p.gain_out);
  w.u8((uint8_t)p.capture);
  w.u8(p.nb);
  for (int i = 0; i < p.nb; i++) {
    const Block& b = p.b[i];
    w.u8(b.type); w.u8((uint8_t)b.bank); w.u8(b.lane); w.u8(b.on ? 1 : 0); w.u8(b.np);
    for (int k = 0; k < kMaxParams; k++) w.u16((uint16_t)b.p[k]);
  }
  for (int s = 0; s < kScenes; s++) {
    const Scene& c = p.sc[s];
    w.u8(c.used ? 1 : 0);
    w.raw(c.name, kSceneNameLen);
    w.u8((uint8_t)c.gain_in); w.u8((uint8_t)c.gain_out); w.u8(c.amp_on ? 1 : 0);
    for (int i = 0; i < 6; i++) w.u16((uint16_t)c.ampeq[i]);
    w.u8(c.nb);
    for (int i = 0; i < c.nb; i++) {
      w.u8(c.b[i].on ? 1 : 0);
      for (int k = 0; k < kMaxParams; k++) w.u16((uint16_t)c.b[i].p[k]);
    }
  }
  return seal(out, cap, kPresetMagic, w);
}

CodecResult decodePreset(const uint8_t* d, size_t n, Preset& p) {
  const uint8_t* pl;
  size_t pn;
  CodecResult rc = unseal(d, n, kPresetMagic, pl, pn);
  if (rc != CodecResult::Ok) return rc;
  R r{pl, pn};
  Preset& q = p;  // decoded in place; contents are undefined if an error is returned
  q = Preset();
  r.raw(q.name, kNameLen);
  r.raw(q.color, 8);
  uint8_t fl = r.u8();
  q.amp_present = fl & 1;
  q.amp_on = fl & 2;
  for (int i = 0; i < 6; i++) q.ampeq[i] = (int16_t)r.u16();
  q.gain_in = (int8_t)r.u8();
  q.gain_out = (int8_t)r.u8();
  q.capture = (int8_t)r.u8();
  q.nb = r.u8();
  if (!r.ok || q.nb > kMaxBlocks || !strOk(q.name, kNameLen) || !strOk(q.color, 8)) return CodecResult::BadField;
  for (int i = 0; i < q.nb; i++) {
    Block& b = q.b[i];
    b.type = r.u8(); b.bank = (char)r.u8(); b.lane = r.u8(); b.on = r.u8() != 0; b.np = r.u8();
    for (int k = 0; k < kMaxParams; k++) b.p[k] = (int16_t)r.u16();
    if (!r.ok || !fxById(b.type) || (b.bank != 'P' && b.bank != 'Q') || b.np > kMaxParams) return CodecResult::BadField;
  }
  for (int s = 0; s < kScenes; s++) {
    Scene& c = q.sc[s];
    c.used = r.u8() != 0;
    r.raw(c.name, kSceneNameLen);
    c.gain_in = (int8_t)r.u8(); c.gain_out = (int8_t)r.u8(); c.amp_on = r.u8() != 0;
    for (int i = 0; i < 6; i++) c.ampeq[i] = (int16_t)r.u16();
    c.nb = r.u8();
    if (!r.ok || c.nb > kMaxBlocks || !strOk(c.name, kSceneNameLen)) return CodecResult::BadField;
    for (int i = 0; i < c.nb; i++) {
      c.b[i].on = r.u8() != 0;
      for (int k = 0; k < kMaxParams; k++) c.b[i].p[k] = (int16_t)r.u16();
    }
  }
  if (!r.ok) return CodecResult::Truncated;
  return CodecResult::Ok;
}

void infoFromPreset(const Preset& p, PresetInfo& i) {
  i.used = true;
  memcpy(i.name, p.name, kNameLen);
  memcpy(i.color, p.color, 8);
  i.nb = p.nb;
  i.amp_present = p.amp_present;
  i.scene_mask = 0;
  for (int s = 0; s < kScenes; s++)
    if (p.sc[s].used) i.scene_mask |= (uint8_t)(1u << s);
}

// Settings: TLV items so new fields can be added without breaking old records.
enum : uint8_t { kHpf = 1, kGeq, kLayout, kFsa, kFspress, kLed, kFsled, kCaps, kIrs, kLast, kBank };

static void putLib(W& t, const LibSlot* s, int n) {
  for (int i = 0; i < n; i++) { t.u8(s[i].used ? 1 : 0); t.raw(s[i].name, kNameLen); t.u32(s[i].size); }
}

size_t encodeSettings(const Settings& s, uint8_t* out, size_t cap) {
  if (cap < kHeader + kTrailer) return 0;
  W w(out + kHeader, cap - kHeader - kTrailer);
  size_t at;
  at = w.tlvBegin(kHpf); w.u16((uint16_t)s.hpf); w.tlvEnd(at);
  at = w.tlvBegin(kGeq); for (int i = 0; i < 6; i++) w.u16((uint16_t)s.geq[i]); w.tlvEnd(at);
  at = w.tlvBegin(kLayout); w.u8((uint8_t)s.layout); w.tlvEnd(at);
  at = w.tlvBegin(kFsa);
  for (int f = 0; f < 4; f++) for (int g = 0; g < 3; g++) { w.u8(s.fsa[f][g].type); w.u8(s.fsa[f][g].a); w.u8(s.fsa[f][g].b); }
  w.tlvEnd(at);
  at = w.tlvBegin(kFspress); for (int f = 0; f < 4; f++) w.u8(s.fspress[f] ? 1 : 0); w.tlvEnd(at);
  at = w.tlvBegin(kLed); for (int i = 0; i < 7; i++) w.raw(s.led[i], 12); w.tlvEnd(at);
  at = w.tlvBegin(kFsled);
  for (int f = 0; f < 4; f++) { w.u8(s.fsled[f].custom ? 1 : 0); w.raw(s.fsled[f].on, 8); w.raw(s.fsled[f].off, 8); }
  w.tlvEnd(at);
  at = w.tlvBegin(kCaps); putLib(w, s.caps, kCaptureSlots); w.tlvEnd(at);
  at = w.tlvBegin(kIrs); putLib(w, s.irs, kIrSlots); w.tlvEnd(at);
  at = w.tlvBegin(kLast); w.u8((uint8_t)s.last_preset); w.tlvEnd(at);
  at = w.tlvBegin(kBank); w.u8(s.bank); w.tlvEnd(at);
  return seal(out, cap, kSetMagic, w);
}

CodecResult decodeSettings(const uint8_t* d, size_t n, Settings& s) {
  const uint8_t* pl;
  size_t pn;
  CodecResult rc = unseal(d, n, kSetMagic, pl, pn);
  if (rc != CodecResult::Ok) return rc;
  Settings q;
  R r{pl, pn};
  while (r.i < pn) {
    uint8_t key = r.u8();
    uint16_t len = r.u16();
    if (!r.ok || r.i + len > pn) return CodecResult::Truncated;
    R t{pl + r.i, len};
    r.i += len;
    switch (key) {
      case kHpf: q.hpf = (int16_t)t.u16(); break;
      case kGeq: for (int i = 0; i < 6; i++) q.geq[i] = (int16_t)t.u16(); break;
      case kLayout: { uint8_t l = t.u8(); if (l > 2) return CodecResult::BadField; q.layout = (Layout)l; break; }
      case kFsa: for (int f = 0; f < 4; f++) for (int g = 0; g < 3; g++) { q.fsa[f][g].type = t.u8(); q.fsa[f][g].a = t.u8(); q.fsa[f][g].b = t.u8(); } break;
      case kFspress: for (int f = 0; f < 4; f++) q.fspress[f] = t.u8() != 0; break;
      case kLed: for (int i = 0; i < 7; i++) { t.raw(q.led[i], 12); if (!strOk(q.led[i], 12)) return CodecResult::BadField; } break;
      case kFsled: for (int f = 0; f < 4; f++) { q.fsled[f].custom = t.u8() != 0; t.raw(q.fsled[f].on, 8); t.raw(q.fsled[f].off, 8); if (!strOk(q.fsled[f].on, 8) || !strOk(q.fsled[f].off, 8)) return CodecResult::BadField; } break;
      case kCaps: for (int i = 0; i < kCaptureSlots; i++) { q.caps[i].used = t.u8() != 0; t.raw(q.caps[i].name, kNameLen); q.caps[i].size = t.u32(); if (!strOk(q.caps[i].name, kNameLen)) return CodecResult::BadField; } break;
      case kIrs: for (int i = 0; i < kIrSlots; i++) { q.irs[i].used = t.u8() != 0; t.raw(q.irs[i].name, kNameLen); q.irs[i].size = t.u32(); if (!strOk(q.irs[i].name, kNameLen)) return CodecResult::BadField; } break;
      case kLast: q.last_preset = (int8_t)t.u8(); break;
      case kBank: q.bank = t.u8() & 7; break;
      default: break;  // unknown key from a newer version: ignore
    }
    if (!t.ok) return CodecResult::Truncated;
  }
  s = q;
  return CodecResult::Ok;
}

}  // namespace pd
