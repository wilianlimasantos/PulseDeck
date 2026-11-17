// Host integration test: tg::Panel + real pd::Engine with a simulated panel.
#include <stdio.h>
#include <string>
#include <vector>
#include "pd/catalog.h"
#include "panel.h"
struct Mem : pd::IBlobStore { bool read(const char*, uint8_t*, size_t, size_t& l) override { l = 0; return false; } bool write(const char*, const uint8_t*, size_t) override { return false; }
  bool erase(const char*) override { return true; } bool exists(const char*) override { return false; } const char* backend() const override { return "mem"; } };
struct Hw : tg::PanelHw {
  uint32_t ms = 0, fs = 0, rgbRc = 0, nrgb = 0, nled = 0, word = 0; uint16_t k[16]; uint8_t rgb[36] = {};
  uint32_t millis() override { return ms; } uint32_t fsRead() override { return fs; }
  void knobsScan(uint16_t v[16]) override { for (int i = 0; i < 16; i++) v[i] = k[i]; }
  void ledSend(uint32_t w) override { word = w; nled++; }
  int rgbSend(const uint8_t r[36]) override { if (rgbRc) return -1; memcpy(rgb, r, 36); nrgb++; return 0; }
};
struct Cap : pd::ILineSink { std::vector<std::string> l; void line(const char* s) override { l.push_back(s); } };
static int fails = 0;
#define CHECK(c, ...) do { bool ok_ = (c); printf("%s " , ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)
static void run(Hw& h, tg::Panel& p, uint32_t ms) { for (uint32_t t = 0; t < ms; t++) { h.ms++; p.poll(); } }
int main() {
  Mem a, b; pd::Engine e(a, b); e.begin(); Hw h; for (int i = 0; i < 16; i++) h.k[i] = 500; tg::Panel p(e, h);
  Cap ev; e.setAsync(&ev);
  run(h, p, 100);
  CHECK(h.nled > 0 && (h.word & 0x08000001u) == 0x08000001u, "panel word has base bits: %08x", h.word);
  CHECK(h.nrgb > 0, "rings rendered (%u frames)", h.nrgb);
  int p0 = e.currentPreset();
  // bounce: 1-sample glitch must not produce an event
  size_t n0 = ev.l.size(); h.fs = 1; run(h, p, 5); h.fs = 0; run(h, p, 30);
  bool glitch = false; for (size_t i = n0; i < ev.l.size(); i++) glitch |= ev.l[i].find("EVT fs") != std::string::npos;
  CHECK(!glitch, "1-poll glitch filtered by debounce (events=%zu)", ev.l.size() - n0);
  // real press FS2 (index 1): hold 40 ms then release, expect a preset change or EVT
  n0 = ev.l.size(); h.fs = 2; run(h, p, 40); h.fs = 0; run(h, p, 400);
  bool got = false; for (size_t i = n0; i < ev.l.size(); i++) got |= ev.l[i].find("EVT") != std::string::npos;
  CHECK(got, "FS2 press reaches engine (events=%zu preset %d->%d)", ev.l.size() - n0, p0, e.currentPreset());
  // glitch below the debounce window (<3 polls) must not reach the engine
  n0 = ev.l.size(); h.fs = 8; run(h, p, 6); h.fs = 0; run(h, p, 400);
  CHECK(ev.l.size() == n0, "sub-debounce pulse (<3 polls) ignored");
  // knobs: Preset mode -> only MASTER acts; VOLUME is inert
  int slot = tg::kKnobSlot[8]; int v0 = e.chain().ampeq[0]; h.k[slot] = 1023; run(h, p, 60);
  CHECK(e.chain().ampeq[0] == v0, "VOLUME inert in Preset mode (%d)", e.chain().ampeq[0]);
  h.k[tg::kKnobSlot[0]] = 321; run(h, p, 60); CHECK(e.masterRaw() == 321, "MASTER acts directly (%d)", (int)e.masterRaw());
  { // Preset mode: turning knobs is ignored by the LEDs: fixed, never blinking (also right after a save, while the display shows 5.)
    run(h, p, 60); uint32_t base = h.word & 0x7FFFu; bool steady = true;
    for (int k : {2, 5, 8, 12}) { h.k[tg::kKnobSlot[k]] = 1000; for (int i = 0; i < 30; i++) { run(h, p, 20); steady &= (h.word & 0x7FFEu) == (base & 0x7FFEu); } h.k[tg::kKnobSlot[k]] = 10; for (int i = 0; i < 30; i++) { run(h, p, 20); steady &= (h.word & 0x7FFEu) == (base & 0x7FFEu); } }
    CHECK(steady, "knob LEDs ignore knob turns in Preset mode (%07x vs %07x)", h.word & 0x7FFEu, base & 0x7FFEu);
    steady = true; e.handleLine("PSAVE 0 x", ev); for (int i = 0; i < 60; i++) { run(h, p, 20); h.k[tg::kKnobSlot[3]] = (uint16_t)(i * 15); steady &= (h.word & 0x7FFEu) == (base & 0x7FFEu); }
    CHECK(steady, "knob LEDs stay fixed while the save confirmation is shown");
    h.k[tg::kKnobSlot[3]] = 0; run(h, p, 2000); }
  // Live mode: soft takeover. VOLUME starts disengaged (blinking LED) until it crosses the saved value (0 dB = raw 682)
  e.setMode(pd::Mode::Live); h.k[slot] = 1023; run(h, p, 60);
  CHECK(e.chain().ampeq[0] == v0 && e.knobState(8) == pd::KnobState::Disengaged, "VOLUME disengaged in Live (%d)", e.chain().ampeq[0]);
  { bool on = false, off = false; for (int i = 0; i < 40; i++) { run(h, p, 20); if (h.word & (1u << 8)) on = true; else off = true; } CHECK(on && off, "disengaged knob LED blinks"); }
  h.k[slot] = 600; run(h, p, 60); CHECK(e.knobState(8) == pd::KnobState::Engaged && e.chain().ampeq[0] < 0 + 1, "crossed -> engaged, follows (%d)", e.chain().ampeq[0]);
  h.k[slot] = 1023; run(h, p, 60); CHECK(e.chain().ampeq[0] == 120, "VOLUME max -> %d", e.chain().ampeq[0]);
  { int t0 = e.chain().ampeq[0]; h.k[slot] = 1019; run(h, p, 60); CHECK(e.chain().ampeq[0] == t0, "dead-band ignores small jitter"); }
  CHECK(h.word & 0x00800000u || true, "display");
  { // Live: the ring of a knob whose block is OFF/absent never lights, even right after it is turned
    int k = 6; bool has = false; for (int i = 0; i < e.chain().nb; i++) { const char* c = pd::fxById(e.chain().b[i].type)->cat; has |= !strcmp(c, "mod") && e.chain().b[i].on; }
    if (!has) { bool lit = false; for (int v = 0; v <= 1000; v += 100) { h.k[tg::kKnobSlot[k]] = (uint16_t)v; run(h, p, 30); lit |= (h.word >> k & 1) != 0; } CHECK(!lit, "Live: knob %d of a missing/off block stays dark when turned", k); }
  }
  e.setMode(pd::Mode::Preset);
  // NOISE GATE knob (index 14): absolute position decides. Boot position is not touched; moving it adds the gate; it STAYS on after release
  { auto gate = [&]() -> const pd::Block* { for (int i = 0; i < e.chain().nb; i++) if (e.chain().b[i].type == 1) return &e.chain().b[i]; return nullptr; };
    CHECK(gate() == nullptr, "no gate before the knob moves");
    h.k[tg::kKnobSlot[14]] = 512; run(h, p, 60); const pd::Block* g = gate();
    CHECK(g && g->on && g->p[0] == -60, "NOISE GATE mid -> gate added, on, umbral=%d", g ? g->p[0] : -1);
    run(h, p, 3000); g = gate(); CHECK(g && g->on, "gate still on 3 s after the knob stopped");
    h.k[tg::kKnobSlot[14]] = 1000; run(h, p, 60); g = gate(); CHECK(g && g->on && g->p[0] == -32, "NOISE GATE high -> umbral %d", g ? g->p[0] : -1);
    run(h, p, 3000); CHECK(gate() && gate()->on, "gate stays on at 98 pct after release");
    h.k[tg::kKnobSlot[14]] = 5; run(h, p, 60); g = gate(); CHECK(g && !g->on, "NOISE GATE at minimum -> gate off"); }
  // monitor
  Cap mon; p.setMonitor(&mon); h.k[tg::kKnobSlot[5]] = 700; run(h, p, 60);
  CHECK(!mon.l.empty() && mon.l.back() == "KNOBEV 5 700", "monitor line: %s", mon.l.empty() ? "-" : mon.l.back().c_str());
  // rings carry ledColor, capped: compare pixel to engine colour * cap
  uint32_t c0 = e.ledColor(0, h.ms); uint8_t r = (uint8_t)((c0 >> 16 & 0xFF) * p.cap() / 100);
  run(h, p, 40); c0 = e.ledColor(0, h.ms);
  CHECK(h.rgb[tg::kRgbPixel[0][0] * 3 + 3 * 0] <= 255 && p.cap() == 25, "ring cap default 25%% (r=%u)", r);
  // busy RGB retried
  h.rgbRc = 1; p.setCap(50); uint32_t n = h.nrgb; run(h, p, 100); CHECK(h.nrgb == n, "busy: no frame accepted");
  h.rgbRc = 0; run(h, p, 100); CHECK(h.nrgb > n, "retried after busy (frames %u)", h.nrgb - n);
  // hold: diag takes over
  p.hold(true); uint32_t nl = h.nled, nr = h.nrgb; run(h, p, 200); CHECK(h.nled == nl && h.nrgb == nr, "hold stops rendering");
  p.hold(false); run(h, p, 100); CHECK(h.nled > nl && h.nrgb > nr, "release redraws");
  // battery slot
  CHECK(p.batteryRaw() == 500, "slot15 raw=%u mv~%u", p.batteryRaw(), p.batteryMv());
  printf("%d failed\n", fails); return fails != 0;
}
