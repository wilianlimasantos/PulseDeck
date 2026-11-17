// Host test of LEDBIT/LEDRAW parsing in src/main.cpp with simulated PA0..PA3.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
static uint32_t g_o, g_dir; static std::vector<int> g_bits; static uint32_t g_words;
static uint32_t g_fs = 0;
static uint32_t g_kch; static uint16_t g_kv[16] = {0,10,20,30,40,50,60,1023, 100,101,102,103,104,105,106,0xFFFF};
static uint8_t g_ws[324]; static int g_wsN, g_wsRc; static uint32_t g_wsDiv;
static uint32_t g_ms; static int g_au, g_pin = -1; static uint32_t g_ast[6]; static uint32_t g_cfg[5] = {1, 0x180, 0xD5, 0x83, 256}; extern "C" { void tg_audio_load(uint32_t* o) { o[0] = 700; o[1] = 900; o[2] = 2666; } int tg_audio_set(const char* n, uint32_t v) { if (!strcmp(n, "CLK") && v < 16) { g_cfg[0] = v; return 0; } if (!strcmp(n, "CON1")) { g_cfg[2] = v; return 0; } return -1; } void tg_audio_cfg(uint32_t* o) { for (int i = 0; i < 5; i++) o[i] = g_cfg[i]; } int tg_pin_out(uint32_t p, uint32_t b, uint32_t v) { if (p > 7 || b > 15 || (p == 0 && b < 4)) return -1; g_pin = p * 100 + b * 2 + v; return 0; } int tg_audio_start(void) { g_au = 1; return 0; } void tg_audio_stop(void) { g_au = 0; } void tg_audio_stat(uint32_t* o) { for (int i = 0; i < 6; i++) o[i] = g_ast[i]; } uint32_t tg_millis(void) { return g_ms; } void tg_knobs_scan(uint32_t ch, uint16_t* o) { g_kch = ch; for (int i = 0; i < 16; i++) o[i] = g_kv[i]; } int tg_ws_send(const uint8_t* b, uint32_t n, uint32_t d) { memcpy(g_ws, b, n < 324 ? n : 324); g_wsN++; g_wsDiv = d; return g_wsRc; } void tg_ports_scan(int pu, uint32_t* o) { for (int i = 0; i < 8; i++) o[i] = pu ? 0xFFF0u + i : 0x0100u + i; } uint32_t tg_fs_read(uint32_t* a, uint32_t* h) { if (a) *a = 0xFBFF; if (h) *h = 0xFD3F; return g_fs; } uint32_t tg_pa_out(void) { return g_o; }
 void tg_pa_set(uint32_t v) { if (!(g_dir & 4) && !(g_o & 4) && (v & 4)) g_bits.push_back(!(v & 8)); if (!(g_dir & 1) && !(g_o & 1) && (v & 1)) g_words++; g_o = (g_o & ~0xFu) | (v & 0xF); }
 void tg_pa_nopull(uint32_t) {} void tg_pa_dirout(uint32_t m) { g_dir &= ~m; } void tg_delay_us(uint32_t) {} void tg_healthy(void) {} static int g_uboot; void tg_enter_uboot(void) { g_uboot++; }
 void tg_usb_init(void) {} void tg_usb_service(void) {} int tg_usb_getc(void) { return -1; } void tg_usb_putc(char) {} }
