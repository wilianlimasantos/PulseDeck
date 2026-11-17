// Uploaded media for the Tank-G: amp captures (A2L1 blobs) and impulse responses. Implements pd::IMedia for the Engine.
//  - upload goes into a RAM staging buffer; LDEND/IREND validate it and make it the LIVE (sounding) item at once;
//  - CSAVE/ISAVE persist it through the same power-cut-safe log store as the presets (keys c00..c14, i0..i7), inside flash 0x75000..0x7B000;
//  - everything stored is mirrored in RAM at boot, so selecting a slot never reads the flash while audio runs;
//  - the audio ISR only ever sees complete objects: a new model / IR is built in the spare slot and then published (index flip).
// Limits: capture blob <= 8192 B to play, <= LogStore::kMaxData to be saved; an IR is reduced to <= 512 taps at 24 kHz (21 ms).
// The flash bank is 12 KiB for presets + captures + IRs together: a save that does not fit fails cleanly (the item keeps playing until power off).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pd/store.h"
#include "a2l.h"
#include "dsp_fx.h"
#include "dsp_gain.h"
namespace tg {
constexpr int kCapSlots = 15, kIrSlots = 8;
// Staged int16 mono samples (fs 48 or 24 kHz) -> FIR taps at 24 kHz. Integer only.
//  48 kHz input: 11-tap half-band low-pass (Hamming-windowed sinc) and every 2nd sample, x2 so the 24 kHz convolution has the same gain;
//  pre-emphasis e[k] = h[k] + a (2 h[k] - h[k-1] - h[k+1]), a = 0.2, makes up for the run-time pair-average + linear interpolation (+-0.5 dB to 8 kHz);
//  at most kIrMaxTaps taps, the last 32 faded out when the response is cut; scaled into int16 with the shift folded into the gain.
inline void makeIr(const int16_t* s, int n, int fs_khz, long gain_mdb, dsp::IrData& o, int32_t* work /* >= 2050 words */) {
  auto at = [&](int i) -> int32_t { return (i >= 0 && i < n) ? s[i] : 0; };
  int m = fs_khz == 24 ? n : (n + 1) / 2; if (m > 2048) m = 2048;
  for (int k = 0; k < m; k++) { int c = 2 * k;
    work[k] = fs_khz == 24 ? at(k) : (2022 * at(c) + 1189 * (at(c - 1) + at(c + 1)) - 173 * (at(c - 3) + at(c + 3)) + 21 * (at(c - 5) + at(c + 5))) / 2048; }
  int nt = m < dsp::kIrMaxTaps ? m : dsp::kIrMaxTaps;
  int32_t prev = 0, peak = 1;
  for (int k = 0; k < nt; k++) {
    int32_t cur = work[k], nxt = k + 1 < m ? work[k + 1] : 0;
    int32_t e = cur + ((51 * (2 * cur - prev - nxt)) >> 8);
    if (m > nt && k >= nt - 32) e = e * (nt - k) / 32;
    prev = cur; work[k] = e; int32_t a = e < 0 ? -e : e; if (a > peak) peak = a;
  }
  int sh = 0; while ((peak >> sh) > 32767) sh++;
  for (int k = 0; k < nt; k++) o.taps[k] = (int16_t)(work[k] >> sh);
  while (nt > 1 && o.taps[nt - 1] == 0) nt--;
  o.n = (uint16_t)nt; o.gainQ12 = mdbQ12(gain_mdb) << sh;
}

// Platform hook: a free-running tick counter and its rate (firmware: TIMER4, 24 MHz). Unset on the host: no measurement, fixed limit.
inline uint32_t (*g_ticks)() = nullptr; inline uint32_t g_ticksPerUs = 24;
inline void (*g_audioPause)(bool pause) = nullptr;   // stop the audio ISR while timing (a few ms of silence at load), so the number is the real cost
inline uint32_t g_capShareUs = 1600;               // firmware: 2200 when the second core runs the effects
constexpr uint32_t kBlockUs = 2666, kCapShareUs = 1600;   // 128 frames at 48 kHz; at most 60 % of a block for the capture network

struct MediaView { const a2l::Model* model = nullptr; uint32_t capGen = 0; const dsp::IrData* ir = nullptr; uint32_t irGen = 0; };

class TgMedia : public pd::IMedia {
 public:
  static constexpr size_t kStage = 8192, kCapPool = 12288;
  void attach(pd::IBlobStore* st) { st_ = st; reload(); }   // call once at boot, before audio starts (reads the flash)

