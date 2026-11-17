// Portable pedal engine: protocol parser, presets/scenes, CPU/RAM budget,
// footswitch actions, LED frame and persistence (through IBlobStore).
#pragma once
#include <stdint.h>

#include "pd/codec.h"
#include "pd/footswitch.h"
#include "pd/model.h"
#include "pd/store.h"
#include "pd/tokens.h"

namespace pd {

constexpr uint32_t kCpuBudget = 1000;
constexpr uint32_t kAmpCost = 380;
constexpr uint32_t kFxMemBudget = 49152;
constexpr uint32_t kAmpMem = 16384;
constexpr size_t kMaxUpload = 262144;
constexpr size_t kMaxLine = 199;  // PROTOCOL_SPEC: absolute line limit (without the newline)

enum class Mode : uint8_t { Preset = 0, Live = 1 };
enum class KnobState : uint8_t { Inert = 0, Disengaged = 1, Engaged = 2 };
constexpr int kKnobs = 15;  // Tank-G panel order: MASTER, IR CAB, RVB MIX, RVB DECAY, DLY TIME, DLY MIX, MOD SPEED, MOD FX, VOLUME, BASS, MIDDLE, TREBLE, GAIN, TYPE, NOISE GATE

class Engine : private IFsHost {
 public:
  Engine(IBlobStore& presets, IBlobStore& settings);

  // Loads settings and the preset index from flash. First boot (no settings
  // record) seeds two demo presets and writes them.
  void begin();
  void handleLine(const char* line, ILineSink& out);

  // Footswitch inputs (HAL) and periodic service. Times in milliseconds.
  void fsEvent(uint8_t fs, bool down, uint32_t now_ms) { now_ = now_ms; fsm_.event(fs, down, now_ms); }
  void tick(uint32_t now_ms);
  // RGB 0xRRGGBB for footswitch f at time now_ms (LED frame).
  uint32_t ledColor(uint8_t f, uint32_t now_ms) const;
  // Unsolicited lines (EVT ...). Optional.
  void setAsync(ILineSink* s) { async_ = s; }
  // Platform media player/store for uploaded captures and IRs (optional). Call before begin().
  void setMedia(IMedia* m) { media_ = m; }
  bool liveIr() const { return live_ir_; }   // an uploaded IR is sounding without (or regardless of) a slot
  // Built-in cabinets shown by ILIST in slots that hold no uploaded IR (the open Tank-G firmware ships 8 voicings instead of IR files).
  void setBuiltinIrNames(const char* const* names, int n) { bi_names_ = names; bi_n_ = n < kIrSlots ? n : kIrSlots; }

  // Introspection (tests, FSST).
  int currentPreset() const { return cur_slot_; }
  int currentScene() const { return cur_scene_; }
  const Preset& chain() const { return cur_; }
  const PresetInfo& info(int slot) const { return idx_[slot]; }
  const Settings& settings() const { return set_; }
  uint32_t cpuUsed() const;
  uint32_t memUsed() const;
  bool bypassed() const { return bypass_; }
  bool pendingBank() const { return bank_pending_; }
  bool tunerOn() const { return tuner_on_; }

  // --- Preset / Live mode and physical knobs (src/live.cpp)
  // Preset mode (boot): footswitches load presets. Live mode: A..D toggle reverb, delay, modulation, amp; knobs edit the first ON block of
  // their kind with soft takeover. B+C tapped toggles the mode. MASTER and NOISE GATE work in both modes.
  Mode mode() const { return mode_; }
  void setMode(Mode m);
  // raw 0..1023 of knob `slot`; call it regularly (the stable value, not every ADC wobble). Returns the knob's state.
  KnobState knob(uint8_t slot, uint16_t raw);
  KnobState knobState(uint8_t slot) const { return slot < kKnobs ? kn_[slot].state : KnobState::Inert; }
  // Panel LED of a knob: Live = steady (engaged) / blinking (waiting to cross the saved value) / dark (no such block);
  // Preset mode = lit when the effect it controls is on (MASTER always lit; everything dark but MASTER while bypassed).
  bool knobLed(uint8_t slot, uint32_t now_ms) const;
  // Result of the last preset save (hold gesture or PSAVE): the panel shows it on the display.
  uint32_t saveSeq() const { return save_seq_; }
  bool saveOk() const { return save_ok_; }
  uint16_t masterRaw() const { return master_; }  // output volume knob, 0..1023 (applied directly, not saved)