#define TG_HOST_TEST 1
#include <new>
#include "../src/main.cpp"
struct Cap : pd::ILineSink { std::string last; void line(const char* s) override { last = s; } };
static int fails = 0;
static uint32_t lastWord() { uint32_t r = 0; for (size_t i = g_bits.size() >= 29 ? g_bits.size() - 29 : 0; i < g_bits.size(); i++) r = (r << 1) | g_bits[i]; return r; }
static void t(const char* cmd, bool handled, const char* prefix, long word = -1) {
  Cap c; g_o = 0; g_dir = 0xFFFF; size_t before = g_bits.size(); led = new (ledbuf) tg::LedSr(pahw);   // fresh driver per case
  bool h = ledCmd(cmd, c); bool ok = h == handled && c.last.compare(0, strlen(prefix), prefix) == 0;
  if (word >= 0) ok = ok && g_bits.size() - before == 29 && lastWord() == (uint32_t)word;
  if (word < 0 && handled) ok = ok && g_bits.size() == before;       // errors must not touch the pins
  printf("%s %-22s -> %s\n", ok ? "ok  " : "FAIL", cmd, c.last.c_str()); fails += !ok;
}
static void seq() {   // regression: successive words on ONE driver must each end with a PA0 rising edge (stock lowers LATCH every call)
  Cap c; g_o = 0; g_dir = 0xFFFF; g_words = 0; led = new (ledbuf) tg::LedSr(pahw);
  ledCmd("LEDRAW 1FFFFFFF", c); ledCmd("LEDRAW 0", c); ledCmd("LEDRAW 5", c); ledCmd("LEDRAW 5", c);  // last one is a cached repeat: no edge
  bool ok = g_words == 3 && (g_o & 1) && !(g_o & 2) && lastWord() == 5;
  printf("%s sequence latch edges=%u (want 3), PA0 high, PA1 low, word=%x\n", ok ? "ok  " : "FAIL", g_words, lastWord()); fails += !ok;
}
int main() {
  for (int n = 0; n <= 28; n++) { char b[32]; snprintf(b, sizeof b, "LEDBIT %d", n); char e[32]; snprintf(e, sizeof e, "OK LEDBIT %d", n); t(b, true, e, 1L << n); }
  t("LEDRAW 086d0009", true, "OK LEDRAW word=086d0009", 0x086d0009); t("LEDRAW 0", true, "OK LEDRAW word=00000000", 0); t("LEDRAW 1FFFFFFF", true, "OK LEDRAW word=1fffffff", 0x1FFFFFFF);
  t("LEDBIT 29", true, "ERR LEDBIT"); t("LEDBIT -1", true, "ERR LEDBIT"); t("LEDBIT", true, "ERR LEDBIT"); t("LEDBIT 5x", true, "ERR LEDBIT"); t("LEDBIT  7", true, "OK LEDBIT 7", 1L << 7);
  t("LEDRAW", true, "ERR LEDRAW"); t("LEDRAW 20000000", true, "ERR LEDRAW"); t("LEDRAW xyz", true, "ERR LEDRAW"); t("LEDRAW 12 34", true, "ERR LEDRAW");
  t("LEDBASE 08000001", true, "OK LEDBASE 08000001"); t("LEDBIT 5", true, "OK LEDBIT 5", (1L << 5) | 0x08000001L);
  t("LEDBASE 0", true, "OK LEDBASE 00000000"); t("LEDBIT 5", true, "OK LEDBIT 5", 1L << 5);
  t("LEDBASE", true, "ERR LEDBASE"); t("LEDBASE zz", true, "ERR LEDBASE"); t("LEDBASE 20000000", true, "ERR LEDBASE"); t("LEDRAW 10", true, "OK LEDRAW", 0x10);
  seq();
  t("KNOBS", true, "OK KNOBS ch=8 mux0..7 ch3(PB1)=0,10,20,30,40,50,60,1023 ch8=100,101,102,103,104,105,106,x");
  { bool ok = g_kch == 8; printf("%s KNOBS default channel is 8 (%u)\n", ok ? "ok  " : "FAIL", g_kch); fails += !ok; }
  t("KNOBS 5", true, "OK KNOBS ch=5 mux0..7 ch3(PB1)=0,10,20,30,40,50,60,1023 ch5=");
  { bool ok = g_kch == 5; printf("%s KNOBS 5 passes channel 5 (%u)\n", ok ? "ok  " : "FAIL", g_kch); fails += !ok; }
  t("KNOBS 15", true, "OK KNOBS ch=15"); t("KNOBS 16", true, "ERR KNOBS"); t("KNOBS -1", true, "ERR KNOBS"); t("KNOBS x", true, "ERR KNOBS"); t("KNOBS 3 4", true, "ERR KNOBS"); t("KNOBSX", false, "");
  { Cap c; ledCmd("KNOBS", c); bool ok = strlen(c.last.c_str()) < 199; printf("%s KNOBS reply fits the 199-char line limit (%zu)\n", ok ? "ok  " : "FAIL", c.last.size()); fails += !ok; }
  { g_wsN = 0; ledCmd("RGB 5 0000ff", *new Cap); ledCmd("RGB 6 0000ff", *new Cap);
    bool ok = g_wsN == 3; printf("%s first frame is preceded by one dark frame (sends=%d, want 3)\n", ok ? "ok  " : "FAIL", g_wsN); fails += !ok; }
  t("FSRGB 2 00ff00", true, "OK FSRGB 2 00ff00 sent");
  { Cap c; ledCmd("RGBOFF", c); ledCmd("FSRGB 4 ff0000", c); bool ok = g_ws[36 + 9 * 24 + 8] == 0x7E && g_ws[36 + 11 * 24 + 8] == 0x7E && g_ws[36 + 8 * 24 + 8] == 0x60 && g_ws[36 + 9 * 24] == 0x60;
    printf("%s FSRGB 4 lights pixels 9..11 only (R byte-group 0x7E, G 0x60)\n", ok ? "ok  " : "FAIL"); fails += !ok; }
  t("FSRGB 0 ff0000", true, "ERR FSRGB"); t("FSRGB 5 ff0000", true, "ERR FSRGB"); t("FSRGB 1 zz", true, "ERR FSRGB"); t("FSRGB 1", true, "ERR FSRGB"); t("FSRGBX 1 ff0000", false, "");
  t("RGB 0 FF0000", true, "OK RGB px ff0000 sent"); t("RGB 11 0000ff", true, "OK RGB px 0000ff sent"); t("RGB ALL 010203", true, "OK RGB ALL 010203 sent");
  t("RGB 12 ff0000", true, "ERR RGB"); t("RGB -1 ff0000", true, "ERR RGB"); t("RGB 0 ff00", true, "ERR RGB"); t("RGB 0 ff00000", true, "ERR RGB"); t("RGB 0 gg0000", true, "ERR RGB"); t("RGB", false, ""); t("RGBX 1", false, "");
  { int n0 = g_wsN; Cap c; ledCmd("RGB 1 FF8000", c); ledCmd("RGB 3 0000FF", c); bool ok = g_wsN == n0 + 2 && g_ws[36 + 24 + 0] == 0x7E && g_ws[36 + 24 + 8] == 0x7E && g_ws[36 + 24 + 16] == 0x60 && g_ws[36 + 3 * 24 + 16] == 0x7E && g_ws[36 + 3 * 24 + 8] == 0x60;
    printf("%s RGB frames keep earlier pixels (px1 G=ff? no: px1=FF8000 -> G=80 R=FF)\n", ok ? "ok  " : "FAIL"); fails += !ok; }
  g_wsRc = -1; t("RGB 2 00ff00", true, "OK RGB px 00ff00 busy"); g_wsRc = 0;
  t("RGBPAT", true, "OK RGBPAT one=7e zero=60"); t("RGBPAT F8 C0", true, "OK RGBPAT one=f8 zero=c0"); t("RGBPAT 1 2", true, "OK RGBPAT one=01 zero=02");
  { Cap c; ledCmd("RGB 0 800000", c); bool ok = g_ws[36 + 8] == 0x01 && g_ws[36 + 9] == 0x02; printf("%s RGBPAT changes the encoded bytes\n", ok ? "ok  " : "FAIL"); fails += !ok; }
  t("RGBPAT 100 60", true, "ERR RGBPAT"); t("RGBPAT zz 60", true, "ERR RGBPAT"); t("RGBPAT 7E", true, "ERR RGBPAT"); t("RGBPAT 7E 60 1", true, "ERR RGBPAT"); t("RGBPAT 7e 60", true, "OK RGBPAT one=7e zero=60");
  t("RGBOFF", true, "OK RGBOFF sent"); t("RGBDIV 7", true, "OK RGBDIV 7"); t("RGBDIV 0", true, "ERR RGBDIV"); t("RGBDIV 256", true, "ERR RGBDIV"); t("RGBDIV", true, "ERR RGBDIV");
  { Cap c; ledCmd("RGB 0 ffffff", c); printf("%s divider passed to driver (%u)\n", g_wsDiv == 7 ? "ok  " : "FAIL", g_wsDiv); fails += g_wsDiv != 7; }
  t("PORTS", true, "OK PORTS RAW A=0100 B=0101 C=0102 D=0103 E=0104 F=0105 G=0106 H=0107"); t("PORTS PU", true, "OK PORTS PU A=fff0 B=fff1 C=fff2 D=fff3 E=fff4 F=fff5 G=fff6 H=fff7"); t("PORTS X", false, "");
  g_fs = 0; t("FSREAD", true, "OK FSREAD pressed=00 fs1=0 fs2=0 fs3=0 fs4=0 aux=0 rawA=fbff rawH=fd3f");
  g_fs = 0x0A; t("FSREAD", true, "OK FSREAD pressed=0a fs1=0 fs2=1 fs3=0 fs4=1 aux=0"); g_fs = 0x11; t("FSREAD", true, "OK FSREAD pressed=11 fs1=1 fs2=0 fs3=0 fs4=0 aux=1"); g_fs = 0;
  t("FSREAD x", false, "");
  t("PING", false, ""); t("LEDBITS 3", false, ""); t("LEDRAWX", false, "");
  { Cap c; bool h = ledCmd("UBOOT", c); int ok = h && g_uboot == 1 && c.last == "OK UBOOT"; printf("%s UBOOT -> %s\n", ok ? "ok  " : "FAIL", c.last.c_str()); fails += !ok; }
  { Cap c; ledCmd("UBOOTX", c); ledCmd("uboot", c); int ok = g_uboot == 1; printf("%s UBOOT is exact-match only\n", ok ? "ok  " : "FAIL"); fails += !ok; }
  { // boot: default presets from RAM store, engine state consistent, AUDIO commands
    pd::Engine e(ps, ss); e.begin(); Cap c; engp = &e; bootPresets(e, c);
    { const char* nm[4] = {"Clean", "Crunch", "Lead", "Ambient"}; int want[4] = {3, 4, 5, 3}, cab[4] = {1, 2, 3, 0}, geq[4] = {0, 1, 1, 0}; bool ok = true; char b[24];
      for (int i = 0; i < 4; i++) { snprintf(b, sizeof b, "PLOAD %d", i); e.handleLine(b, c); int slot = 0, ng = 0;
        for (int k = 0; k < e.chain().nb; k++) { ok &= e.chain().b[k].on; if (e.chain().b[k].type == 7) slot = e.chain().b[k].p[2]; if (e.chain().b[k].type == 36) ng++; }
        bool good = !strcmp(e.info(i).name, nm[i]) && e.chain().nb == want[i] && slot == cab[i] && ng == geq[i] && e.cpuUsed() <= pd::kCpuBudget && e.memUsed() <= pd::kFxMemBudget;
        if (!good) printf("  preset %d: name=%s nb=%d cab=%d geq=%d cpu=%u mem=%u\n", i, e.info(i).name, e.chain().nb, slot, ng, e.cpuUsed(), e.memUsed()); ok &= good; }
      e.handleLine("PLOAD 0", c); printf("%s factory presets: 4 different chains (Clean/Crunch/Lead with an active IR Cab, Crunch/Lead with GEQ) inside the CPU/RAM budget\n", ok ? "ok  " : "FAIL"); fails += !ok; }
    { Cap sc; bool h = ledCmd("SAVE", sc); bool ok = h && sc.last == "SAVE ok slot=0" && e.saveSeq() > 0 && e.saveOk(); printf("%s SAVE -> %s\n", ok ? "ok  " : "FAIL", sc.last.c_str()); fails += !ok; }
    int ok = e.currentPreset() == 0 && !g_au;   // autostart itself runs in tg_main (needs hardware) printf("%s boot loads slot 0 (preset=%d), audio off\n", ok ? "ok  " : "FAIL", e.currentPreset()); fails += !ok;
    Cap a; ledCmd("AUDIO THRU", a); ok = g_au == 1 && !ledCmd("AUDIO THRU", a);   /* hardware started; the Engine then answers "AUDIO 2" */ printf("%s AUDIO THRU -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    ledCmd("AUDIO TONE", a); ok = g_au == 1 && a.last.compare(0, 13, "OK AUDIO TONE") == 0; printf("%s AUDIO TONE\n", ok ? "ok  " : "FAIL"); fails += !ok;
    g_ast[1] = 7; ledCmd("AUDIO STAT", a); ok = a.last.find("irqs=7") != std::string::npos; printf("%s AUDIO STAT -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    ledCmd("AUDIO PIN A9 0", a); ok = g_pin == 18 && a.last == "OK AUDIO PIN PA9=0"; printf("%s AUDIO PIN A9 0 -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    ledCmd("AUDIO PIN c5 1", a); ok = g_pin == 211 && a.last == "OK AUDIO PIN PC5=1"; printf("%s AUDIO PIN c5 1 -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    g_pin = -1; ledCmd("AUDIO PIN A2 1", a); ok = g_pin == -1 && a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO PIN A2 refused\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO PIN Z1 1", a); ok = a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO PIN Z1 refused\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO PIN A9 2", a); ok = a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO PIN A9 2 refused\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO SET CLK 0", a); ok = g_cfg[0] == 0 && a.last.compare(0, 18, "OK AUDIO SET CLK=0") == 0 && g_au == 1 && a.last.find("restarted") != std::string::npos; printf("%s AUDIO SET CLK 0 (restarts) -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    ledCmd("AUDIO SET CON1 11d5", a); ok = g_cfg[2] == 0x11d5; printf("%s AUDIO SET CON1\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO CFG", a); ok = a.last == "OK AUDIO CFG CLK=0 CON0=180 CON1=11d5 CON3=83 LEN=256"; printf("%s AUDIO CFG -> %s\n", ok ? "ok  " : "FAIL", a.last.c_str()); fails += !ok;
    ledCmd("AUDIO SET BAD 1", a); ok = a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO SET BAD rejected\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO SET CLK", a); ok = a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO SET CLK (no value) rejected\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO OFF", a); ok = g_au == 0; printf("%s AUDIO OFF\n", ok ? "ok  " : "FAIL"); fails += !ok;
    ledCmd("AUDIO X", a); ok = a.last.compare(0, 9, "ERR AUDIO") == 0; printf("%s AUDIO X rejected\n", ok ? "ok  " : "FAIL"); fails += !ok;
  }
  printf("%d failed\n", fails); return fails != 0;
}
