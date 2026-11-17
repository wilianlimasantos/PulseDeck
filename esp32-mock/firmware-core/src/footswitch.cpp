#include "pd/footswitch.h"

namespace pd {

static bool adjacent(uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) == 1; }

void Footswitches::clearComboIfIdle() {
  if (!s_[0].down && !s_[1].down && !s_[2].down && !s_[3].down) {
    comboMask_ = 0;
    comboStage_ = 0;
    toggleDone_ = false;
  }
}

void Footswitches::event(uint8_t fs, bool down, uint32_t now) {
  if (fs >= 4) return;
  S& s = s_[fs];
  if (down) {
    if (s.down) return;
    s.down = true;
    s.t_down = now;
    s.consumed = false;
    if (s.pending) {  // second tap inside the window -> double press
      s.pending = false;
      s.consumed = true;
      host_.fsGesture(fs, kDouble);
      return;
    }
    // B (or C) was just released and its short press is still deferred: this press makes it a B+C combo instead
    if ((fs == 1 || fs == 2) && s_[3 - fs].defer && now - s_[3 - fs].t_def < kDeferMs) {
      S& o = s_[3 - fs];
      o.defer = false; o.consumed = false; s.consumed = true;
      comboMask_ = 0x06; comboT_ = now; comboStage_ = 0; toggleDone_ = true;
      host_.fsCombo(kModeToggle);
      return;
    }
    // combo: an adjacent switch went down at most kComboMs ago
    for (uint8_t o = 0; o < 4; o++) {
      if (o == fs || !s_[o].down || !adjacent(o, fs)) continue;
      if (now - s_[o].t_down > kComboMs) continue;
      s.consumed = s_[o].consumed = true;
      s_[o].pending = false;
      comboMask_ = (uint8_t)((1u << o) | (1u << fs));
      comboT_ = now;
      comboStage_ = 0;
      if (comboMask_ == 0x03) host_.fsCombo(kPrevBank);
      else if (comboMask_ == 0x0C) host_.fsCombo(kNextBank);
      return;
    }
    if (host_.fsActOnPress(fs)) {
      s.consumed = true;
      host_.fsGesture(fs, kShort);
    }
    return;
  }
  if (!s.down) return;
  s.down = false;
  // B+C tapped (released before the 1 s tuner stage): Preset <-> Live. Fires once, on the first release of the pair.
  if (comboMask_ == 0x06 && comboStage_ == 0 && !toggleDone_ && (fs == 1 || fs == 2)) {
    toggleDone_ = true;
    host_.fsCombo(kModeToggle);
  }
  if (!s.consumed) {
    if (now - s.t_down >= kLongMs) {
      host_.fsGesture(fs, kLong);
    } else if ((fs == 1 || fs == 2) && host_.fsComboDefer(fs)) {
      s.defer = true;
      s.t_def = now;
    } else if (host_.fsDoubleEnabled(fs)) {
      s.pending = true;
      s.t_pend = now;
    } else {
      host_.fsGesture(fs, kShort);
    }
  }
  s.consumed = false;
  clearComboIfIdle();
}

void Footswitches::tick(uint32_t now) {
  for (uint8_t f = 0; f < 4; f++) {
    S& s = s_[f];
    if (s.defer && now - s.t_def >= kDeferMs) { s.defer = false; host_.fsGesture(f, kShort); }
    // holding fires the long gesture as soon as the threshold passes (feedback while the foot is still down), once
    if (s.down && !s.consumed && !s.pending && now - s.t_down >= kLongMs) { s.consumed = true; host_.fsGesture(f, kLong); }
    if (s.pending && now - s.t_pend > kDoubleMs) {
      s.pending = false;
      host_.fsGesture(f, kShort);
    }
  }
  // B+C held: 1 s tuner, 3 s BLE pairing, 6 s BLE off (one-shot stages)
  if (comboMask_ == 0x06 && s_[1].down && s_[2].down) {
    uint32_t held = now - comboT_;
    if (comboStage_ < 1 && held >= 1000) { comboStage_ = 1; host_.fsCombo(kTunerHold); }
    if (comboStage_ < 2 && held >= 3000) { comboStage_ = 2; host_.fsCombo(kBlePair); }
    if (comboStage_ < 3 && held >= 6000) { comboStage_ = 3; host_.fsCombo(kBleOff); }
  }
}

}  // namespace pd
