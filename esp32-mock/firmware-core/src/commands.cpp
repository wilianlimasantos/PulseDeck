// Protocol parser: one text line in, reply lines out (PROTOCOL_SPEC.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd/catalog.h"
#include "pd/crc.h"
#include "pd/engine.h"

namespace pd {

using Tok = Tokens;

static long num(const Tok& t, int i, long def = 0) { return t.num(i, def); }
static bool eq(const char* a, const char* b) { return strcmp(a, b) == 0; }
// Tokens i.. joined by single spaces (a name); the temporary lives until the end of the full expression.
struct Rest {
  char b[64];
  Rest(const Tok& t, int i) { t.rest(i, b, sizeof(b)); }
};
static bool isHex(const char* s) {
  size_t n = strlen(s);
  if (!n || (n & 1)) return false;
  for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return false;
  return true;
}
static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

void Engine::cmdFx(ILineSink& o) {
  for (int i = 0; i < fxCount(); i++) {
    const FxDef* d = fxAt(i);
    say(o, "FXT %u %s cost=%u cat=%s", d->id, d->name, d->cost, d->cat);
    for (int k = 0; k < d->np; k++) say(o, "FXT %d %s %d..%d (%d)", k, d->p[k].name, d->p[k].min, d->p[k].max, d->p[k].def);
  }
  say(o, "FXT end");
}

static int findBlock(const Preset& p, uint32_t id) {
  for (int i = 0; i < p.nb; i++) if (p.b[i].id == id) return i;
  return -1;
}

// Insert position: index inside the bank (pos 99 = end) -> index in the flat array.
static int flatIndex(const Preset& p, char bank, int pos) {
  int count = 0, ins = p.nb;
  for (int i = 0; i < p.nb; i++) {
    if (p.b[i].bank != bank) continue;
    if (count == pos) return i;
    count++;
    ins = i + 1;
  }
  return ins;
}

bool Engine::cmdChain(const Tok& t, ILineSink& o) {
  const char* c = t.c(0);
  if (eq(c, "FXT")) { cmdFx(o); return true; }
  if (eq(c, "FXS")) {
    for (int bi = 0; bi < 2; bi++) {
      const char bk = "PQ"[bi];
      int pos = 0;
      for (int i = 0; i < cur_.nb; i++) {
        const Block& b = cur_.b[i];
        if (b.bank != bk) continue;
        char ps[64];
        size_t pn = 0;
        ps[0] = 0;
        for (int k = 0; k < b.np && pn + 8 < sizeof(ps); k++) pn += (size_t)snprintf(ps + pn, sizeof(ps) - pn, k ? ",%d" : "%d", b.p[k]);
        say(o, "FX %c%d id=%u %s on=%d p=%s ticks=%u l=%u", bk, pos++, (unsigned)b.id, fxById(b.type)->name, b.on, ps, fxById(b.type)->cost, b.lane);
      }
    }
    say(o, "FXS amp=%d on=%d", cur_.amp_present, cur_.amp_on);
    uint32_t u = cpuUsed();
    say(o, "FXS cpu=%u/%u ticks (%u%%) mem_fx=%u", u, kCpuBudget, u * 100 / kCpuBudget, memUsed());
    say(o, "FXS peak=%u ticks skipped=0", u);
    return true;
  }
  if (eq(c, "FXA")) {
    if (t.size() < 4 || (!eq(t.c(1), "P") && !eq(t.c(1), "Q"))) { say(o, "ERR FXA bad argument"); return true; }
    const FxDef* d = fxByName(t.c(3));
    if (!d) { say(o, "ERR FXA bad argument"); return true; }
    char bk = t.c(1)[0];
    int inBank = 0;
    for (int i = 0; i < cur_.nb; i++) inBank += cur_.b[i].bank == bk;
    if (cur_.nb >= kMaxBlocks || inBank >= kMaxBankBlocks) { say(o, "ERR FXA bank full"); return true; }
    if (cpuUsed() + d->cost > kCpuBudget) { say(o, "ERR FXA not enough CPU (bypass or remove an effect first)"); return true; }
    if (memUsed() + d->mem > kFxMemBudget) { say(o, "ERR FXA out of memory"); return true; }
    int at = flatIndex(cur_, bk, (int)num(t, 2));
    for (int i = cur_.nb; i > at; i--) cur_.b[i] = cur_.b[i - 1];
    Block& b = cur_.b[at];
    b = Block();
    b.id = next_id_++;
    b.type = d->id;
    b.bank = bk;
    b.np = d->np;
    for (int k = 0; k < d->np; k++) b.p[k] = d->p[k].def;
    cur_.nb++;
    generation_++;
    say(o, "FXA id=%u %s", (unsigned)b.id, d->name);
    say(o, "FX cpu=%u/%u ticks (%u%%) mem_fx=%u", cpuUsed(), kCpuBudget, cpuUsed() * 100 / kCpuBudget, memUsed());
    return true;
  }
  if (eq(c, "FXR")) {
    int i = findBlock(cur_, (uint32_t)num(t, 1));
    if (i < 0) { say(o, "ERR FXR bad argument"); return true; }
    uint32_t id = cur_.b[i].id;
    for (int k = i; k + 1 < cur_.nb; k++) cur_.b[k] = cur_.b[k + 1];
    cur_.nb--;
    generation_++;
    say(o, "FXR ok %u", (unsigned)id);
    return true;
  }
  if (eq(c, "FXM")) {
    int i = t.size() < 4 ? -1 : findBlock(cur_, (uint32_t)num(t, 1));
    if (i < 0 || (!eq(t.c(2), "P") && !eq(t.c(2), "Q"))) { say(o, "ERR FXM bad argument"); return true; }
    Block b = cur_.b[i];
    for (int k = i; k + 1 < cur_.nb; k++) cur_.b[k] = cur_.b[k + 1];
    cur_.nb--;
    b.bank = t.c(2)[0];
    int at = flatIndex(cur_, b.bank, (int)num(t, 3));
    for (int k = cur_.nb; k > at; k--) cur_.b[k] = cur_.b[k - 1];
    cur_.b[at] = b;
    cur_.nb++;
    generation_++;
    say(o, "FXM ok %u", (unsigned)b.id);
    return true;
  }
  if (eq(c, "FXE")) {
    int i = findBlock(cur_, (uint32_t)num(t, 1));
    if (i < 0) { say(o, "ERR FXE bad argument"); return true; }
    bool on = num(t, 2, 1) != 0;
    if (on && !cur_.b[i].on && cpuUsed() + fxById(cur_.b[i].type)->cost > kCpuBudget) { say(o, "ERR FXE not enough CPU"); return true; }
    cur_.b[i].on = on;
    generation_++;
    say(o, "FXE ok cpu=%u/%u ticks (%u%%)", cpuUsed(), kCpuBudget, cpuUsed() * 100 / kCpuBudget);
    return true;
  }
  if (eq(c, "FXP")) {
    int i = t.size() < 4 ? -1 : findBlock(cur_, (uint32_t)num(t, 1));
    int k = (int)num(t, 2);
    if (i < 0 || k < 0 || k >= cur_.b[i].np) { say(o, "ERR FXP bad argument"); return true; }
    const ParamDef& pd = fxById(cur_.b[i].type)->p[k];
    long v = num(t, 3);
    cur_.b[i].p[k] = (int16_t)(v < pd.min ? pd.min : (v > pd.max ? pd.max : v));
    if (cur_.b[i].type == 7 && !strcmp(pd.name, "ir_slot")) live_ir_ = false;  // choosing a slot ends the "just uploaded" override
    say(o, "FXP ok %d", cur_.b[i].p[k]);
    return true;
  }
  if (eq(c, "FXL")) {
    int i = t.size() < 3 ? -1 : findBlock(cur_, (uint32_t)num(t, 1));
    if (i < 0 || num(t, 2) < 0 || num(t, 2) > 2) { say(o, "ERR FXL bad argument"); return true; }
    cur_.b[i].lane = (uint8_t)num(t, 2);
    generation_++;
    say(o, "FXL ok %u %u", (unsigned)cur_.b[i].id, cur_.b[i].lane);
    return true;
  }
  if (eq(c, "AMP")) {
    bool on = num(t, 1, 1) != 0;
    if (on && !cur_.amp_on && cur_.amp_present && cpuUsed() + kAmpCost > kCpuBudget) { say(o, "ERR AMP not enough CPU (needs %u)", kAmpCost); return true; }
    cur_.amp_on = on;
    generation_++;
    say(o, "AMP %d", cur_.amp_on);
    return true;
  }
  if (eq(c, "AMPX")) {
    cur_.amp_present = num(t, 1, 1) != 0;
    int pc = 0;
    for (int i = 0; i < cur_.nb; i++) pc += cur_.b[i].bank == 'P';
    say(o, "AMPX %d %d", cur_.amp_present, pc);
    return true;
  }
  if (eq(c, "AMPEQ")) {
    if (t.size() >= 6) {
      for (int i = 0; i < 5; i++) cur_.ampeq[i] = (int16_t)num(t, i + 1);
      if (t.size() >= 7) cur_.ampeq[5] = (int16_t)num(t, 6);
    }
    const int16_t* e = cur_.ampeq;
    say(o, "AMPEQ volume=%d bass=%d mid=%d treble=%d presence=%d drive=%d", e[0], e[1], e[2], e[3], e[4], e[5]);
    return true;
  }
  if (eq(c, "GAIN")) {
    if (t.size() >= 3) {
      long a = num(t, 1), b = num(t, 2);
      cur_.gain_in = (int8_t)(a < -30 ? -30 : (a > 12 ? 12 : a));
      cur_.gain_out = (int8_t)(b < -30 ? -30 : (b > 12 ? 12 : b));
    }
    say(o, "GAIN in=%d out=%d dB", cur_.gain_in, cur_.gain_out);
    return true;
  }
  return false;
}

bool Engine::cmdPreset(const Tok& t, ILineSink& o) {
  const char* c = t.c(0);
  if (eq(c, "PLIST")) {
    for (int i = 0; i < kPresets; i++) {
      const PresetInfo& p = idx_[i];
      if (!p.used) continue;
      char col[16] = "";
      if (p.color[0]) snprintf(col, sizeof(col), " col=%s", p.color);
      say(o, "PRE %d %d 0 0 '%s' s=%u%s amp=%d", i, p.nb, p.name, p.scene_mask, col, p.amp_present);
    }
    say(o, "PLIST end current=%d", cur_slot_);
    return true;
  }
  if (eq(c, "PLOAD")) {
    long n = num(t, 1, -1);
    if (n < 0 || n >= kPresets || !idx_[n].used) { say(o, "ERR PLOAD empty or invalid slot"); return true; }
    if (!loadStored((int)n, tmp_)) { say(o, "ERR PLOAD flash record corrupt"); return true; }
    int miss = 0;
    applyPreset(tmp_, (int)n, &miss);
    say(o, "PLOAD ok slot=%ld missing=%d ms=12", n, miss);
    return true;
  }
  if (eq(c, "PNEW")) { newChain(); say(o, "PNEW ok"); return true; }
  if (eq(c, "PSAVE")) {
    long s = num(t, 1, -1);
    if (s < 0 || s >= kPresets) { say(o, "ERR PSAVE bad slot"); return true; }
    if (!savePreset((int)s, Rest(t, 2).b)) { save_seq_++; save_ok_ = false; say(o, "ERR PSAVE flash write failed"); return true; }
    save_seq_++; save_ok_ = true;
    say(o, "PSAVE ok slot=%ld capture=%d", s, cur_.capture);
    return true;
  }
  if (eq(c, "PNAME")) {
    long s = num(t, 1, -1);
    if (s < 0 || s >= kPresets || !idx_[s].used || !loadStored((int)s, tmp_)) { say(o, "ERR PNAME bad argument"); return true; }
    copyName(tmp_.name, kNameLen, Rest(t, 2).b);
    if (!writeStored((int)s, tmp_)) { say(o, "ERR PNAME flash write failed"); return true; }
    if (s == cur_slot_) memcpy(cur_.name, tmp_.name, kNameLen);
    say(o, "PNAME ok %ld '%s'", s, tmp_.name);
    return true;
  }
  if (eq(c, "PDEL")) {
    long s = num(t, 1, -1);
    if (s < 0 || s >= kPresets || !idx_[s].used) { say(o, "ERR PDEL bad argument"); return true; }
    char key[16];
    snprintf(key, sizeof(key), "p%02ld", s);
    presets_.erase(key);
    idx_[s] = PresetInfo();
    if (s == cur_slot_) { cur_slot_ = -1; cur_scene_ = -1; }
    say(o, "PDEL ok %ld", s);
    return true;
  }
  if (eq(c, "PCOLOR")) {
    if (t.size() > 1) {
      if (t.is(1, "none")) cur_.color[0] = 0;
      else copyName(cur_.color, 8, t.c(1));
    }
    say(o, "PCOLOR %s preset=%d", cur_.color[0] ? cur_.color : "none", cur_slot_);
    return true;
  }
  if (eq(c, "SNAPS")) {
    char bits[5];
    for (int i = 0; i < kScenes; i++) bits[i] = cur_.sc[i].used ? '1' : '0';
    bits[4] = 0;
    say(o, "SNAPS %s current=%d", bits, cur_scene_);
    return true;
  }
  if (eq(c, "SNAP")) {
    long n = num(t, 1, -1);
    if (n < 0 || n >= kScenes || !cur_.sc[n].used) { say(o, "ERR SNAP empty slot"); return true; }
    int miss = applyScene(cur_.sc[n]);
    cur_scene_ = (int)n;
    bypass_ = false;
    say(o, "SNAP ok %ld missing=%d ms=3", n, miss);
    return true;
  }
  if (eq(c, "SNAPSAVE")) {
    long n = num(t, 1, -1);
    if (n < 0 || n >= kScenes) { say(o, "ERR SNAPSAVE bad argument"); return true; }
    captureScene(cur_.sc[n]);
    if (!persistScene((int)n)) { say(o, "ERR SNAPSAVE flash write failed"); return true; }
    cur_scene_ = (int)n;
    say(o, "SNAPSAVE ok %ld preset=%d", n, cur_slot_);
    return true;
  }
  if (eq(c, "SNAPDEL")) {
    long n = num(t, 1, -1);
    if (n < 0 || n >= kScenes) { say(o, "ERR SNAPDEL bad argument"); return true; }
    cur_.sc[n] = Scene();
    if (!persistScene((int)n)) { say(o, "ERR SNAPDEL flash write failed"); return true; }
    if (cur_scene_ == n) cur_scene_ = -1;
    say(o, "SNAPDEL ok %ld", n);
    return true;
  }
  if (eq(c, "SNAPNAME")) {
    if (t.size() >= 3) {
      long n = num(t, 1, -1);
      if (n < 0 || n >= kScenes) { say(o, "ERR SNAPNAME bad argument"); return true; }
      copyName(cur_.sc[n].name, kSceneNameLen, Rest(t, 2).b);
      if (cur_.sc[n].used && !persistScene((int)n)) { say(o, "ERR SNAPNAME flash write failed"); return true; }
      say(o, "SNAPNAME ok %ld '%s' preset=%d", n, cur_.sc[n].name, cur_slot_);
    } else {
      for (int i = 0; i < kScenes; i++) {
        if (cur_.sc[i].name[0]) say(o, "SNAPNAME %d '%s'", i, cur_.sc[i].name);
        else say(o, "SNAPNAME %d 'Scene %d'", i, i + 1);
      }
      say(o, "SNAPNAME end");
    }
    return true;
  }
  return false;
}

bool Engine::cmdLib(const Tok& t, ILineSink& o) {
  const char* c = t.c(0);
  if (eq(c, "MODEL")) {
    int k = cur_.capture;
    say(o, "MODEL '%s' layers=%d engine=A2L", live_cap_ ? cap_name_ : ((k >= 0 && k < kCaptureSlots && set_.caps[k].used) ? set_.caps[k].name : ""), cap_layers_);
    return true;
  }
  if (eq(c, "CLIST")) {
    for (int i = 0; i < kCaptureSlots; i++) if (set_.caps[i].used) say(o, "CAP %d %u '%s'", i, (unsigned)set_.caps[i].size, set_.caps[i].name);
    say(o, "CLIST end current=%d slots=%d", (cur_.capture >= 0 && cur_.capture < kCaptureSlots && set_.caps[cur_.capture].used) ? cur_.capture : -1, kCaptureSlots);   // an empty slot is never "loaded"
    return true;
  }
  if (eq(c, "ILIST")) {
    for (int i = 0; i < kIrSlots; i++) {
      if (set_.irs[i].used) say(o, "IRS %d %u '%s' fs=48", i, (unsigned)set_.irs[i].size, set_.irs[i].name);
      else if (i < bi_n_ && bi_names_) say(o, "IRS %d 0 '%s' fs=48", i, bi_names_[i]);   // built-in cabinet: ir_slot = slot + 1
    }
    say(o, "ILIST end current=-1 slots=%d", kIrSlots);
    return true;
  }
  if (eq(c, "LDBEGIN") || eq(c, "IRBEGIN")) {
    bool ld = eq(c, "LDBEGIN");
    long n = num(t, 1);
    if (n == 0) { staging_ = false; if (media_) media_->abort(); say(o, "OK %s 0", c); return true; }
    if (ld ? (n < 0 || (size_t)n > kMaxUpload) : (n < 1 || n > 4096)) {
      if (ld) say(o, "ERR LDBEGIN length"); else say(o, "ERR IRBEGIN 1..4096");
      return true;
    }
    if (!ld && num(t, 3, 48) != 24 && num(t, 3, 48) != 48) { say(o, "ERR IRBEGIN fs 24 or 48"); return true; }
    if (media_ && !media_->begin(ld ? 'C' : 'I', ld ? (size_t)n : (size_t)n * 2, ld ? 0 : num(t, 2, 0), ld ? 48 : (int)num(t, 3, 48))) {
      staging_ = false;
      if (ld) say(o, "ERR LDBEGIN length %ld (max 8192)", n); else say(o, "ERR IRBEGIN out of memory");
      return true;
    }
    stage_gain_ = ld ? 0 : num(t, 2, 0);
    staging_ = true;
    stage_kind_ = ld ? 'C' : 'I';
    stage_expected_ = ld ? (size_t)n : (size_t)n * 2;
    stage_received_ = 0;
    stage_crc_ = kCrc32Init;
    say(o, "OK %s %ld", c, n);
    return true;
  }
  if (eq(c, "LDB") || eq(c, "IRB")) {
    if (!staging_ || stage_kind_ != (eq(c, "LDB") ? 'C' : 'I')) { say(o, "ERR %s no upload", c); return true; }
    if (t.size() < 3 || !isHex(t.c(2))) { say(o, "ERR %s hex", c); return true; }
    if ((size_t)num(t, 1, -1) != stage_received_) { say(o, "ERR %s offset", c); return true; }
    size_t n = strlen(t.c(2)) / 2;
    if (stage_received_ + n > stage_expected_) { say(o, "ERR %s offset", c); return true; }
    uint8_t chunk[32]; size_t fill = 0, base = stage_received_;
    for (size_t i = 0; i < n; i++) {
      uint8_t b = (uint8_t)(hexv(t.c(2)[2 * i]) << 4 | hexv(t.c(2)[2 * i + 1]));
      stage_crc_ = crc32_update(stage_crc_, &b, 1);
      chunk[fill++] = b;
      if (fill == sizeof(chunk) || i + 1 == n) { if (media_) media_->data(base, chunk, fill); base += fill; fill = 0; }
    }
    say(o, "OK %s %u %u", c, (unsigned)stage_received_, (unsigned)n);   // (%zu is not supported by the pedal's printf)
    stage_received_ += n;
    return true;
  }
  if (eq(c, "LDEND") || eq(c, "IREND")) {
    if (!staging_) { say(o, "ERR %s no upload", c); return true; }
    staging_ = false;
    char hex[12];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)crc32_final(stage_crc_));
    if (t.size() < 2 || !t.is(1, hex) || stage_received_ != stage_expected_) { if (media_) media_->abort(); say(o, "ERR %s crc mismatch", c); return true; }
    if (eq(c, "LDEND")) {
      char nm[kNameLen] = "esp32"; int layers = 10, ch = 3, rc = 0;
      if (media_) rc = media_->end('C', nm, sizeof(nm), &layers, &ch);
      if (rc == 0) {  // the uploaded capture sounds at once; it belongs to no slot until CSAVE
        last_blob_ = stage_received_; live_cap_ = true; cap_layers_ = layers; copyName(cap_name_, kNameLen, nm);
        cur_.capture = -1; generation_++;
      }
      say(o, "LOAD rc=%d name='%s' layers=%d channels=%d audio=%d engine=A2L ms=5", rc, nm, layers, ch, audio_mode_);
    } else {
      char nm[kNameLen] = ""; int a = 0, b = 0;
      if (media_ && media_->end('I', nm, sizeof(nm), &a, &b) != 0) { say(o, "ERR IREND invalid impulse response"); return true; }
      last_ir_ = stage_received_; live_ir_ = true; generation_++;
      say(o, "IR ok %u samples gain %ld mdB fs 48", (unsigned)(stage_received_ / 2), stage_gain_);
    }
    return true;
  }
  if (eq(c, "CSAVE") || eq(c, "ISAVE")) {
    bool cap = eq(c, "CSAVE");
    long s = num(t, 1, -1);
    if (s < 0 || s >= (cap ? kCaptureSlots : kIrSlots)) { say(o, "ERR %s bad slot", c); return true; }
    size_t sz = cap ? last_blob_ : last_ir_;
    if (!sz) { say(o, "ERR %s nothing to save", c); return true; }
    if (media_ && !media_->save(cap ? 'C' : 'I', (int)s)) { say(o, "ERR %s flash write failed (no room: delete a capture/IR or a preset)", c); return true; }
    LibSlot& ls = cap ? set_.caps[s] : set_.irs[s];
    ls.used = true;
    if (cap) { cur_.capture = (int8_t)s; live_cap_ = false; }
    generation_++;
    ls.size = (uint32_t)sz;
    copyName(ls.name, kNameLen, Rest(t, 2).b);
    if (!saveSettings()) { say(o, "ERR %s flash write failed", c); return true; }
    if (cap) say(o, "CSAVE ok %ld '%s' %u B", s, ls.name, (unsigned)sz);
    else say(o, "ISAVE ok %ld '%s' %u samples fs=48", s, ls.name, (unsigned)(sz / 2));
    return true;
  }
  if (eq(c, "CDEL") || eq(c, "IDEL")) {
    bool cap = eq(c, "CDEL");
    long s = num(t, 1, -1);
    if (s < 0 || s >= (cap ? kCaptureSlots : kIrSlots)) { say(o, "ERR %s invalid slot", c); return true; }
    (cap ? set_.caps[s] : set_.irs[s]) = LibSlot();
    if (media_) media_->erase(cap ? 'C' : 'I', (int)s);
    if (cap && cur_.capture == s) selectCapture();
    generation_++;
    saveSettings();
    say(o, "%s ok %ld", c, s);
    return true;
  }
  if (eq(c, "CLOAD")) {
    long s = num(t, 1, -1);
    if (s < 0 || s >= kCaptureSlots || !set_.caps[s].used) { say(o, "ERR CLOAD invalid"); return true; }
    cur_.capture = (int8_t)s;
    selectCapture();
    generation_++;
    say(o, "CLOAD ok %ld audio=%d", s, audio_mode_);
    return true;
  }
  if (eq(c, "CTRIM")) {  // CTRIM <slot|live> <trim_dB> [<input dBu x10 | ->]: accepted and acknowledged; the level trim itself is not applied by this firmware
    if (t.size() < 3) { say(o, "ERR CTRIM bad argument"); return true; }
    if (t.is(1, "live")) { say(o, "CTRIM ok slot=-1 trim=%s saved=0", t.c(2)); return true; }
    long s = num(t, 1, -1);
    if (s < 0 || s >= kCaptureSlots || !set_.caps[s].used) { say(o, "ERR CTRIM invalid slot"); return true; }
    say(o, "CTRIM ok slot=%ld trim=%s saved=1", s, t.c(2));
    return true;
  }
  if (eq(c, "FXIR")) {  // FXIR <block_id> <slot>: this ir block plays library slot <slot> (ir_slot = slot + 1)
    int i = t.size() < 3 ? -1 : findBlock(cur_, (uint32_t)num(t, 1));
    long s = num(t, 2, -1);
    if (i < 0 || cur_.b[i].type != 7 || s < 0 || s >= kIrSlots) { say(o, "ERR FXIR bad argument"); return true; }
    const FxDef* d = fxById(7);
    for (int k = 0; k < d->np; k++) if (!strcmp(d->p[k].name, "ir_slot")) cur_.b[i].p[k] = (int16_t)(s + 1);
    live_ir_ = false; generation_++;
    say(o, "FXIR ok %s %ld", t.c(1), s);
    return true;
  }
  if (eq(c, "ILOAD")) { say(o, "%s ok %s", c, t.size() > 1 ? t.c(1) : "0"); return true; }
  return false;
}

