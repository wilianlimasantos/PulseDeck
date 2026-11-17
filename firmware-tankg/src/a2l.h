// A2L1 capture player: the NAM "A2 Lite" WaveNet (open architecture, MIT project) in the pedal's quantised blob format.
// Written from the FORMAT description in PulseDeck's protocol.json (file_transfer.a2l1_blob_format / a2l1_quantization) and the public
// WaveNet structure; no vendor code. Integer only: int16 weights, int32 activations, int64 accumulators (pi32v2 has a native 32x32->64 multiply).
// The shift rules are derived from the documented fractional-bit fields; bit-exactness with the stock engine is NOT claimed, the sound is checked
// against a float64 forward pass of the same network (tests/test_a2l.cpp). UNTESTED ON HARDWARE.
#pragma once
#include <stdint.h>
#include <string.h>
#include "pd/crc.h"
namespace tg {
namespace a2l {
constexpr int kMaxLayers = 32, kMaxC = 4, kMaxW = 4096, kMaxB = 2 * kMaxLayers * kMaxC + 1, kArenaWords = 16384;
// CPU guard: multiply-accumulates per sample we accept. Measured from the pi32v2 code of the inner loop: 6 instructions per MAC (64-bit accumulate)
// plus ~80 per layer; at 360 MHz one 48 kHz sample has 7500 cycles. 700 MACs ~ 4.9 k instructions ~ 65 % of the core, leaving the rest to the
// effects. A stock 23-layer A2 Lite (K = 6, ~1730 MACs) does NOT fit: the editor compacts it first (PulseDeck: pulsedeck-core/src/distill.rs).
constexpr int kMaxMacs = 2400;   // parse limit; the real-time limit is MEASURED on the chip (TgMedia::macLimit, CAPS a2l_macs)
constexpr int kDefaultMacs = 700; // used when no timer is available to measure (host tests)
constexpr size_t kMaxBlob = 8192;
struct Layer {
  uint8_t K; uint16_t dil, len; int8_t convSh, mixSh, l1Sh, resSh, headSh; int32_t lmul;
  uint16_t convW, mixW, l1W, convB, l1B; uint32_t hist;
};
struct Model {
  bool valid = false; uint8_t C = 0, headK = 0; uint16_t nLayers = 0; int8_t rechSh = 0, outSh = 0, q23Sh = 0, inSh = 0;
  uint16_t rechW = 0, headW = 0, headB = 0; uint32_t headHist = 0, arenaWords = 0, macs = 0; char name[17] = {};
  Layer L[kMaxLayers]; alignas(4) int16_t w[kMaxW]; int64_t b[kMaxB];
};
struct State { int32_t arena[kArenaWords]; uint16_t pos[kMaxLayers]; uint16_t hpos; };

inline int64_t sshift(int64_t v, int s) { return s >= 0 ? v >> s : (int64_t)((uint64_t)v << (-s)); }
inline int32_t sat32(int64_t v) { return v > 2147483647LL ? 2147483647 : (v < -2147483647LL - 1 ? (int32_t)(-2147483647LL - 1) : (int32_t)v); }

// 0 ok, -4 rejected (not a valid A2L1 blob), -100 too big / too heavy for this pedal.
inline int parse(const uint8_t* d, size_t n, Model& m) {
  m.valid = false;
  if (n < 36 + 4 || n > kMaxBlob || memcmp(d, "A2L1", 4)) return -4;
  if (pd::crc32(d, n - 4) != (uint32_t)(d[n - 4] | d[n - 3] << 8 | (uint32_t)d[n - 2] << 16 | (uint32_t)d[n - 1] << 24)) return -4;
  size_t o = 0, end = n - 4; bool bad = false;
  auto u8 = [&]() -> uint32_t { if (o + 1 > end) { bad = true; return 0; } return d[o++]; };
  auto i8 = [&]() -> int { return (int8_t)u8(); };
  auto u16 = [&]() -> uint32_t { uint32_t a = u8(); return a | u8() << 8; };
  auto u32 = [&]() -> uint32_t { uint32_t a = u16(); return a | u16() << 16; };
  auto i64 = [&]() -> int64_t { uint64_t a = u32(); return (int64_t)(a | (uint64_t)u32() << 32); };
  o = 4;
  if (u16() != 1 || u8() != 16) return -4;
  if (u8() != 32) return -4;                       // 16-bit activations: not supported by this player
  if (u32() != 48000) return -4;
  uint32_t nl = u16(), C = u8(), hk = u8();
  int Fin = i8(), Fx30 = i8(), Fh = i8(), Fout = i8();
  memcpy(m.name, d + 20, 16); m.name[16] = 0; o = 36;
  if (nl < 1 || C < 1 || C > 16 || hk < 1 || hk > 64) return -4;
  if (nl > (uint32_t)kMaxLayers || C > (uint32_t)kMaxC) return -100;
  uint32_t wi = 0, bi = 0, arena = 0, macs = 0;
  auto takeW = [&](uint32_t cnt) -> int { wi += wi & 1; if (wi + cnt > (uint32_t)kMaxW) { bad = true; return 0; } uint32_t at = wi; for (uint32_t k = 0; k < cnt; k++) m.w[wi++] = (int16_t)u16(); return (int)at; };
  auto takeB = [&](uint32_t cnt) -> int { if (bi + cnt > (uint32_t)kMaxB) { bad = true; return 0; } uint32_t at = bi; for (uint32_t k = 0; k < cnt; k++) m.b[bi++] = i64(); return (int)at; };
  auto ok62 = [](int s) { return s >= -62 && s <= 62; };
  int rF = i8(); m.rechW = (uint16_t)takeW(C);
  int sh = rF + Fin - Fx30; if (!ok62(sh)) return -4; m.rechSh = (int8_t)sh;
  int Fp = Fx30;
  for (uint32_t i = 0; i < nl && !bad; i++) {
    Layer& L = m.L[i];
    uint32_t K = u8(), dil = u16(); int Fz = i8(), Fx3 = i8(); L.lmul = (int32_t)u32(); int cF = i8();
    if (K < 1 || K > 15 || dil < 1) return -4;
    L.K = (uint8_t)K; L.dil = (uint16_t)dil;
    uint32_t len = (K - 1) * dil + 1; if (len > 65535u) return -100; L.len = (uint16_t)len;
    L.convW = (uint16_t)takeW(C * C * K); L.convB = (uint16_t)takeB(C);
    if (!bad) {   // file order [co][ci][k] -> run order [co][k][ci] (the order the history is walked)
      int16_t tmp[15 * kMaxC];
      for (uint32_t co = 0; co < C; co++) { int16_t* W = &m.w[L.convW + co * C * K]; for (uint32_t ci = 0; ci < C; ci++) for (uint32_t k = 0; k < K; k++) tmp[k * C + ci] = W[ci * K + k]; memcpy(W, tmp, C * K * 2); }
    }
    int mF = i8(); L.mixW = (uint16_t)takeW(C);
    int lF = i8(); L.l1W = (uint16_t)takeW(C * C); L.l1B = (uint16_t)takeB(C);
    int cs = cF + Fp - Fz, ms = mF + Fin - Fz, ls = lF + Fz - Fx3, rs = Fp - Fx3, hs = Fz - Fh;
    if (cs < 0 || cs > 31 || !ok62(ms) || !ok62(ls) || !ok62(rs) || !ok62(hs)) return -4;
    L.convSh = (int8_t)cs; L.mixSh = (int8_t)ms; L.l1Sh = (int8_t)ls; L.resSh = (int8_t)rs; L.headSh = (int8_t)hs;
    L.hist = arena; arena += 2 * len * C; macs += C * C * K + C + C * C;   // history: [t][channel], written twice (mirror) so taps are read with a constant stride
    if (arena > (uint32_t)kArenaWords) return -100;
    Fp = Fx3;
  }
  int hF = i8(); m.headW = (uint16_t)takeW(C * hk); m.headB = (uint16_t)takeB(1);
  if (bad) return wi + 1 > (uint32_t)kMaxW || bi + 1 > (uint32_t)kMaxB ? -100 : -4;
  if (o != end) return -4;
  int os = hF + Fh - Fout; if (os < 0 || os > 31) return -4;
  m.outSh = (int8_t)os; m.q23Sh = (int8_t)(Fout - 23); m.inSh = (int8_t)(23 - Fin);
  m.headHist = arena; arena += 2 * hk * C; macs += C * hk;
  if (arena > (uint32_t)kArenaWords || macs > (uint32_t)kMaxMacs) return -100;
  m.C = (uint8_t)C; m.headK = (uint8_t)hk; m.nLayers = (uint16_t)nl; m.arenaWords = arena; m.macs = macs; m.valid = true;
  return 0;
}
inline void reset(const Model& m, State& s) { memset(s.arena, 0, (size_t)m.arenaWords * 4); memset(s.pos, 0, sizeof s.pos); s.hpos = 0; }

// One sample, Q23 in -> output in the model's own Fout format (Fout = q23Sh + 23 fractional bits).
__attribute__((always_inline)) inline int64_t runRaw(const Model& m, State& s, int32_t x) {
  const int C = m.C;
  const int64_t xin = sshift((int64_t)x, m.inSh);   // input in the model's Fin format
  int32_t cur[kMaxC], a[kMaxC], nx[kMaxC], head[kMaxC];
  for (int c = 0; c < C; c++) { cur[c] = sat32(sshift((int64_t)m.w[m.rechW + c] * xin, m.rechSh)); head[c] = 0; }
  for (int i = 0; i < m.nLayers; i++) {
    const Layer& L = m.L[i];
    int32_t* H = s.arena + L.hist; const int len = L.len, K = L.K, stride = L.dil * C; const int p = s.pos[i];
    int32_t* cell = H + p * C; int32_t* mirror = cell + len * C;
    for (int c = 0; c < C; c++) { cell[c] = cur[c]; mirror[c] = cur[c]; }
    const int32_t* oldest = mirror - (K - 1) * stride;   // newest sample is at `mirror`; (K-1)*dil = len-1 samples back never leaves the buffer
    if (C == 3 && K == 3) {   // fast path (the compact students and most Lite layers): 9 history values loaded once, 27 + 9 MACs straight-line, no inner branches
      const int32_t h0 = oldest[0], h1 = oldest[1], h2 = oldest[2], h3 = oldest[stride], h4 = oldest[stride + 1], h5 = oldest[stride + 2],
                    h6 = oldest[2 * stride], h7 = oldest[2 * stride + 1], h8 = oldest[2 * stride + 2];
      const int16_t* w = &m.w[L.convW]; const int64_t* b = &m.b[L.convB]; const int ms = L.mixSh, cs = L.convSh;
      const int64_t mx = sshift((int64_t)m.w[L.mixW] * xin, ms), my = sshift((int64_t)m.w[L.mixW + 1] * xin, ms), mz = sshift((int64_t)m.w[L.mixW + 2] * xin, ms);
      // the 27 weights are read as 14 32-bit words (two int16 each, little endian) instead of 27 halfword loads: main RAM costs ~35 ns a read
      const uint32_t* wp = (const uint32_t*)(const void*)w; uint32_t q[14]; for (int j = 0; j < 14; j++) q[j] = wp[j];
      auto W = [&](int i) -> int32_t { uint32_t v = q[i >> 1]; return (int32_t)(int16_t)(i & 1 ? v >> 16 : v & 0xFFFFu); };
#define TG_A2L_DOT9(o) (b[o] + (int64_t)W(o*9+0)*h0 + (int64_t)W(o*9+1)*h1 + (int64_t)W(o*9+2)*h2 + (int64_t)W(o*9+3)*h3 + (int64_t)W(o*9+4)*h4 + (int64_t)W(o*9+5)*h5 + (int64_t)W(o*9+6)*h6 + (int64_t)W(o*9+7)*h7 + (int64_t)W(o*9+8)*h8)
      int32_t z0 = sat32((TG_A2L_DOT9(0) >> cs) + mx), z1 = sat32((TG_A2L_DOT9(1) >> cs) + my), z2 = sat32((TG_A2L_DOT9(2) >> cs) + mz);
#undef TG_A2L_DOT9
      a[0] = z0 >= 0 ? z0 : (int32_t)(((int64_t)z0 * L.lmul) >> 24); a[1] = z1 >= 0 ? z1 : (int32_t)(((int64_t)z1 * L.lmul) >> 24); a[2] = z2 >= 0 ? z2 : (int32_t)(((int64_t)z2 * L.lmul) >> 24);
    } else
    for (int co = 0; co < C; co++) {
      int64_t acc = m.b[L.convB + co]; const int16_t* w = &m.w[L.convW + co * C * K]; const int32_t* h = oldest;
      if (C == 3) for (int k = 0; k < K; k++) { acc += (int64_t)w[0] * h[0] + (int64_t)w[1] * h[1] + (int64_t)w[2] * h[2]; w += 3; h += stride; }
      else for (int k = 0; k < K; k++) { for (int ci = 0; ci < C; ci++) acc += (int64_t)w[ci] * h[ci]; w += C; h += stride; }
      int32_t z = sat32((acc >> L.convSh) + sshift((int64_t)m.w[L.mixW + co] * xin, L.mixSh));
      a[co] = z >= 0 ? z : (int32_t)(((int64_t)z * L.lmul) >> 24);   // LeakyReLU
    }
    if (C == 3) {
      const int16_t* W = &m.w[L.l1W]; const int64_t* B = &m.b[L.l1B]; const int ls = L.l1Sh, rs = L.resSh;
      for (int co = 0; co < 3; co++) nx[co] = sat32(sshift(B[co] + (int64_t)W[co * 3] * a[0] + (int64_t)W[co * 3 + 1] * a[1] + (int64_t)W[co * 3 + 2] * a[2], ls) + sshift((int64_t)cur[co], rs));
    } else
    for (int co = 0; co < C; co++) {
      int64_t acc = m.b[L.l1B + co]; const int16_t* W = &m.w[L.l1W + co * C];
      for (int ci = 0; ci < C; ci++) acc += (int64_t)W[ci] * a[ci];
      nx[co] = sat32(sshift(acc, L.l1Sh) + sshift((int64_t)cur[co], L.resSh));
    }
    for (int c = 0; c < C; c++) { head[c] = sat32((int64_t)head[c] + sshift((int64_t)a[c], L.headSh)); cur[c] = nx[c]; }
    s.pos[i] = (uint16_t)(p + 1 == len ? 0 : p + 1);
  }
  int32_t* HH = s.arena + m.headHist; const int hk = m.headK, hp = s.hpos;
  int64_t acc = m.b[m.headB];
  for (int c = 0; c < C; c++) {   // per channel 2*hk words, mirrored: the hk newest values are contiguous (oldest first) at h + hp + 1
    int32_t* h = HH + c * 2 * hk; h[hp] = head[c]; h[hp + hk] = head[c];
    const int16_t* wk = &m.w[m.headW + c * hk]; const int32_t* src = h + hp + 1;
    for (int k = 0; k < hk; k++) acc += (int64_t)wk[k] * src[k];
  }
  s.hpos = (uint16_t)(hp + 1 == hk ? 0 : hp + 1);
  return acc >> m.outSh;
}
// One sample, Q23 in -> Q23 out (clipped at full scale).
// Platform hook: the firmware points this at a copy of runRaw placed in RAM (no flash/XIP fetches in the hot loop). Default: runRaw itself.
using RunFn = int64_t (*)(const Model&, State&, int32_t);
inline int64_t runFlash(const Model& m, State& s, int32_t x) { return runRaw(m, s, x); }
inline RunFn g_run = runFlash;
inline int32_t run(const Model& m, State& s, int32_t x) {
  int64_t y = sshift(g_run(m, s, x), m.q23Sh);
  return y > 8388607 ? 8388607 : (y < -8388608 ? -8388608 : (int32_t)y);
}
}  // namespace a2l
}  // namespace tg