  // ---- pd::IMedia
  bool begin(char kind, size_t bytes, long gain_mdb, int fs_khz) override {
    holds_ = 0; if (bytes == 0 || bytes > kStage || (kind == 'I' && (bytes & 1))) return false;
    kind_ = kind; len_ = bytes; gain_ = gain_mdb; fs_ = fs_khz; return true;
  }
  void data(size_t off, const uint8_t* d, size_t n) override { if (off + n <= len_ && off + n <= kStage) memcpy(stage_ + off, d, n); }
  int end(char kind, char* name, size_t cap, int* layers, int* channels) override {
    if (kind != kind_ || len_ == 0) return -4;
    if (kind == 'C') {
      a2l::Model& m = model_[1 - mi_]; int rc = a2l::parse(stage_, len_, m); if (rc) return rc;
      if (!fits(m)) { m.valid = false; return -100; }
      mi_ = 1 - mi_; capOn_ = true; capGen_++; holds_ = 'C';
      snprintf(name, cap, "%s", m.name); *layers = m.nLayers; *channels = m.C; return 0;
    }
    dsp::IrData& d = ir_[1 - ii_]; makeIr((const int16_t*)(const void*)stage_, (int)(len_ / 2), fs_, gain_, d, work_);
    if (d.n == 0) return -4;
    ii_ = 1 - ii_; irSlot_ = -2; irGen_++; holds_ = 'I'; return 0;
  }
  bool save(char kind, int slot) override {
    if (!st_ || holds_ != kind) return false;
    char key[8];
    if (kind == 'C') {
      if (slot < 0 || slot >= kCapSlots) return false;
      snprintf(key, sizeof key, "c%02d", slot); if (!st_->write(key, stage_, len_)) return false;
      holds_ = 0; reloadCaps(); return true;   // (the staging buffer is the scratch of reloadCaps)
    }
    if (slot < 0 || slot >= kIrSlots) return false;
    const dsp::IrData& d = ir_[ii_]; uint8_t* b = (uint8_t*)(void*)work_;
    b[0] = (uint8_t)d.n; b[1] = (uint8_t)(d.n >> 8); b[2] = 1; b[3] = 0; memcpy(b + 4, &d.gainQ12, 4); memcpy(b + 8, d.taps, (size_t)d.n * 2);
    snprintf(key, sizeof key, "i%d", slot); if (!st_->write(key, b, 8 + (size_t)d.n * 2)) return false;
    irLib_[slot] = d; irUsed_[slot] = true; irSlot_ = slot; return true;
  }
  bool select(char kind, int slot) override {
    if (kind != 'C') return true;
    if (slot < 0 || slot >= kCapSlots || !capLen_[slot]) { if (capOn_) { capOn_ = false; capGen_++; } return slot < 0; }
    a2l::Model& m = model_[1 - mi_]; if (a2l::parse(capPool_ + capOff_[slot], capLen_[slot], m) || !fits(m)) { if (capOn_) { capOn_ = false; capGen_++; } return false; }
    mi_ = 1 - mi_; capOn_ = true; capGen_++; return true;
  }
  bool erase(char kind, int slot) override {
    char key[8]; if (!st_) return false;
    if (kind == 'C') { if (slot < 0 || slot >= kCapSlots) return false; snprintf(key, sizeof key, "c%02d", slot); st_->erase(key); holds_ = 0; reloadCaps(); return true; }
    if (slot < 0 || slot >= kIrSlots) return false;
    snprintf(key, sizeof key, "i%d", slot); st_->erase(key); irUsed_[slot] = false; if (irSlot_ == slot) irSlot_ = -1; return true;
  }
  void abort() override { holds_ = 0; len_ = 0; }

