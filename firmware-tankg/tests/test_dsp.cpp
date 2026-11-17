// Real-audio tests of the open DSP blocks (src/dsp_fx.h + src/audio.h): every block must change the signal, behave as its parameters say,
// stay stable, and the factory presets must be distinct and inside the engine's CPU/RAM budget. Host-side: it proves behaviour, NOT cycles on the chip.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>
#include <string>
#include "pd/engine.h"
#include "ram_store.h"
#include "audio.h"
static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
struct Null : pd::ILineSink { void line(const char*) override {} };
static const double kPi = 3.14159265358979;
static tg::RamBlobStore<36, 1100> ps; static tg::RamBlobStore<4, 1500> ss;
static tg::AudioDsp* dsp;
struct Rig {
  Null n; pd::Engine* e;
  Rig() { e = new pd::Engine(ps, ss); e->begin(); e->handleLine("PNEW", n); }
  ~Rig() { delete e; }
  void cmd(const std::string& c) { e->handleLine(c.c_str(), n); }
  unsigned add(const char* bank, const char* name) { std::string c = std::string("FXA ") + bank + " 0 " + name; cmd(c); for (int i = e->chain().nb - 1; i >= 0; i--) if (!strcmp(pd::fxById(e->chain().b[i].type)->name, name)) return e->chain().b[i].id; return 0; }
  void set(unsigned id, int k, int v) { char b[48]; snprintf(b, sizeof b, "FXP %u %d %d", id, k, v); cmd(b); }
};
using Sig = std::vector<int32_t>;
static Sig sine(int n, double hz, double amp) { Sig s(n); for (int i = 0; i < n; i++) s[i] = (int32_t)(amp * 8388607 * sin(2 * kPi * hz * i / 48000)); return s; }
static Sig noise(int n, double amp, unsigned seed = 1) { Sig s(n); srand(seed); for (int i = 0; i < n; i++) s[i] = (int32_t)(amp * 8388607 * ((rand() / (double)RAND_MAX) * 2 - 1)); return s; }
static Sig impulse(int n, double amp = 0.5) { Sig s(n, 0); s[100] = (int32_t)(amp * 8388607); return s; }
// run the engine's chain through a fresh DSP (master at unity), 128-frame blocks like the real driver
static Sig process(Rig& r, const Sig& in) {
  static int32_t li[256], lo[256]; r.e->knob(0, 1023);
  delete dsp; dsp = new tg::AudioDsp(); tg::AudioParams p; tg::buildParams(*r.e, false, p); dsp->setParams(p);
  Sig out(in.size());
  for (size_t o = 0; o < in.size(); o += 128) { for (int f = 0; f < 128; f++) { li[f * 2] = o + f < in.size() ? in[o + f] : 0; li[f * 2 + 1] = 0; } dsp->process(li, lo, 128); for (int f = 0; f < 128 && o + f < in.size(); f++) out[o + f] = lo[f * 2]; }
  return out;
}
static double rms(const Sig& s, size_t a = 0, size_t b = (size_t)-1) { if (b > s.size()) b = s.size(); double t = 0; for (size_t i = a; i < b; i++) t += (double)s[i] * s[i]; return sqrt(t / (b - a > 0 ? b - a : 1)) / 8388607.0; }
static double diff(const Sig& a, const Sig& b) { double t = 0; for (size_t i = 0; i < a.size(); i++) { double d = (double)a[i] - b[i]; t += d * d; } return sqrt(t / a.size()) / 8388607.0; }
static double peak(const Sig& s, size_t a = 0, size_t b = (size_t)-1) { if (b > s.size()) b = s.size(); int32_t m = 0; for (size_t i = a; i < b; i++) m = abs(s[i]) > m ? abs(s[i]) : m; return m / 8388607.0; }
// level of one sine component (single bin DFT)
static double tone(const Sig& s, double hz, size_t a = 4800) { double re = 0, im = 0; size_t n = 0; for (size_t i = a; i < s.size(); i++, n++) { re += s[i] * cos(2 * kPi * hz * i / 48000); im += s[i] * sin(2 * kPi * hz * i / 48000); } return 2 * sqrt(re * re + im * im) / n / 8388607.0; }
static double db(double x) { return 20 * log10(x > 1e-12 ? x : 1e-12); }
static bool finite_bounded(const Sig& s) { for (int32_t v : s) if (v > 8388607 || v < -8388608) return false; return true; }

