#include "pd/engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd/catalog.h"

namespace pd {

static const char* const kLedDefaultOn[4] = {"28beff", "28ff5a", "ffaa1e", "e63cff"};

static void presetKey(int slot, char* k) { snprintf(k, 16, "p%02d", slot & 31); }

Engine::Engine(IBlobStore& presets, IBlobStore& settings) : presets_(presets), settings_(settings), fsm_(*this) {
  defaultFsa(set_, Layout::Scenes);
  copyName(cur_.name, kNameLen, "");
}

void Engine::emit(const char* fmt, ...) {
  if (!async_) return;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  async_->line(buf);
}

// ------------------------------------------------------------------ persistence

void Engine::refreshIndex(int slot) {
  idx_[slot] = PresetInfo();
  if (loadStored(slot, tmp_)) infoFromPreset(tmp_, idx_[slot]);
}

bool Engine::loadStored(int slot, Preset& p) {
  char key[16];
  presetKey(slot, key);
  size_t n = 0;
  if (!presets_.read(key, io_, sizeof(io_), n)) return false;
  return decodePreset(io_, n, p) == CodecResult::Ok;
}

bool Engine::writeStored(int slot, const Preset& p) {
  char key[16];
  presetKey(slot, key);
  size_t n = encodePreset(p, io_, sizeof(io_));
  if (!n || !presets_.write(key, io_, n)) return false;
  refreshIndex(slot);
  return true;
}

bool Engine::saveSettings() {
  set_.last_preset = (int8_t)cur_slot_;
  size_t n = encodeSettings(set_, io_, sizeof(io_));
  return n && settings_.write("gset", io_, n);
}

void Engine::begin() {
  bool loaded = false;
  {
    size_t n = 0;
    if (settings_.read("gset", io_, sizeof(io_), n)) loaded = decodeSettings(io_, n, set_) == CodecResult::Ok;
  }
  for (int i = 0; i < kPresets; i++) refreshIndex(i);
  if (!loaded) {
    set_ = Settings();
    defaultFsa(set_, Layout::Scenes);
    seedDefaults();
    return;
  }
  int last = set_.last_preset;
  if (last >= 0 && last < kPresets && idx_[last].used && loadStored(last, tmp_)) applyPreset(tmp_, last, nullptr);
  else newChain();
}

// The preset's capture slot becomes the live amp model (or none: the platform falls back to its own amp stage).
void Engine::selectCapture() {
  live_cap_ = false;
  if (!media_) return;
  int k = cur_.capture;
  media_->select('C', (k >= 0 && k < kCaptureSlots && set_.caps[k].used) ? k : -1);
}

void Engine::seedDefaults() {
  newChain();
  auto add = [&](char bank, const char* type, bool on) {
    const FxDef* d = fxByName(type);
    Block& b = cur_.b[cur_.nb++];
    b = Block();
    b.id = next_id_++;
    b.type = d->id;
    b.bank = bank;
    b.on = on;
    b.np = d->np;
    for (int i = 0; i < d->np; i++) b.p[i] = d->p[i].def;
  };
  add('P', "boost", true);
  add('P', "overdrive", false);
  add('Q', "ir", true);
  add('Q', "delay", true);
  add('Q', "reverb2", true);
  cur_slot_ = -1;
  savePreset(0, "Clean Demo");
  cur_.b[1].on = true;
  cur_.b[1].p[0] = 30;
  savePreset(1, "Crunch Demo");
  if (loadStored(0, tmp_)) applyPreset(tmp_, 0, nullptr);
  saveSettings();
}

// ------------------------------------------------------------------ chain / presets / scenes

void Engine::newChain() {
  rearmKnobs();
  cur_ = Preset();
  cur_slot_ = -1;
  cur_scene_ = -1;
  bypass_ = false;
  generation_++;
}

uint32_t Engine::cpuUsed() const {
  uint32_t c = 0;
  for (int i = 0; i < cur_.nb; i++)
    if (cur_.b[i].on) c += fxById(cur_.b[i].type)->cost;
  return c + ((cur_.amp_on && cur_.amp_present) ? kAmpCost : 0);
}

uint32_t Engine::memUsed() const {
  uint32_t m = cur_.amp_present ? kAmpMem : 0;
  for (int i = 0; i < cur_.nb; i++) m += fxById(cur_.b[i].type)->mem;
  return m;
}

bool Engine::applyPreset(const Preset& p, int slot, int* missing) {
  // p may be tmp_ (never cur_). Copy, then compact the chain in place against the budget.
  cur_ = p;
  int keep = 0, miss = 0;
  uint32_t cpu = (p.amp_on && p.amp_present) ? kAmpCost : 0;
  uint32_t mem = p.amp_present ? kAmpMem : 0;
  for (int i = 0; i < p.nb; i++) {
    const FxDef* d = fxById(p.b[i].type);
    uint32_t c = p.b[i].on ? d->cost : 0;
    if (cpu + c > kCpuBudget || mem + d->mem > kFxMemBudget) { miss++; continue; }
    cpu += c;
    mem += d->mem;
    cur_.b[keep] = p.b[i];
    cur_.b[keep].id = next_id_++;
    keep++;
  }
  for (int i = keep; i < kMaxBlocks; i++) cur_.b[i] = Block();
  cur_.nb = (uint8_t)keep;
  cur_slot_ = slot;
  cur_scene_ = -1;
  bypass_ = false;
  bank_pending_ = false;
  if (slot >= 0) set_.bank = (uint8_t)(slot / 4);
  generation_++;
  rearmKnobs();
  live_ir_ = false;
  selectCapture();
  if (missing) *missing = miss;
  return true;
}

bool Engine::savePreset(int slot, const char* name) {
  if (slot < 0 || slot >= kPresets) return false;
  Preset& out = tmp_;
  out = cur_;
  for (int i = 0; i < out.nb; i++) out.b[i].id = 0;
  if (name && *name) copyName(out.name, kNameLen, name);
  else if (!out.name[0]) {
    char tmp[kNameLen];
    snprintf(tmp, sizeof(tmp), "Preset %d", slot + 1);
    copyName(out.name, kNameLen, tmp);
  }
  bool saveAs = slot != cur_slot_;
  if (saveAs)  // scenes belong to the preset they were captured in
    for (int s = 0; s < kScenes; s++) out.sc[s] = Scene();
  if (!writeStored(slot, out)) return false;
  memcpy(cur_.name, out.name, kNameLen);
  if (saveAs) {
    for (int s = 0; s < kScenes; s++) cur_.sc[s] = Scene();
    cur_scene_ = -1;
  }
  cur_slot_ = slot;
  set_.bank = (uint8_t)(slot / 4);
  return true;
}

void Engine::captureScene(Scene& s) const {
  char keep[kSceneNameLen];
  memcpy(keep, s.name, sizeof(keep));
  s = Scene();
  memcpy(s.name, keep, sizeof(keep));
  s.used = true;
  s.gain_in = cur_.gain_in;
  s.gain_out = cur_.gain_out;
  s.amp_on = cur_.amp_on;
  memcpy(s.ampeq, cur_.ampeq, sizeof(s.ampeq));
  s.nb = cur_.nb;
  for (int i = 0; i < cur_.nb; i++) {
    s.b[i].on = cur_.b[i].on;
    memcpy(s.b[i].p, cur_.b[i].p, sizeof(s.b[i].p));
  }
}

int Engine::applyScene(const Scene& s) {
  cur_.gain_in = s.gain_in;
  cur_.gain_out = s.gain_out;
  cur_.amp_on = s.amp_on;
  memcpy(cur_.ampeq, s.ampeq, sizeof(cur_.ampeq));
  int missing = s.nb > cur_.nb ? s.nb - cur_.nb : 0;
  int n = s.nb < cur_.nb ? s.nb : cur_.nb;
  for (int i = 0; i < n; i++) {
    cur_.b[i].on = s.b[i].on;
    memcpy(cur_.b[i].p, s.b[i].p, sizeof(cur_.b[i].p));
  }
  uint32_t cpu = (cur_.amp_on && cur_.amp_present) ? kAmpCost : 0;  // never exceed the budget
  for (int i = 0; i < cur_.nb; i++) {
    if (!cur_.b[i].on) continue;
    uint32_t c = fxById(cur_.b[i].type)->cost;
    if (cpu + c > kCpuBudget) { cur_.b[i].on = false; missing++; }
    else cpu += c;
  }
  generation_++;
  return missing;
}

bool Engine::persistScene(int n) {
  if (cur_slot_ < 0) return true;
  if (!loadStored(cur_slot_, tmp_)) return false;
  tmp_.sc[n] = cur_.sc[n];
  return writeStored(cur_slot_, tmp_);
}

int Engine::nextUsed(int from, int dir) const {
  for (int k = 1; k <= kPresets; k++) {
    int s = ((from < 0 ? (dir > 0 ? -1 : 0) : from) + dir * k + kPresets * 2) % kPresets;
    if (idx_[s].used) return s;
  }
  return -1;
}

// ------------------------------------------------------------------ footswitch actions

void Engine::doBypass(bool on) {
  if (on && !bypass_) {
    memset(bypass_snap_.name, 0, kSceneNameLen);
    captureScene(bypass_snap_);
    for (int i = 0; i < cur_.nb; i++) cur_.b[i].on = false;
    cur_.amp_on = false;
    bypass_ = true;
  } else if (!on && bypass_) {
    applyScene(bypass_snap_);
    bypass_ = false;
  }
  generation_++;
  emit("EVT bypass %d", bypass_ ? 1 : 0);
}

void Engine::sceneGoto(int n) {
  if (n < 0 || n >= kScenes || !cur_.sc[n].used) { emit("EVT scene %d empty", n); return; }
  bypass_ = false;
  applyScene(cur_.sc[n]);
  cur_scene_ = n;
  rearmKnobs();
  emit("EVT scene %d", n);
}

void Engine::presetGoto(int slot) {
  if (slot < 0 || slot >= kPresets || !idx_[slot].used || !loadStored(slot, tmp_)) { emit("EVT preset %d empty", slot); return; }
  applyPreset(tmp_, slot, nullptr);
  emit("EVT preset %d", slot);
}

void Engine::toggleBlockByType(uint8_t type, uint8_t nth) {
  uint8_t seen = 0;
  for (int i = 0; i < cur_.nb; i++) {
    if (cur_.b[i].type != type) continue;
    if (seen++ == nth || nth == 255) {
      cur_.b[i].on = !cur_.b[i].on;
      generation_++;
      emit("EVT block %u on=%d", (unsigned)cur_.b[i].id, cur_.b[i].on ? 1 : 0);
      return;
    }
  }
  emit("EVT block type=%u missing", (unsigned)type);
}

void Engine::tap(uint32_t now) {
  uint32_t dt = now - last_tap_;
  if (last_tap_ && dt >= 200 && dt <= 2500) {
    int b = (int)(600000u / dt);
    bpm10_ = b < 400 ? 400 : (b > 3000 ? 3000 : b);
  }
  last_tap_ = now;
  emit("EVT tap bpm=%d.%d", bpm10_ / 10, bpm10_ % 10);
}

void Engine::execAction(const FsAction& a) {
  switch (a.type) {
    case 1: sceneGoto(a.a); break;
    case 2: presetGoto(a.a); if (a.b < kScenes) sceneGoto(a.b); break;
    case 3: presetGoto(nextUsed(cur_slot_, +1)); break;
    case 4: presetGoto(nextUsed(cur_slot_, -1)); break;
    case 5:
    case 6: {
      int dir = a.type == 5 ? 1 : -1;
      for (int k = 1; k <= kScenes; k++) {
        int s = ((cur_scene_ < 0 ? (dir > 0 ? -1 : 0) : cur_scene_) + dir * k + kScenes * 2) % kScenes;
        if (cur_.sc[s].used) { sceneGoto(s); return; }
      }
      break;
    }
    case 7: tap(now_); break;
    case 8: toggleBlockByType(a.a, a.b); break;
    case 9: tuner_on_ = !tuner_on_; emit("EVT tuner %d", tuner_on_ ? 1 : 0); break;
    case 10: cur_.amp_on = !cur_.amp_on; generation_++; emit("EVT amp %d", cur_.amp_on ? 1 : 0); break;
    default: break;
  }
}

void Engine::factoryGesture(uint8_t fs, uint8_t g) {
  if (mode_ == Mode::Live) {
    if (g == kShort) { liveToggle(fs); return; }
    if (g == kLong && cur_slot_ >= 0) {  // holding in Live saves the knob/footswitch edits into the CURRENT preset
      bool ok = savePreset(cur_slot_, idx_[cur_slot_].used ? idx_[cur_slot_].name : cur_.name);
      save_seq_++; save_ok_ = ok; emit("EVT save %d %s", cur_slot_, ok ? "ok" : "fail");
      return;
    }
    return;
  }
  int slot = set_.bank * 4 + fs;
  if (g == kShort) {
    if (slot == cur_slot_ && !bank_pending_) {
      doBypass(!bypass_);
    } else {
      bypass_ = false;
      presetGoto(slot);
      bank_pending_ = false;
    }
  } else if (g == kLong) {  // holding saves, like the factory layout
    bool ok = savePreset(slot, idx_[slot].used ? idx_[slot].name : cur_.name);
    bank_pending_ = false;
    save_seq_++; save_ok_ = ok;
    emit("EVT save %d %s", slot, ok ? "ok" : "fail");
  }
}

bool Engine::fsDoubleEnabled(uint8_t fs) const { return set_.layout != Layout::Factory && fs < 4 && set_.fsa[fs][2].type != 0; }

void Engine::fsGesture(uint8_t fs, uint8_t g) {
  emit("EVT fs %u %u", fs, g);
  if (set_.layout == Layout::Factory) factoryGesture(fs, g);
  else execAction(set_.fsa[fs][g]);
}

void Engine::fsCombo(uint8_t kind) {
  switch (kind) {
    case kPrevBank:
    case kNextBank:
      if (set_.layout != Layout::Factory) return;
      set_.bank = (uint8_t)((set_.bank + (kind == kNextBank ? 1 : 7)) & 7);
      bank_pending_ = true;
      emit("EVT bank %d", set_.bank + 1);
      break;
    case kTunerHold: tuner_on_ = !tuner_on_; emit("EVT tuner %d", tuner_on_ ? 1 : 0); break;
    case kBlePair: ble_on_ = true; ble_pair_ = true; emit("EVT ble pair"); break;
    case kBleOff: ble_on_ = false; ble_pair_ = false; emit("EVT ble off"); break;
    case kModeToggle: if (set_.layout == Layout::Factory) setMode(mode_ == Mode::Live ? Mode::Preset : Mode::Live); break;
  }
}

void Engine::tick(uint32_t now_ms) {
  now_ = now_ms;
  fsm_.tick(now_ms);
}

static uint32_t parseHex(const char* s) { return (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFF; }

uint32_t Engine::ledColor(uint8_t f, uint32_t now) const {
  if (f >= 4) return 0;
  uint32_t base = set_.fsled[f].custom ? parseHex(set_.fsled[f].on) : parseHex(kLedDefaultOn[f]);
  int level = 0;  // percent
  if (set_.layout == Layout::Factory) {
    int slot = set_.bank * 4 + f;
    if (mode_ == Mode::Live) level = levelOf(f);
    else if (bypass_) level = 0;
    else if (bank_pending_) level = idx_[slot].used ? ((now / 250) % 2 ? 100 : 0) : 0;
    else level = slot == cur_slot_ ? 100 : (idx_[slot].used ? 25 : 0);
  } else {
    const FsAction& a = set_.fsa[f][0];
    switch (a.type) {
      case 0: level = 0; break;
      case 1: level = (a.a < kScenes && cur_scene_ == a.a) ? 100 : ((a.a < kScenes && cur_.sc[a.a].used) ? 25 : 0); break;
      case 2: level = a.a == cur_slot_ ? 100 : ((a.a < kPresets && idx_[a.a].used) ? 25 : 0); break;
      case 7: level = (now % (600000u / (uint32_t)bpm10_ * 10u / 10u + 1)) < 60 ? 100 : 25; break;
      case 8: {
        level = 25;
        for (int i = 0; i < cur_.nb; i++)
          if (cur_.b[i].type == a.a) { level = cur_.b[i].on ? 100 : 25; break; }
        break;
      }
      case 9: level = tuner_on_ ? 100 : 25; break;
      case 10: level = cur_.amp_on ? 100 : 25; break;
      default: level = 25; break;
    }
  }
  int bright = atoi(set_.led[1]);
  if (bright < 0) bright = 0;
  if (bright > 100) bright = 100;
  level = level * bright / 100;
  uint32_t r = ((base >> 16) & 0xFF) * level / 100, g = ((base >> 8) & 0xFF) * level / 100, b = (base & 0xFF) * level / 100;
  return (r << 16) | (g << 8) | b;
}

}  // namespace pd
