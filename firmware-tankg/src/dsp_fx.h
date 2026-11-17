// Open DSP building blocks for the Tank-G (clean room: textbook algorithms only -- RBJ biquads, Schroeder/Freeverb-style reverb with the
// public Jezar tunings scaled to 48 kHz, LFO-modulated delay lines, feedback delay, all-pass phaser; no vendor/disassembly code).
// Integer only (no libm, no 64-bit multiplies in the sample loop), no heap: the state lives in tg::FxState (static, ~62 KB RAM).
// Samples are Q23 in int32 (as in audio.h); delay lines store 16 bit (sample >> 8). UNTESTED ON HARDWARE.
#pragma once
#include <stdint.h>
#include <string.h>
#include "eq_tables.h"
namespace tg {
namespace dsp {
inline int32_t clamp24(int32_t v) { return v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v); }
inline int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
// c (Q14, |c| < 2^17) * x (24 bit) >> 14, exact enough, 32 bit only
inline int32_t mulq14(int32_t c, int32_t x) { return c * (x >> 14) + ((c * (x & 16383) + 8192) >> 14); }   // rounded: a truncation bias would be amplified ~100x by the low-shelf poles
// Q15 gain (0..32768) * x (24 bit)
inline int32_t mulq15(int32_t g, int32_t x) { return g * (x >> 15) + ((g * (x & 32767)) >> 15); }
inline int32_t mulq12(int32_t g, int32_t x) { return g * (x >> 12) + ((g * (x & 4095)) >> 12); }
// sine from a 32 bit phase: parabola 4x(1-|x|) -> Q15 (-32768..32768), max error ~ 6 %, THD ~ 2 % (fine for an LFO)
inline int32_t lfoSin(uint32_t ph) { int32_t x = (int32_t)ph >> 16; int32_t a = 32768 - (x < 0 ? -x : x); return (x * a) >> 13; }
inline uint32_t lfoInc(int cHz) { return (uint32_t)cHz * 895u; }   // centi-Hz -> phase step at 48 kHz (2^32 / (100 * 48000) = 894.78)

struct BqS { int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0; };   // filter state; the coefficients travel in the parameter snapshot
inline int32_t bqRun(const int32_t* c, BqS& s, int32_t x) {
  int32_t y = mulq14(c[0], x) + mulq14(c[1], s.x1) + mulq14(c[2], s.x2) - mulq14(c[3], s.y1) - mulq14(c[4], s.y2);
  y = clamp24(y); s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y; return y;
}
// table lookup with linear interpolation of the coefficients; gain10 = dB x 10 (-120..120)
inline void eqCoef(const int32_t (*t)[5], int gain10, int32_t* out) {
  int u = (gain10 < -120 ? -120 : gain10 > 120 ? 120 : gain10) + 120, i = u / 20, f = u % 20;
  if (i >= kEqSteps - 1) { i = kEqSteps - 2; f = 20; }
  for (int k = 0; k < 5; k++) out[k] = t[i][k] + (t[i + 1][k] - t[i][k]) * f / 20;
}

constexpr int kDlyLen = 12288;                 // 24 kHz samples = 512 ms
constexpr int kModLen = 2048;                  // 48 kHz samples = 42.6 ms (chorus / flanger / vibrato)
constexpr int kPreLen = 2400;                  // reverb pre-delay 50 ms
constexpr int kCombLen[8] = {1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760};   // Freeverb comb tunings x 48/44.1
constexpr int kApLen[4] = {605, 480, 371, 245};
constexpr int kIrMaxTaps = 512;                // uploaded IR: 512 taps at 24 kHz = 21.3 ms
constexpr int kPitLen = 2048;                  // pitch shifter line, 42 ms
struct IrData { int16_t taps[kIrMaxTaps]; uint16_t n = 0; int32_t gainQ12 = 4096; };   // taps Q15 at 24 kHz, see tg::makeIr (media.h)
constexpr int kRevShift = 10;   // wet makeup (tuned in tests/test_dsp.cpp: tail level ~ input level at 100 % mix)
constexpr int kRevTotal = 1215 + 1293 + 1390 + 1476 + 1548 + 1623 + 1695 + 1760 + 605 + 480 + 371 + 245;

struct FxState {
  int16_t dly[kDlyLen]; uint16_t dWp = 0, dCur = 2, dSlew = 0; uint8_t dPh = 0; int32_t dWetPrev = 0, dWetCur = 0, dIn = 0, dLp = 0;
  int16_t mod[kModLen]; uint16_t mWp = 0; uint32_t mPh = 0;
  int16_t rev[kRevTotal]; uint16_t rIdx[12] = {}; int32_t rFl[8] = {}; int16_t pre[kPreLen]; uint16_t pWp = 0;
  int32_t phZ[4] = {}, phFb = 0; uint32_t phPh = 0;
  int32_t irH[2 * kIrMaxTaps]; uint16_t irW = 0; uint8_t irPh = 0; int32_t irPrevX = 0, irOutPrev = 0, irOutCur = 0;          // uploaded IR (FIR at 24 kHz)
  int16_t pit[kPitLen]; uint16_t pitW = 0; uint32_t pitPh = 0;                                                             // pitch shifter
  int16_t rot[256]; uint8_t rotW = 0; uint32_t rotPhH = 0, rotPhD = 0; int32_t rotLp = 0, rotRate = 80 << 8;                 // rotary
  int32_t efEnv = 0, efLp = 0, efBp = 0;                                                                                   // envelope filter
  int32_t cpEnv = 0, cpG = 32768;                                                                                          // compressor
  FxState() { memset(irH, 0, sizeof irH); memset(pit, 0, sizeof pit); memset(rot, 0, sizeof rot); memset(dly, 0, sizeof dly); memset(mod, 0, sizeof mod); memset(rev, 0, sizeof rev); memset(pre, 0, sizeof pre); }
};

// ---- feedback delay at 24 kHz. p0 ms, p1 feedback %, p2 mix %, p3 damping %. The wet path is low-passed by the half rate and the damping.
inline int32_t delayRun(FxState& s, const int16_t* p, int32_t x) {
  if (s.dPh == 0) {
    int tgt = p[0] * 24; tgt = tgt < 2 ? 2 : tgt > kDlyLen - 2 ? kDlyLen - 2 : tgt;
    int dd = tgt - (int)s.dCur;   // small changes glide (tape-style, no clicks); a big jump (new preset) is taken at once, the old echoes are stale anyway
    if (dd > 1200 || dd < -1200) s.dCur = (uint16_t)tgt; else if ((++s.dSlew & 1) == 0) { if (dd > 0) s.dCur++; else if (dd < 0) s.dCur--; }
    int32_t rd = (int32_t)s.dly[(s.dWp + kDlyLen - s.dCur) % kDlyLen] << 8;
    s.dWetPrev = s.dWetCur; s.dWetCur = rd;
    int k = 32768 - p[3] * 300; k = k < 3000 ? 3000 : k;
    s.dLp += mulq15(k, rd - s.dLp);
    s.dly[s.dWp] = clamp16(((s.dIn + x) / 2 + mulq15(p[1] * 327, s.dLp)) >> 8);
    s.dWp = (uint16_t)((s.dWp + 1) % kDlyLen);
    s.dPh = 1;
    return clamp24(x + mulq15(p[2] * 327, s.dWetPrev));
  }
  s.dIn = x; s.dPh = 0;
  return clamp24(x + mulq15(p[2] * 327, (s.dWetPrev + s.dWetCur) >> 1));
}

// ---- modulated delay line: chorus (11), flanger (12), vibrato (15). p0 rate cHz, p1 depth %, p2 mix %, p3 feedback % (flanger, signed)
inline int32_t modRead(const FxState& s, int32_t dQ8) {   // delay in samples x256, >= 256
  int32_t di = dQ8 >> 8, fr = dQ8 & 255;
  int32_t a = s.mod[(s.mWp + kModLen - di) & (kModLen - 1)], b = s.mod[(s.mWp + kModLen - di - 1) & (kModLen - 1)];
  return (a + (((b - a) * fr) >> 8)) << 8;
}
inline int32_t modRun(FxState& s, uint8_t type, const int16_t* p, int32_t x) {
  s.mPh += lfoInc(p[0]);
  int32_t l1 = lfoSin(s.mPh), l2 = lfoSin(s.mPh + 0x80000000u), wet;
  int32_t dep = p[1] < 0 ? 0 : p[1] > 100 ? 100 : p[1];
  if (type == 12) {   // flanger: 2.5 ms +- 2 ms x depth
    wet = modRead(s, 120 * 256 + ((l1 * (100 * dep / 100)) >> 7));
    s.mod[s.mWp] = clamp16((x + mulq15(p[3] * 327, wet)) >> 8);
    s.mWp = (s.mWp + 1) & (kModLen - 1);
    return mulq15((100 - p[2]) * 327, x) + mulq15(p[2] * 327, wet);
  }
  s.mod[s.mWp] = clamp16(x >> 8);
  if (type == 15) {   // vibrato: 100 % wet, 3 ms +- 2 ms x depth
    wet = modRead(s, 144 * 256 + ((l1 * (96 * dep / 100)) >> 7));
    s.mWp = (s.mWp + 1) & (kModLen - 1);
    return wet;
  }
  int32_t sw = 384 * dep / 100;   // chorus: 15 ms +- 8 ms x depth, two voices in antiphase
  wet = (modRead(s, 720 * 256 + ((l1 * sw) >> 7)) + modRead(s, 720 * 256 + ((l2 * sw) >> 7))) >> 1;
  s.mWp = (s.mWp + 1) & (kModLen - 1);
  return mulq15((100 - p[2]) * 327, x) + mulq15(p[2] * 327, wet);
}
// ---- tremolo (14): p0 rate cHz, p1 depth %, p2 shape % (0 sine .. 100 squarer)
inline int32_t tremRun(uint32_t& ph, const int16_t* p, int32_t x) {
  ph += lfoInc(p[0]);
  int32_t v = lfoSin(ph) * (100 + p[2] * 5) / 100; v = v > 32768 ? 32768 : (v < -32768 ? -32768 : v);
  int32_t g = 32768 - mulq15(p[1] * 327, 32768 - ((v + 32768) >> 1));
  return mulq15(g, x);
}
// ---- phaser (13): 4 first-order all-passes swept by an LFO. p0 rate cHz, p1 depth %, p3 feedback %
inline int32_t phaserRun(FxState& s, const int16_t* p, int32_t x) {
  s.phPh += lfoInc(p[0]);
  int32_t u = (lfoSin(s.phPh) + 32768) >> 1, g = 9830 + ((u * (180 * p[1])) >> 15);   // 0.30 .. 0.85 at full depth
  int32_t y = clamp24(x + mulq15(p[3] * 327, s.phFb));
  for (int i = 0; i < 4; i++) { int32_t o = s.phZ[i] - mulq15(g, y); s.phZ[i] = y + mulq15(g, o); y = o; }
  s.phFb = y;
  return (x + y) >> 1;
}
// ---- reverb: 8 damped combs + 4 all-passes (Schroeder/Freeverb topology). p0 decay %, p1 damping %, p2 mix %, p3 level dB, p4 pre-delay ms
inline int32_t reverbRun(FxState& s, const int16_t* p, int32_t levelQ12, int32_t x) {
  int32_t xin = x >> 8;
  if (p[4] > 0) {
    int d = p[4] * 48; d = d >= kPreLen ? kPreLen - 1 : d;
    s.pre[s.pWp] = clamp16(xin); int32_t r = s.pre[(s.pWp + kPreLen - d) % kPreLen]; s.pWp = (uint16_t)((s.pWp + 1) % kPreLen); xin = r;
  }
  int32_t inn = (xin * 983) >> 15;
  int32_t fb = 23592 + 85 * (p[0] < 0 ? 0 : p[0] > 100 ? 100 : p[0]), dmp = 1600 + 240 * (p[1] < 0 ? 0 : p[1] > 100 ? 100 : p[1]), sum = 0;
  int off = 0;
  for (int i = 0; i < 8; i++) {
    int16_t& cell = s.rev[off + s.rIdx[i]];
    int32_t o = cell;
    s.rFl[i] = (o * (32768 - dmp) + s.rFl[i] * dmp + 16384) >> 15;
    cell = clamp16(inn + ((s.rFl[i] * fb + 16384) >> 15));   // rounded: truncation would leave a DC residue in the tail
    s.rIdx[i] = (uint16_t)(s.rIdx[i] + 1 == kCombLen[i] ? 0 : s.rIdx[i] + 1);
    sum += o; off += kCombLen[i];
  }
  for (int i = 0; i < 4; i++) {
    int16_t& cell = s.rev[off + s.rIdx[8 + i]];
    int32_t bo = cell;
    cell = clamp16(sum + (bo >> 1));
    sum = bo - sum;
    s.rIdx[8 + i] = (uint16_t)(s.rIdx[8 + i] + 1 == kApLen[i] ? 0 : s.rIdx[8 + i] + 1);
    off += kApLen[i];
  }
  int32_t wet = mulq12(levelQ12, clamp24(sum << kRevShift));
  return clamp24(x + mulq15(p[2] * 327, wet));
}
// ---- uploaded impulse response: FIR at 24 kHz (input pair-averaged, output linearly interpolated; the taps are pre-emphasised at load time to
// make up for both). 512 taps = 256 multiply-accumulates per 48 kHz sample.
inline int32_t irRun(FxState& s, const IrData& d, int32_t x) {
  if (s.irPh) { s.irPh = 0; s.irPrevX = x; return (s.irOutPrev + s.irOutCur) >> 1; }
  s.irPh = 1;
  int32_t in = (s.irPrevX + x) >> 1;
  s.irH[s.irW] = in; s.irH[s.irW + kIrMaxTaps] = in;
  const int32_t* h = &s.irH[s.irW + kIrMaxTaps]; const int16_t* t = d.taps; int64_t acc = 0;
  for (int k = 0, n = d.n; k < n; k++) acc += (int64_t)t[k] * h[-k];
  s.irW = (uint16_t)(s.irW + 1 == kIrMaxTaps ? 0 : s.irW + 1);
  int64_t y = ((acc >> 15) * d.gainQ12) >> 12;
  s.irOutPrev = s.irOutCur; s.irOutCur = y > 8388607 ? 8388607 : (y < -8388608 ? -8388608 : (int32_t)y);
  return s.irOutPrev;
}
// ---- pitch shifter / octaver (29): two read taps gliding through a delay window, cross-faded with a triangle so the tap that jumps is silent
// (the classic delay-line pitch shifter). p0 semitones -12..12, p1 cents, p2 mix %, p4 window ms. ratio 0.5 = octave down, 2 = octave up.
inline uint32_t pitchRatioQ16(int st, int cents) {
  static const uint32_t t[13] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072};
  st = st < -12 ? -12 : st > 12 ? 12 : st;
  uint32_t r = st >= 0 ? t[st] : t[12 + st] >> 1;
  return (uint32_t)((int64_t)r + (int64_t)r * cents * 38 / 65536);   // ln(2)/1200 per cent
}
inline int32_t pitchRun(FxState& s, const int16_t* p, int32_t x) {
  int W = p[4] * 48; W = W < 480 ? 480 : W > kPitLen - 16 ? kPitLen - 16 : W;
  int32_t inc = (int32_t)(((int64_t)65536 - (int64_t)pitchRatioQ16(p[0], p[1])) * 65536 / W);   // delay slope (1 - ratio) per sample, as phase / window
  s.pit[s.pitW] = clamp16(x >> 8);
  s.pitPh += (uint32_t)inc;
  int32_t wet = 0;
  for (int v = 0; v < 2; v++) {
    uint32_t ph = s.pitPh + (v ? 0x80000000u : 0u);
    int32_t dQ8 = (int32_t)(((uint64_t)(ph >> 8) * (uint32_t)W) >> 16) + 512;       // delay in samples x256 (+2 samples)
    int32_t tri = (int32_t)(ph >> 16); tri = tri < 32768 ? tri : 65535 - tri;         // 0..32767..0: zero where the tap wraps
    int32_t di = dQ8 >> 8, fr = dQ8 & 255;
    int32_t a = s.pit[(s.pitW + kPitLen - di) & (kPitLen - 1)], b = s.pit[(s.pitW + kPitLen - di - 1) & (kPitLen - 1)];
    wet += ((a + (((b - a) * fr) >> 8)) * tri) >> 7;                                // back to Q23, weight tri/32768 (the two weights add up to 1)
  }
  s.pitW = (s.pitW + 1) & (kPitLen - 1);
  int mix = p[2] < 0 ? 0 : p[2] > 100 ? 100 : p[2];
  return clamp24(mulq15((100 - mix) * 327, x) + mulq15(mix * 327, clamp24(wet)));
}
// ---- rotary speaker (39): 800 Hz crossover, horn = Doppler (tiny modulated delay) + tremolo, drum = slow tremolo, speed glides (inertia).
// p0 speed 0 slow / 1 fast / 2 brake, p2 fast rate cHz, p3 inertia %, p4 horn level %, p6 mix %. Drive and distance are not modelled.
inline int32_t rotaryRun(FxState& s, const int16_t* p, int32_t x) {
  int32_t tgt = (p[0] == 1 ? p[2] : p[0] == 2 ? 0 : 80) << 8;
  s.rotRate += (tgt - s.rotRate) >> (p[3] > 100 ? 15 : p[3] > 30 ? 14 : 13);
  s.rotPhH += lfoInc(s.rotRate >> 8); s.rotPhD += lfoInc((s.rotRate >> 8) * 85 / 100);
  s.rotLp += mulq15(3430, x - s.rotLp);
  int32_t low = s.rotLp, high = x - s.rotLp, lh = lfoSin(s.rotPhH), lc = lfoSin(s.rotPhH + 0x40000000u), ld = lfoSin(s.rotPhD);
  s.rot[s.rotW] = clamp16(high >> 8);
  int32_t dQ8 = 24 * 256 + ((lh * 16) >> 7), di = dQ8 >> 8, fr = dQ8 & 255;
  int32_t a = s.rot[(uint8_t)(s.rotW - di)], b = s.rot[(uint8_t)(s.rotW - di - 1)];
  s.rotW++;
  int32_t horn = mulq15(24576 + (lc >> 2), (a + (((b - a) * fr) >> 8)) << 8);       // 0.5 .. 1.0
  int32_t drum = mulq15(27853 + ((ld * 4915) >> 15), low);                           // 0.7 .. 1.0
  int hl = p[4] < 0 ? 0 : p[4] > 100 ? 100 : p[4], mix = p[6] < 0 ? 0 : p[6] > 100 ? 100 : p[6];
  int32_t wet = clamp24(drum + mulq15(hl * 655 > 32767 ? 32767 : hl * 655, horn) * 1);   // horn level: 50 % = unity
  return clamp24(mulq15((100 - mix) * 327, x) + mulq15(mix * 327, wet));
}
// ---- envelope filter / auto-wah (38): Chamberlin state-variable filter, cutoff driven by the input envelope.
// p0 sensitivity %, p1 f low Hz, p2 f high Hz, p3 resonance %, p4 decay %, p5 mode (0 low-pass, 2 high-pass, else band-pass), p6 mix %
inline int32_t envFilterRun(FxState& s, const int16_t* p, int32_t x) {
  int32_t a = x < 0 ? -x : x;
  if (a > s.efEnv) s.efEnv += (a - s.efEnv) >> 5; else s.efEnv -= s.efEnv >> (9 + (p[4] < 0 ? 0 : p[4] > 100 ? 4 : p[4] / 25));
  int64_t e = (int64_t)s.efEnv * (1 + p[0] * 31 / 100); if (e > 8388607) e = 8388607;
  int32_t fc = p[1] + (int32_t)(((int64_t)(p[2] - p[1]) * e) >> 23); fc = fc < 40 ? 40 : fc > 6500 ? 6500 : fc;
  int32_t f = fc * 4290 / 1000, q = 52428 - (p[3] < 0 ? 0 : p[3] > 100 ? 100 : p[3]) * 475;   // f = 2 pi fc / fs (Q15), q = 1.6 .. 0.15 (Q15)
  int32_t hp = clamp24(x - s.efLp - (int32_t)(((int64_t)q * s.efBp) >> 15));
  s.efBp = clamp24(s.efBp + (int32_t)(((int64_t)f * hp) >> 15));
  s.efLp = clamp24(s.efLp + (int32_t)(((int64_t)f * s.efBp) >> 15));
  int32_t wet = p[5] == 0 ? s.efLp : p[5] == 2 ? hp : s.efBp;
  int mix = p[6] < 0 ? 0 : p[6] > 100 ? 100 : p[6];
  return clamp24(mulq15((100 - mix) * 327, x) + mulq15(mix * 327, wet));
}
// ---- compressor (8): peak follower, gain = (thr + (env - thr) / ratio) / env above the threshold (ratio on the amplitude), smoothed.
// thr = threshold amplitude (Q23), p1 ratio x10, p2 attack ms, p3 release ms, p5 mix %; make-up is applied by the caller.
inline int32_t compRun(FxState& s, const int16_t* p, int32_t thr, int32_t x) {
  int32_t a = x < 0 ? -x : x;
  s.cpEnv = a > s.cpEnv ? a : s.cpEnv - (s.cpEnv >> 10);
  int32_t g = 32768;
  if (s.cpEnv > thr && s.cpEnv > 256) {
    int r = p[1] < 10 ? 10 : p[1];
    int32_t num = thr + (int32_t)(((int64_t)(s.cpEnv - thr) * 10) / r);
    g = (int32_t)(((int64_t)num << 15) / s.cpEnv);
  }
  int sh = g < s.cpG ? (p[2] <= 5 ? 5 : p[2] <= 20 ? 7 : 9) : (p[3] <= 80 ? 10 : p[3] <= 300 ? 12 : 14);
  s.cpG += (g - s.cpG) >> sh;
  int mix = p[5] < 0 ? 0 : p[5] > 100 ? 100 : p[5];
  return mulq15((100 - mix) * 327, x) + mulq15(mix * 327, mulq15(s.cpG, x));
}
}  // namespace dsp
}  // namespace tg
