// Tank-G audio DSP (header only, integer, no heap). Runs in the ALNK0 ISR via tg_audio_process(); parameters come from the pd::Engine chain
// through AudioParams snapshots (double-buffered, written by the main loop, read by the ISR). UNTESTED ON HARDWARE.
// Samples: int32 holding signed 24 bit (Q23). Input = LEFT channel taken as the guitar [ASSUMPTION], output on both channels.
// Implemented blocks (protocol.json ids): 1 gate, 2 boost, 3 overdrive, 9 ts9, 10 ds1, 17 rat, 18 muff, 20 bd2 (all clippers share one soft-clip stage). Every other block type is a bypass
// (the shared firmware-core is protocol/state only: it has no DSP and no processBlock).
#pragma once
#include <stdint.h>
#include <string.h>
#include "pd/engine.h"
#include "pd/catalog.h"
#include "dsp_gain.h"
#include "dsp_fx.h"
#include "a2l.h"
#include "media.h"
namespace tg {
constexpr int kAuMax = 18;   // 16 chain blocks + the amp + 1 spare
enum AuMode : uint8_t { kAuThru = 0, kAuTone = 1 };
struct AudioParams {
  struct B { uint8_t type = 0, on = 0, own = 1; int16_t p[7] = {}; int32_t q[6][5] = {}; } b[kAuMax];   // own: first block of a stateful class (delay, mod line, phaser, reverb); q: biquad coefficients of amp / eq / cab / geq
  uint8_t n = 0, mode = kAuThru, bypass = 0;
  uint8_t aLo = 0, aHi = 0;   // b[aLo..aHi) = the amp (capture network or open amp); before = P effects, after = Q effects
  int32_t gin = 4096, gout = 4096;   // Q12 linear
  const a2l::Model* model = nullptr; uint32_t capGen = 0;   // live amp capture (null: the open amp stage) -- objects owned by TgMedia, complete when published
  const dsp::IrData* ir = nullptr; uint32_t irGen = 0;      // uploaded IR used by the first IR block (null: built-in cabinet voicing)
};
namespace detail {
inline int pidx(const pd::FxDef* d, const char* n) { for (int k = 0; k < d->np; k++) if (!strcmp(d->p[k].name, n)) return k; return -1; }
inline int16_t pv(const pd::Block& b, const pd::FxDef* d, const char* n, int def) { int k = pidx(d, n); return (int16_t)(k >= 0 && k < b.np ? b.p[k] : def); }
inline void ampCoefs(AudioParams::B& o, int bass10, int mid10, int treb10, bool ownCab) {
  dsp::eqCoef(kEqLow, bass10, o.q[0]); dsp::eqCoef(kEqMid, mid10, o.q[1]); dsp::eqCoef(kEqHigh, treb10, o.q[2]);
  for (int k = 0; k < 5; k++) o.q[3][k] = ownCab ? (k == 0 ? 16384 : 0) : kCabLp[k];   // the amp's fixed roll-off steps aside when an IR Cab block is active
}
}  // namespace detail
// Engine chain -> DSP snapshot. Order: bank P blocks, the amp (the preset's AMPEQ "capture" slot), bank Q blocks. Parameters of delay/mod/reverb/amp/eq blocks are
// normalised here (by catalog names) to the layouts documented in dsp_fx.h / AudioDsp::fx, so the DSP never depends on catalog positions.
inline void buildParams(const pd::Engine& e, bool toneMode, AudioParams& o, TgMedia* media = nullptr) {
  const pd::Preset& c = e.chain();
  o.n = 0;
  using namespace detail;
  bool cabOn = false;
  for (int i = 0; i < c.nb; i++) if (c.b[i].type == 7 && c.b[i].on) { const pd::FxDef* d = pd::fxById(7); if (pv(c.b[i], d, "ir_slot", 0) > 0) cabOn = true; }
  int wantIr = -1; bool irSeen = false;
  for (int i = 0; i < c.nb; i++) if (c.b[i].type == 7 && c.b[i].on) { wantIr = pv(c.b[i], pd::fxById(7), "ir_slot", 0) - 1; break; }
  MediaView mv; if (media) mv = media->view(wantIr, e.liveIr());
  o.model = (c.amp_present && c.amp_on) ? mv.model : nullptr; o.capGen = mv.capGen; o.ir = mv.ir; o.irGen = mv.irGen;
  if (mv.ir) cabOn = true;
  bool seen[8] = {};   // 0 delay, 1 mod line, 2 phaser, 3 reverb, 4 pitch, 5 rotary, 6 envelope filter, 7 compressor
  auto add = [&](const pd::Block* src) {
    if (o.n >= kAuMax) return;
    AudioParams::B& b = o.b[o.n++]; b = AudioParams::B();
    const pd::FxDef* d = pd::fxById(src->type); b.type = (uint8_t)src->type; b.on = src->on ? 1 : 0;
    for (int k = 0; k < 7; k++) b.p[k] = k < src->np ? src->p[k] : 0;
    int cls = -1;
    switch (src->type) {
      case 5: cls = 0; break;
      case 11: case 15: cls = 1; break;
      case 12: cls = 1; b.p[2] = pv(*src, d, "mezcla_pct", 50); b.p[3] = pv(*src, d, "realim_pct", 0); break;
      case 13: cls = 2; b.p[3] = pv(*src, d, "realim_pct", 0); break;
      case 4: ampCoefs(b, pv(*src, d, "graves_db", 0) * 10, pv(*src, d, "medios_db", 0) * 10, pv(*src, d, "agudos_db", 0) * 10, false); break;
      case 21: case 22: case 23: {
        int base = src->type == 21 ? 0 : src->type == 22 ? 6 : 12;
        b.p[0] = (int16_t)(base + pv(*src, d, "ganancia_pct", 40) * 30 / 100);
        int bs = pv(*src, d, "graves_pct", 50), md = pv(*src, d, "medios_pct", 50), tr = pv(*src, d, "agudos_pct", 50);
        b.p[1] = (int16_t)((bs - 50) * 12 / 5); b.p[2] = (int16_t)((md - 50) * 12 / 5); b.p[3] = (int16_t)((tr - 50) * 12 / 5);
        b.p[4] = pv(*src, d, "nivel_db", -6); b.p[5] = (int16_t)(src->type - 21);
        ampCoefs(b, b.p[1], b.p[2], b.p[3], cabOn); break;
      }
      case 7: {   // IR Cab: p0 slot (0 off, 1..8), p1 mix %, p2 level dB; q = the 4 biquads of the chosen cabinet voicing
        int sl = pv(*src, d, "ir_slot", 0); sl = sl < 0 ? 0 : sl > kCabCount ? kCabCount : sl;
        b.p[0] = (int16_t)sl; b.p[1] = pv(*src, d, "mezcla_pct", 100); b.p[2] = pv(*src, d, "nivel_db", 0);
        if (sl > 0) for (int k = 0; k < 4; k++) for (int j = 0; j < 5; j++) b.q[k][j] = kCabs[sl - 1][k][j];
        if (src->on && !irSeen) { irSeen = true; b.p[3] = mv.ir ? 1 : 0; }   // p3 = 1: this block convolves the uploaded IR instead
        break;
      }
      case 36: {   // GEQ: p0 level dB; q[0..5] = the six peaking bands
        static const char* const nm[6] = {"g50_db", "g125_db", "g315_db", "g800_db", "g2k_db", "g5k_db"};
        b.p[0] = pv(*src, d, "nivel_db", 0);
        for (int k = 0; k < 6; k++) dsp::eqCoef(kGeq[k], pv(*src, d, nm[k], 0) * 10, b.q[k]);
        break;
      }
      case 29: cls = 4; break;                                                        // pitch / octaver (catalog order is the DSP order)
      case 39: cls = 5; break;                                                        // rotary
      case 38: cls = 6; break;                                                        // envelope filter
      case 8: cls = 7; b.q[0][0] = dbToAmp(pv(*src, d, "umbral_db", -24)); break;     // compressor: threshold as an amplitude
      case 28: b.type = 1; b.p[0] = pv(*src, d, "umbral_db", -52); b.p[1] = pv(*src, d, "release_ms", 200); break;   // sgate -> the gate
      case 16: b.type = 3; b.p[0] = (int16_t)(pv(*src, d, "ganancia_pct", 40) * 30 / 100); b.p[1] = (int16_t)(1000 + pv(*src, d, "agudos_pct", 50) * 80);   // klon -> shared clipper with clean blend
        b.p[2] = pv(*src, d, "nivel_db", -6); b.p[4] = (int16_t)(100 - pv(*src, d, "mezcla_pct", 50)); break;
      case 19: b.type = 3; b.p[0] = (int16_t)(10 + pv(*src, d, "fuzz_pct", 70) * 30 / 100); b.p[1] = (int16_t)(1000 + pv(*src, d, "tono_pct", 60) * 80);   // fuzzface -> shared clipper, hotter
        b.p[2] = pv(*src, d, "nivel_db", -10); b.p[4] = pv(*src, d, "limpio_pct", 0); break;
      case 25: { cls = 1; b.type = 11; int ct = pv(*src, d, "detune_ct", 7); b.p[0] = 40; b.p[1] = (int16_t)(4 + (ct < 0 ? -ct : ct) / 2); b.p[2] = pv(*src, d, "mezcla_pct", 50); break; }   // doubler -> slow shallow chorus
      case 37: {   // bass eq -> five of the GEQ bands (100 Hz shelf, 125, 800, 2k, 5k)
        static const char* const nm[5] = {"graves_db", "bajos_db", "medios_db", "altos_db", "agudos_db"}; static const int band[5] = {0, 1, 3, 4, 5};
        b.type = 36; b.p[0] = pv(*src, d, "nivel_db", 0);
        for (int k = 0; k < 5; k++) dsp::eqCoef(kGeq[band[k]], pv(*src, d, nm[k], 0) * 10, b.q[k]);
        b.q[5][0] = 16384; break;
      }
      case 27: case 30: case 31: case 32: case 33: case 34: case 35:   // every reverb of the family runs the same algorithm; only the parameters differ
        cls = 3; b.p[0] = pv(*src, d, "decay_pct", 50); b.p[1] = pv(*src, d, "amort_pct", pv(*src, d, "tono_pct", 40));
        b.p[2] = pv(*src, d, "mezcla_pct", 30); b.p[3] = pv(*src, d, "nivel_db", 0); b.p[4] = pv(*src, d, "predelay_ms", 0); break;
      default: break;
    }
    if (cls >= 0) { b.own = seen[cls] ? 0 : 1; seen[cls] = true; }
  };
  auto addAmp = [&]() {
    if (!c.amp_present || !c.amp_on || o.n >= kAuMax) return;
    AudioParams::B& b = o.b[o.n++]; b = AudioParams::B(); b.type = 100; b.on = 1;
    b.p[0] = (int16_t)(6 + c.ampeq[5] / 10); b.p[1] = c.ampeq[1]; b.p[2] = c.ampeq[2]; b.p[3] = c.ampeq[3];
    b.p[4] = (int16_t)(c.ampeq[0] / 10); b.p[5] = 1;
    if (o.model) { b.p[0] = (int16_t)(c.ampeq[5] / 10); b.p[5] = 3; }   // a capture is loaded: GAIN = drive into the network, no clipper, no fixed roll-off
    ampCoefs(b, b.p[1], b.p[2], b.p[3], cabOn || o.model != nullptr);
  };
  for (int i = 0; i < c.nb; i++) if (c.b[i].bank != 'Q') add(&c.b[i]);
  o.aLo = o.n; addAmp(); o.aHi = o.n;
  for (int i = 0; i < c.nb; i++) if (c.b[i].bank == 'Q') add(&c.b[i]);
  o.gin = dbQ12(c.gain_in); o.gout = (int32_t)(((int64_t)dbQ12(c.gain_out) * masterQ12(e.masterRaw())) >> 12); o.bypass = e.bypassed() ? 1 : 0; o.mode = toneMode ? kAuTone : kAuThru;
}
class AudioDsp {
 public:
  void setParams(const AudioParams& p) { int n = 1 - cur_; par_[n] = p; __asm_barrier(); cur_ = n; }   // main loop only
  // Platform hook for the second core: run(fn, arg) hands a job over (true) or refuses (false: do it here); wait() waits for it.
  struct Other { bool (*run)(void (*)(uint32_t), uint32_t) = nullptr; void (*wait)() = nullptr; };
  Other other;
  bool twoCores() const { return other.run != nullptr; }
  // ISR only. One core: input -> P -> amp -> Q -> output in the same block.
  // Two cores: a three-stage pipeline (+2 blocks = 5.3 ms of latency). The second core runs Q of block n-2 and P of block n while this core
  // runs the amp (the capture network) of block n-1, so the network has this core to itself.
  void process(const int32_t* in, int32_t* out, uint32_t frames) {
    const AudioParams& p = par_[cur_];
    if (p.capGen != capSeen_) { capSeen_ = p.capGen; if (p.model) a2l::reset(*p.model, a2s_); dcX_ = dcY_ = 0; }   // new capture: clear the network history
    if (p.irGen != irSeen_) { irSeen_ = p.irGen; memset(fxs_.irH, 0, sizeof fxs_.irH); fxs_.irOutPrev = fxs_.irOutCur = 0; }
    while (frames > kBlk) { process(in, out, kBlk); in += 2 * kBlk; out += 2 * kBlk; frames -= kBlk; }
    job_ = {&p, in, frames};
    if (!other.run) {
      stageP(p, in, a_, frames); stageAmp(p, a_, frames); stageQ(p, a_, frames);
      for (uint32_t f = 0; f < frames; f++) { out[f * 2] = a_[f]; out[f * 2 + 1] = a_[f]; }
      return;
    }
    // pipeline buffers: pq_ = P output waiting for the amp, aq_ = amp output waiting for Q
    memcpy(ampIn_, pq_, sizeof pq_); memcpy(qIn_, aq_, sizeof aq_);
    self_ = this;   // (the job argument is 32 bits; the object is passed through a static)
    bool handed = other.run(&AudioDsp::otherJob, 0);
    if (!handed) otherJob(0);
    stageAmp(p, ampIn_, frames);                               // the network: this core only
    if (handed) other.wait();
    memcpy(aq_, ampIn_, sizeof aq_);
    for (uint32_t f = 0; f < frames; f++) { out[f * 2] = qIn_[f]; out[f * 2 + 1] = qIn_[f]; }
  }
  static constexpr uint32_t kBlk = 128;
 private:
  struct Job { const AudioParams* p = nullptr; const int32_t* in = nullptr; uint32_t frames = 0; };
  static inline AudioDsp* self_ = nullptr;
  Job job_; int32_t a_[kBlk] = {}, pq_[kBlk] = {}, aq_[kBlk] = {}, ampIn_[kBlk] = {}, qIn_[kBlk] = {};
  static void otherJob(uint32_t arg) {   // second core: Q of the older block, then P of the new one
    (void)arg; AudioDsp* d = self_; const AudioParams& p = *d->job_.p;
    d->stageQ(p, d->qIn_, d->job_.frames);
    d->stageP(p, d->job_.in, d->pq_, d->job_.frames);
  }
  void stageP(const AudioParams& p, const int32_t* in, int32_t* o, uint32_t frames) {
    for (uint32_t f = 0; f < frames; f++) {
      int32_t x;
      if (p.mode == kAuTone) { ph_ += 39370534u; int32_t t = (int32_t)(ph_ >> 8) - (1 << 23); if (t < 0) t = -t; x = (t - (1 << 22)) / 5; }   // 440 Hz triangle, -20 dB
      else x = sx(in[f * 2]);
      x = mulq12(x, p.gin);
      if (!p.bypass) for (int i = 0; i < p.aLo; i++) if (p.b[i].on) x = fx(p, p.b[i], i, x);
      o[f] = x;
    }
  }
  void stageAmp(const AudioParams& p, int32_t* io, uint32_t frames) {
    if (p.bypass || p.aLo >= p.aHi) return;
    for (uint32_t f = 0; f < frames; f++) { int32_t x = io[f]; for (int i = p.aLo; i < p.aHi; i++) if (p.b[i].on) x = fx(p, p.b[i], i, x); io[f] = x; }
  }
  void stageQ(const AudioParams& p, int32_t* io, uint32_t frames) {
    for (uint32_t f = 0; f < frames; f++) {
      int32_t x = io[f];
      if (!p.bypass) for (int i = p.aHi; i < p.n; i++) if (p.b[i].on) x = fx(p, p.b[i], i, x);
      x = mulq12(x, p.gout);
      io[f] = x > 8388607 ? 8388607 : (x < -8388608 ? -8388608 : x);
    }
  }
 public:

