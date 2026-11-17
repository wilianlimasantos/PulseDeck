// Footswitch gesture state machine: raw press/release + time -> gestures.
// Platform independent: the HAL feeds event(); the host (Engine) executes actions.
#pragma once
#include <stdint.h>

namespace pd {

enum Gesture : uint8_t { kShort = 0, kLong = 1, kDouble = 2 };
enum ComboKind : uint8_t { kPrevBank = 0, kNextBank = 1, kTunerHold = 2, kBlePair = 3, kBleOff = 4, kModeToggle = 5 };

struct IFsHost {
  virtual ~IFsHost() {}
  virtual void fsGesture(uint8_t fs, uint8_t gesture) = 0;
  virtual void fsCombo(uint8_t kind) = 0;
  virtual bool fsDoubleEnabled(uint8_t fs) const = 0;
  virtual bool fsActOnPress(uint8_t fs) const = 0;
  virtual bool fsComboDefer(uint8_t fs) const { (void)fs; return false; }  // delay this switch's short press so a late partner can still make a combo
};

class Footswitches {
 public:
  static constexpr uint32_t kLongMs = 1000;
  static constexpr uint32_t kDoubleMs = 300;
  static constexpr uint32_t kComboMs = 250, kDeferMs = 180;  // humans rarely press two switches within 80 ms

  explicit Footswitches(IFsHost& host) : host_(host) {}
  void event(uint8_t fs, bool down, uint32_t now);
  void tick(uint32_t now);
  bool isDown(uint8_t fs) const { return fs < 4 && s_[fs].down; }

 private:
  struct S {
    bool down = false, consumed = false, pending = false, defer = false;
    uint32_t t_down = 0, t_pend = 0, t_def = 0;
  };
  void clearComboIfIdle();
  IFsHost& host_;
  S s_[4];
  uint8_t comboMask_ = 0;
  uint32_t comboT_ = 0;
  uint8_t comboStage_ = 0;
  bool toggleDone_ = false;
};

}  // namespace pd
