// Tank-G front panel glue: footswitches, knobs, panel LEDs/display and RGB rings <-> pd::Engine. UNTESTED ON HARDWARE.
// Hardware access goes through PanelHw so everything here is host-testable. No allocation, no STL.
//  - footswitches (debounced 3 x 5 ms) -> Engine::fsEvent; Engine::tick every 5 ms
//  - knobs (15, via src/knob_map.h): dead-band filtered; VOLUME/BASS/MIDDLE/TREBLE/GAIN write Engine's AMPEQ [INFERRED mapping, see below];
//    every knob can also be streamed as "KNOBEV <idx> <raw>" lines (PANEL MON 1)
//  - panel: base word (bits 0 and 27 as stock), LED of the knob last moved (bit = knob index), 7-segment = preset number (dp = bypass)
//  - RGB rings: Engine::ledColor(fs) scaled by a brightness cap -> 3 pixels per footswitch (src/rgb_map.h)
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pd/engine.h"
#include "led_sr.h"
#include "ws_enc.h"
#include "rgb_map.h"
#include "knob_map.h"
namespace tg {
struct PanelHw {
  virtual uint32_t millis() = 0;
  virtual uint32_t fsRead() = 0;                     // pressed bits, bit0..3 = FS1..FS4
  virtual void knobsScan(uint16_t v[16]) = 0;        // raw 10 bit; 0xFFFF = no reading
  virtual void ledSend(uint32_t word) = 0;           // 29-bit panel word
  virtual int rgbSend(const uint8_t rgb[36]) = 0;    // 12 pixels RGB; 0 = started, nonzero = busy (retry later)
};
struct NullSink : pd::ILineSink { void line(const char*) override {} };

class Panel {
 public:
  static constexpr uint32_t kFsPollMs = 5, kFsStable = 3, kKnobPollMs = 25, kRenderMs = 20, kKnobLedMs = 800;
  static constexpr int kDeadband = 8;  // raw counts of 1023 [assumed; the ADC noise on the real pedal was not measured]
  Panel(pd::Engine& e, PanelHw& hw) : eng_(e), hw_(hw) {}

  void poll() {
    uint32_t now = hw_.millis();
    if (!started_ || now - lastFs_ >= kFsPollMs) { lastFs_ = now; scanFs(now); eng_.tick(now); }
    if (!started_ || now - lastKnob_ >= kKnobPollMs) { lastKnob_ = now; scanKnobs(now); }
    if (!hold_ && (!started_ || now - lastRender_ >= kRenderMs)) { lastRender_ = now; render(now); }
    started_ = true;
  }
  // Diagnostic commands (LEDBIT/LEDRAW/RGB...) take over the LEDs: hold(true). hold(false) forces a full redraw.
  void hold(bool h) { if (hold_ && !h) { rgbValid_ = false; } hold_ = h; }
  bool held() const { return hold_; }
  void setCap(int pct) { capPct_ = pct < 1 ? 1 : pct > 100 ? 100 : pct; rgbValid_ = false; }
  int cap() const { return capPct_; }
  void setMonitor(pd::ILineSink* s) { mon_ = s; }
  bool monitoring() const { return mon_ != nullptr; }
  // Brief display feedback after a save (called by whoever saves): 5 = ok, E = error, for ~1.5 s
  void saveFeedback(bool ok, uint32_t now) { saveOk_ = ok; saveMsgUntil_ = now + 1500; if (!saveMsgUntil_) saveMsgUntil_ = 1; }
  uint16_t knobRaw(int k) const { return (k >= 0 && k < 15) ? rep_[k] : 0; }
  uint16_t batteryRaw() const { return batRaw_; }
  uint32_t batteryMv() const { return batRaw_ == 0xFFFF ? 0 : (uint32_t)batRaw_ * 6600u / 1023u; }  // scale read from the stock code (x6600/1023); meaning [inferred]
  uint32_t fsStable() const { return fsStable_; }
  // knob index -> AMPEQ field (0 volume,1 bass,2 mid,3 treble,4 presence,5 gain/drive); -1 = not wired to the engine  [INFERRED from the knob names]
  static int ampField(int k) { return k == 8 ? 0 : k == 9 ? 1 : k == 10 ? 2 : k == 11 ? 3 : k == 12 ? 5 : -1; }
  static int ampValue(int field, uint16_t raw) {  // dB x 10; volume -240..120, others -120..120 (PROTOCOL_SPEC, unit "not verified")
    int lo = field == 0 ? -240 : -120, hi = 120; return lo + (int)((uint32_t)raw * (uint32_t)(hi - lo) / 1023u);
  }

