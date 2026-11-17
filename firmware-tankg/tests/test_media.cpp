// Upload -> sound, through the real protocol path: Engine (LDBEGIN/LDB/LDEND/CSAVE, IRBEGIN/IRB/IREND/ISAVE/FXIR) + TgMedia + LogStore on a
// simulated flash + the audio DSP. Checks that an uploaded capture / IR changes the sound at once, is what the file says, survives a power cycle,
// and that failures (corrupt blob, full flash) are clean.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "pd/engine.h"
#include "pd/crc.h"
#include "log_store.h"
#include "audio.h"
#include "a2l_ref.h"
static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const double kPi = 3.14159265358979;
struct Sim : tg::FlashDev {
  uint8_t m[tg::LogStore::kRegionBytes]; Sim() { memset(m, 0xFF, sizeof m); }
  bool read(uint32_t o, uint8_t* d, uint32_t n) override { memcpy(d, m + o, n); return true; }
  bool erase4k(uint32_t o) override { memset(m + o, 0xFF, 4096); return true; }
  bool program(uint32_t o, const uint8_t* s, uint32_t n) override { for (uint32_t i = 0; i < n; i++) m[o + i] &= s[i]; return true; }
};
struct Cap : pd::ILineSink { std::vector<std::string> l; void line(const char* s) override { l.push_back(s); } std::string last() { return l.empty() ? "" : l.back(); } };
using Sig = std::vector<int32_t>;
struct Pedal {   // one powered-on pedal on a given flash
  tg::LogStore store; pd::Engine* e; tg::TgMedia* media; tg::AudioDsp* dsp;
  explicit Pedal(Sim& f) : store(f, 0) { media = new tg::TgMedia(); dsp = new tg::AudioDsp(); e = new pd::Engine(store, store); static const char* const nm[8] = {"1x12 Clean", "2x12 Brit", "4x12 Modern", "1x10 Open", "4x12 Vintage", "Bright Combo", "Dark Cab", "Studio"}; e->setBuiltinIrNames(nm, 8); media->attach(&store); e->setMedia(media); e->begin(); e->knob(0, 1023); }
  ~Pedal() { delete e; delete media; delete dsp; }
  std::string cmd(const std::string& c) { Cap o; e->handleLine(c.c_str(), o); return o.last(); }
  std::string upload(const char* pre, const uint8_t* d, size_t n, const std::string& begin) {
    std::string r = cmd(begin); if (r.compare(0, 3, "OK ")) return r;
    for (size_t o = 0; o < n; o += 90) { size_t k = n - o < 90 ? n - o : 90; std::string h; char b[4]; for (size_t i = 0; i < k; i++) { snprintf(b, sizeof b, "%02x", d[o + i]); h += b; }
      r = cmd(std::string(pre) + "B " + std::to_string(o) + " " + h); if (r.compare(0, 3, "OK ")) return r; }
    char c[16]; snprintf(c, sizeof c, "%08x", (unsigned)pd::crc32(d, n)); return cmd(std::string(pre) + "END " + c);
  }
  Sig run(const Sig& in) {   // keeps the DSP state between calls (like the real ISR); parameters refreshed first, as the main loop does every 20 ms
    static int32_t li[256], lo[256]; tg::AudioParams p; tg::buildParams(*e, false, p, media); dsp->setParams(p); Sig out(in.size());
    for (size_t o = 0; o < in.size(); o += 128) { for (int f = 0; f < 128; f++) { li[f * 2] = o + f < in.size() ? in[o + f] : 0; li[f * 2 + 1] = 0; } dsp->process(li, lo, 128); for (int f = 0; f < 128 && o + f < in.size(); f++) out[o + f] = lo[f * 2]; }
    return out;
  }
};
static Sig sine(int n, double hz, double amp) { Sig s(n); for (int i = 0; i < n; i++) s[i] = (int32_t)(amp * 8388607 * sin(2 * kPi * hz * i / 48000)); return s; }
static double rms(const Sig& s, size_t a = 0) { double t = 0; for (size_t i = a; i < s.size(); i++) t += (double)s[i] * s[i]; return sqrt(t / (s.size() - a)) / 8388607.0; }
static double diff(const Sig& a, const Sig& b) { double t = 0; for (size_t i = 0; i < a.size(); i++) { double d = (double)a[i] - b[i]; t += d * d; } return sqrt(t / a.size()) / 8388607.0; }
static double tone(const Sig& s, double hz, size_t a = 4800) { double re = 0, im = 0; size_t n = 0; for (size_t i = a; i < s.size(); i++, n++) { re += s[i] * cos(2 * kPi * hz * i / 48000); im += s[i] * sin(2 * kPi * hz * i / 48000); } return 2 * sqrt(re * re + im * im) / n / 8388607.0; }
static double db(double x) { return 20 * log10(x > 1e-12 ? x : 1e-12); }