bool Engine::cmdGlobal(const Tok& t, ILineSink& o) {
  const char* c = t.c(0);
  if (eq(c, "PING")) { say(o, "PONG tankg_fw M5.26 rel amx ble v1 Nov 12 2026 10:00:00"); return true; }
  if (eq(c, "LVL")) {
    uint32_t u = cpuUsed();
    say(o, "LVL in=-300 out=-250 ci=0 co=0 fx=%u/%u mem=1 sk=0 m=%d g=%d/%d a=%d e=1/2 s=%d c=%u p=%d tu=%d fm=%d,0,%d,%d",
        u, kCpuBudget, audio_mode_, cur_.gain_in, cur_.gain_out, cur_.amp_on, cur_scene_, (unsigned)generation_, cur_slot_,
        tuner_on_, (int)set_.layout, set_.bank + 1, bypass_);
    return true;
  }
  if (eq(c, "MEM")) {
    say(o, "MEM free=%u effects=%u fxcap=%u", kFxMemBudget - memUsed(), memUsed(), kFxMemBudget);
    return true;
  }
  if (eq(c, "AUDIO")) {
    audio_mode_ = (t.size() > 1 && t.is(1, "OFF")) ? 0 : 2;
    say(o, "AUDIO %d", audio_mode_);
    return true;
  }
  if (eq(c, "TUNER")) {
    if (t.size() >= 2) {
      tuner_on_ = num(t, 1) == 1;
      if (t.size() >= 3) tuner_mute_ = num(t, 2) == 1;
      if (t.size() >= 4) tuner_ref_ = (uint32_t)num(t, 3, 440);
    }
    say(o, "TUNER %d mute=%d ref=%u", tuner_on_, tuner_mute_, (unsigned)tuner_ref_);
    return true;
  }
  if (eq(c, "TUNE")) {
    if (!tuner_on_) { say(o, "TUNE off"); return true; }
    static const char* const notes[6] = {"E2", "A2", "D3", "G3", "B3", "E4"};
    static const int freqs100[6] = {8241, 11000, 14683, 19600, 24694, 32963};  // Hz x 100
    tune_tick_++;
    int i = (tune_tick_ / 20) % 6;
    int cents10 = ((int)(tune_tick_ % 20) - 10) * 15;  // cents x 10
    int ac = cents10 < 0 ? -cents10 : cents10;
    say(o, "TUNE %s hz=%d.%02d cents=%c%d.%d level=-210 ons=%u", notes[i], freqs100[i] / 100, freqs100[i] % 100,
        cents10 < 0 ? '-' : '+', ac / 10, ac % 10, (unsigned)(tune_tick_ / 20));
    return true;
  }
  if (eq(c, "BPM")) {
    long v = num(t, 1, bpm10_);
    bpm10_ = (int)(v < 400 ? 400 : (v > 3000 ? 3000 : v));
    say(o, "BPM %d.%d", bpm10_ / 10, bpm10_ % 10);
    return true;
  }
  if (eq(c, "HPF")) {
    if (t.size() > 1) set_.hpf = (int16_t)num(t, 1);
    say(o, "HPF hz=%d", set_.hpf);
    return true;
  }
  if (eq(c, "GEQ")) {
    if (t.size() >= 3) {
      long b = num(t, 1);
      b = b < 0 ? 0 : (b > 5 ? 5 : b);
      set_.geq[b] = (int16_t)num(t, 2);
      say(o, "GEQ %ld %d", b, set_.geq[b]);
    } else {
      say(o, "GEQ %d %d %d %d %d", set_.geq[0], set_.geq[1], set_.geq[2], set_.geq[3], set_.geq[4]);
    }
    return true;
  }
  if (eq(c, "LEDCFG")) {
    static const char* const keys[7] = {"anim", "bright", "hold", "idle", "meter", "tap", "tuner"};
    for (int i = 1; i < t.size(); i++) {
      const char* kv = t.c(i);
      const char* sep = strchr(kv, '=');
      if (!sep) continue;
      for (int k = 0; k < 7; k++)
        if (strlen(keys[k]) == (size_t)(sep - kv) && strncmp(kv, keys[k], (size_t)(sep - kv)) == 0) copyName(set_.led[k], 12, sep + 1);
    }
    char body[160];
    size_t bn = 0;
    body[0] = 0;
    for (int k = 0; k < 7; k++) bn += (size_t)snprintf(body + bn, sizeof(body) - bn, k ? " %s=%s" : "%s=%s", keys[k], set_.led[k]);
    say(o, "LEDCFG %s", body);
    return true;
  }
  if (eq(c, "FSMODE")) {
    if (t.size() > 1 && strncmp(t.c(1), "layout=", 7) == 0) {
      const char* l = t.c(1) + 7;
      Layout nl = eq(l, "factory") ? Layout::Factory : (eq(l, "scenes") ? Layout::Scenes : (eq(l, "custom") ? Layout::Custom : set_.layout));
      if (nl == Layout::Scenes && set_.layout != Layout::Scenes) defaultFsa(set_, Layout::Scenes);
      set_.layout = nl;
    }
    static const char* const names[3] = {"factory", "scenes", "custom"};
    say(o, "FSMODE layout=%s mode=preset bank=%d bypass=%d", names[(int)set_.layout], set_.bank + 1, bypass_);
    return true;
  }
  if (eq(c, "FSA")) {
    if (t.size() >= 4) {
      long f = num(t, 1, -1), g = num(t, 2, -1);
      if (f < 0 || f > 3 || g < 0 || g > 2) { say(o, "ERR FSA bad argument"); return true; }
      set_.fsa[f][g] = {(uint8_t)num(t, 3), (uint8_t)num(t, 4), (uint8_t)num(t, 5, 255)};
      say(o, "FSA %ld %ld %d %d %d", f, g, set_.fsa[f][g].type, set_.fsa[f][g].a, set_.fsa[f][g].b);
    } else {
      for (int f = 0; f < 4; f++)
        for (int g = 0; g < 3; g++) say(o, "FSA %d %d %d %d %d", f, g, set_.fsa[f][g].type, set_.fsa[f][g].a, set_.fsa[f][g].b);
      say(o, "FSA end");
    }
    return true;
  }
  if (eq(c, "FSPRESS")) {
    if (t.size() >= 3) {
      long f = num(t, 1, -1);
      if (f < 0 || f > 3) { say(o, "ERR FSPRESS bad argument"); return true; }
      set_.fspress[f] = num(t, 2) != 0;
    }
    say(o, "FSPRESS %d %d %d %d acts=%d", set_.fspress[0], set_.fspress[1], set_.fspress[2], set_.fspress[3], 4);
    return true;
  }
  if (eq(c, "FSDO")) {  // dev: run a gesture as if the pedal had been used
    long f = num(t, 1, -1), g = num(t, 2, -1);
    if (f < 0 || f > 3 || g < 0 || g > 2) { say(o, "ERR FSDO bad argument"); return true; }
    fsGesture((uint8_t)f, (uint8_t)g);
    say(o, "FSDO ok %ld %ld", f, g);
    return true;
  }
  if (eq(c, "FSLED")) {
    auto show = [&](int f) { say(o, "FSLED %d %s on=%s off=%s", f, set_.fsled[f].custom ? "custom" : "auto", set_.fsled[f].on, set_.fsled[f].off); };
    if (t.size() >= 3) {
      long f = num(t, 1, 0);
      f = f < 0 ? 0 : (f > 3 ? 3 : f);
      if (t.is(2, "auto")) set_.fsled[f].custom = false;
      else {
        set_.fsled[f].custom = true;
        copyName(set_.fsled[f].on, 8, t.c(2));
        copyName(set_.fsled[f].off, 8, t.size() > 3 ? t.c(3) : "000000");
      }
      show((int)f);
    } else {
      for (int f = 0; f < 4; f++) show(f);
      say(o, "FSLED end");
    }
    return true;
  }
  if (eq(c, "FSST")) {  // dev: footswitch/LED state
    say(o, "FSST layout=%d bank=%d pending=%d bypass=%d preset=%d scene=%d tuner=%d ble=%d/%d leds=%06x,%06x,%06x,%06x",
        (int)set_.layout, set_.bank + 1, bank_pending_, bypass_, cur_slot_, cur_scene_, tuner_on_, ble_on_, ble_pair_,
        (unsigned)ledColor(0, now_), (unsigned)ledColor(1, now_), (unsigned)ledColor(2, now_), (unsigned)ledColor(3, now_));
    return true;
  }
  if (eq(c, "GSAVE")) {
    if (!saveSettings()) { say(o, "ERR GSAVE flash write failed"); return true; }
    say(o, "GSAVE ok");
    return true;
  }
  if (eq(c, "STORE")) {  // dev: storage backends + what is on flash
    int n = 0;
    for (int i = 0; i < kPresets; i++) n += idx_[i].used;
    say(o, "STORE presets=%s settings=%s used=%d/%d", presets_.backend(), settings_.backend(), n, kPresets);
    return true;
  }
  if (eq(c, "FACTORYRESET")) {  // dev: wipe flash and reseed demo presets
    for (int i = 0; i < kPresets; i++) { char k[16]; snprintf(k, sizeof(k), "p%02d", i); presets_.erase(k); idx_[i] = PresetInfo(); }
    settings_.erase("gset");
    set_ = Settings();
    defaultFsa(set_, Layout::Scenes);
    seedDefaults();
    say(o, "FACTORYRESET ok");
    return true;
  }
  return false;
}

void Engine::handleLine(const char* line, ILineSink& out) {
  if (strlen(line) > kMaxLine) { say(out, "ERR line too long"); return; }
  Tok t(line);
  if (t.empty()) return;
  if (cmdGlobal(t, out) || cmdChain(t, out) || cmdPreset(t, out) || cmdLib(t, out)) return;
  say(out, "ERR %s unknown command", t.c(0));
}

}  // namespace pd
