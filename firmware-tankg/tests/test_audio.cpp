// Host test of tg::AudioDsp: gain table, passthrough, gate, boost, clipping, tone, bypass, clamping.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "audio.h"
static int fails = 0;
#define CHECK(c, ...) do { bool ok_ = (c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)
static const uint32_t N = 256;
static int32_t in[N * 2], out[N * 2];
static int32_t peak() { int32_t m = 0; for (uint32_t i = 64; i < N; i++) { int32_t v = abs(out[i * 2]); if (v > m) m = v; } return m; }
static void sine(int32_t amp) { for (uint32_t i = 0; i < N; i++) { in[i * 2] = (int32_t)(amp * sin(i * 0.2)); in[i * 2 + 1] = 0; } }
static tg::AudioParams P(uint8_t type, int16_t p0 = 50, int16_t p1 = 50, int16_t p2 = 50) { tg::AudioParams p; p.n = 1; p.b[0].type = type; p.b[0].on = 1; p.b[0].p[0] = p0; p.b[0].p[1] = p1; p.b[0].p[2] = p2; return p; }
int main() {
  CHECK(tg::dbQ12(0) == 4096 && tg::dbQ12(-30) == 130 && tg::dbQ12(12) == 16306 && tg::dbQ12(24) == 64917 && tg::dbQ12(99) == 64917 && tg::dbQ12(-99) == 130, "dB table");
  CHECK(tg::gainQ8(0) == 256 && tg::gainQ8(40) > 25000 && tg::gainQ8(40) < 26200 && tg::dbToAmp(0) > 8380000 && tg::dbToAmp(-90) >= 1 && tg::dbToAmp(-50) > 25000 && tg::dbToAmp(-50) < 28000, "gainQ8 / dbToAmp (g40=%d a-50=%d)", tg::gainQ8(40), tg::dbToAmp(-50));
  static tg::AudioDsp d; tg::AudioParams p0; d.setParams(p0); sine(1000000); d.process(in, out, N);
  bool same = true; for (uint32_t i = 0; i < N; i++) same &= abs(out[i * 2] - in[i * 2]) <= 1 && out[i * 2] == out[i * 2 + 1];
  CHECK(same, "empty chain = passthrough, both channels");
  { tg::AudioParams p = P(2, 0); p.bypass = 1; d.setParams(p); d.process(in, out, N); CHECK(abs(out[200] - in[200]) <= 1, "engine bypass skips blocks"); }
  { tg::AudioParams p = P(2, 24); d.setParams(p); d.process(in, out, N); int32_t pk = peak(); CHECK(pk > 6 * 1000000 && pk <= 8388608, "boost +24 dB raises ~16x and clamps (peak %d)", pk); }
  { tg::AudioParams p = P(2, 6); p.b[0].on = 0; d.setParams(p); d.process(in, out, N); CHECK(abs(out[200] - in[200]) <= 1, "disabled block ignored"); }
  { tg::AudioParams p = P(10, 80, 50, -6); d.setParams(p); sine(100000); for (int k = 0; k < 8; k++) d.process(in, out, N); int32_t pk = peak(); CHECK(pk > 2000000 && pk < 8388608, "distortion lifts a quiet signal and stays below full scale (%d)", pk); }
  { tg::AudioParams p = P(3, 40, 9000, 0); d.setParams(p); sine(8000000); for (int k = 0; k < 8; k++) d.process(in, out, N); int32_t pk = peak(); CHECK(pk <= 8388608 && pk > 0, "overdrive loud input bounded (%d)", pk); }
  { tg::AudioParams p = P(1, -50, 120); static tg::AudioDsp g; g.setParams(p); sine(2000); for (int k = 0; k < 30; k++) g.process(in, out, N); int32_t lo = peak(); sine(2000000); for (int k = 0; k < 30; k++) g.process(in, out, N); int32_t hi = peak();
    CHECK(lo < 200 && hi > 1500000, "gate closes on quiet (%d) and opens on loud (%d)", lo, hi); }
  { tg::AudioParams p; p.gin = tg::dbQ12(-30); p.gout = tg::dbQ12(12); d.setParams(p); sine(4000000); d.process(in, out, N); int32_t pk = peak(); CHECK(pk > 400000 && pk < 700000, "gain in -30 dB, out +12 dB ~ -18 dB (%d)", pk); }
  { tg::AudioParams p; p.mode = tg::kAuTone; d.setParams(p); for (int k = 0; k < 4; k++) d.process(in, out, N); int32_t pk = peak(); int zc = 0; for (uint32_t i = 1; i < N; i++) zc += (out[i * 2] < 0) != (out[(i - 1) * 2] < 0);
    CHECK(pk > 500000 && pk < 3000000 && zc >= 1, "tone present (peak %d, zero crossings %d in %u frames)", pk, zc, N); }
  { for (uint32_t i = 0; i < N; i++) { in[i * 2] = 0x00FFFFFF; } tg::AudioParams p; d.setParams(p); d.process(in, out, N); CHECK(out[0] == -1, "24 bit sign extension of raw input (0xFFFFFF -> -1, got %d)", out[0]); }
  CHECK(tg::masterQ12(1023) == 4096 && tg::masterQ12(0) == 0 && tg::masterQ12(512) > 900 && tg::masterQ12(512) < 1100, "MASTER taper: unity at max, mute at 0, about -12 dB at half (%d)", (int)tg::masterQ12(512));
  printf("%d failed\n", fails); return fails != 0;
}
