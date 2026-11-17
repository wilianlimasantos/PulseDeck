// Footswitch inputs. BOOT (GPIO0) is footswitch A; GPIO 32/33/25 (to GND) are B/C/D.
// Active-low with pull-up and 25 ms debounce. Unwired pins read "released".
#pragma once
#include <Arduino.h>

#include "pd/engine.h"

#ifndef PD_FS_PINS
#define PD_FS_PINS {0, 32, 33, 25}
#endif

class Buttons {
 public:
  void begin();
  void poll(pd::Engine& e, uint32_t now_ms);

 private:
  static constexpr uint32_t kDebounceMs = 25;
  uint8_t pins_[4] = PD_FS_PINS;
  bool state_[4] = {false, false, false, false};
  bool raw_[4] = {false, false, false, false};
  uint32_t changed_[4] = {0, 0, 0, 0};
};