  // ---- for the DSP snapshot (main loop). wantIr: library slot the first IR block asks for (-1 none); live: keep the just-uploaded IR.
  MediaView view(int wantIr, bool live) {
    MediaView v; v.capGen = capGen_; v.model = capOn_ ? &model_[mi_] : nullptr;
    if (live && irSlot_ == -2) { v.ir = &ir_[ii_]; v.irGen = irGen_; return v; }
    if (wantIr < 0 || wantIr >= kIrSlots || !irUsed_[wantIr]) return v;   // no uploaded IR there: the built-in cabinet voicing is used
    if (irSlot_ != wantIr) { ir_[1 - ii_] = irLib_[wantIr]; ii_ = 1 - ii_; irSlot_ = wantIr; irGen_++; }
    v.ir = &ir_[ii_]; v.irGen = irGen_; return v;
  }
  // Real cost on this chip: time 256 samples of the model on a scratch state (the audio ISR keeps running, so this is pessimistic).
  // Updates the measured ns per multiply-accumulate used by macLimit(). Returns false if the model would not run in real time.
  bool fits(const a2l::Model& m) {
    if (!g_ticks) return m.macs <= (uint32_t)a2l::kDefaultMacs;
    if (g_audioPause) g_audioPause(true);
    a2l::reset(m, bench_); uint32_t s = 1, t0 = g_ticks();
    for (int i = 0; i < 256; i++) { s = s * 1664525u + 1013904223u; (void)a2l::run(m, bench_, (int32_t)(s >> 9) - 4194304); }
    uint32_t us = (g_ticks() - t0) / g_ticksPerUs;   // 256 samples = 2 blocks
    if (g_audioPause) g_audioPause(false);
    lastUsPerBlock_ = us / 2; if (m.macs) nsPerMac_ = (us * 1000u / 256u) * 1000u / m.macs;   // x1000 fixed point
    return lastUsPerBlock_ <= g_capShareUs;
  }
  // MACs per sample that fit the capture share, from the last measurement (kDefaultMacs until one was made).
  uint32_t macLimit() const {
    if (!nsPerMac_) return a2l::kDefaultMacs;
    uint32_t l = (uint32_t)((uint64_t)g_capShareUs * 1000000u / 128u / nsPerMac_);
    return l > (uint32_t)a2l::kMaxMacs ? (uint32_t)a2l::kMaxMacs : l;
  }
  // diagnostics: re-time the last parsed (even if refused) capture
  uint32_t rebench() { a2l::Model& m = model_[1 - mi_]; bool v = m.valid; m.valid = true; fits(m); m.valid = v; return lastUsPerBlock_; }
  int32_t* scratch() { return bench_.arena; }   // 16384 words, free between uploads (diagnostics)
  uint32_t lastUsPerBlock() const { return lastUsPerBlock_; }
  uint32_t nsPerMacX1000() const { return nsPerMac_; }
  bool irStored(int slot) const { return slot >= 0 && slot < kIrSlots && irUsed_[slot]; }
  bool capStored(int slot) const { return slot >= 0 && slot < kCapSlots && capLen_[slot] != 0; }
  const a2l::Model* liveModel() const { return capOn_ ? &model_[mi_] : nullptr; }

 private:
  void reload() {
    for (int i = 0; i < kIrSlots; i++) {
      char key[8]; snprintf(key, sizeof key, "i%d", i); size_t n = 0; uint8_t* b = (uint8_t*)(void*)work_; irUsed_[i] = false;
      if (!st_ || !st_->read(key, b, sizeof work_, n) || n < 10) continue;
      unsigned nt = b[0] | b[1] << 8; if (b[2] != 1 || nt == 0 || nt > (unsigned)dsp::kIrMaxTaps || n != 8 + (size_t)nt * 2) continue;
      irLib_[i].n = (uint16_t)nt; memcpy(&irLib_[i].gainQ12, b + 4, 4); memcpy(irLib_[i].taps, b + 8, (size_t)nt * 2); irUsed_[i] = true;
    }
    reloadCaps();
  }
  void reloadCaps() {   // uses the staging buffer as scratch
    size_t used = 0;
    for (int i = 0; i < kCapSlots; i++) {
      char key[8]; snprintf(key, sizeof key, "c%02d", i); size_t n = 0; capLen_[i] = 0;
      if (!st_ || !st_->read(key, stage_, kStage, n) || n == 0 || used + n > kCapPool) continue;
      memcpy(capPool_ + used, stage_, n); capOff_[i] = (uint16_t)used; capLen_[i] = (uint16_t)n; used += (n + 3) & ~(size_t)3;
    }
    len_ = 0;
  }
  pd::IBlobStore* st_ = nullptr;
  a2l::State bench_; uint32_t lastUsPerBlock_ = 0, nsPerMac_ = 0;
  alignas(4) uint8_t stage_[kStage]; char kind_ = 0, holds_ = 0; size_t len_ = 0; long gain_ = 0; int fs_ = 48;
  int32_t work_[2052];
  a2l::Model model_[2]; int mi_ = 0; bool capOn_ = false; uint32_t capGen_ = 0;
  alignas(4) uint8_t capPool_[kCapPool]; uint16_t capOff_[kCapSlots] = {}, capLen_[kCapSlots] = {};
  dsp::IrData ir_[2], irLib_[kIrSlots]; bool irUsed_[kIrSlots] = {}; int ii_ = 0, irSlot_ = -1; uint32_t irGen_ = 0;
};
}  // namespace tg
