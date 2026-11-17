#include "pd/catalog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string>

#include "mem_store.h"
#include "pd/codec.h"
#include "pd/crc.h"
#include "pd/engine.h"

using namespace pd;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { g_n++; std::string _a = (a), _b = (b); if (_a != _b) { g_fail++; printf("  FAIL %s:%d\n    got:  %s\n    want: %s\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)

struct Rig {  // one "board": stores survive Engine re-creation = reboot
  MemStore presets, settings;
  std::unique_ptr<Engine> e;
  Capture ev;
  Rig() { boot(); }
  void boot() { e.reset(new Engine(presets, settings)); e->setAsync(&ev); e->begin(); }
  Capture run(const std::string& cmd) { Capture c; e->handleLine(cmd.c_str(), c); return c; }
  std::string one(const std::string& cmd) { return run(cmd).last(); }
};

static void test_footprint() {
  printf("footprint\n");
  printf("  sizeof Preset=%zu Settings=%zu Engine=%zu\n", sizeof(Preset), sizeof(Settings), sizeof(Engine));
  CHECK(sizeof(Preset) < 2048 && sizeof(Settings) < 1536);
}

static void test_crc() {
  printf("crc\n");
  CHECK_EQ([] { char b[12]; snprintf(b, sizeof b, "%08x", (unsigned)crc32((const uint8_t*)"123456789", 9)); return std::string(b); }(), "cbf43926");
  CHECK(crc16_xmodem((const uint8_t*)"123456789", 9) == 0x31C3);
}

static void test_codec() {
  printf("codec\n");
  Preset p;
  copyName(p.name, kNameLen, "Round Trip");
  p.nb = 2;
  p.b[0].type = 3; p.b[0].np = 2; p.b[0].p[0] = 11; p.b[0].p[1] = -5; p.b[0].bank = 'P';
  p.b[1].type = 8; p.b[1].np = 4; p.b[1].bank = 'Q'; p.b[1].lane = 2;
  p.sc[2].used = true; copyName(p.sc[2].name, kSceneNameLen, "Solo"); p.sc[2].nb = 2; p.sc[2].b[1].p[3] = 77; p.sc[2].gain_out = -4;
  std::vector<uint8_t> buf(kPresetMaxBytes);
  buf.resize(encodePreset(p, buf.data(), buf.size()));
  CHECK(buf.size() > 14);
  uint8_t tiny[64];
  CHECK(encodePreset(p, tiny, sizeof tiny) == 0);  // does not fit -> 0, no overflow
  Preset q;
  CHECK(decodePreset(buf.data(), buf.size(), q) == CodecResult::Ok);
  CHECK_EQ(q.name, "Round Trip");
  CHECK(q.b[0].p[1] == -5 && q.b[1].lane == 2 && q.sc[2].b[1].p[3] == 77 && q.sc[2].gain_out == -4 && q.sc[2].used && !q.sc[0].used);
  CHECK_EQ(q.sc[2].name, "Solo");
  for (size_t i = 0; i < buf.size(); i += 17) {  // any single flipped bit must be rejected
    std::vector<uint8_t> bad = buf;
    bad[i] ^= 0x10;
    Preset r;
    CHECK(decodePreset(bad.data(), bad.size(), r) != CodecResult::Ok);
  }
  Preset r;
  CHECK(decodePreset(buf.data(), buf.size() - 1, r) != CodecResult::Ok);
  CHECK(decodePreset(buf.data(), 5, r) == CodecResult::Truncated);
  std::vector<uint8_t> magic = buf; magic[0] = 'X';
  CHECK(decodePreset(magic.data(), magic.size(), r) == CodecResult::BadMagic);
  Settings s;
  defaultFsa(s, Layout::Scenes);
  s.hpf = 120; s.geq[3] = -7; s.layout = Layout::Custom; s.fsa[2][1] = {8, 4, 1}; s.caps[3].used = true; copyName(s.caps[3].name, kNameLen, "Plexi"); s.caps[3].size = 2200;
  buf.assign(kSettingsMaxBytes, 0);
  buf.resize(encodeSettings(s, buf.data(), buf.size()));
  CHECK(buf.size() > 14);
  Settings t;
  CHECK(decodeSettings(buf.data(), buf.size(), t) == CodecResult::Ok);
  CHECK(t.hpf == 120 && t.geq[3] == -7 && t.layout == Layout::Custom && t.fsa[2][1].a == 4 && t.caps[3].size == 2200);
  CHECK_EQ(t.caps[3].name, "Plexi");
}

static void test_boot_and_persistence() {
  printf("boot + presets persist across reboot\n");
  Rig r;
  CHECK(r.presets.exists("p00") && r.presets.exists("p01") && r.settings.exists("gset"));  // first boot seeds flash
  CHECK_EQ(r.one("PLIST"), "PLIST end current=0");
  CHECK(r.run("PLIST").has("PRE 0 5 0 0 'Clean Demo'"));

  r.run("PNEW");
  CHECK_EQ(r.one("PSAVE 5 My Tone"), "PSAVE ok slot=5 capture=0");  // empty chain
  Capture a = r.run("FXA P 99 overdrive");
  CHECK(a.has("FXA id="));
  CHECK_EQ(r.one("FXS").substr(0, 13), "FXS peak=425 ");  // amp 380 + overdrive 45
  std::string fx = r.run("FXS").find("FX P0");
  unsigned id = 0;
  sscanf(fx.c_str(), "FX P0 id=%u", &id);
  CHECK(id > 0);
  CHECK_EQ(r.one("FXP " + std::to_string(id) + " 0 33"), "FXP ok 33");
  CHECK_EQ(r.one("PSAVE 5 My Tone"), "PSAVE ok slot=5 capture=0");
  r.run("PCOLOR ff8800");
  r.run("PSAVE 5 My Tone");

  r.boot();  // power cycle
  Capture pl = r.run("PLIST");
  CHECK(pl.has("PRE 5 1 0 0 'My Tone' s=0 col=ff8800 amp=1"));
  CHECK(pl.has("PRE 0 5") && pl.has("PRE 1 5"));
  CHECK_EQ(r.one("PLOAD 5"), "PLOAD ok slot=5 missing=0 ms=12");
  CHECK(r.run("FXS").has("FX P0 id=") && r.run("FXS").find("FX P0").find("overdrive on=1 p=33,4000,-8,1,0") != std::string::npos);
  CHECK_EQ(r.one("PLOAD 9"), "ERR PLOAD empty or invalid slot");

  CHECK_EQ(r.one("PNAME 5 Renamed"), "PNAME ok 5 'Renamed'");
  r.boot();
  CHECK(r.run("PLIST").has("PRE 5 1 0 0 'Renamed'"));
  CHECK_EQ(r.one("PDEL 5"), "PDEL ok 5");
  r.boot();
  CHECK(!r.run("PLIST").has("PRE 5"));
}

static void test_scenes() {
  printf("scenes persist\n");
  Rig r;
  r.run("PLOAD 0");
  CHECK_EQ(r.one("SNAPS"), "SNAPS 0000 current=-1");
  std::string fx = r.run("FXS").find("FX P0");
  unsigned id = 0;
  sscanf(fx.c_str(), "FX P0 id=%u", &id);
  r.run("FXP " + std::to_string(id) + " 0 20");
  CHECK_EQ(r.one("SNAPSAVE 0"), "SNAPSAVE ok 0 preset=0");
  r.run("FXP " + std::to_string(id) + " 0 24");
  r.run("FXE " + std::to_string(id) + " 0");
  r.run("GAIN -3 2");
  CHECK_EQ(r.one("SNAPSAVE 2"), "SNAPSAVE ok 2 preset=0");
  CHECK_EQ(r.one("SNAPNAME 2 Lead Boost"), "SNAPNAME ok 2 'Lead Boost' preset=0");
  CHECK_EQ(r.one("SNAPS"), "SNAPS 1010 current=2");
  // unsaved chain edit after the snapshot must NOT reach flash through SNAPSAVE
  r.run("FXP " + std::to_string(id) + " 0 11");
  CHECK_EQ(r.one("SNAPSAVE 3"), "SNAPSAVE ok 3 preset=0");

  r.boot();
  CHECK(r.run("PLIST").has("PRE 0 5 0 0 'Clean Demo' s=13 amp=1"));  // scenes 0,2,3 -> mask 1+4+8
  r.run("PLOAD 0");
  CHECK_EQ(r.one("SNAPS"), "SNAPS 1011 current=-1");
  CHECK(r.run("FXS").find("FX P0").find("on=1 p=6 ") != std::string::npos);  // chain = what PSAVE stored
  CHECK_EQ(r.one("SNAP 0").substr(0, 9), "SNAP ok 0");
  CHECK(r.run("FXS").find("FX P0").find("p=20 ") != std::string::npos);
  CHECK_EQ(r.one("SNAP 2").substr(0, 9), "SNAP ok 2");
  CHECK(r.run("FXS").find("FX P0").find("on=0 p=24 ") != std::string::npos);
  CHECK(r.one("GAIN").find("in=-3 out=2") != std::string::npos);
  CHECK_EQ(r.one("SNAP 1"), "ERR SNAP empty slot");
  Capture nm = r.run("SNAPNAME");
  CHECK(nm.has("SNAPNAME 2 'Lead Boost'") && nm.has("SNAPNAME 1 'Scene 2'"));
  CHECK_EQ(r.one("SNAPDEL 0"), "SNAPDEL ok 0");
  r.boot();
  r.run("PLOAD 0");
  CHECK_EQ(r.one("SNAPS"), "SNAPS 0011 current=-1");
}

static void test_corruption_and_power_loss() {
  printf("corruption / failed writes\n");
  Rig r;
  r.presets.m["p01"][20] ^= 0x40;  // flip a bit in preset 1
  r.boot();
  Capture pl = r.run("PLIST");
  CHECK(pl.has("PRE 0") && !pl.has("PRE 1 "));
  CHECK_EQ(r.one("PLOAD 1"), "ERR PLOAD empty or invalid slot");

  std::vector<uint8_t> before = r.presets.m["p00"];
  r.presets.failWrites = true;
  r.run("PLOAD 0");
  CHECK_EQ(r.one("PSAVE 0 Hacked"), "ERR PSAVE flash write failed");
  CHECK(r.presets.m["p00"] == before);  // old record untouched
  CHECK_EQ(r.one("GSAVE"), "GSAVE ok");  // settings store is a separate backend
  r.settings.failWrites = true;
  CHECK_EQ(r.one("GSAVE"), "ERR GSAVE flash write failed");
  r.presets.failWrites = r.settings.failWrites = false;

  r.settings.m["gset"][12] ^= 1;  // corrupt settings: boot must fall back to defaults, not crash
  r.boot();
  CHECK(r.run("FSA").has("FSA end"));
}

static void test_budget() {
  printf("cpu/ram budget\n");
  Rig r;
  r.run("PNEW");
  int ok = 0;
  std::string last;
  for (int i = 0; i < 12; i++) {
    last = r.one("FXA P 99 overdrive");
    if (last.compare(0, 6, "FX cpu") == 0) ok++;
    else break;
  }
  CHECK(ok >= 1);
  CHECK(last.find("ERR FXA") == 0);  // cpu (380 amp + 45*n) or bank full, never a crash
  Capture f = r.run("FXS");
  unsigned used = 0, tot = 0;
  sscanf(f.find("FXS cpu=").c_str(), "FXS cpu=%u/%u", &used, &tot);
  CHECK(used <= tot && tot == 1000);
  r.run("PNEW");
  r.run("FXA Q 99 reverb2");
  r.run("FXA Q 99 delay");
  CHECK(r.one("FXA Q 99 ir").find("FX cpu=") == 0);       // 16384 amp + 8192 + 12288 + 8152 = 45016 <= 49152
  CHECK_EQ(r.one("FXA Q 99 delay"), "ERR FXA out of memory");  // +12288 would pass 49152
  CHECK_EQ(r.one("FXA P 99 nonexistent"), "ERR FXA bad argument");
  r.run("PNEW");
  int bank = 0;
  for (int i = 0; i < 16; i++) { std::string l = r.one("FXA P 99 gate"); if (l.find("ERR FXA bank full") == 0) break; bank++; }
  CHECK(bank <= 10 && bank >= 1);
  r.run("PNEW");
  CHECK_EQ(r.one("AMP 1"), "AMP 1");
}

static std::string crcHex(const std::vector<uint8_t>& d) { char b[12]; snprintf(b, sizeof b, "%08x", (unsigned)crc32(d.data(), d.size())); return b; }
static std::string toHex(const uint8_t* d, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", d[i]); s += b; } return s; }

static void test_upload() {
  printf("upload crc\n");
  Rig r;
  std::vector<uint8_t> blob(300);
  for (size_t i = 0; i < blob.size(); i++) blob[i] = (uint8_t)(i * 7 + 3);
  CHECK_EQ(r.one("LDBEGIN 300"), "OK LDBEGIN 300");
  CHECK_EQ(r.one("LDB 0 " + toHex(blob.data(), 90)), "OK LDB 0 90");
  CHECK_EQ(r.one("LDB 50 " + toHex(blob.data(), 10)), "ERR LDB offset");  // not contiguous
  CHECK_EQ(r.one("LDB 100 zz"), "ERR LDB hex");
  r.run("LDB 90 " + toHex(blob.data() + 90, 90));
  CHECK_EQ(r.one("LDEND 00000000"), "ERR LDEND crc mismatch");
  CHECK_EQ(r.one("CSAVE 0 x"), "ERR CSAVE nothing to save");
  r.run("LDBEGIN 300");
  for (size_t off = 0; off < 300; off += 90) r.run("LDB " + std::to_string(off) + " " + toHex(blob.data() + off, std::min<size_t>(90, 300 - off)));
  CHECK_EQ(r.one(std::string(200, 'X')), "ERR line too long");
  CHECK(r.one("LDEND " + crcHex(blob)).find("LOAD rc=0") == 0);
  CHECK_EQ(r.one("CSAVE 3 Plexi 50W"), "CSAVE ok 3 'Plexi 50W' 300 B");
  r.boot();
  CHECK(r.run("CLIST").has("CAP 3 300 'Plexi 50W'"));  // slot table survives reboot
  CHECK_EQ(r.one("LDBEGIN 99999999"), "ERR LDBEGIN length");
  CHECK_EQ(r.one("IRBEGIN 5000 0 48"), "ERR IRBEGIN 1..4096");
  CHECK_EQ(r.one("IRBEGIN 8 0 44"), "ERR IRBEGIN fs 24 or 48");
  std::vector<uint8_t> ir(16, 0x5a);
  r.run("IRBEGIN 8 0 48");
  r.run("IRB 0 " + toHex(ir.data(), 16));
  CHECK(r.one("IREND " + crcHex(ir)).find("IR ok 8 samples") == 0);
}

static void test_settings() {
  printf("global settings (GSAVE)\n");
  Rig r;
  r.run("HPF 150");
  r.run("GEQ 2 -6");
  r.run("FSMODE layout=custom");
  r.run("FSA 1 1 8 4 1");
  r.run("FSLED 2 ff0000 220000");
  r.run("LEDCFG bright=40 anim=0");
  r.run("FSPRESS 3 1");
  r.boot();
  CHECK_EQ(r.one("HPF"), "HPF hz=80");  // not saved yet -> lost on reboot
  r.run("HPF 150"); r.run("GEQ 2 -6"); r.run("FSMODE layout=custom"); r.run("FSA 1 1 8 4 1");
  r.run("FSLED 2 ff0000 220000"); r.run("LEDCFG bright=40 anim=0"); r.run("FSPRESS 3 1");
  CHECK_EQ(r.one("GSAVE"), "GSAVE ok");
  r.boot();
  CHECK_EQ(r.one("HPF"), "HPF hz=150");
  CHECK_EQ(r.one("GEQ"), "GEQ 0 0 -6 0 0");
  CHECK(r.one("FSMODE").find("layout=custom") != std::string::npos);
  CHECK(r.run("FSA").has("FSA 1 1 8 4 1"));
  CHECK_EQ(r.run("FSLED").find("FSLED 2"), "FSLED 2 custom on=ff0000 off=220000");
  CHECK_EQ(r.one("LEDCFG"), "LEDCFG anim=0 bright=40 hold=1 idle=0 meter=edit tap=1 tuner=needle");
  CHECK_EQ(r.one("FSPRESS"), "FSPRESS 0 0 0 1 acts=4");
  // GSAVE also remembers the current preset
  r.run("PLOAD 1");
  r.run("GSAVE");
  r.boot();
  CHECK_EQ(r.one("PLIST"), "PLIST end current=1");
  CHECK_EQ(r.one("FACTORYRESET"), "FACTORYRESET ok");
  CHECK_EQ(r.one("HPF"), "HPF hz=80");
}

static void tapFs(Rig& r, int fs, uint32_t t, uint32_t hold) { r.e->fsEvent(fs, true, t); r.e->tick(t + hold - 1); r.e->fsEvent(fs, false, t + hold); r.e->tick(t + hold + 200); }

static void test_footswitches() {
  printf("footswitch state machine\n");
  Rig r;
  // --- factory layout: A..D load presets of the bank, hold saves
  r.run("PLOAD 0");
  tapFs(r, 1, 100, 50);  // slot 1 (Crunch)
  CHECK_EQ(r.one("PLIST"), "PLIST end current=1");
  tapFs(r, 1, 400, 50);  // same preset again -> bypass
  CHECK(r.e->bypassed());
  CHECK(r.run("FXS").has("FXS amp=1 on=0"));
  tapFs(r, 1, 700, 50);
  CHECK(!r.e->bypassed());
  tapFs(r, 3, 1000, 1200);  // hold D: save current chain into slot 3 (bank 0)
  CHECK(r.ev.has("EVT save 3 ok"));
  r.boot();
  CHECK(r.run("PLIST").has("PRE 3 5"));
  // combos: C+D -> next bank, A+B -> prev bank
  r.e->fsEvent(2, true, 2000); r.e->fsEvent(3, true, 2030);
  CHECK(r.ev.has("EVT bank 2"));
  r.e->fsEvent(2, false, 2100); r.e->fsEvent(3, false, 2110);
  CHECK(r.one("FSMODE").find("bank=2") != std::string::npos);
  CHECK(r.e->pendingBank());
  r.e->fsEvent(0, true, 3000); r.e->fsEvent(1, true, 3010);
  r.e->fsEvent(0, false, 3100); r.e->fsEvent(1, false, 3110);
  CHECK(r.one("FSMODE").find("bank=1") != std::string::npos);
  // B+C held: tuner at 1 s, BLE pair at 3 s, BLE off at 6 s
  r.ev.clear();
  r.e->fsEvent(1, true, 4000); r.e->fsEvent(2, true, 4020);
  for (uint32_t t = 4100; t <= 11000; t += 100) r.e->tick(t);
  CHECK(r.ev.has("EVT tuner 1") && r.ev.has("EVT ble pair") && r.ev.has("EVT ble off"));
  r.e->fsEvent(1, false, 11100); r.e->fsEvent(2, false, 11110);
  CHECK(r.e->tunerOn());

  // --- scenes layout: short = scene n, long = presets/tuner/tap, double optional
  Rig s;
  s.run("PLOAD 0");
  s.run("SNAPSAVE 0");
  s.run("FXE 1 0");
  s.run("SNAPSAVE 1");
  s.run("FSMODE layout=scenes");
  s.e->tick(10);
  tapFs(s, 0, 100, 40);
  s.e->tick(200);
  CHECK_EQ(s.one("SNAPS"), "SNAPS 1100 current=0");
  tapFs(s, 1, 300, 40);
  CHECK_EQ(s.one("SNAPS"), "SNAPS 1100 current=1");
  tapFs(s, 2, 500, 40);
  CHECK(s.ev.has("EVT scene 2 empty"));
  tapFs(s, 3, 700, 1100);  // long on D = next preset
  CHECK_EQ(s.one("PLIST"), "PLIST end current=1");
  // double press: needs a double action to enable the wait window
  s.run("FSA 0 2 10 0 255");
  s.run("PLOAD 0");
  s.e->fsEvent(0, true, 2000); s.e->fsEvent(0, false, 2040);
  s.e->tick(2100);
  CHECK(s.ev.has("EVT fs 0 0") == true);  // not yet resolved...
  s.ev.clear();
  s.e->fsEvent(0, true, 2150); s.e->fsEvent(0, false, 2190);  // second tap inside 300 ms
  CHECK(s.ev.has("EVT fs 0 2") && s.ev.has("EVT amp 0"));
  // a single tap resolves to "short" once the window expires
  s.ev.clear();
  s.e->fsEvent(0, true, 5000); s.e->fsEvent(0, false, 5030);
  CHECK(!s.ev.has("EVT fs"));
  s.e->tick(5400);
  CHECK(s.ev.has("EVT fs 0 0"));
  // act-on-press fires at press time
  s.ev.clear();
  s.run("FSPRESS 2 1");
  s.e->fsEvent(2, true, 7000);
  CHECK(s.ev.has("EVT fs 2 0"));
  s.e->fsEvent(2, false, 7020);
  // LEDs
  CHECK(s.e->ledColor(0, 8000) != 0);
  s.run("LEDCFG bright=0");
  CHECK(s.e->ledColor(0, 8000) == 0);
  CHECK(s.one("FSST").find("leds=000000,000000,000000,000000") != std::string::npos);
  // FSDO (virtual footswitch from the host)
  CHECK_EQ(s.one("FSDO 0 0"), "FSDO ok 0 0");
  CHECK_EQ(s.one("FSDO 9 0"), "ERR FSDO bad argument");
}

static void test_catalog() {
  printf("catalog\n");
  CHECK_EQ(std::to_string(pd::fxCount()), "37");                       // protocol.json effects.table (ids 1..39 minus the retired 6 and 24)
  bool ok = true; int prev = 0;
  for (int i = 0; i < pd::fxCount(); i++) {
    const pd::FxDef* d = pd::fxAt(i);
    ok = ok && d->id > prev && d->np >= 1 && d->np <= pd::kMaxParams && d->cost > 0 && d->mem > 0 && pd::fxById(d->id) == d && pd::fxByName(d->name) == d;
    prev = d->id;
    for (int k = 0; k < d->np; k++) ok = ok && d->p[k].name && d->p[k].min <= d->p[k].def && d->p[k].def <= d->p[k].max;
    if (!ok) { printf("  bad catalog entry id=%d %s\n", d->id, d->name); break; }
  }
  CHECK(ok);
  CHECK(!pd::fxById(6) && !pd::fxById(24) && !pd::fxById(0) && !pd::fxById(40));
  CHECK(pd::fxByName("amp_hi") && pd::fxByName("amp_hi")->id == 23 && pd::fxByName("reverb2")->id == 27 && pd::fxByName("pitchpro")->np == 7);
  CHECK_EQ(std::string(pd::fxById(1)->p[0].name), "umbral_db");
  CHECK_EQ(std::to_string(pd::fxById(1)->p[0].min), "-90");
  CHECK_EQ(std::to_string(pd::fxByName("rotary")->p[3].max), "2660");
  Rig r;
  Capture t = r.run("FXT");
  CHECK(t.has("FXT 39 rotary cost=70 cat=mod") && t.has("FXT 3 inercia_pct 0..2660 (50)") && t.has("FXT end"));
  r.run("PNEW");
  for (int i = 0; i < pd::fxCount(); i++) {  // every type can be added to an (otherwise empty) chain, alone, with defaults
    r.run("PNEW");
    Capture a = r.run(std::string("FXA P 99 ") + pd::fxAt(i)->name);
    if (!a.has("FXA id=")) { printf("  cannot add %s: %s\n", pd::fxAt(i)->name, a.find("ERR").c_str()); ok = false; }
  }
  CHECK(ok);
}

static const Block* blockOf(Rig& r, const char* name) {
  for (int i = 0; i < r.e->chain().nb; i++) if (!strcmp(fxById(r.e->chain().b[i].type)->name, name)) return &r.e->chain().b[i];
  return nullptr;
}
static void bcTap(Rig& r, uint32_t t, uint32_t held) {  // B+C pressed together (20 ms apart), released after `held` ms
  r.e->fsEvent(1, true, t); r.e->fsEvent(2, true, t + 20);
  for (uint32_t x = t + 50; x <= t + held; x += 50) r.e->tick(x);
  r.e->fsEvent(1, false, t + held); r.e->fsEvent(2, false, t + held + 10);
}

static void test_live_mode() {
  printf("preset/live mode\n");
  Rig r;
  r.run("PLOAD 0");
  CHECK(r.e->mode() == Mode::Preset);
  bcTap(r, 1000, 200);
  CHECK(r.e->mode() == Mode::Live && r.ev.has("EVT mode live"));
  bcTap(r, 2000, 200);
  CHECK(r.e->mode() == Mode::Preset && r.ev.has("EVT mode preset"));
  r.ev.clear();
  bcTap(r, 3000, 1200);  // held: tuner, not a mode change
  CHECK(r.e->mode() == Mode::Preset && r.e->tunerOn());
  r.run("TUNER 0");
  r.e->fsEvent(1, true, 5000); r.e->fsEvent(2, true, 5500);  // 500 ms apart: two single presses, not a combo
  r.e->fsEvent(1, false, 5600); r.e->fsEvent(2, false, 5700); r.e->tick(5900);
  CHECK(r.e->mode() == Mode::Preset);
  // Preset mode: A..D load presets 0..3 (checked elsewhere); Live: A..D = reverb, delay, mod, amp
  bcTap(r, 6000, 200);
  CHECK(r.e->mode() == Mode::Live);
  const Block* rev = blockOf(r, "reverb2"); const Block* dly = blockOf(r, "delay");
  CHECK(rev && rev->on && dly && dly->on && blockOf(r, "chorus") == nullptr);
  int p0 = r.e->currentPreset();
  tapFs(r, 0, 7000, 50);  // A: reverb off
  CHECK(!blockOf(r, "reverb2")->on && r.ev.has("EVT live 0 0"));
  CHECK(r.e->currentPreset() == p0);  // no preset change in Live mode
  CHECK(r.e->ledColor(0, 0) != 0 && r.e->ledColor(0, 0) == r.e->ledColor(0, 0));
  uint32_t offC = r.e->ledColor(0, 0);
  tapFs(r, 0, 7500, 50);  // A: reverb on again
  CHECK(blockOf(r, "reverb2")->on && r.e->ledColor(0, 0) > offC);
  tapFs(r, 1, 8000, 50);  // B: delay off
  CHECK(!blockOf(r, "delay")->on);
  CHECK(r.e->ledColor(2, 0) == 0);  // C: no modulation block -> dark
  tapFs(r, 2, 8500, 50);  // C: nothing to toggle
  CHECK(r.ev.has("EVT live 2 missing"));
  tapFs(r, 3, 9000, 50);  // D: amp off
  CHECK(!r.e->chain().amp_on);
  tapFs(r, 3, 9500, 50);
  CHECK(r.e->chain().amp_on);
  r.run("FXA P 0 flanger");  // add a modulation block: C controls it
  tapFs(r, 2, 10000, 50);
  CHECK(blockOf(r, "flanger")->on == false);  // new blocks are added ON, so the toggle turned it off
  bcTap(r, 11000, 200);
  CHECK(r.e->mode() == Mode::Preset);
  tapFs(r, 1, 12000, 50);  // preset mode again: B loads slot 1
  CHECK(r.e->currentPreset() == 1);
  // Live: hold saves the edits into the CURRENT preset and reports it
  bcTap(r, 13000, 200);
  uint32_t sq = r.e->saveSeq();
  tapFs(r, 3, 14000, 1200);
  CHECK(r.e->saveSeq() == sq + 1 && r.e->saveOk() && r.ev.has("EVT save 1 ok") && r.e->currentPreset() == 1);
}

static void test_fs_robust() {
  printf("B+C with human timing, hold = save\n");
  Rig r; r.run("PLOAD 0"); int p0 = r.e->currentPreset();
  // B down, C down 150 ms later (both still down), then released: Live, no preset change
  r.e->fsEvent(1, true, 1000); r.e->fsEvent(2, true, 1150); r.e->tick(1200); r.e->fsEvent(1, false, 1300); r.e->fsEvent(2, false, 1320); r.e->tick(1700);
  CHECK(r.e->mode() == Mode::Live && r.e->currentPreset() == p0);
  // B pressed and released, C pressed 100 ms after B came up: still the combo (B's short press was deferred and cancelled)
  r.e->fsEvent(1, true, 3000); r.e->fsEvent(1, false, 3060); r.e->fsEvent(2, true, 3160); r.e->fsEvent(2, false, 3260); r.e->tick(3700);
  CHECK(r.e->mode() == Mode::Preset && r.e->currentPreset() == p0);
  // a lone B tap is delayed by < 0.2 s and then loads slot 1
  r.e->fsEvent(1, true, 5000); r.e->fsEvent(1, false, 5050); r.e->tick(5100);
  CHECK(r.e->currentPreset() == p0);
  r.e->tick(5300);
  CHECK(r.e->currentPreset() == 1);
  // hold saves while the foot is still down, even if the stored "act on press" flag is set; never a preset change
  r.run("FSPRESS 1 1 1 1");
  uint32_t sq = r.e->saveSeq();
  r.e->fsEvent(3, true, 8000); for (uint32_t t = 8050; t <= 9200; t += 50) r.e->tick(t);
  CHECK(r.e->saveSeq() == sq + 1 && r.e->saveOk());
  r.e->fsEvent(3, false, 9300); r.e->tick(9600);
  CHECK(r.e->saveSeq() == sq + 1);  // releasing does not save twice
  Rig q; q.run("PLOAD 0"); q.run("FSPRESS 1 1 1 1"); q.e->fsEvent(0, true, 100); q.e->fsEvent(0, false, 150);
  CHECK(q.e->currentPreset() == 0);
}

static void test_knobs() {
  printf("knobs: soft takeover, master, gate\n");
  Rig r;
  r.run("PLOAD 0");
  // MASTER works in both modes and immediately
  CHECK(r.e->masterRaw() == 890);
  r.e->knob(0, 300);
  CHECK(r.e->masterRaw() == 300 && r.e->knobState(0) == KnobState::Engaged);
  // Preset mode: other knobs do nothing
  const Block* d = blockOf(r, "delay"); int mix0 = d->p[2];
  r.e->knob(5, 1000);
  CHECK(d->p[2] == mix0 && r.e->knobState(5) == KnobState::Inert);
  // Live mode: knob starts disengaged when it differs from the saved value
  bcTap(r, 1000, 200);
  CHECK(r.e->mode() == Mode::Live);
  CHECK(r.e->knob(5, 900) == KnobState::Disengaged);   // saved mezcla_pct = 30 -> about raw 306
  CHECK(blockOf(r, "delay")->p[2] == mix0);
  CHECK(r.e->knobLed(5, 0) != r.e->knobLed(5, 250));   // blinking
  CHECK(r.e->knob(5, 700) == KnobState::Disengaged && blockOf(r, "delay")->p[2] == mix0);
  CHECK(r.e->knob(5, 250) == KnobState::Engaged);       // crossed the saved value from above
  CHECK(blockOf(r, "delay")->p[2] == 24);               // 250 * 100 / 1023
  CHECK(r.e->knobLed(5, 0) && r.e->knobLed(5, 250));    // steady
  r.e->knob(5, 1023);
  CHECK(blockOf(r, "delay")->p[2] == 100);
  r.e->knob(5, 1022);                                   // below the apply dead band: no new value
  CHECK(r.ev.has("EVT knob 5 100"));
  // a preset change disengages every knob again; the saved value stands
  r.run("PLOAD 0");
  CHECK(r.e->knobState(5) != KnobState::Engaged);
  CHECK(r.e->knob(5, 900) == KnobState::Disengaged && blockOf(r, "delay")->p[2] == mix0);
  // exactly on the saved value: engaged at once
  r.run("PLOAD 0");
  CHECK(r.e->knob(5, 306) == KnobState::Engaged);
  // knob that would control a missing block: inert and dark
  CHECK(r.e->knob(6, 500) == KnobState::Inert && !r.e->knobLed(6, 0));
  CHECK(r.e->knob(13, 500) == KnobState::Inert);
  // amp knobs go to the capture's AMPEQ (volume -240..120, saved 0 -> raw 682)
  r.run("PLOAD 0");
  CHECK(r.e->knob(8, 100) == KnobState::Disengaged);
  CHECK(r.e->chain().ampeq[0] == 0);
  CHECK(r.e->knob(8, 900) == KnobState::Engaged);      // crossed 682 going up
  CHECK(r.e->chain().ampeq[0] > 50);
  // entering Live again disengages
  bcTap(r, 3000, 200); bcTap(r, 4000, 200);
  CHECK(r.e->mode() == Mode::Live && r.e->knobState(8) != KnobState::Engaged);
  // IR CAB: picks the cabinet 1..8 of the IR block while it is ON; it never switches the block
  r.run("PLOAD 0");
  const Block* ir = blockOf(r, "ir"); CHECK(ir && ir->on);
  r.e->knob(1, 0);
  r.e->knob(1, 600);
  CHECK(blockOf(r, "ir")->on && blockOf(r, "ir")->p[2] == 5);
  r.e->knob(1, 0);
  CHECK(blockOf(r, "ir")->on && blockOf(r, "ir")->p[2] == 1);
}

static unsigned lvlGen(Rig& r) { std::string l = r.one("LVL"); size_t i = l.find(" c="); return i == std::string::npos ? 0 : (unsigned)atoi(l.c_str() + i + 3); }
static void test_editor_sync() {
  printf("changes made on the pedal bump LVL c= (the editor polls it)\n");
  Rig r; r.run("PLOAD 0"); bcTap(r, 1000, 200); CHECK(r.e->mode() == Mode::Live);
  unsigned g0 = lvlGen(r); r.e->knob(5, 0); r.e->knob(5, 1000); CHECK(lvlGen(r) > g0);                          // knob edits a parameter
  g0 = lvlGen(r); tapFs(r, 0, 3000, 50); CHECK(lvlGen(r) > g0);                                               // footswitch toggles a block
  g0 = lvlGen(r); r.e->knob(14, 0); r.e->knob(14, 700); CHECK(lvlGen(r) > g0);                                 // noise gate knob
  g0 = lvlGen(r); r.run("FXA P 0 flanger"); CHECK(lvlGen(r) > g0);                                             // command from the editor
  g0 = lvlGen(r); r.e->knob(1, 0); r.e->knob(1, 900); CHECK(lvlGen(r) >= g0);
  CHECK(r.run("FXS").has("FXS peak"));
}

static void test_builtin_cabs_listed() {
  printf("ILIST shows built-in cabinets\n");
  Rig r; static const char* const nm[3] = {"A", "B", "C"}; r.e->setBuiltinIrNames(nm, 3);
  Capture c = r.run("ILIST"); CHECK(c.has("IRS 0 0 'A'") && c.has("IRS 2 0 'C'") && !c.has("IRS 3") && c.has("ILIST end current=-1 slots="));
}

struct FakeMedia : IMedia {
  std::string log; std::vector<uint8_t> got; int rc = 0; bool room = true, saveOk = true;
  bool begin(char k, size_t n, long g, int fs) override { log += std::string("B") + k + std::to_string(n) + "/" + std::to_string(g) + "/" + std::to_string(fs) + " "; got.clear(); return room; }
  void data(size_t off, const uint8_t* d, size_t n) override { if (off == got.size()) got.insert(got.end(), d, d + n); else log += "GAP "; }
  int end(char k, char* name, size_t cap, int* layers, int* ch) override { log += std::string("E") + k + " "; if (k == 'C') { snprintf(name, cap, "TestAmp"); *layers = 7; *ch = 3; } return rc; }
  bool save(char k, int s) override { log += std::string("S") + k + std::to_string(s) + " "; return saveOk; }
  bool select(char k, int s) override { log += std::string("L") + k + std::to_string(s) + " "; return true; }
  bool erase(char k, int s) override { log += std::string("X") + k + std::to_string(s) + " "; return true; }
  void abort() override { log += "A "; }
};
static std::string hexOf(const std::vector<uint8_t>& v, size_t a, size_t n) { std::string h; char b[4]; for (size_t i = a; i < a + n; i++) { snprintf(b, sizeof b, "%02x", v[i]); h += b; } return h; }
static bool upload(Rig& r, const char* pre, const std::vector<uint8_t>& v, const std::string& begin, std::string* endReply) {
  if (r.one(begin).compare(0, 3, "OK ") != 0) return false;
  for (size_t o = 0; o < v.size(); o += 80) { size_t n = v.size() - o < 80 ? v.size() - o : 80; if (r.one(std::string(pre) + "B " + std::to_string(o) + " " + hexOf(v, o, n)).compare(0, 3, "OK ") != 0) return false; }
  char c[16]; snprintf(c, sizeof c, "%08x", (unsigned)crc32(v.data(), v.size()));
  *endReply = r.one(std::string(pre) + "END " + c); return true;
}
static void test_media_upload() {
  printf("capture / IR upload, slots and selection (IMedia)\n");
  Rig r; FakeMedia m; r.e->setMedia(&m); r.run("PLOAD 0");
  CHECK(m.log.find("LC-1") != std::string::npos);                                  // preset without a stored capture: none selected
  std::vector<uint8_t> blob(1500); for (size_t i = 0; i < blob.size(); i++) blob[i] = (uint8_t)(i * 7 + 3);
  std::string rep; m.log.clear();
  unsigned g0 = lvlGen(r);
  CHECK(upload(r, "LD", blob, "LDBEGIN 1500", &rep));
  CHECK_EQ(rep, "LOAD rc=0 name='TestAmp' layers=7 channels=3 audio=2 engine=A2L ms=5");
  CHECK(m.got == blob && m.log == "BC1500/0/48 EC ");                              // every byte reached the platform, in order
  CHECK(lvlGen(r) > g0 && r.one("MODEL") == "MODEL 'TestAmp' layers=7 engine=A2L"); // sounding now, no slot yet
  CHECK(r.one("CLIST") == "CLIST end current=-1 slots=15");
  CHECK_EQ(r.one("CSAVE 3 My Amp"), "CSAVE ok 3 'My Amp' 1500 B");
  CHECK(m.log.find("SC3") != std::string::npos && r.run("CLIST").has("CAP 3 1500 'My Amp'") && r.one("CLIST") == "CLIST end current=3 slots=15");
  r.run("PSAVE 0 WithAmp"); m.log.clear(); r.run("PLOAD 1"); r.run("PLOAD 0");
  CHECK(m.log.find("LC3") != std::string::npos);                                   // the preset brings its capture back
  m.log.clear(); CHECK_EQ(r.one("CLOAD 3"), "CLOAD ok 3 audio=2"); CHECK(m.log == "LC3 ");
  CHECK(r.one("CLOAD 5") == "ERR CLOAD invalid");
  // rejected blob / no room / flash full
  m.rc = -4; CHECK(upload(r, "LD", blob, "LDBEGIN 1500", &rep)); CHECK(rep.compare(0, 10, "LOAD rc=-4") == 0); m.rc = 0;
  m.room = false; CHECK(r.one("LDBEGIN 9000") == "ERR LDBEGIN length 9000 (max 8192)"); CHECK(r.one("IRBEGIN 4000 0 48") == "ERR IRBEGIN out of memory"); m.room = true;
  CHECK(upload(r, "LD", blob, "LDBEGIN 1500", &rep)); m.saveOk = false; CHECK(r.one("CSAVE 4 x").compare(0, 9, "ERR CSAVE") == 0 && !r.run("CLIST").has("CAP 4")); m.saveOk = true;
  // wrong CRC aborts
  r.run("LDBEGIN 4"); r.run("LDB 0 01020304"); m.log.clear(); CHECK(r.one("LDEND 00000000") == "ERR LDEND crc mismatch" && m.log == "A ");
  // impulse response: 600 samples, gain -3500 mdB
  std::vector<uint8_t> ir(1200); for (size_t i = 0; i < ir.size(); i++) ir[i] = (uint8_t)(i * 13);
  m.log.clear(); CHECK(upload(r, "IR", ir, "IRBEGIN 600 -3500 48", &rep));
  CHECK_EQ(rep, "IR ok 600 samples gain -3500 mdB fs 48"); CHECK(m.got == ir && m.log == "BI1200/-3500/48 EI " && r.e->liveIr());
  CHECK_EQ(r.one("ISAVE 2 Cab One"), "ISAVE ok 2 'Cab One' 600 samples fs=48"); CHECK(r.run("ILIST").has("IRS 2 1200 'Cab One'"));
  r.run("FXA Q 0 ir"); unsigned id = blockOf(r, "ir")->id;
  CHECK_EQ(r.one("FXIR " + std::to_string(id) + " 2"), "FXIR ok " + std::to_string(id) + " 2"); CHECK(blockOf(r, "ir")->p[2] == 3 && !r.e->liveIr());
  m.log.clear(); CHECK(r.one("IDEL 2") == "IDEL ok 2" && m.log == "XI2 " && !r.run("ILIST").has("IRS 2 1200"));
  r.run("CLOAD 3"); m.log.clear(); CHECK(r.one("CDEL 3") == "CDEL ok 3" && m.log.find("XC3") != std::string::npos && m.log.find("LC-1") != std::string::npos);
  CHECK(r.one("LDBEGIN 0") == "OK LDBEGIN 0");
  // survives a reboot: names from settings, selection from the preset
  CHECK(upload(r, "LD", blob, "LDBEGIN 1500", &rep)); r.run("CSAVE 1 Keep"); r.run("PSAVE 0 P"); r.boot(); FakeMedia m2; r.e->setMedia(&m2); r.run("PLOAD 0");
  CHECK(r.run("CLIST").has("CAP 1 1500 'Keep'") && m2.log.find("LC1") != std::string::npos);
}

static void test_octaver_block() {
  printf("pitch / octaver block: catalog, budget, knobs and footswitch\n");
  Rig r; r.run("PNEW");
  const FxDef* d = fxByName("pitchpro"); CHECK(d && d->id == 29 && !strcmp(d->cat, "mod") && d->np == 7);
  CHECK(!strcmp(d->p[0].name, "semitonos_st") && d->p[0].min == -12 && d->p[0].max == 12 && !strcmp(d->p[2].name, "mezcla_pct"));
  CHECK(r.run("FXT").has("FXT 29 pitchpro"));                                       // offered to the editor's "add block" menu
  CHECK(r.run("FXA P 0 pitchpro").has("FXA id="));
  const Block* b = blockOf(r, "pitchpro"); CHECK(b && b->on && b->p[0] == 12 && r.e->cpuUsed() <= kCpuBudget && r.e->memUsed() <= kFxMemBudget);
  r.run("FXP " + std::to_string(b->id) + " 0 -12"); CHECK(blockOf(r, "pitchpro")->p[0] == -12);   // octave down
  r.run("FXP " + std::to_string(b->id) + " 0 40"); CHECK(blockOf(r, "pitchpro")->p[0] == 12);     // clamped to +1 octave
  // Live: MOD SPEED = interval, MOD FX = mix (soft takeover as usual), footswitch C = on/off
  bcTap(r, 1000, 200); CHECK(r.e->mode() == Mode::Live);
  r.e->knob(6, 1023); r.e->knob(6, 0); CHECK(r.e->knobState(6) == KnobState::Engaged && blockOf(r, "pitchpro")->p[0] == -12);
  r.e->knob(6, 512); CHECK(blockOf(r, "pitchpro")->p[0] == 0);
  r.e->knob(7, 0); r.e->knob(7, 1023); CHECK(blockOf(r, "pitchpro")->p[2] == 100);
  tapFs(r, 2, 3000, 50); CHECK(!blockOf(r, "pitchpro")->on && !r.e->knobLed(6, 0));
  tapFs(r, 2, 4000, 50); CHECK(blockOf(r, "pitchpro")->on);
  // rotary and envelope filter are reachable the same way
  Rig q; q.run("PNEW"); q.run("FXA P 0 envfilter"); bcTap(q, 1000, 200); q.e->knob(6, 0); q.e->knob(6, 1023); CHECK(blockOf(q, "envfilter")->p[0] == 100);
  Rig w; w.run("PNEW"); w.run("FXA P 0 rotary"); bcTap(w, 1000, 200); w.e->knob(6, 0); w.e->knob(6, 1023); CHECK(blockOf(w, "rotary")->p[2] == 1000);
}

static void test_led_lock() {
  printf("knob LEDs ignore knobs outside Live\n");
  Rig r; r.run("PLOAD 0");
  auto frame = [&](uint32_t t) { unsigned w = 0; for (int k = 0; k < kKnobs; k++) w |= (r.e->knobLed((uint8_t)k, t) ? 1u : 0u) << k; return w; };
  unsigned base = frame(0); bool steady = true;
  for (int k = 1; k < 14; k++) { r.e->knob((uint8_t)k, 1000); r.e->knob((uint8_t)k, 20); for (uint32_t t = 0; t < 2000; t += 125) steady &= frame(t) == base; steady &= r.e->knobState((uint8_t)k) == KnobState::Inert; }
  CHECK(steady && (base & 1));
  // Live -> Preset: whatever was blinking goes back to the fixed state at once
  bcTap(r, 1000, 200); CHECK(r.e->mode() == Mode::Live); r.e->knob(5, 1000); r.e->knob(2, 1000);
  bool blink = false; for (uint32_t t = 0; t < 1000; t += 125) blink |= frame(t) != frame(t + 250); CHECK(blink);
  bcTap(r, 3000, 200); CHECK(r.e->mode() == Mode::Preset);
  steady = true; for (uint32_t t = 0; t < 2000; t += 125) steady &= frame(t) == frame(0); CHECK(steady);
  CHECK(r.e->knobState(5) == KnobState::Inert && r.e->knobState(2) == KnobState::Inert);
  // a save in Preset mode changes nothing about the rings
  unsigned before = frame(0); r.run("PSAVE 0 Test"); CHECK(frame(0) == before && frame(125) == before);
}

static void test_live_on_off_leds() {
  printf("Live: lit knobs = blocks that are ON, dark = OFF, knobs never switch a block\n");
  Rig r; r.run("PLOAD 0");
  r.run("FXA P 0 chorus"); r.run("FXA Q 0 reverb2");   // preset 0: delay + reverb2 + chorus + amp (+ whatever the seed has)
  bcTap(r, 1000, 200); CHECK(r.e->mode() == Mode::Live);
  auto lit = [&](int k, uint32_t t) { return r.e->knobLed((uint8_t)k, t); };
  auto always = [&](int k) { bool a = true, n = false; for (uint32_t t = 0; t < 1000; t += 50) { bool v = lit(k, t); a &= v; n |= v; } return a ? 1 : (n ? 2 : 0); };   // 1 steady, 2 blinking, 0 dark
  // everything ON: engage each knob on its saved position, then it is steady
  for (int k = 0; k < 15; k++) r.e->knob((uint8_t)k, 0);   // first look: far from the saved values -> blinking (disengaged) but lit-class
  CHECK(always(2) != 0 && always(4) != 0 && always(6) != 0);   // reverb, delay, mod are ON: their rings are alive (blink = waiting to take over)
  // turn reverb OFF with footswitch A: its two rings go completely dark and stay dark
  tapFs(r, 0, 3000, 50);
  CHECK(!blockOf(r, "reverb2")->on);
  r.e->knob(2, 500); r.e->knob(3, 500);
  CHECK(always(2) == 0 && always(3) == 0);
  // turning the knobs of the OFF block does nothing: block stays OFF, value untouched, ring stays dark, no event
  int mix = blockOf(r, "reverb2")->p[4], dec = blockOf(r, "reverb2")->p[1]; r.ev.clear();
  for (int v = 0; v <= 1023; v += 31) { r.e->knob(2, (uint16_t)v); r.e->knob(3, (uint16_t)(1023 - v)); }
  CHECK(!blockOf(r, "reverb2")->on && blockOf(r, "reverb2")->p[4] == mix && blockOf(r, "reverb2")->p[1] == dec && always(2) == 0 && always(3) == 0 && !r.ev.has("EVT knob 2") && !r.ev.has("EVT knob 3"));
  CHECK(r.e->knobState(2) == KnobState::Inert);
  // switch it back ON with the footswitch: the rings come back (soft takeover again: engaged only after crossing the saved value)
  tapFs(r, 0, 4000, 50);
  CHECK(blockOf(r, "reverb2")->on);
  r.e->knob(2, 0); CHECK(always(2) != 0 && r.e->knobState(2) == KnobState::Disengaged && blockOf(r, "reverb2")->p[4] == mix);
  // amp OFF (footswitch D): VOLUME/BASS/MIDDLE/TREBLE/GAIN rings dark; ON again: alive
  r.e->knob(9, 0); CHECK(always(9) != 0);
  tapFs(r, 3, 5000, 50); CHECK(!r.e->chain().amp_on);
  for (int k = 8; k <= 12; k++) { r.e->knob((uint8_t)k, 900); CHECK(always(k) == 0); }
  int eq0[6]; for (int i = 0; i < 6; i++) eq0[i] = r.e->chain().ampeq[i];
  for (int k = 8; k <= 12; k++) r.e->knob((uint8_t)k, 100);
  bool same = true; for (int i = 0; i < 6; i++) same &= r.e->chain().ampeq[i] == eq0[i]; CHECK(same);
  tapFs(r, 3, 6000, 50); CHECK(r.e->chain().amp_on); r.e->knob(9, 0); CHECK(always(9) != 0);
  // IR CAB: dark while the cab block is OFF
  const Block* ir = blockOf(r, "ir");
  if (ir) { char b[40]; snprintf(b, sizeof b, "FXE %u 0", (unsigned)ir->id); r.run(b); r.e->knob(1, 700); CHECK(always(1) == 0 && !blockOf(r, "ir")->on); }
  // NOISE GATE ring follows its block; TYPE and nothing-to-edit knobs are dark
  CHECK(always(13) == 0 && always(0) == 1);
}

static void test_noise_gate_knob() {
  printf("noise gate knob keeps its state\n");
  Rig r;
  r.run("PLOAD 0");
  CHECK(blockOf(r, "gate") == nullptr);
  r.e->knob(14, 500);                                 // first reading: the knob has not been moved yet
  r.e->knob(14, 505);
  CHECK(blockOf(r, "gate") == nullptr);               // boot: preset state stands
  r.e->knob(14, 700);                                 // moved: gate added and on
  const Block* g = blockOf(r, "gate");
  CHECK(g && g->on && g->p[0] == -49);                // -90 + 700 * 60 / 1023
  for (int i = 0; i < 100; i++) r.e->knob(14, 700);   // knob released and idle: stays on
  CHECK(blockOf(r, "gate")->on);
  r.run("FXE " + std::to_string(blockOf(r, "gate")->id) + " 0");  // something switches it off while the knob sits at 70 %
  CHECK(!blockOf(r, "gate")->on);
  r.e->knob(14, 700);
  CHECK(blockOf(r, "gate")->on);                      // position decides, not movement
  r.e->knob(14, 20);                                  // <= 2 % : off
  CHECK(!blockOf(r, "gate")->on);
  r.e->knob(14, 22);                                  // hysteresis: still off
  CHECK(!blockOf(r, "gate")->on);
  r.e->knob(14, 40);
  CHECK(blockOf(r, "gate")->on && blockOf(r, "gate")->p[0] == -88);
  r.run("PLOAD 0");                                   // new preset (no gate): the knob position still rules
  r.e->knob(14, 40);
  CHECK(blockOf(r, "gate") && blockOf(r, "gate")->on);
  r.e->knob(14, 0);
  CHECK(!blockOf(r, "gate")->on);
}

int main() {
  test_footprint();
  test_crc();
  test_codec();
  test_boot_and_persistence();
  test_scenes();
  test_corruption_and_power_loss();
  test_budget();
  test_upload();
  test_settings();
  test_footswitches();
  test_catalog();
  test_live_mode();
  test_fs_robust();
  test_knobs();
  test_editor_sync();
  test_media_upload();
  test_octaver_block();
  test_builtin_cabs_listed();
  test_led_lock();
  test_live_on_off_leds();
  test_noise_gate_knob();
  printf("\n%d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