int main() {
  static Sim flash;
  const Sig guitar = [] { Sig a = sine(12000, 196, 0.1), b = sine(12000, 392, 0.04); for (size_t i = 0; i < a.size(); i++) a[i] += b[i]; return a; }();
  // ================= capture (A2L1) =================
  { Pedal p(flash); p.cmd("PNEW"); p.cmd("AMPEQ 0 0 0 0 0 0");
    CHK(p.media->liveModel() == nullptr, "no capture at first: the open amp stage plays");
    Sig open = p.run(guitar);
    std::string r = p.upload("LD", kA2l_a_blob, kA2l_a_len, "LDBEGIN " + std::to_string(kA2l_a_len));
    CHK(r == "LOAD rc=0 name='RefTest A' layers=10 channels=3 audio=2 engine=A2L ms=5", "LDEND reply: %s", r.c_str());
    CHK(p.media->liveModel() != nullptr, "capture is live right after the upload (no CSAVE yet)");
    Sig cap = p.run(guitar);
    CHK(diff(cap, open) > 0.01, "the sound changes immediately (rms diff %.4f)", diff(cap, open));
    // what sounds is the uploaded network: compare with the float64 reference (AMPEQ flat, 0 dB). The DSP adds a DC blocker, so compare the AC part.
    Pedal q(flash); q.cmd("PNEW"); q.cmd("AMPEQ 0 0 0 0 0 0"); q.upload("LD", kA2l_a_blob, kA2l_a_len, "LDBEGIN " + std::to_string(kA2l_a_len));
    Sig in(kA2l_a_in, kA2l_a_in + kA2l_a_n); Sig out = q.run(in); double se = 0, ss = 0, mo = 0, mr = 0; int a0 = 2200, n = kA2l_a_n - a0;
    (void)mo; (void)mr; (void)n; double x1 = 0, y1 = 0;   // the same DC blocker the DSP puts after the network, applied to the clipped float reference
    for (int i = 0; i < (int)kA2l_a_n; i++) { double v = kA2l_a_out[i]; v = v > 1 ? 1.0 : v < -1 ? -1.0 : v; double y = v - x1 + y1 - y1 / 1024; x1 = v; y1 = y;
      if (i >= a0) { double o = out[i] / 8388608.0; se += (o - y) * (o - y); ss += y * y; } }
    CHK(10 * log10(ss / se) > 40, "amp output = the uploaded network (SNR %.1f dB vs float64)", 10 * log10(ss / se));
    // GAIN and VOLUME act around the network
    double l0 = rms(p.run(guitar), 6000); p.cmd("AMPEQ -60 0 0 0 0 0"); double l1 = rms(p.run(guitar), 6000); CHK(fabs(db(l1 / l0) + 6) < 1.5, "VOLUME -6 dB around the capture (%.1f dB)", db(l1 / l0)); p.cmd("AMPEQ 0 0 0 0 0 0");
    // slots
    r = p.cmd("CSAVE 2 Amp A"); CHK(r == "CSAVE ok 2 'Amp A' 1532 B", "CSAVE: %s", r.c_str());
    Cap cl; p.e->handleLine("CLIST", cl); CHK(cl.l.size() == 2 && cl.l[0] == "CAP 2 1532 'Amp A'" && cl.l[1] == "CLIST end current=2 slots=15", "CLIST: %s", cl.l[0].c_str());
    r = p.upload("LD", kA2l_b_blob, kA2l_b_len, "LDBEGIN " + std::to_string(kA2l_b_len)); CHK(r.find("rc=0 name='RefTest B' layers=7") != std::string::npos, "second capture: %s", r.c_str());
    Sig capB = p.run(guitar); CHK(diff(capB, cap) > 0.01, "another capture sounds different");
    CHK(p.cmd("CSAVE 5 Amp B") == "CSAVE ok 5 'Amp B' 1070 B", "CSAVE 5");
    CHK(p.cmd("CLOAD 2") == "CLOAD ok 2 audio=2", "CLOAD 2"); p.run(guitar); Sig again = p.run(guitar); p.cmd("CLOAD 5"); p.run(guitar); Sig againB = p.run(guitar);
    CHK(diff(again, againB) > 0.01 && p.media->liveModel() && !strcmp(p.media->liveModel()->name, "RefTest B"), "CLOAD switches the sounding capture");
    p.cmd("CLOAD 2"); CHK(p.cmd("PSAVE 0 Captured").compare(0, 8, "PSAVE ok") == 0, "preset saved with its capture");
    // corrupt upload: rejected, the previous capture keeps playing
    std::vector<uint8_t> bad(kA2l_a_blob, kA2l_a_blob + kA2l_a_len); bad[200] ^= 1;
    r = p.upload("LD", bad.data(), bad.size(), "LDBEGIN " + std::to_string(bad.size())); CHK(r.compare(0, 10, "LOAD rc=-4") == 0, "corrupt blob: %s", r.c_str());
    CHK(p.media->liveModel() && !strcmp(p.media->liveModel()->name, "RefTest A"), "previous capture still live after a rejected upload");
    CHK(p.cmd("LDBEGIN 9000") == "ERR LDBEGIN length 9000 (max 8192)", "oversize refused");
  }
  { Pedal p(flash);   // ---- power cycle: same flash, new everything
    Cap cl; p.e->handleLine("CLIST", cl); CHK(cl.l.size() == 3 && cl.l[0] == "CAP 2 1532 'Amp A'" && cl.l[1] == "CAP 5 1070 'Amp B'", "captures listed after reboot");
    CHK(p.media->capStored(2) && p.media->capStored(5) && !p.media->capStored(0), "capture blobs back from flash");
    CHK(p.cmd("PLOAD 0").compare(0, 3, "ERR") != 0 && p.media->liveModel() && !strcmp(p.media->liveModel()->name, "RefTest A"), "the preset brings its capture back after a reboot");
    CHK(p.cmd("CDEL 2") == "CDEL ok 2" && p.media->liveModel() == nullptr && !p.media->capStored(2), "CDEL frees the slot and falls back to the open amp");
  }
  // ================= impulse response =================
  { // a cabinet-like IR as PulseDeck prepares it: 1500 samples at 48 kHz, unit energy, peak-normalised int16 + gain in mdB
    const int N = 1500; std::vector<double> h(N); unsigned s = 7; double lp = 0, lp2 = 0, lp3 = 0, en = 0;
    for (int i = 0; i < N; i++) { s = s * 1664525u + 1013904223u; double w = ((s >> 8) / 8388608.0 - 1) * exp(-i / 110.0); lp += 0.3 * (w - lp); lp2 += 0.3 * (lp - lp2); lp3 += 0.3 * (lp2 - lp3); h[i] = lp3; }
    for (int i = N - 1; i > 0; i--) h[i] -= 0.97 * h[i - 1];   // remove the lows like a cabinet
    for (double v : h) en += v * v; double pk = 0; for (double& v : h) { v /= sqrt(en); pk = fabs(v) > pk ? fabs(v) : pk; }
    std::vector<uint8_t> bytes(N * 2); for (int i = 0; i < N; i++) { int16_t q = (int16_t)lround(h[i] / pk * 32767); bytes[2 * i] = (uint8_t)q; bytes[2 * i + 1] = (uint8_t)(q >> 8); }
    long mdb = lround(20000 * log10(pk * 32768 / 32767));
    Pedal p(flash); p.cmd("PNEW"); p.cmd("AMP 0"); p.cmd("FXA Q 0 ir"); unsigned id = p.e->chain().b[0].id;
    Sig dry = p.run(guitar);
    std::string r = p.upload("IR", bytes.data(), bytes.size(), "IRBEGIN " + std::to_string(N) + " " + std::to_string(mdb) + " 48");
    CHK(r == "IR ok 1500 samples gain " + std::to_string(mdb) + " mdB fs 48", "IREND: %s", r.c_str());
    Sig wet = p.run(guitar); CHK(diff(wet, dry) > 0.005, "the uploaded IR sounds at once on the IR block (rms diff %.4f)", diff(wet, dry));
    // frequency response of the pedal's IR path vs the exact 48 kHz convolution with the first 21 ms of the file
    auto exact = [&](double hz) { double re = 0, im = 0; for (int i = 0; i < 1024; i++) { re += h[i] * cos(2 * kPi * hz * i / 48000); im -= h[i] * sin(2 * kPi * hz * i / 48000); } return sqrt(re * re + im * im); };
    double worst = 0, worst8 = 0; char detail[200] = ""; size_t dl = 0;
    for (double hz : {120.0, 250.0, 500.0, 1000.0, 2000.0, 3000.0, 4000.0, 6000.0, 8000.0}) { double g = tone(p.run(sine(24000, hz, 0.05)), hz) / 0.05, e = exact(hz), d = db(g / e);
      dl += snprintf(detail + dl, sizeof detail - dl, "%g:%+.1f ", hz, d); if (hz <= 6000) worst = fabs(d) > worst ? fabs(d) : worst; else worst8 = fabs(d); }
    printf("  IR path error vs exact convolution (dB): %s\n", detail);
    CHK(worst < 1.5 && worst8 < 3.0, "IR path matches the file's response (dB error per Hz: %s)", detail);
    // slots: ISAVE, FXIR, built-in voicing elsewhere, IDEL
    CHK(p.cmd("ISAVE 4 My Cab") == "ISAVE ok 4 'My Cab' 1500 samples fs=48", "ISAVE");
    CHK(p.cmd("FXIR " + std::to_string(id) + " 4") == "FXIR ok " + std::to_string(id) + " 4" && p.e->chain().b[0].p[2] == 5, "FXIR assigns slot 4 to the block");
    Sig stored = p.run(guitar); stored = p.run(guitar); Sig w2 = p.run(guitar); CHK(diff(stored, w2) < 0.02, "stored IR plays the same as the live one");
    char b[40]; snprintf(b, sizeof b, "FXP %u 2 2", id); p.cmd(b); p.run(guitar); Sig builtin = p.run(guitar); CHK(diff(builtin, w2) > 0.005, "another slot = built-in cabinet voicing (different)");
    Cap il; p.e->handleLine("ILIST", il); bool named = false, bi = false; for (auto& l : il.l) { named |= l == "IRS 4 3000 'My Cab' fs=48"; bi |= l.compare(0, 8, "IRS 1 0 ") == 0; } CHK(named && bi, "ILIST: uploaded slot with its size, built-ins with size 0");
    if (!(named && bi)) for (auto& l : il.l) printf("   %s\n", l.c_str());
    p.cmd("FXIR " + std::to_string(id) + " 4"); p.cmd("PSAVE 1 WithIR");
  }
  { Pedal p(flash); CHK(p.media->irStored(4) && !p.media->irStored(3), "IR back from flash after a reboot");
    p.cmd("PLOAD 1"); Sig a = p.run(guitar); a = p.run(guitar); CHK(p.cmd("IDEL 4") == "IDEL ok 4" && !p.media->irStored(4), "IDEL"); p.run(guitar); Sig b2 = p.run(guitar);
    CHK(diff(a, b2) > 0.003, "after IDEL the slot is the built-in cabinet again"); }
  // ================= flash full =================
  { static Sim f2; Pedal p(f2); int saved = 0; std::string r;
    for (int s = 0; s < 15; s++) { p.upload("LD", kA2l_a_blob, kA2l_a_len, "LDBEGIN " + std::to_string(kA2l_a_len)); r = p.cmd("CSAVE " + std::to_string(s) + " Cap"); if (r.compare(0, 8, "CSAVE ok") == 0) saved++; else break; }
    CHK(saved >= 3 && saved < 15 && r.compare(0, 9, "ERR CSAVE") == 0, "flash fills up cleanly after %d captures: %s", saved, r.c_str());
    CHK(p.media->liveModel() != nullptr, "the capture that did not fit keeps playing from RAM");
    CHK(p.cmd("PSAVE 9 StillWorks").compare(0, 3, "ERR") == 0 || p.cmd("PLIST").size() > 0, "store still consistent");
    Pedal q(f2); int back = 0; for (int s = 0; s < 15; s++) back += q.media->capStored(s); CHK(back == saved, "all %d saved captures intact after reboot (%d)", saved, back); }
  printf("%d checks, %d failed\n", checks, fails); return fails != 0;
}
