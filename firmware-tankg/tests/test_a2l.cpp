// The fixed-point A2L1 player against a float64 forward pass of the same NAM network (tests/a2l_ref.h, generated from PulseDeck's fixtures).
#include <math.h>
#include <stdio.h>
#include <vector>
#include "a2l.h"
#include "a2l_ref.h"
static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static tg::a2l::Model m; static tg::a2l::State st;
static double snr(const uint8_t* blob, unsigned len, const int32_t* in, const double* ref, unsigned n, const char* tag) {
  int rc = tg::a2l::parse(blob, len, m); CHK(rc == 0 && m.valid, "%s parses (rc %d)", tag, rc); if (rc) return 0;
  tg::a2l::reset(m, st); double se = 0, ss = 0;
  for (unsigned i = 0; i < n; i++) { double y = (double)tg::a2l::runRaw(m, st, in[i]) / pow(2.0, m.q23Sh + 23); se += (y - ref[i]) * (y - ref[i]); ss += ref[i] * ref[i]; }
  double r = 10 * log10(ss / (se > 1e-30 ? se : 1e-30));
  printf("  %s: '%s' layers=%d C=%d headK=%d macs=%u arena=%u words, SNR vs float64 = %.1f dB\n", tag, m.name, m.nLayers, m.C, m.headK, (unsigned)m.macs, (unsigned)m.arenaWords, r);
  return r;
}
int main() {
  CHK(snr(kA2l_a_blob, kA2l_a_len, kA2l_a_in, kA2l_a_out, kA2l_a_n, "a") > 50, "capture a matches the float network");
  CHK(snr(kA2l_b_blob, kA2l_b_len, kA2l_b_in, kA2l_b_out, kA2l_b_n, "b") > 50, "capture b matches the float network");
  // rejects: wrong magic, bad CRC, truncated, any flipped byte
  std::vector<uint8_t> v(kA2l_a_blob, kA2l_a_blob + kA2l_a_len);
  { auto w = v; w[0] = 'X'; CHK(tg::a2l::parse(w.data(), w.size(), m) == -4 && !m.valid, "wrong magic rejected"); }
  { CHK(tg::a2l::parse(v.data(), v.size() - 9, m) == -4, "truncated blob rejected"); }
  { int bad = 0; for (size_t i = 0; i < v.size(); i += 7) { auto w = v; w[i] ^= 0x20; if (tg::a2l::parse(w.data(), w.size(), m) == 0) bad++; } CHK(bad == 0, "every corrupted blob rejected (%d slipped through)", bad); }
  { CHK(tg::a2l::parse(v.data(), 20, m) == -4 && tg::a2l::parse(v.data(), 9000, m) == -4, "size limits"); }
  // silence in -> (near) silence out, and the output is always bounded
  CHK(tg::a2l::parse(v.data(), v.size(), m) == 0, "reparse"); tg::a2l::reset(m, st); int32_t lo = 1 << 30, hi = -(1 << 30); for (int i = 0; i < 6000; i++) { int32_t y = tg::a2l::run(m, st, 0); if (i > 5000) { lo = y < lo ? y : lo; hi = y > hi ? y : hi; } }
  CHK(hi - lo < 64, "idle output is steady (only the network's own DC: %d..%d)", lo, hi);
  tg::a2l::reset(m, st); bool ok = true; unsigned s = 1; for (int i = 0; i < 20000; i++) { s = s * 1664525u + 1013904223u; int32_t y = tg::a2l::run(m, st, (int32_t)(s >> 8) - 8388608); ok &= y <= 8388607 && y >= -8388608; }
  CHK(ok, "full-scale noise stays bounded");
  printf("%d checks, %d failed\n", checks, fails); return fails != 0;
}
