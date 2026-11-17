// Tank-G minimal firmware: line protocol over USB CDC through firmware-core's Engine (PING -> PONG).
// UNTESTED ON HARDWARE. Presets live in RAM until a flash IBlobStore is written (see README).
#include <stdint.h>
#include <string.h>
#include "pd/engine.h"
#include "pd/crc.h"
#include "ws_enc.h"
#include "rgb_map.h"
#include "log_store.h"
#include "led_sr.h"
#include "panel.h"
#include "audio.h"
#include "ram_store.h"
#include <stdio.h>
#include <stdlib.h>
extern "C" { int tg_cpu1_start(void); int tg_cpu1_run(void (*)(uint32_t), uint32_t); void tg_cpu1_wait(void); void tg_cpu1_stat(uint32_t*); uint32_t tg_ticks(void); void tg_audio_load(uint32_t*); int tg_audio_set(const char*, uint32_t); void tg_audio_cfg(uint32_t*); int tg_pin_out(uint32_t, uint32_t, uint32_t); int tg_audio_start(void); void tg_audio_stop(void); void tg_audio_stat(uint32_t*); void tg_knobs_scan(uint32_t, uint16_t*); int tg_ws_send(const uint8_t*, uint32_t, uint32_t); void tg_ports_scan(int, uint32_t*); uint32_t tg_fs_read(uint32_t*, uint32_t*); uint32_t tg_millis(void); void tg_enter_uboot(void); uint32_t tg_pa_out(void); void tg_pa_set(uint32_t); void tg_pa_nopull(uint32_t); void tg_pa_dirout(uint32_t); void tg_delay_us(uint32_t); void tg_healthy(void); int tg_fl_read(uint32_t, uint8_t*, uint32_t); int tg_fl_erase(uint32_t); int tg_fl_prog(uint32_t, const uint8_t*, uint32_t); void tg_usb_init(void); void tg_usb_service(void); int tg_usb_getc(void); void tg_usb_putc(char); }
#ifndef TG_HOST_TEST  // freestanding runtime glue (the host test uses libstdc++)
extern "C" void __cxa_pure_virtual() { for (;;) {} }
inline void* operator new(__SIZE_TYPE__, void* p) { return p; }
void operator delete(void*) {}
void operator delete(void*, __SIZE_TYPE__) {}
#endif
namespace {
using PresetRam = tg::RamBlobStore<36, 1100>;   // volatile: lost at power off (flash store still disabled)
using SettingsRam = tg::RamBlobStore<4, 1500>;
#ifdef TG_FLASH_STORE
struct Dev : tg::FlashDev {
  bool read(uint32_t o, uint8_t* d, uint32_t n) override { return tg_fl_read(o, d, n); }
  bool erase4k(uint32_t o) override { return tg_fl_erase(o); }
  bool program(uint32_t o, const uint8_t* s, uint32_t n) override { return tg_fl_prog(o, s, n); }
};
Dev dev; alignas(8) char fsbuf[sizeof(tg::LogStore)];
#endif
struct PaHw : tg::Pa {  // only PA0..PA3 are driven
  uint32_t out() override { return tg_pa_out(); }
  void setOut(uint32_t v) override { tg_pa_set(v); }
  void noPull(uint32_t m) override { tg_pa_nopull(m); }
  void dirOut(uint32_t m) override { tg_pa_dirout(m); }
  void delay() override { tg_delay_us(3); }
};
PaHw pahw; alignas(8) char ledbuf[sizeof(tg::LedSr)]; tg::LedSr* led;
struct Usb : pd::ILineSink { void line(const char* s) override { while (*s) tg_usb_putc(*s++); tg_usb_putc('\r'); tg_usb_putc('\n'); } };
alignas(8) char engbuf[sizeof(pd::Engine)]; PresetRam ps; SettingsRam ss; Usb usb; pd::Engine* engp;
}
static uint8_t wsRgb[36];
static int wsFlush();
namespace { tg::AudioDsp dsp; tg::TgMedia media; bool audioOn = false, audioTone = false; }
#ifndef TG_HOST_TEST
// The capture network's hot loop, copied to RAM at boot (.ram_text): no instruction fetch from the flash/XIP cache while it runs.
// runRaw is force-inlined here and uses no library calls (checked in tools/build.sh), so nothing in it reaches back into the flash.
extern "C" __attribute__((section(".ram_text"), noinline, used)) int64_t a2lRunRam(const tg::a2l::Model& m, tg::a2l::State& s, int32_t x) { return tg::a2l::runRaw(m, s, x); }
#endif
extern "C" void tg_audio_process(const int32_t* in, int32_t* out, uint32_t n) { dsp.process(in, out, n); }
static void audioSync() { tg::AudioParams p; tg::buildParams(*engp, audioTone, p, &media); dsp.setParams(p); }
#if !defined(TG_AUDIO_NO_AUTOSTART) && !defined(TG_AUDIO_AUTOSTART)
#define TG_AUDIO_AUTOSTART 1   // default: AUDIO THRU starts at boot (build with -DTG_AUDIO_NO_AUTOSTART for the old manual start)
#endif
// Factory presets, deliberately different so the DSP can be heard at once (FS A..D = slots 0..3 in Preset mode). Revision kFactoryRev: when it changes
// (or on a blank flash) boot rewrites slots 0..3 ONCE and records the revision; other slots are never touched.
//   0 Clean   : clean amp + chorus + room reverb + 1x12 Clean cab      1 Crunch : overdrive + mid-gain amp + 2x12 Brit cab + GEQ + delay
//   2 Lead    : DS-1 + hi-gain amp + 4x12 Modern cab + GEQ + delay + plate     3 Ambient: clean amp + phaser + long delay + cloud reverb
constexpr uint8_t kFactoryRev = 3;
struct KV { const char* k; int v; };
static void bootPresets(pd::Engine& e, pd::ILineSink& o) {
  auto run = [&](const char* c) { e.handleLine(c, o); };
  auto fx = [&](const char* bank, const char* name, const KV* kv, int n) {   // add a block and set its parameters by catalog name
    char b[48]; snprintf(b, sizeof b, "FXA %s 0 %s", bank, name); run(b);
    const pd::FxDef* d = pd::fxByName(name); if (!d) return;
    unsigned id = 0; for (int i = 0; i < e.chain().nb; i++) if (e.chain().b[i].type == d->id && e.chain().b[i].id > id) id = e.chain().b[i].id;
    for (int j = 0; j < n; j++) for (int k = 0; k < d->np; k++) if (!strcmp(d->p[k].name, kv[j].k)) { snprintf(b, sizeof b, "FXP %u %d %d", id, k, kv[j].v); run(b); }
  };
  run("PNEW"); run("AMPEQ 0 20 0 30 0 -60");
  { const KV c[] = {{"vel_chz", 70}, {"prof_pct", 45}, {"mezcla_pct", 45}}; fx("P", "chorus", c, 3); }
  { const KV r[] = {{"decay_pct", 55}, {"mezcla_pct", 28}, {"predelay_ms", 12}}; fx("Q", "reverb2", r, 3); }
  { const KV c[] = {{"ir_slot", 1}}; fx("Q", "ir", c, 1); }
  run("PSAVE 0 Clean");
  run("PNEW"); run("AMPEQ 0 10 20 10 0 30");
  { const KV d[] = {{"tiempo_ms", 380}, {"realim_pct", 28}, {"mezcla_pct", 24}, {"amort_pct", 40}}; fx("Q", "delay", d, 4); }
  fx("P", "overdrive", nullptr, 0);
  { const KV g[] = {{"g125_db", -2}, {"g800_db", 3}, {"g5k_db", -2}}; fx("Q", "geq", g, 3); }
  { const KV c[] = {{"ir_slot", 2}}; fx("Q", "ir", c, 1); }
  run("PSAVE 1 Crunch");
  run("PNEW"); run("AMPEQ 0 10 20 -10 0 90");
  { const KV d[] = {{"tiempo_ms", 420}, {"realim_pct", 30}, {"mezcla_pct", 20}, {"amort_pct", 50}}; fx("Q", "delay", d, 4); }
  { const KV r[] = {{"decay_pct", 40}, {"mezcla_pct", 18}}; fx("Q", "plate", r, 2); }
  fx("P", "ds1", nullptr, 0);
  { const KV g[] = {{"g50_db", -4}, {"g125_db", -2}, {"g2k_db", 3}, {"g5k_db", -3}}; fx("Q", "geq", g, 4); }
  { const KV c[] = {{"ir_slot", 3}}; fx("Q", "ir", c, 1); }
  run("PSAVE 2 Lead");
  run("PNEW"); run("AMPEQ 0 0 -10 20 0 -90");
  { const KV p[] = {{"vel_chz", 25}, {"prof_pct", 60}, {"realim_pct", 30}}; fx("P", "phaser", p, 3); }
  { const KV d[] = {{"tiempo_ms", 500}, {"realim_pct", 55}, {"mezcla_pct", 40}, {"amort_pct", 50}}; fx("Q", "delay", d, 4); }
  { const KV r[] = {{"decay_pct", 85}, {"mezcla_pct", 50}}; fx("Q", "cloud", r, 2); }
  run("PSAVE 3 Ambient");
  run("PLOAD 0");
#ifndef TG_AUDIO_AUTOSTART
  run("AUDIO OFF");   // engine state matches the hardware: audio starts off
#endif
}
namespace {
struct TgHw : tg::PanelHw {
  uint32_t millis() override { return tg_millis(); }
  uint32_t fsRead() override { uint32_t a, h; return tg_fs_read(&a, &h) & 0xFu; }
  void knobsScan(uint16_t v[16]) override { tg_knobs_scan(8, v); }
  void ledSend(uint32_t w) override { led->send(w); }
  int rgbSend(const uint8_t rgb[36]) override { memcpy(wsRgb, rgb, 36); return wsFlush(); }
};
TgHw tghw; alignas(8) char panelbuf[sizeof(tg::Panel)]; tg::Panel* panel;
}
// Diagnostics: LEDBIT <0..28> lights exactly one word bit; LEDRAW <hex, max 1FFFFFFF> sends a whole word; LEDRAW 0 = all bits 0.
// Returns true if the line was a LED command (handled here, not passed to the engine).
alignas(4) static uint8_t wsBuf[tg::kWsBytes]; static uint32_t wsDiv = 9;   // = stock value, confirmed on the pedal (calibration 2026-11-15, src/rgb_map.h)
static uint8_t wsOne = 0x7E, wsZero = 0x60;
static bool wsPrimed = false;
static int wsFlush() {
  if (!wsPrimed) {   // On the real pedal the first frame right after peripheral init came out with wrong colours; an earlier all-dark frame fixed it (cause not proven)
    uint8_t dark[36] = {0}; tg::wsEncode(dark, wsBuf, wsOne, wsZero); tg_ws_send(wsBuf, tg::kWsBytes, wsDiv); wsPrimed = true;
  }
  tg::wsEncode(wsRgb, wsBuf, wsOne, wsZero); return tg_ws_send(wsBuf, tg::kWsBytes, wsDiv);
}
static uint32_t ledBase = 0;  // OR'd into LEDBIT words only; the stock firmware always sends 0x08000001 (bits 27 and 0)
static bool ledCmd(const char* l, pd::ILineSink& out) {
  char r[128];
  if (!strncmp(l, "AUDIO", 5) && (l[5] == ' ' || !l[5])) {   // AUDIO OFF | THRU (=A2L) | TONE | STAT. Hardware start is INFERRED from the Galaxia (see bsp_glue.c)
    const char* a = l[5] ? l + 6 : "STAT";
    if (!strcmp(a, "STAT")) { uint32_t v[6], ld[3], c1[4] = {}; tg_audio_stat(v); tg_audio_load(ld);
#ifndef TG_HOST_TEST
      tg_cpu1_stat(c1);
#endif
        /* load = time of the last DSP block / the block period; max = worst since the previous STAT */ snprintf(r, sizeof r, "OK AUDIO on=%u tone=%d irqs=%u missed=%u alnk_con0=%04x load=%u/%u us max=%u cpu1=%u/%u/%u", (unsigned)v[0], (int)audioTone, (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)ld[0], (unsigned)ld[2], (unsigned)ld[1], (unsigned)c1[0], (unsigned)c1[1], (unsigned)c1[2]); out.line(r); return true; }
    if (!strcmp(a, "TONE")) { audioTone = true; audioSync(); tg_audio_start(); audioOn = true; out.line("OK AUDIO TONE (440 Hz triangle, about -20 dB)"); return true; }
    if (!strcmp(a, "THRU") || !strcmp(a, "A2L")) { audioTone = false; audioSync(); tg_audio_start(); audioOn = true; return false; }   // the Engine answers "AUDIO 2" (protocol reply)
    if (!strncmp(a, "PIN ", 4)) {   // AUDIO PIN <A..H><0..15> <0|1> : drive a pin as output (bring-up: find reset/mute/amp-enable). Not PA0..3.
      char pc = a[4]; char* e; long b = strtol(a + 5, &e, 10); long v = (e != a + 5 && *e == ' ') ? strtol(e + 1, nullptr, 10) : -1;
      if (pc >= 'a' && pc <= 'h') pc = (char)(pc - 32);
      if (pc < 'A' || pc > 'H' || e == a + 5 || b < 0 || b > 15 || (v != 0 && v != 1) || tg_pin_out((uint32_t)(pc - 'A'), (uint32_t)b, (uint32_t)v)) { out.line("ERR AUDIO PIN usage: AUDIO PIN <A..H><0..15> <0|1> (not PA0..PA3)"); return true; }
      snprintf(r, sizeof r, "OK AUDIO PIN P%c%ld=%ld", pc, b, v); out.line(r); return true;
    }
    if (!strcmp(a, "CFG")) { uint32_t c[5]; tg_audio_cfg(c); snprintf(r, sizeof r, "OK AUDIO CFG CLK=%x CON0=%03x CON1=%04x CON3=%02x LEN=%u", (unsigned)c[0], (unsigned)c[1], (unsigned)c[2], (unsigned)c[3], (unsigned)c[4]); out.line(r); return true; }
    if (!strncmp(a, "SET ", 4)) {   // AUDIO SET <CLK|CON0|CON1|CON3|LEN> <hex> : change an ALNK parameter (restarts audio if it is running)
      char nm[8]; const char* q = a + 4; unsigned k = 0; while (*q && *q != ' ' && k < sizeof nm - 1) nm[k++] = *q++; nm[k] = 0;
      char* e; unsigned long v = (*q == ' ') ? strtoul(q + 1, &e, 16) : 0; bool ok = *q == ' ' && e != q + 1 && !*e && tg_audio_set(nm, (uint32_t)v) == 0;
      if (!ok) { out.line("ERR AUDIO SET usage: AUDIO SET <CLK 0..f|CON0|CON1|CON3|LEN> <hex>"); return true; }
      if (audioOn) { tg_audio_stop(); tg_audio_start(); }
      snprintf(r, sizeof r, "OK AUDIO SET %s=%lx%s", nm, v, audioOn ? " (restarted)" : ""); out.line(r); return true;
    }
    if (!strcmp(a, "OFF")) { tg_audio_stop(); audioOn = false; return false; }   // the Engine answers "AUDIO 0"
    out.line("ERR AUDIO usage: AUDIO OFF|THRU|TONE|STAT|CFG|SET <name> <hex>|PIN <pin> <0|1>"); return true;
  }
  if (panel && !strcmp(l, "PANEL AUTO")) { panel->hold(false); out.line("OK PANEL AUTO (engine drives LEDs/rings again)"); return true; }
  if (panel && !strncmp(l, "PANEL MON", 9)) { panel->setMonitor(l[9] == ' ' && l[10] == '1' ? &usb : nullptr); out.line(panel->monitoring() ? "OK PANEL MON 1 (KNOBEV <idx> <raw>)" : "OK PANEL MON 0"); return true; }
  if (panel && !strncmp(l, "PANEL CAP ", 10)) { panel->setCap(atoi(l + 10)); snprintf(r, sizeof r, "OK PANEL CAP %d", panel->cap()); out.line(r); return true; }
  if (panel && !strcmp(l, "PANEL")) { snprintf(r, sizeof r, "OK PANEL hold=%d cap=%d fs=%x bat_raw=%u mv~%u", (int)panel->held(), panel->cap(), (unsigned)panel->fsStable(), (unsigned)panel->batteryRaw(), (unsigned)panel->batteryMv()); out.line(r); return true; }
  if (panel && (!strncmp(l, "LEDBIT", 6) || !strncmp(l, "LEDRAW", 6) || !strncmp(l, "RGB", 3) || !strncmp(l, "FSRGB", 5))) panel->hold(true);   // manual LED test pauses the engine rendering until PANEL AUTO
  if (!strncmp(l, "RGBDIV", 6) && (l[6] == ' ' || !l[6])) {   // SPI divider for the RGB stream; stock value is 9
    char* e; long d = l[6] ? strtol(l + 7, &e, 10) : -1;
    if (!l[6] || e == l + 7 || *e || d < 1 || d > 255) { out.line("ERR RGBDIV usage: RGBDIV <1..255> (stock 9)"); return true; }
    wsDiv = (uint32_t)d; snprintf(r, sizeof r, "OK RGBDIV %ld", d); out.line(r); return true;
  }
  if (!strncmp(l, "RGBPAT", 6) && (l[6] == ' ' || !l[6])) {   // RGBPAT <one> <zero> : SPI byte for a WS bit 1 / 0 (hex, 00..FF); stock 7E 60. RGBPAT alone = show
    if (l[6]) {
      char *e1, *e0; const char* p1 = l + 7; unsigned long o1 = strtoul(p1, &e1, 16);
      bool ok = e1 != p1 && e1 - p1 <= 2 && *e1 == ' '; unsigned long o0 = 0;
      if (ok) { const char* p0 = e1 + 1; o0 = strtoul(p0, &e0, 16); ok = e0 != p0 && e0 - p0 <= 2 && !*e0; }
      if (!ok) { out.line("ERR RGBPAT usage: RGBPAT <one hex> <zero hex> (stock 7E 60)"); return true; }
      wsOne = (uint8_t)o1; wsZero = (uint8_t)o0;
    }
    snprintf(r, sizeof r, "OK RGBPAT one=%02x zero=%02x", wsOne, wsZero); out.line(r); return true;
  }
  if (!strncmp(l, "KNOBS", 5) && (l[5] == ' ' || !l[5])) {   // KNOBS [<adc channel of mux B>] : 16 raw 10-bit values, mux addr 0..7 on PB1 (ch3) then on the 2nd mux (stock ch8)
    long ch = 8;
    if (l[5]) { char* e; ch = strtol(l + 6, &e, 10); if (e == l + 6 || *e || ch < 0 || ch > 15) { out.line("ERR KNOBS usage: KNOBS [adc channel 0..15, default 8]"); return true; } }
    uint16_t v[16]; tg_knobs_scan((uint32_t)ch, v);
    char b[192]; int n = snprintf(b, sizeof b, "OK KNOBS ch=%ld mux0..7 ch3(PB1)=", ch);
    for (int i = 0; i < 16 && n < (int)sizeof b - 8; i++) {
      if (i == 8) n += snprintf(b + n, sizeof b - n, " ch%ld=", ch);
      else if (i) n += snprintf(b + n, sizeof b - n, ",");
      if (v[i] == 0xFFFF) n += snprintf(b + n, sizeof b - n, "x"); else n += snprintf(b + n, sizeof b - n, "%d", (int)v[i]);
    }
    out.line(b); return true;
  }
  if (!strncmp(l, "FSRGB", 5) && l[5] == ' ') {   // FSRGB <1..4> <RRGGBB> : colour the 3 pixels of one footswitch ring (mapping measured on the pedal)
    char* e; long f = strtol(l + 6, &e, 10); unsigned long c = 0; bool ok = e != l + 6 && f >= 1 && f <= 4 && *e == ' ';
    if (ok) { const char* h = e + 1; char* he; c = strtoul(h, &he, 16); ok = he == h + 6 && !*he; }
    if (!ok) { out.line("ERR FSRGB usage: FSRGB <1..4> <RRGGBB>"); return true; }
    for (int k = 0; k < 3; k++) { int px = tg::kRgbPixel[f - 1][k]; if (px >= 0) { wsRgb[px * 3] = (uint8_t)(c >> 16); wsRgb[px * 3 + 1] = (uint8_t)(c >> 8); wsRgb[px * 3 + 2] = (uint8_t)c; } }
    int st = wsFlush(); snprintf(r, sizeof r, "OK FSRGB %ld %06lx %s", f, c, st ? "busy" : "sent"); out.line(r); return true;
  }
  if (!strcmp(l, "RGBOFF")) { memset(wsRgb, 0, sizeof wsRgb); int st = wsFlush(); snprintf(r, sizeof r, "OK RGBOFF %s", st ? "busy" : "sent"); out.line(r); return true; }
  if (!strncmp(l, "RGB", 3) && l[3] == ' ') {   // RGB <0..11|ALL> <RRGGBB> : set one pixel (or all) and send the whole 12-pixel frame
    char* e; long n; const char* a = l + 4; bool all = false;
    if (!strncmp(a, "ALL ", 4)) { all = true; n = 0; e = (char*)a + 3; } else n = strtol(a, &e, 10);
    unsigned long c = 0; bool ok = (all || (e != a && n >= 0 && n < tg::kWsPixels)) && *e == ' ';
    if (ok) { const char* h = e + 1; char* he; c = strtoul(h, &he, 16); ok = he == h + 6 && !*he; }
    if (!ok) { out.line("ERR RGB usage: RGB <0..11|ALL> <RRGGBB>"); return true; }
    for (int i = 0; i < tg::kWsPixels; i++) if (all || i == n) { wsRgb[i * 3] = (uint8_t)(c >> 16); wsRgb[i * 3 + 1] = (uint8_t)(c >> 8); wsRgb[i * 3 + 2] = (uint8_t)c; }
    int st = wsFlush(); snprintf(r, sizeof r, "OK RGB %s %06lx %s", all ? "ALL" : "px", c, st ? "busy" : "sent"); out.line(r); return true;
  }
  if (!strcmp(l, "FSREAD")) {   // read-only. FS1=PA10 FS2=PH9 FS3=PH8 FS4=PH6 (measured, active low); aux=PH7 is not a footswitch
    uint32_t ra = 0, rh = 0, m = tg_fs_read(&ra, &rh);
    snprintf(r, sizeof r, "OK FSREAD pressed=%02lx fs1=%d fs2=%d fs3=%d fs4=%d aux=%d rawA=%04lx rawH=%04lx", (unsigned long)m, (int)(m & 1), (int)((m >> 1) & 1), (int)((m >> 2) & 1), (int)((m >> 3) & 1), (int)((m >> 4) & 1), (unsigned long)ra, (unsigned long)rh);
    out.line(r); return true;
  }
  if (!strcmp(l, "PORTS") || !strcmp(l, "PORTS PU")) {   // diagnostic read of the input registers of ports A..H; "PU" = temporary DIE+pull-up, restored after
    uint32_t v[8]; tg_ports_scan(l[5] != 0, v);
    snprintf(r, sizeof r, "OK PORTS %s A=%04lx B=%04lx C=%04lx D=%04lx E=%04lx F=%04lx G=%04lx H=%04lx", l[5] ? "PU" : "RAW", (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2], (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5], (unsigned long)v[6], (unsigned long)v[7]);
    out.line(r); return true;
  }
  if (!strncmp(l, "LEDBASE", 7) && (l[7] == ' ' || !l[7])) {
    char* e; unsigned long b = l[7] ? strtoul(l + 8, &e, 16) : 0xFFFFFFFFul;
    if (!l[7] || e == l + 8 || *e || b > 0x1FFFFFFFul) { out.line("ERR LEDBASE usage: LEDBASE <hex, <=1FFFFFFF>"); return true; }
    ledBase = (uint32_t)b; snprintf(r, sizeof r, "OK LEDBASE %08lx", b); out.line(r); return true;
  }
  if (!strcmp(l, "UBOOT")) { out.line("OK UBOOT"); tg_enter_uboot(); return true; }   // recovery: re-enter the mask-ROM bootloader
  if (!strncmp(l, "LEDBIT", 6) && (l[6] == ' ' || !l[6])) {
    char* e; long n = l[6] ? strtol(l + 7, &e, 10) : -1;
    if (!l[6] || e == l + 7 || *e || n < 0 || n > 28) { out.line("ERR LEDBIT usage: LEDBIT <0..28>"); return true; }
    uint32_t w = (1u << n) | ledBase; led->send(w); snprintf(r, sizeof r, "OK LEDBIT %ld word=%08lx", n, (unsigned long)w); out.line(r); return true;
  }
#ifdef TG_HOST_SIM
  if (!strcmp(l, "XAUDIO")) {   // host simulator only: push a 196 Hz triangle (0.1 FS) through the real DSP and report level + CRC of the output
    static int32_t in[256], o[256]; uint32_t crc = pd::kCrc32Init, ph = 0; uint64_t e = 0;
    dsp.~AudioDsp(); new (&dsp) tg::AudioDsp(); audioSync();   // fresh DSP state: the answer depends only on the chain, not on what played before
    for (int blk = 0; blk < 96; blk++) {
      for (int f = 0; f < 128; f++) { ph += 17538930u; int32_t t = (int32_t)(ph >> 8) - (1 << 23); if (t < 0) t = -t; in[f * 2] = (t - (1 << 22)) / 5; in[f * 2 + 1] = 0; }
      tg_audio_process(in, o, 128);
      for (int f = 0; f < 128; f++) { int32_t v = o[f * 2]; if (blk >= 48) e += (uint64_t)((int64_t)v * v >> 16); crc = pd::crc32_update(crc, (const uint8_t*)&v, 4); }
    }
    snprintf(r, sizeof r, "XAUDIO energy=%llu crc=%08x", (unsigned long long)e, (unsigned)pd::crc32_final(crc)); out.line(r); return true;
  }
#endif
  if (!strcmp(l, "CPUBENCH")) {   // micro benchmarks with audio stopped: cycles of a loop, int32 / int64 multiply-accumulate, sequential and strided reads
#ifndef TG_HOST_TEST
    int32_t* buf = media.scratch(); for (int i = 0; i < 16384; i++) buf[i] = i * 7;
    if (audioOn) tg_audio_stop();
    volatile int32_t sink32; volatile int64_t sink64; uint32_t t0, tl, t32, t64, tseq, tstr; const int N = 200000;
    t0 = tg_ticks(); { int32_t a = 1; for (int i = 0; i < N; i++) { a += i; __asm__ volatile("" : "+r"(a)); } sink32 = a; } tl = tg_ticks() - t0;
    t0 = tg_ticks(); { int32_t a = 1, b = 3; for (int i = 0; i < N; i++) { a += b * i; __asm__ volatile("" : "+r"(a)); } sink32 = a; } t32 = tg_ticks() - t0;
    t0 = tg_ticks(); { int64_t a = 1; int32_t b = 3; for (int i = 0; i < N; i++) { a += (int64_t)b * i; __asm__ volatile("" : "+r"(a)); } sink64 = a; } t64 = tg_ticks() - t0;
    t0 = tg_ticks(); { int32_t a = 0; for (int r = 0; r < 12; r++) for (int i = 0; i < 16384; i++) a += buf[i]; sink32 = a; } tseq = tg_ticks() - t0;
    t0 = tg_ticks(); { int32_t a = 0; for (int r = 0; r < 12; r++) for (int j = 0; j < 1536; j++) for (int i = j; i < 16384; i += 1536) a += buf[i]; sink32 = a; } tstr = tg_ticks() - t0;
    if (audioOn) tg_audio_start();
    (void)sink32; (void)sink64;
    // ns per iteration x10
    auto ns = [](uint32_t t, uint32_t n) { return (unsigned)((uint64_t)t * 10000u / 24u / n); };
    snprintf(r, sizeof r, "CPUBENCH ns_x10: loop=%u mac32=%u mac64=%u read_seq=%u read_stride6k=%u", ns(tl, N), ns(t32, N), ns(t64, N), ns(tseq, 196608), ns(tstr, 196608)); out.line(r);
#endif
    return true;
  }
  if (!strncmp(l, "A2LBENCH", 8)) {   // A2LBENCH [RAM|FLASH|NOIRQ]: time the last uploaded capture with the loop in RAM or in flash, optionally with audio stopped
#ifndef TG_HOST_TEST
    const char* a = l[8] ? l + 9 : "RAM"; bool noirq = !strcmp(a, "NOIRQ");
    tg::a2l::g_run = !strcmp(a, "FLASH") ? tg::a2l::runFlash : a2lRunRam;
    if (noirq && audioOn) tg_audio_stop();
    uint32_t us = media.rebench();
    if (noirq && audioOn) tg_audio_start();
    tg::a2l::g_run = a2lRunRam;
    snprintf(r, sizeof r, "A2LBENCH %s us_per_block=%u ns_per_mac=%u", a, (unsigned)us, (unsigned)(media.nsPerMacX1000() / 1000)); out.line(r);
#endif
    return true;
  }
  if (!strcmp(l, "CAPS")) {   // what this firmware can play/keep: the editor compacts a capture that is heavier than this before sending it
    snprintf(r, sizeof r, "CAPS a2l_macs=%d a2l_arena=%d a2l_layers=%d a2l_blob=%d ir_taps=%d a2l_ns_per_mac=%u.%03u a2l_us_last=%u", (int)media.macLimit(), tg::a2l::kArenaWords, tg::a2l::kMaxLayers, (int)tg::LogStore::kMaxData, tg::dsp::kIrMaxTaps, (unsigned)(media.nsPerMacX1000() / 1000), (unsigned)(media.nsPerMacX1000() % 1000), (unsigned)media.lastUsPerBlock());
    out.line(r); return true;
  }
  if (!strcmp(l, "SAVE")) {   // save the live chain (incl. knob/Live edits) into the CURRENT preset slot; the display shows 5. (ok) or E (error)
    int sl = engp->currentPreset(); char b[64];
    if (sl < 0) { out.line("ERR SAVE no current preset (PLOAD one first)"); return true; }
    snprintf(b, sizeof b, "PSAVE %d %s", sl, engp->info(sl).used ? engp->info(sl).name : "");
    struct Last : pd::ILineSink { char t[64] = ""; void line(const char* x) override { snprintf(t, sizeof t, "%s", x); } } cap;
    engp->handleLine(b, cap);
    if (!strncmp(cap.t, "PSAVE ok", 8)) snprintf(r, sizeof r, "SAVE ok slot=%d", sl); else snprintf(r, sizeof r, "ERR SAVE flash write failed");   // reply starts with the command name (protocol rule)
    out.line(r); return true;
  }
  if (!strncmp(l, "LEDRAW", 6) && (l[6] == ' ' || !l[6])) {
    char* e; unsigned long w = l[6] ? strtoul(l + 7, &e, 16) : 0xFFFFFFFFul;
    if (!l[6] || e == l + 7 || *e || w > 0x1FFFFFFFul) { out.line("ERR LEDRAW usage: LEDRAW <hex, <=1FFFFFFF>"); return true; }
    led->send((uint32_t)w); snprintf(r, sizeof r, "OK LEDRAW word=%08lx", w); out.line(r); return true;
  }
  return false;
}
extern "C" void tg_main() {
  static char buf[200]; unsigned n = 0;
  // crt0 does not run C++ global constructors (.init_array is never walked): objects with vtables / member initialisers are built here
  new (&dsp) tg::AudioDsp(); new (&media) tg::TgMedia();
#ifndef TG_HOST_TEST
  tg::g_audioPause = [](bool p) { if (!audioOn) return; if (p) tg_audio_stop(); else tg_audio_start(); };
  tg::g_ticks = tg_ticks; tg::g_ticksPerUs = 24; tg::a2l::g_run = a2lRunRam;   // measured limits + the capture network running from RAM
  if (tg_cpu1_start() == 0) {   // second core up: effects there, the capture network alone on this core (see AudioDsp::process)
    dsp.other.run = [](void (*fn)(uint32_t), uint32_t a) { return tg_cpu1_run(fn, a) != 0; };
    dsp.other.wait = [] { tg_cpu1_wait(); };
    tg::g_capShareUs = 2200;   // this core only runs the network now (of a 2666 us block; the rest is the ISR and the hand-over)
  }
#endif
  tg_usb_init(); led = new (ledbuf) tg::LedSr(pahw);
#ifdef TG_FLASH_STORE
  pd::IBlobStore* fs = new (fsbuf) tg::LogStore(dev, TG_STORE_OFF);
  uint8_t rev = 0; size_t rn = 0;
  bool firstBoot = !(fs->read("fver", &rev, 1, rn) && rn == 1 && rev == kFactoryRev);   // blank flash or new factory revision: rewrite slots 0..3 once
  engp = new (engbuf) pd::Engine(*fs, *fs);
#else
  bool firstBoot = true;
  engp = new (engbuf) pd::Engine(ps, ss);
#endif
  static const char* const kCabNames[8] = {"1x12 Clean", "2x12 Brit", "4x12 Modern", "1x10 Open", "4x12 Vintage", "Bright Combo", "Dark Cab", "Studio"};   // = tg::kCabs order
  engp->setBuiltinIrNames(kCabNames, 8);
#ifdef TG_FLASH_STORE
  media.attach(fs);
#else
  media.attach(&ps);
#endif
  engp->setMedia(&media);
  engp->begin();
  { tg::NullSink ns; if (firstBoot) { bootPresets(*engp, ns);
#ifdef TG_FLASH_STORE
      const uint8_t r = kFactoryRev; fs->write("fver", &r, 1);
#endif
    }
#ifndef TG_AUDIO_AUTOSTART
    else engp->handleLine("AUDIO OFF", ns);
#endif
  }
#ifdef TG_AUDIO_AUTOSTART
  audioSync(); tg_audio_start(); audioOn = true;
#endif
  panel = new (panelbuf) tg::Panel(*engp, tghw); uint32_t t = 0;
  for (;;) {
    tg_usb_service(); if (++t == 200000u) tg_healthy();
    panel->poll();
    if (audioOn) { static uint32_t la; uint32_t m = tg_millis(); if (m - la >= 20) { la = m; audioSync(); } }
    int c;
    while ((c = tg_usb_getc()) >= 0) {
      if (c == '\n' || c == '\r') { buf[n] = 0; if (n && !ledCmd(buf, usb)) engp->handleLine(buf, usb); n = 0; }
      else if (n < sizeof buf - 1) buf[n++] = (char)c;
    }
  }
}
#ifndef TG_HOST_TEST
extern "C" { void* __dso_handle; int __cxa_atexit(void (*)(void*), void*, void*) { return 0; } }
#endif