 private:
  static void __asm_barrier() { __asm__ volatile("" ::: "memory"); }
  static int32_t sx(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
  static int32_t mulq12(int32_t x, int32_t g) {   // x * g / 4096 without 64-bit: split
    int32_t hi = x >> 12, lo = x & 4095; return hi * g + ((lo * g) >> 12);
  }
  static int32_t clamp24(int32_t v) { return v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v); }
  // Clipper shared by overdrive/ts9/ds1/rat/muff/bd2. drive in dB, tone = one pole low-pass cutoff in Hz, level in dB, clean = dry mix percent.
  int32_t clip(int i, int32_t x, int driveDb, int toneHz, int levelDb, int cleanPct) {
    int32_t g = gainQ8(driveDb);                                  // Q8 pre gain (<= ~100x)
    int32_t v = ((x >> 8) * g) >> 8;                              // Q15 (|x>>8| <= 2^15, g <= 2^15 -> fits)
    v = v > 32768 ? 32768 : (v < -32768 ? -32768 : v);
    int32_t cube = ((v * v) >> 15) * v >> 15;
    int32_t y = (3 * v - cube) / 2;                               // soft clip 1.5 (v - v^3/3), max 1.0
    int32_t a = toneHz * 36 / 1000; a = a < 8 ? 8 : (a > 255 ? 255 : a);   // alpha/256 ~ 2 pi fc / fs  (44.1 kHz)
    lp_[i] += ((y - lp_[i]) * a) >> 8;
    int32_t wet = mulq12(lp_[i] << 8, dbQ12(levelDb));
    if (cleanPct <= 0) return clamp24(wet);
    return clamp24((wet * (100 - cleanPct) + x * cleanPct) / 100);
  }
  // Open amp stage: pre-gain -> soft clipper (1 stage; 2 for the hi-gain voicing) -> 3 band tone stack (RBJ biquads) -> fixed 5.2 kHz roll-off -> level. Presence is not modelled.
  int32_t amp(const AudioParams& p, const AudioParams::B& b, int i, int32_t x) {
    if (b.p[5] == 3 && p.model) {   // amp capture (A2L1 WaveNet): GAIN -> network -> DC blocker -> tone stack -> VOLUME
      x = a2l::run(*p.model, a2s_, clamp24(mulq12(x, dbQ12(b.p[0]))));
      int32_t y = x - dcX_ + dcY_ - (dcY_ >> 10); dcX_ = x; dcY_ = y; x = clamp24(y);
      for (int k = 0; k < 3; k++) x = dsp::bqRun(b.q[k], bq_[i][k], x);
      return clamp24(mulq12(x, dbQ12(b.p[4])));
    }
    int32_t g = gainQ8(b.p[0]);
    for (int st = 0; st <= (b.p[5] >= 2 ? 1 : 0); st++) {
      int32_t v = ((x >> 8) * (st ? 384 : g)) >> 8;
      v = v > 32768 ? 32768 : (v < -32768 ? -32768 : v);
      int32_t cube = ((v * v) >> 15) * v >> 15;
      x = ((3 * v - cube) / 2) << 8;
    }
    x >>= 1;   // -6 dB: a fully saturated amp peaks at half scale, leaving headroom for the tone stack and the effects after it
    for (int k = 0; k < 4; k++) x = dsp::bqRun(b.q[k], bq_[i][k], x);
    return clamp24(mulq12(x, dbQ12(b.p[4])));
  }
  int32_t fx(const AudioParams& p, const AudioParams::B& b, int i, int32_t x) {
    switch (b.type) {
      case 1: {  // gate: umbral_db (-90..0), release_ms (5..1000)
        int32_t a = x < 0 ? -x : x, thr = dbToAmp(b.p[0]);
        int sh = b.p[1] <= 20 ? 9 : b.p[1] <= 80 ? 11 : b.p[1] <= 250 ? 13 : 15;   // slower decay for longer release
        env_[i] = a > env_[i] ? a : env_[i] - (env_[i] >> sh) - 1; if (env_[i] < 0) env_[i] = 0;
        int32_t tgt = env_[i] > thr ? 32768 : 0; g_[i] += (tgt - g_[i]) >> 7;
        return (x >> 15) * g_[i] + (((x & 32767) * g_[i]) >> 15);
      }
      case 2: return clamp24(mulq12(x, dbQ12(b.p[0])));                                   // boost: ganancia_db -12..24
      case 3: return clip(i, x, b.p[0], b.p[1], b.p[2], b.p[4]);                          // overdrive: drive_db, tono_hz, nivel_db, os_n, limpio_pct
      case 9: case 20: return clip(i, x, b.p[0] * 30 / 100, 1000 + b.p[1] * 80, b.p[2], b.p[4]);   // ts9, bd2: *_pct
      case 10: case 17: return clip(i, x, b.p[0] * 40 / 100, 1000 + b.p[1] * 80, b.p[2], b.p[4]);  // ds1, rat
      case 18: return clip(i, x, b.p[0] * 40 / 100, 1000 + b.p[1] * 80, b.p[2], b.p[4]);           // muff (sustain, tono)
      case 4: x = dsp::bqRun(b.q[0], bq_[i][0], x); x = dsp::bqRun(b.q[1], bq_[i][1], x); return dsp::bqRun(b.q[2], bq_[i][2], x);   // eq: 3 bands (mid fixed at 800 Hz)
      case 7: {   // IR Cab: an uploaded impulse response (p3 = 1) or one of the built-in voicings
        int32_t y = x;
        if (b.p[3] && p.ir) y = dsp::irRun(fxs_, *p.ir, x);
        else { if (b.p[0] == 0) return x; for (int k = 0; k < 4; k++) y = dsp::bqRun(b.q[k], bq_[i][k], y); }
        y = mulq12(y, dbQ12(b.p[2]));
        return clamp24(x + dsp::mulq15((b.p[1] < 0 ? 0 : b.p[1] > 100 ? 100 : b.p[1]) * 327, y - x));
      }
      case 36: { int32_t y = x; for (int k = 0; k < 6; k++) y = dsp::bqRun(b.q[k], bq_[i][k], y); return clamp24(mulq12(y, dbQ12(b.p[0]))); }   // GEQ
      case 5: return b.own ? dsp::delayRun(fxs_, b.p, x) : x;
      case 11: case 12: case 15: return b.own ? dsp::modRun(fxs_, b.type, b.p, x) : x;
      case 13: return b.own ? dsp::phaserRun(fxs_, b.p, x) : x;
      case 14: return dsp::tremRun(lfo_[i], b.p, x);
      case 27: case 30: case 31: case 32: case 33: case 34: case 35: return b.own ? dsp::reverbRun(fxs_, b.p, dbQ12(b.p[3]), x) : x;
      case 21: case 22: case 23: case 100: return amp(p, b, i, x);
      case 29: return b.own ? clamp24(mulq12(dsp::pitchRun(fxs_, b.p, x), dbQ12(b.p[3]))) : x;
      case 39: return b.own ? dsp::rotaryRun(fxs_, b.p, x) : x;
      case 38: return b.own ? dsp::envFilterRun(fxs_, b.p, x) : x;
      case 8: return b.own ? clamp24(mulq12(dsp::compRun(fxs_, b.p, b.q[0][0], x), dbQ12(b.p[4]))) : x;
      default: return x;   // not implemented (hum): bypass
    }
  }
  dsp::FxState fxs_; dsp::BqS bq_[kAuMax][6]; uint32_t lfo_[kAuMax] = {};
  a2l::State a2s_; uint32_t capSeen_ = 0, irSeen_ = 0; int32_t dcX_ = 0, dcY_ = 0;
  AudioParams par_[2]; volatile int cur_ = 0; uint32_t ph_ = 0; int32_t env_[kAuMax] = {}, g_[kAuMax] = {}, lp_[kAuMax] = {};
};
}  // namespace tg
