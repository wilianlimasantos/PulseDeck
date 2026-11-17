#include "buttons.h"

void Buttons::begin() {
  for (uint8_t f = 0; f < 4; f++) pinMode(pins_[f], INPUT_PULLUP);
}

void Buttons::poll(pd::Engine& e, uint32_t now) {
  for (uint8_t f = 0; f < 4; f++) {
    bool down = digitalRead(pins_[f]) == LOW;
    if (down != raw_[f]) { raw_[f] = down; changed_[f] = now; }
    if (raw_[f] != state_[f] && now - changed_[f] >= kDebounceMs) {
      state_[f] = raw_[f];
      e.fsEvent(f, state_[f], now);
    }
  }
}