int main() {
  const Sig guitar = [] { Sig a = sine(24000, 196, 0.12), b = sine(24000, 392, 0.05); for (size_t i = 0; i < a.size(); i++) a[i] += b[i]; return a; }();
  // ---- every implemented block changes the signal
  struct { const char* name; const char* bank; } blocks[] = {{"gate", "P"}, {"boost", "P"}, {"overdrive", "P"}, {"ts9", "P"}, {"ds1", "P"}, {"rat", "P"}, {"muff", "P"}, {"bd2", "P"}, {"eq", "P"},
    {"comp", "P"}, {"klon", "P"}, {"fuzzface", "P"}, {"sgate", "P"}, {"pitchpro", "P"}, {"rotary", "P"}, {"envfilter", "P"}, {"doubler", "P"}, {"basseq", "Q"}, {"chorus", "P"}, {"flanger", "P"}, {"phaser", "P"}, {"tremolo", "P"}, {"vibrato", "P"}, {"amp_clean", "P"}, {"amp_brit", "P"}, {"amp_hi", "P"},
    {"ir", "Q"}, {"geq", "Q"}, {"delay", "Q"}, {"reverb2", "Q"}, {"plate", "Q"}, {"hybrid", "Q"}, {"spring", "Q"}, {"cloud", "Q"}, {"shimmer", "Q"}, {"spectral", "Q"}};
  for (auto& bl : blocks) {
    Rig r; r.cmd("AMP 0");   // isolate the block: no capture amp
    Sig ref = process(r, guitar);
    unsigned id = r.add(bl.bank, bl.name);
    if (!strcmp(bl.name, "eq")) { r.set(id, 0, 12); r.set(id, 3, 12); }
    if (!strcmp(bl.name, "gate") || !strcmp(bl.name, "sgate")) r.set(id, 0, -20);
    if (!strcmp(bl.name, "basseq")) { r.set(id, 0, 9); r.set(id, 2, -9); }
    if (!strcmp(bl.name, "ir")) r.set(id, 2, 3);
    if (!strcmp(bl.name, "geq")) { r.set(id, 3, 12); r.set(id, 5, -12); }
    Sig o = process(r, guitar);
    double d = diff(o, ref);
    CHK(d > 0.002 && finite_bounded(o), "%s changes the signal (rms diff %.5f)", bl.name, d);
    CHK(r.e->cpuUsed() <= pd::kCpuBudget && r.e->memUsed() <= pd::kFxMemBudget, "%s inside budget (cpu %u mem %u)", bl.name, r.e->cpuUsed(), r.e->memUsed());
    Rig r2; r2.cmd("AMP 0"); unsigned id2 = r2.add(bl.bank, bl.name); char b[40]; snprintf(b, sizeof b, "FXE %u 0", id2); r2.cmd(b);
    CHK(diff(process(r2, guitar), ref) < 1e-6, "%s disabled = transparent", bl.name);
  }
  // ---- amp: gain adds harmonics, tone controls act on their band, volume scales, stays bounded
  { Rig r; r.cmd("AMPEQ 0 0 0 0 0 -120"); Sig lo = process(r, sine(24000, 220, 0.1)); r.cmd("AMPEQ 0 0 0 0 0 120"); Sig hi = process(r, sine(24000, 220, 0.1));
    auto thd = [](const Sig& s) { double f = tone(s, 220), h = 0; for (int k = 2; k <= 6; k++) h += pow(tone(s, 220 * k), 2); return sqrt(h) / f; };
    CHK(thd(hi) > 4 * thd(lo) && thd(hi) > 0.02, "amp GAIN adds harmonics (THD %.3f -> %.3f)", thd(lo), thd(hi)); CHK(finite_bounded(hi), "amp bounded");
    CHK(rms(hi) > 0.005, "amp output present"); }
  { Rig r; r.cmd("AMPEQ 0 0 0 0 0 -60"); double b0 = db(tone(process(r, sine(24000, 60, 0.01)), 60)), t0 = db(tone(process(r, sine(24000, 8000, 0.01)), 8000)), m0 = db(tone(process(r, sine(24000, 800, 0.01)), 800));
    r.cmd("AMPEQ 0 120 0 0 0 -60"); double b1 = db(tone(process(r, sine(24000, 60, 0.01)), 60));
    r.cmd("AMPEQ 0 0 120 0 0 -60"); double m1 = db(tone(process(r, sine(24000, 800, 0.01)), 800));
    r.cmd("AMPEQ 0 0 0 120 0 -60"); double t1 = db(tone(process(r, sine(24000, 8000, 0.01)), 8000));
    CHK(b1 - b0 > 8 && b1 - b0 < 14, "BASS +12 dB lifts 60 Hz by %.1f dB", b1 - b0); CHK(m1 - m0 > 8 && m1 - m0 < 14, "MIDDLE +12 dB lifts 800 Hz by %.1f dB", m1 - m0);
    CHK(t1 - t0 > 8 && t1 - t0 < 14, "TREBLE +12 dB lifts 8 kHz by %.1f dB", t1 - t0);
    r.cmd("AMPEQ 0 0 0 0 0 -60"); double v0 = db(tone(process(r, sine(24000, 800, 0.01)), 800)); r.cmd("AMPEQ 60 0 0 0 0 -60"); double v1 = db(tone(process(r, sine(24000, 800, 0.01)), 800));
    CHK(v1 - v0 > 5 && v1 - v0 < 7, "VOLUME +6 dB (%.1f)", v1 - v0); }
  // ---- biquad stability over the whole tone-stack grid (impulse must die out, no limit cycle)
  for (int g = -120; g <= 120; g += 40) for (int band = 0; band < 3; band++) { Rig r; r.cmd("AMPEQ 0 0 0 0 0 -120"); char b[64]; int e[3] = {0, 0, 0}; e[band] = g; snprintf(b, sizeof b, "AMPEQ 0 %d %d %d 0 -120", e[0], e[1], e[2]); r.cmd(b);
    Sig o = process(r, impulse(48000, 0.5)); CHK(rms(o, 40000, 48000) < 0.0005, "tone stack band %d at %+d dB settles (tail rms %.6f)", band, g / 10, rms(o, 40000, 48000)); }
  // ---- delay: echo arrives at tiempo_ms, feedback repeats, mix 0 is clean
  { Rig r; r.cmd("AMP 0"); unsigned id = r.add("Q", "delay"); r.set(id, 0, 300); r.set(id, 1, 50); r.set(id, 2, 100); r.set(id, 3, 0);
    Sig o = process(r, impulse(48000)); size_t pk = 0; double best = 0; for (size_t i = 5000; i < 30000; i++) if (fabs((double)o[i]) > best) { best = fabs((double)o[i]); pk = i; }
    CHK(pk > 100 + 14400 - 120 && pk < 100 + 14400 + 120, "first echo at %zu samples (want ~14500)", pk);
    double e1 = peak(o, 14000, 15000), e2 = peak(o, 28400, 29400); CHK(e1 > 0.2 && e2 > 0.5 * e1 * 0.4 && e2 < e1, "feedback repeats and decays (%.3f -> %.3f)", e1, e2);
    r.set(id, 2, 0); Sig c = process(r, impulse(48000)); CHK(peak(c, 5000, 48000) < 1e-5, "mix 0 leaves no echo");
    r.set(id, 2, 100); r.set(id, 1, 95); r.set(id, 0, 100); Sig big = process(r, noise(96000, 0.3)); CHK(finite_bounded(big) && peak(big) <= 1.0001, "delay at 95 %% feedback stays bounded"); }
  // ---- reverb: tail grows with decay, mix 0 is dry, long noise stays bounded
  { Rig r; r.cmd("AMP 0"); unsigned id = r.add("Q", "reverb2"); r.set(id, 4, 100); r.set(id, 3, 0); r.set(id, 1, 20); Sig a = process(r, impulse(96000)); r.set(id, 1, 90); Sig b = process(r, impulse(96000));
    double ta = rms(a, 48000, 96000), tb = rms(b, 48000, 96000), early = rms(b, 2000, 9000);
    CHK(tb > 4 * ta && tb > 1e-4, "decay 90 rings longer than 20 (tail rms %.6f vs %.6f)", tb, ta); CHK(early > 1e-3 && early < 0.6, "reverb early level sane (%.4f)", early);
    r.set(id, 4, 0); Sig c = process(r, impulse(48000)); double dd = 0; for (size_t i = 0; i < c.size(); i++) if (i != 100 && fabs((double)c[i]) > 2) dd++; CHK(dd == 0, "mix 0 reverb is dry");
    r.set(id, 4, 100); r.set(id, 1, 100); r.set(id, 2, 0); Sig big = process(r, noise(240000, 0.1)); CHK(finite_bounded(big) && peak(big) <= 1.0001 && rms(big, 200000, 240000) < 0.5, "reverb stable at max decay/min damping (rms %.3f)", rms(big, 200000, 240000));
    r.set(id, 3, 0); r.set(id, 4, 100); r.set(id, 1, 50); Sig p0 = process(r, impulse(48000)); r.set(id, 3, 40); Sig p1 = process(r, impulse(48000)); CHK(peak(p0, 1300, 3000) > 0.001 && peak(p1, 200, 3000) < 0.0005, "pre-delay holds the tail back (%.4f vs %.5f)", peak(p0, 1300, 3000), peak(p1, 200, 3000)); r.set(id, 3, 0); 
    double lvl = rms(process(r, sine(48000, 220, 0.1)), 30000, 48000); CHK(lvl > 0.02 && lvl < 0.5, "reverb steady level near input level (%.3f for 0.07 rms in)", lvl); }
  // ---- modulation: LFO really moves things
  { Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", "tremolo"); r.set(id, 0, 500); r.set(id, 1, 100); Sig o = process(r, sine(48000, 440, 0.2));
    double mn = 1, mx = 0; for (size_t i = 0; i + 960 < o.size(); i += 960) { double v = rms(o, i, i + 960); mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    CHK(mx > 0.12 && mn < 0.02, "tremolo 5 Hz depth 100 sweeps loudness %.3f..%.3f", mn, mx);
    r.set(id, 1, 0); o = process(r, sine(48000, 440, 0.2)); mn = 1; mx = 0; for (size_t i = 0; i + 960 < o.size(); i += 960) { double v = rms(o, i, i + 960); mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    CHK(mx - mn < 0.005, "tremolo depth 0 is steady"); }
  for (const char* nm : {"chorus", "vibrato", "flanger"}) { Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", nm); r.set(id, 0, 150); r.set(id, 1, 100); if (!strcmp(nm, "chorus")) r.set(id, 2, 100); if (!strcmp(nm, "flanger")) { r.set(id, 2, 0); r.set(id, 3, 100); }
    Sig o = process(r, sine(96000, 440, 0.2)); double mn = 1, mx = 0; for (size_t i = 24000; i + 480 < o.size(); i += 480) { double v = rms(o, i, i + 480); mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    // a modulated delay changes the instantaneous pitch/phase: zero-crossing spacing varies with time; measure the spread of crossing intervals
    std::vector<int> iv; int last = -1; for (size_t i = 24001; i < o.size(); i++) if (o[i - 1] < 0 && o[i] >= 0) { if (last >= 0) iv.push_back((int)i - last); last = (int)i; }
    int lo = 1 << 30, hi = 0; for (int v : iv) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
    CHK(finite_bounded(o) && (hi - lo >= 1 || mx - mn > 0.01), "%s modulates over time (intervals %d..%d, level %.3f..%.3f)", nm, lo, hi, mn, mx); }
  // ---- pitch shifter / octaver: +12 st doubles the frequency, -12 st halves it, mix blends, other intervals land where they should
  { // the shifted note is measured by its zero crossings per second (the grains of a delay-line shifter are not phase coherent, a single FFT bin is not a fair meter)
    auto freq = [](const Sig& o) { int n = 0; for (size_t i = 12001; i < o.size(); i++) n += o[i - 1] < 0 && o[i] >= 0; return n * 48000.0 / (o.size() - 12001); };
    Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", "pitchpro"); r.set(id, 2, 100);
    r.set(id, 0, 12); Sig up = process(r, sine(96000, 220, 0.2)); CHK(fabs(freq(up) - 440) < 22 && rms(up, 12000) > 0.08, "octave up: 220 Hz -> %.0f Hz (rms %.3f)", freq(up), rms(up, 12000));
    r.set(id, 0, -12); Sig dn = process(r, sine(96000, 220, 0.2)); CHK(fabs(freq(dn) - 110) < 15 && rms(dn, 12000) > 0.08, "octave down: 220 Hz -> %.0f Hz (rms %.3f)", freq(dn), rms(dn, 12000));
    r.set(id, 0, 7); Sig fi = process(r, sine(96000, 220, 0.2)); CHK(fabs(freq(fi) - 329.6) < 17, "+7 semitones: 220 Hz -> %.0f Hz", freq(fi));
    r.set(id, 0, 0); Sig un = process(r, sine(96000, 220, 0.2)); CHK(fabs(freq(un) - 220) < 2 && fabs(rms(un, 12000) - 0.2 / sqrt(2.0)) < 0.02, "0 semitones: unchanged pitch and level (%.0f Hz, rms %.3f)", freq(un), rms(un, 12000));
    r.set(id, 0, -12); r.set(id, 2, 50); Sig mx = process(r, sine(96000, 220, 0.2)); CHK(tone(mx, 220, 12000) > 0.09 && tone(mx, 220, 12000) < 0.11 && diff(mx, dn) > 0.03, "mix 50: half of the dry note plus the octave");
    r.set(id, 2, 0); Sig cl = process(r, sine(48000, 220, 0.2)); CHK(fabs(tone(cl, 220, 12000) - 0.2) < 0.01 && fabs(freq(cl) - 220) < 2, "mix 0 is the clean signal");
    r.set(id, 2, 100); r.set(id, 0, 12); Sig nz = process(r, noise(48000, 0.5)); CHK(finite_bounded(nz), "bounded on noise");
    CHK(tg::dsp::pitchRatioQ16(12, 0) == 131072 && tg::dsp::pitchRatioQ16(-12, 0) == 32768 && tg::dsp::pitchRatioQ16(0, 0) == 65536 && abs((int)tg::dsp::pitchRatioQ16(0, 50) - 67456) < 80, "ratio table"); }
  // ---- rotary: tremolo at the horn rate, fast vs slow, mix 0 clean
  { auto swing = [](const Sig& o, size_t win) { double mn = 9, mx = 0; for (size_t i = 24000; i + win < o.size(); i += win) { double v = rms(o, i, i + win); mn = v < mn ? v : mn; mx = v > mx ? v : mx; } return mx / (mn > 1e-9 ? mn : 1e-9); };
    Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", "rotary"); r.set(id, 0, 1); r.set(id, 2, 700);
    Sig f = process(r, sine(96000, 2000, 0.2)); CHK(swing(f, 480) > 1.3 && finite_bounded(f), "fast rotor modulates the highs (level swing x%.2f)", swing(f, 480));
    r.set(id, 6, 0); Sig c = process(r, sine(96000, 2000, 0.2)); CHK(swing(c, 480) < 1.02, "rotary mix 0 is steady"); }
  // ---- envelope filter: louder playing opens the filter
  { Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", "envfilter"); r.set(id, 5, 0); r.set(id, 1, 200); r.set(id, 2, 5000); r.set(id, 0, 60); r.set(id, 3, 20);   // low-pass 200..5000 Hz
    double q = tone(process(r, sine(48000, 2500, 0.002)), 2500, 24000) / 0.002, l = tone(process(r, sine(48000, 2500, 0.4)), 2500, 24000) / 0.4;
    CHK(l > 3 * q && q < 0.3, "2.5 kHz passes when played loud (gain %.2f) and is filtered when soft (gain %.3f)", l, q);
    Sig nz = process(r, noise(48000, 0.9)); CHK(finite_bounded(nz), "stable at full-scale noise"); r.set(id, 3, 100); nz = process(r, noise(48000, 0.9)); CHK(finite_bounded(nz) && rms(nz) < 1.0, "stable at max resonance"); }
  // ---- compressor: loud signals are turned down, quiet ones are not
  { Rig r; r.cmd("AMP 0"); unsigned id = r.add("P", "comp"); r.set(id, 0, -30); r.set(id, 1, 80); r.set(id, 4, 0);
    double gq = rms(process(r, sine(48000, 440, 0.005)), 24000) / (0.005 / sqrt(2.0)), gl = rms(process(r, sine(48000, 440, 0.5)), 24000) / (0.5 / sqrt(2.0));
    CHK(fabs(gq - 1) < 0.05 && gl < 0.3, "ratio 8:1 above -30 dB: quiet gain %.2f, loud gain %.2f", gq, gl); }
  // ---- IR Cab: 8 voicings, distinct, shaped like cabinets, stable, selectable; slot 0 / mix 0 transparent; it replaces the amp's own roll-off
  { const double fr[6] = {40, 100, 500, 2000, 5000, 9000}; double resp[8][6];
    for (int sl = 1; sl <= 8; sl++) { Rig r; r.cmd("AMP 0"); unsigned id = r.add("Q", "ir"); r.set(id, 2, sl);
      for (int k = 0; k < 6; k++) resp[sl - 1][k] = db(tone(process(r, sine(24000, fr[k], 0.05)), fr[k]) / 0.05);
      Sig imp = process(r, impulse(48000)); CHK(rms(imp, 40000, 48000) < 0.0002 && finite_bounded(imp), "cab %d impulse settles (tail %.6f)", sl, rms(imp, 40000, 48000));
      CHK(resp[sl - 1][0] < resp[sl - 1][2] - 6, "cab %d cuts the lows (40 Hz %.1f dB vs 500 Hz %.1f dB)", sl, resp[sl - 1][0], resp[sl - 1][2]);
      CHK(resp[sl - 1][5] < resp[sl - 1][3] - 3, "cab %d rolls off the top (9 kHz %.1f dB vs 2 kHz %.1f dB)", sl, resp[sl - 1][5], resp[sl - 1][3]); }
    for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++) { double m = 0; for (int k = 0; k < 6; k++) { double d = fabs(resp[a][k] - resp[b][k]); m = d > m ? d : m; } CHK(m > 1.5, "cab %d and %d have different responses (max %.1f dB)", a + 1, b + 1, m); }
    Rig r; r.cmd("AMP 0"); Sig ref = process(r, guitar); unsigned id = r.add("Q", "ir"); CHK(diff(process(r, guitar), ref) < 1e-6, "IR slot 0 is transparent");
    r.set(id, 2, 3); r.set(id, 0, 0); CHK(diff(process(r, guitar), ref) < 1e-4, "IR mix 0 is transparent"); r.set(id, 0, 100); double l0 = rms(process(r, guitar)); r.set(id, 1, -12); double l1 = rms(process(r, guitar));
    CHK(fabs(db(l1 / l0) + 12) < 1.0, "IR level -12 dB (%.1f)", db(l1 / l0));
    // with an IR active the amp drops its fixed 5.2 kHz roll-off: a bright cab (slot 6) lets more 8 kHz through than no cab at all
    Rig a1; a1.cmd("AMPEQ 0 0 0 0 0 -60"); double t_nocab = tone(process(a1, sine(24000, 8000, 0.02)), 8000); unsigned i2 = a1.add("Q", "ir"); a1.set(i2, 2, 6); a1.set(i2, 0, 100);
    double t_cab = tone(process(a1, sine(24000, 8000, 0.02)), 8000); CHK(t_cab > t_nocab * 1.1, "amp roll-off steps aside for an active cab (%.5f -> %.5f)", t_nocab, t_cab); }
  // ---- GEQ: six bands, each moves only its own region, zero is flat
  { const double fc[6] = {50, 125, 315, 800, 2000, 5000};
    for (int b = 0; b < 6; b++) for (int sgn : {1, -1}) { Rig r; r.cmd("AMP 0"); unsigned id = r.add("Q", "geq"); double base = db(tone(process(r, sine(24000, fc[b], 0.05)), fc[b]));
      r.set(id, b, 12 * sgn); double g = db(tone(process(r, sine(24000, fc[b], 0.05)), fc[b])) - base;
      CHK(fabs(g - 12 * sgn) < 2.5, "GEQ band %d Hz %+d dB gives %+.1f dB", (int)fc[b], 12 * sgn, g);
      double far = fc[b] < 400 ? 5000 : 80; double fb0 = db(tone(process(r, sine(24000, far, 0.05)), far)); r.set(id, b, 0); double fb1 = db(tone(process(r, sine(24000, far, 0.05)), far));
      CHK(fabs(fb0 - fb1) < 3.0, "GEQ band %d Hz leaves %d Hz alone (%.1f dB)", (int)fc[b], (int)far, fb0 - fb1); }
    Rig r; r.cmd("AMP 0"); Sig ref = process(r, guitar); unsigned id = r.add("Q", "geq"); CHK(diff(process(r, guitar), ref) < 5e-4, "GEQ flat is transparent (%.6f)", diff(process(r, guitar), ref));
    r.set(id, 6, 6); double l = rms(process(r, guitar)) / rms(ref); r.set(id, 6, 0); CHK(l > 1.9 && l < 2.1, "GEQ level +6 dB doubles the amplitude (%.2f)", l); 
    for (int b = 0; b < 6; b++) { r.set(id, b, 4); } Sig imp = process(r, impulse(48000)); CHK(rms(imp, 40000, 48000) < 0.0005 && finite_bounded(imp), "GEQ all bands +4 settles (%.6f)", rms(imp, 40000, 48000)); }
  // ---- two-core pipeline: same samples as one core, 2 blocks (256 samples) later
  { Rig r; r.cmd("AMPEQ 0 0 0 0 0 40"); r.add("P", "chorus"); r.add("Q", "delay"); r.add("Q", "reverb2");
    Sig one = process(r, guitar);
    static tg::AudioDsp two; two.other.run = [](void (*fn)(uint32_t), uint32_t a) { fn(a); return true; }; two.other.wait = [] {};
    tg::AudioParams p; r.e->knob(0, 1023); tg::buildParams(*r.e, false, p); two.setParams(p);
    static int32_t li[256], lo[256]; Sig o2(guitar.size());
    for (size_t o = 0; o < guitar.size(); o += 128) { for (int f = 0; f < 128; f++) { li[f * 2] = o + f < guitar.size() ? guitar[o + f] : 0; li[f * 2 + 1] = 0; } two.process(li, lo, 128); for (int f = 0; f < 128 && o + f < guitar.size(); f++) o2[o + f] = lo[f * 2]; }
    double e = 0; for (size_t i = 256; i < guitar.size(); i++) { double d = (double)o2[i] - one[i - 256]; e += d * d; }
    CHK(sqrt(e / guitar.size()) < 1.0 && rms(o2) > 0.01, "two-core pipeline = one core delayed by 256 samples (rms diff %.3f LSB)", sqrt(e / guitar.size())); }
  // ---- factory-style presets are distinct, inside budget and cheap enough on the host (the chip itself is NOT measured)
  { struct Pre { const char* name; std::vector<std::string> c; } pres[] = {
      {"clean", {"AMPEQ 0 0 0 0 0 -60", "FXA P 0 chorus", "FXA Q 0 reverb2"}}, {"crunch", {"AMPEQ 0 0 0 0 0 40", "FXA P 0 overdrive", "FXA Q 0 delay", "FXA Q 0 geq", "FXA Q 0 ir"}},
      {"lead", {"AMPEQ 0 0 0 0 0 100", "FXA P 0 ds1", "FXA Q 0 delay", "FXA Q 1 plate", "FXA Q 0 geq", "FXA Q 0 ir"}}, {"ambient", {"AMPEQ 0 0 0 0 0 -90", "FXA P 0 phaser", "FXA Q 0 delay", "FXA Q 1 cloud"}}};
    std::vector<Sig> outs; double worst_ns = 0;
    for (auto& p : pres) { Rig r; for (auto& c : p.c) r.cmd(c); CHK(r.e->cpuUsed() <= pd::kCpuBudget && r.e->memUsed() <= pd::kFxMemBudget, "%s inside budget (cpu %u/%u mem %u/%u)", p.name, r.e->cpuUsed(), pd::kCpuBudget, r.e->memUsed(), pd::kFxMemBudget);
      Sig in = guitar; in.resize(48000, 0); auto t0 = std::chrono::steady_clock::now(); Sig o = process(r, in); double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / in.size(); worst_ns = ns > worst_ns ? ns : worst_ns;
      CHK(finite_bounded(o) && rms(o) > 0.005 && peak(o) <= 1.0, "%s audible and bounded (rms %.3f)", p.name, rms(o)); outs.push_back(o); }
    for (size_t i = 0; i < outs.size(); i++) for (size_t j = i + 1; j < outs.size(); j++) CHK(diff(outs[i], outs[j]) > 0.01, "presets %zu and %zu sound different (%.4f)", i, j, diff(outs[i], outs[j]));
    printf("host cost of the heaviest preset: %.0f ns/sample (the chip budget is 20833 ns/sample at 100 %% of a core; host-to-chip factor unknown)\n", worst_ns);
    CHK(worst_ns < 3000, "host speed sanity (%.0f ns/sample)", worst_ns); }
  printf("%d checks, %d failed\n", checks, fails); return fails != 0;
}