 private:
  // --- IFsHost
  void fsGesture(uint8_t fs, uint8_t g) override;
  void fsCombo(uint8_t kind) override;
  bool fsDoubleEnabled(uint8_t fs) const override;
  bool fsActOnPress(uint8_t fs) const override { return fs < 4 && set_.layout != Layout::Factory && set_.fspress[fs]; }  // Factory: act on release (hold = save, B+C = mode)
  bool fsComboDefer(uint8_t fs) const override { return fs < 4 && set_.layout == Layout::Factory; }

  // chain/preset/scene operations (return false on failure)
  void seedDefaults();
  void newChain();
  bool applyPreset(const Preset& p, int slot, int* missing);
  bool savePreset(int slot, const char* name);
  bool loadStored(int slot, Preset& p);
  bool writeStored(int slot, const Preset& p);
  void captureScene(Scene& s) const;
  int applyScene(const Scene& s);
  bool persistScene(int n);  // read-modify-write of the stored record
  bool saveSettings();
  void refreshIndex(int slot);
  void emit(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  int nextUsed(int from, int dir) const;
  void doBypass(bool on);
  void sceneGoto(int n);
  void presetGoto(int slot);
  void toggleBlockByType(uint8_t type, uint8_t nth);
  void execAction(const FsAction& a);
  void factoryGesture(uint8_t fs, uint8_t g);
  void tap(uint32_t now);
  // live.cpp
  struct Tgt { int kind = 0, blk = -1, k = -1, lo = 0, hi = 0; };  // kind: 0 none, 1 block param, 2 ampeq field, 3 IR position
  bool resolveKnob(uint8_t slot, Tgt& t) const;
  int tgtGet(const Tgt& t) const;
  void tgtSet(const Tgt& t, int v);
  int tgtRaw(const Tgt& t) const;
  void gateKnob(uint16_t raw);
  void liveToggle(uint8_t fs);
  int firstBlockOf(const char* cat, bool onlyOn) const;
  int levelOf(int cat) const;  // 0 missing, 25 off, 100 on  (cat 0 reverb,1 delay,2 mod,3 amp)
  void rearmKnobs();
  bool cmdChain(const Tokens& t, ILineSink& o);
  bool cmdPreset(const Tokens& t, ILineSink& o);
  bool cmdLib(const Tokens& t, ILineSink& o);
  bool cmdGlobal(const Tokens& t, ILineSink& o);
  void cmdFx(ILineSink& o);

  IBlobStore& presets_;
  IBlobStore& settings_;
  Settings set_;
  Preset cur_;
  uint8_t io_[kPresetMaxBytes];  // flash record buffer (shared, never used re-entrantly)
  Preset tmp_;  // scratch for flash read-modify-write (keeps big structs off the stack)
  PresetInfo idx_[kPresets];
  int cur_slot_ = -1, cur_scene_ = -1;
  uint32_t next_id_ = 1, generation_ = 1;
  int audio_mode_ = 2;
  bool tuner_on_ = false, tuner_mute_ = false;
  uint32_t tuner_ref_ = 440, tune_tick_ = 0;
  int bpm10_ = 1200;
  uint32_t last_tap_ = 0;
  bool bypass_ = false, bank_pending_ = false;
  Scene bypass_snap_;
  bool ble_on_ = true, ble_pair_ = false;
  uint32_t now_ = 0;

  // upload staging (streaming CRC; the payload itself is not buffered here)
  bool staging_ = false;
  char stage_kind_ = 'C';
  size_t stage_expected_ = 0, stage_received_ = 0, last_blob_ = 0, last_ir_ = 0;
  uint32_t stage_crc_ = 0;
  long stage_gain_ = 0;

  uint32_t save_seq_ = 0; bool save_ok_ = false;
  const char* const* bi_names_ = nullptr; int bi_n_ = 0;
  IMedia* media_ = nullptr; bool live_ir_ = false, live_cap_ = false; int cap_layers_ = 10; char cap_name_[kNameLen] = {};
  void selectCapture();
  Mode mode_ = Mode::Preset;
  struct Kn { uint16_t raw = 0, applied = 0; int8_t side = 0; bool armed = false, got = false; KnobState state = KnobState::Inert; } kn_[kKnobs];
  uint16_t master_ = 890;  // ~0 dB, see tg audio
  bool gateInit_ = false, gateOwned_ = false, gateOn_ = false;
  uint16_t gateRaw0_ = 0;
  Footswitches fsm_;
  ILineSink* async_ = nullptr;
  friend struct EngineTestAccess;
};

}  // namespace pd
