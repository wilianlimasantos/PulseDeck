// Preset/Live mode, footswitch live toggles and physical knobs with soft takeover. Behaviour specified in docs (clean room): see NOTICE.md.
#include <stdio.h>
#include <string.h>

#include "pd/catalog.h"
#include "pd/engine.h"

namespace pd {

namespace {
constexpr int kTake = 16;       // raw counts around the saved value that engage a knob
constexpr int kApplyDead = 4;   // raw counts of movement before a new value is applied
constexpr int kGateOff = 20;    // <= 2 % of 1023 : gate off
constexpr int kGateOn = 23;     // hysteresis: > this turns it on
const char* const kCats[4] = {"reverb", "delay", "mod", "amp"};
struct NameSet { const char* cat; const char* n[4]; };
// knob slot -> (category, parameter names tried in order). Slots not listed are special (0 master, 1 IR, 8..12 amp, 13 type, 14 gate).
const NameSet kMap[kKnobs] = {
    {nullptr, {nullptr, nullptr}},          // 0 MASTER
    {nullptr, {nullptr, nullptr}},          // 1 IR CAB
    {"reverb", {"mezcla_pct", nullptr}},    // 2 RVB MIX
    {"reverb", {"decay_pct", nullptr}},     // 3 RVB DECAY
    {"delay", {"tiempo_ms", nullptr}},      // 4 DLY TIME
    {"delay", {"mezcla_pct", nullptr}},     // 5 DLY MIX
    {"mod", {"vel_chz", "rapido_chz", "semitonos_st", "sens_pct"}},     // 6 MOD SPEED (rate; pitch interval; filter sensitivity)
    {"mod", {"prof_pct", "mezcla_pct", "detune_ct", nullptr}},    // 7 MOD FX (depth, else the mix)
    {"amp", {"nivel_db", nullptr}},         // 8 VOLUME
    {"amp", {"graves_pct", nullptr}},       // 9 BASS
    {"amp", {"medios_pct", nullptr}},       // 10 MIDDLE
    {"amp", {"agudos_pct", nullptr}},       // 11 TREBLE
    {"amp", {"ganancia_pct", nullptr}},     // 12 GAIN
    {nullptr, {nullptr, nullptr}},          // 13 TYPE (captures are not loaded by this firmware)
    {nullptr, {nullptr, nullptr}},          // 14 NOISE GATE
};
const int8_t kAmpEq[kKnobs] = {-1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 5, -1, -1};  // AMPEQ field per knob (volume,bass,mid,treble,presence,gain)
int paramIndex(const FxDef* d, const char* name) {
  for (int k = 0; k < d->np; k++) if (!strcmp(d->p[k].name, name)) return k;
  return -1;
}
int sgn(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
int absi(int v) { return v < 0 ? -v : v; }
}  // namespace

int Engine::firstBlockOf(const char* cat, bool onlyOn) const {
  for (int i = 0; i < cur_.nb; i++) {
    const FxDef* d = fxById(cur_.b[i].type);
    if (d && !strcmp(d->cat, cat) && (!onlyOn || cur_.b[i].on)) return i;
  }
  return -1;
}

void Engine::rearmKnobs() {
  for (int i = 0; i < kKnobs; i++) {
    kn_[i].armed = false;
    kn_[i].got = false;
    if (kn_[i].state == KnobState::Engaged) kn_[i].state = KnobState::Disengaged;
  }
}

void Engine::setMode(Mode m) {
  if (m == mode_) return;
  mode_ = m;
  if (m == Mode::Live) rearmKnobs();
  else for (int i = 1; i < kKnobs; i++) if (i != 14) { kn_[i].state = KnobState::Inert; kn_[i].armed = false; kn_[i].got = false; }   // Preset mode: knobs (but MASTER, NOISE GATE) are ignored, LEDs show fixed state
  emit("EVT mode %s", m == Mode::Live ? "live" : "preset");
}

bool Engine::resolveKnob(uint8_t slot, Tgt& t) const {
  t = Tgt();
  if (slot == 1) {  // IR CAB: picks the cabinet 1..8 of the IR block, only while that block is ON (on/off is never a knob's job in Live)
    int i = -1;
    for (int j = 0; j < cur_.nb; j++) if (cur_.b[j].type == 7) { i = j; break; }
    if (i < 0 || !cur_.b[i].on) return false;
    t.kind = 3; t.blk = i; t.k = paramIndex(fxById(7), "ir_slot"); t.lo = 1; t.hi = 8;
    return t.k >= 0;
  }
  if (slot >= 8 && slot <= 12 && cur_.amp_present && cur_.amp_on) {  // the capture's "gain and tone"
    int f = kAmpEq[slot];
    t.kind = 2; t.k = f; t.lo = f == 0 ? -240 : -120; t.hi = 120;
    return true;
  }
  if (slot < 2 || slot > 12) return false;
  const NameSet& m = kMap[slot];
  int i = firstBlockOf(m.cat, true);
  if (i < 0) return false;
  const FxDef* d = fxById(cur_.b[i].type);
  for (int a = 0; a < 4 && m.n[a]; a++) {
    int k = paramIndex(d, m.n[a]);
    if (k >= 0) { t.kind = 1; t.blk = i; t.k = k; t.lo = d->p[k].min; t.hi = d->p[k].max; return true; }
  }
  return false;
}

int Engine::tgtGet(const Tgt& t) const {
  if (t.kind == 1) return cur_.b[t.blk].p[t.k];
  if (t.kind == 2) return cur_.ampeq[t.k];
  if (t.kind == 3) { int v = cur_.b[t.blk].p[t.k]; return v < 1 ? 1 : (v > 8 ? 8 : v); }
  return 0;
}

void Engine::tgtSet(const Tgt& t, int v) {
  if (v < t.lo) v = t.lo;
  if (v > t.hi) v = t.hi;
  if (t.kind == 1) cur_.b[t.blk].p[t.k] = (int16_t)v;
  else if (t.kind == 2) cur_.ampeq[t.k] = (int16_t)v;
  else if (t.kind == 3) cur_.b[t.blk].p[t.k] = (int16_t)v;
  generation_++;   // LVL "c=": PulseDeck polls it and re-reads the chain when it changes (knobs turned on the pedal show up in the editor)
}

int Engine::tgtRaw(const Tgt& t) const {  // current value -> where the knob should be (0..1023)
  int v = tgtGet(t);
  if (t.kind == 3) return (v - 1) * 1023 / 7;
  int span = t.hi - t.lo;
  return span > 0 ? (v - t.lo) * 1023 / span : 0;
}

void Engine::gateKnob(uint16_t raw) {
  if (!gateInit_) { gateInit_ = true; gateRaw0_ = raw; kn_[14].state = KnobState::Disengaged; return; }
  if (!gateOwned_) {  // the knob takes the gate over the first time it is really moved; at boot the preset's own state stands
    if (absi((int)raw - (int)gateRaw0_) < 8) return;
    gateOwned_ = true;
  }
  kn_[14].state = KnobState::Engaged;
  gateOn_ = gateOn_ ? raw > kGateOff : raw > kGateOn;  // absolute position decides, re-asserted on every call
  int i = -1;
  for (int j = 0; j < cur_.nb; j++) if (cur_.b[j].type == 1) { i = j; break; }
  if (!gateOn_) {
    if (i >= 0 && cur_.b[i].on) { cur_.b[i].on = false; generation_++; emit("EVT knob 14 off"); }
    return;
  }
  if (i < 0) {  // preset without gate: add one at the start of the chain
    const FxDef* d = fxByName("gate");
    if (!d || cur_.nb >= kMaxBlocks || cpuUsed() + d->cost > kCpuBudget || memUsed() + d->mem > kFxMemBudget) return;
    for (int j = cur_.nb; j > 0; j--) cur_.b[j] = cur_.b[j - 1];
    Block& b = cur_.b[0];
    b = Block(); b.id = next_id_++; b.type = d->id; b.bank = 'P'; b.np = d->np; b.on = false;
    for (int k = 0; k < d->np; k++) b.p[k] = d->p[k].def;
    cur_.nb++; i = 0;
  }
  int thr = -90 + (int)((uint32_t)raw * 60u / 1023u);  // umbral_db -90 (almost open) .. -30
  bool changed = false;
  if (!cur_.b[i].on) {
    if (cpuUsed() + fxById(1)->cost > kCpuBudget) return;
    cur_.b[i].on = true; changed = true;
  }
  if (cur_.b[i].p[0] != thr) { cur_.b[i].p[0] = (int16_t)thr; changed = true; }
  if (changed) { generation_++; emit("EVT knob 14 %d", thr); }
}

KnobState Engine::knob(uint8_t slot, uint16_t raw) {
  if (slot >= kKnobs) return KnobState::Inert;
  if (raw > 1023) raw = 1023;
  Kn& k = kn_[slot];
  k.raw = raw;
  if (slot == 0) { master_ = raw; k.state = KnobState::Engaged; return k.state; }
  if (slot == 14) { gateKnob(raw); return k.state; }
  if (mode_ != Mode::Live) { k.state = KnobState::Inert; k.armed = false; k.got = false; return k.state; }   // Preset mode: ignored completely
  Tgt t;
  if (slot == 13 || !resolveKnob(slot, t)) { k.armed = false; k.got = false; k.state = KnobState::Inert; return k.state; }
  int tr = tgtRaw(t), d = (int)raw - tr;
  if (!k.armed) {  // first look after a preset/scene change or entering Live: engaged only if already on the saved value
    k.armed = true; k.side = (int8_t)sgn(d); k.got = false;
    k.state = absi(d) <= kTake ? KnobState::Engaged : KnobState::Disengaged;
    if (k.state == KnobState::Engaged) { k.applied = raw; k.got = true; }
    return k.state;
  }
  if (k.state != KnobState::Engaged) {
    int s = sgn(d);
    if (absi(d) <= kTake || (s != 0 && k.side != 0 && s != k.side)) { k.state = KnobState::Engaged; k.got = false; }
    else { if (k.side == 0) k.side = (int8_t)s; return k.state; }
  }
  if (!k.got || absi((int)raw - (int)k.applied) >= kApplyDead) {
    k.got = true; k.applied = raw;
    int v = t.kind == 3 ? 1 + (int)((uint32_t)raw * 8u / 1024u) : t.lo + (int)(((int32_t)raw * (t.hi - t.lo) + 511) / 1023);
    if (v != tgtGet(t)) { tgtSet(t, v); emit("EVT knob %d %d", slot, tgtGet(t)); }
  }
  return k.state;
}

int Engine::levelOf(int c) const {
  if (c == 3 && cur_.amp_present) return cur_.amp_on ? 100 : 25;
  int i = firstBlockOf(kCats[c], false);
  return i < 0 ? 0 : (cur_.b[i].on ? 100 : 25);
}

void Engine::liveToggle(uint8_t fs) {
  if (fs >= 4) return;
  bypass_ = false;
  if (fs == 3 && cur_.amp_present) {
    if (!cur_.amp_on && cpuUsed() + kAmpCost > kCpuBudget) { emit("EVT live amp cpu"); return; }
    cur_.amp_on = !cur_.amp_on; generation_++; emit("EVT live %d %d", fs, cur_.amp_on ? 1 : 0); return;
  }
  int i = firstBlockOf(kCats[fs], false);
  if (i < 0) { emit("EVT live %d missing", fs); return; }
  if (!cur_.b[i].on && cpuUsed() + fxById(cur_.b[i].type)->cost > kCpuBudget) { emit("EVT live %d cpu", fs); return; }
  cur_.b[i].on = !cur_.b[i].on; generation_++;
  emit("EVT live %d %d", fs, cur_.b[i].on ? 1 : 0);
}

bool Engine::knobLed(uint8_t slot, uint32_t now) const {
  if (slot >= kKnobs) return false;
  if (slot == 0) return true;
  if (mode_ == Mode::Live) {
    if (slot == 14) { for (int j = 0; j < cur_.nb; j++) if (cur_.b[j].type == 1) return cur_.b[j].on; return false; }   // lit only while the gate block is ON
    Tgt t; if (slot == 13 || !resolveKnob(slot, t)) return false;   // block off / absent: ring dark, whatever the knob does
    KnobState s = kn_[slot].state;
    return s == KnobState::Engaged ? true : (s == KnobState::Disengaged ? ((now / 250) % 2) != 0 : false);
  }
  if (bypass_) return false;
  switch (slot) {
    case 1: for (int j = 0; j < cur_.nb; j++) if (cur_.b[j].type == 7) return cur_.b[j].on; return false;
    case 2: case 3: return levelOf(0) == 100;
    case 4: case 5: return levelOf(1) == 100;
    case 6: case 7: return levelOf(2) == 100;
    case 8: case 9: case 10: case 11: case 12: return levelOf(3) == 100;
    case 14: for (int j = 0; j < cur_.nb; j++) if (cur_.b[j].type == 1) return cur_.b[j].on; return false;
    default: return false;
  }
}

}  // namespace pd