 private:
  void scanFs(uint32_t now) {
    uint32_t raw = hw_.fsRead() & 0xFu;
    if (raw == fsStable_) { fsCand_ = raw; fsCnt_ = 0; return; }
    if (raw != fsCand_) { fsCand_ = raw; fsCnt_ = 1; return; }
    if (++fsCnt_ < kFsStable) return;
    uint32_t ch = raw ^ fsStable_; fsStable_ = raw; fsCnt_ = 0;
    for (uint8_t i = 0; i < 4; i++) if (ch >> i & 1) eng_.fsEvent(i, (raw >> i & 1) != 0, now);
  }
  void scanKnobs(uint32_t now) {
    uint16_t v[16]; hw_.knobsScan(v); batRaw_ = v[15];
    for (int k = 0; k < 15; k++) {
      int s = kKnobSlot[k]; if (s < 0) continue;
      uint16_t r = v[s]; if (r == 0xFFFF) continue;
      if (!knobInit_) rep_[k] = r;
      else {
        int d = (int)r - (int)rep_[k]; if (d < 0) d = -d;
        if (d >= kDeadband) { rep_[k] = r; lastKnob_k_ = k; lastKnobMs_ = now;
          if (mon_) { char b[32]; snprintf(b, sizeof b, "KNOBEV %d %d", k, (int)r); mon_->line(b); } }
      }
      // The engine gets the held (dead-band filtered) value on EVERY scan: its state follows the absolute position, never the motion.
      eng_.knob((uint8_t)k, rep_[k]);
    }
    knobInit_ = true;
  }
  void render(uint32_t now) {
    uint32_t w = LedSr::base();
    if (eng_.saveSeq() != saveSeen_) { saveSeen_ = eng_.saveSeq(); saveFeedback(eng_.saveOk(), now); }
    for (int k = 0; k < 15; k++) if (eng_.knobLed((uint8_t)k, now)) w |= 1u << k;   // engine state: steady = active/engaged, blinking = waiting for takeover
    if (eng_.mode() == pd::Mode::Live && lastKnob_k_ >= 0 && now - lastKnobMs_ < kKnobLedMs && eng_.knobState((uint8_t)lastKnob_k_) != pd::KnobState::Inert) w |= 1u << lastKnob_k_;   // "last moved" highlight only in Live; Preset mode shows the fixed block state
    int p = eng_.currentPreset(); bool bp = eng_.bypassed();
    if (saveMsgUntil_ && (int32_t)(now - saveMsgUntil_) < 0) w |= saveOk_ ? (LedSr::digit(5, true)) : (0x79u << 16);   // SAVE feedback: 5 = saved, E = error
    else {
      saveMsgUntil_ = 0;
      bool live = eng_.mode() == pd::Mode::Live;   // Live: decimal point on; Preset: dp = bypass; '-' = no preset
      w |= (p < 0) ? ((0x40u | ((bp || live) ? 0x80u : 0u)) << 16) : LedSr::digit((p + 1) % 10, live || bp);
    }
    hw_.ledSend(w);
    uint8_t px[36]; memset(px, 0, sizeof px);
    for (int f = 0; f < 4; f++) {
      uint32_t c = eng_.ledColor((uint8_t)f, now);
      uint8_t r = (uint8_t)((c >> 16 & 0xFF) * capPct_ / 100), g = (uint8_t)((c >> 8 & 0xFF) * capPct_ / 100), b = (uint8_t)((c & 0xFF) * capPct_ / 100);
      for (int k = 0; k < 3; k++) { int i = kRgbPixel[f][k]; if (i >= 0 && i < kWsPixels) { px[i * 3] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b; } }
    }
    if (rgbValid_ && !memcmp(px, rgb_, sizeof px)) return;
    if (hw_.rgbSend(px) == 0) { memcpy(rgb_, px, sizeof px); rgbValid_ = true; }   // busy -> keep invalid, try again next render
  }
  pd::Engine& eng_; PanelHw& hw_; NullSink null_; pd::ILineSink* mon_ = nullptr;
  bool started_ = false, hold_ = false, knobInit_ = false, rgbValid_ = false;
  uint32_t lastFs_ = 0, lastKnob_ = 0, lastRender_ = 0, lastKnobMs_ = 0, fsStable_ = 0, fsCand_ = 0, fsCnt_ = 0;
  uint16_t rep_[15] = {}, batRaw_ = 0xFFFF; uint8_t rgb_[36] = {}; int lastKnob_k_ = -1, capPct_ = 25; bool saveOk_ = false; uint32_t saveMsgUntil_ = 0, saveSeen_ = 0;
};
}  // namespace tg
