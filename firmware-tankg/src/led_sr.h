// Tank-G LED/7-segment shift register, reimplemented from stock M5.48 fn +0x13600. UNTESTED ON HARDWARE.
// PA0 = LATCH, PA1 = /OE(?), PA2 = CLK, PA3 = DATA (inverted: pin low when bit is 1). 29 bits, bit28 first.
// Header-only; GPIO access through a tiny interface so it can be host-tested.
#pragma once
#include <stdint.h>
namespace tg {
struct Pa { virtual uint32_t out() = 0; virtual void setOut(uint32_t) = 0; virtual void noPull(uint32_t mask) = 0; virtual void dirOut(uint32_t mask) = 0; virtual void delay() = 0; };
constexpr uint8_t kLedBit[14] = {3, 2, 6, 1, 5, 4, 7, 8, 12, 10, 14, 9, 13, 11};  // LED index -> word bit [stock table @0x0203242E]
constexpr uint8_t kSeg[9] = {0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // digits 1..9 [stock @0x0203202C]

// BEGIN GENERATED LED MAP (tools/led_sweep.py, 2026-11-15) -- observed by a human on the pedal, -1 = not observed
constexpr const char* kBitLabel[29] = {"MASTER (bit-base 0)", "IR CAB", "RVB MIX", "RVB DECAY", "DLY TIME", "DLY MIX", "MOD SPEED", "MOD FX", "VOLUME", "BASS", "MIDDLE", "TREBLE", "GAIN", "TYPE", "NOISE GATE", "(vazio)", "SEG A", "SEG B", "SEG C", "SEG D", "SEG E", "SEG F", "SEG G", "DP", "(vazio)", "LED VERDE BT", "(vazio)", "(vazio)", "MASTER VERMELHO"};
constexpr int8_t kFsColorBit[4][3] = {{-1,-1,-1}, {-1,-1,-1}, {-1,-1,-1}, {-1,-1,-1}};  // [footswitch 0..3][R,G,B] -> word bit
constexpr int8_t kSegBit[7] = {16,17,18,19,20,21,22};  // segments a..g -> word bit
constexpr int8_t kDpBit = 23;
// END GENERATED LED MAP
// Hand-confirmed on the real pedal (sweep 2026-11-15): bits 1..14 = knob LEDs (order = stock kLedBit users), 16..22 = seg A..G, 23 = DP.
constexpr int kBitMaster = 0;     // base bit 0 (stock always sends it): MASTER LED
constexpr int kBitBtGreen = 25;   // green BT LED above the display
constexpr int kBitMasterRed = 28; // red channel of MASTER (with bit 0 -> pink)
// The 4 RGB footswitch LEDs are NOT on this shift register (no bit lit them).
inline uint32_t fsColor(int fs, int rgb) { return (fs >= 0 && fs < 4 && rgb >= 0 && rgb < 3 && kFsColorBit[fs][rgb] >= 0) ? 1u << kFsColorBit[fs][rgb] : 0u; }
class LedSr {
 public:
  explicit LedSr(Pa& pa) : pa_(pa) {}
  void send(uint32_t w) {
    // stock order: PU/PD cleared, OUT |= 0xF (levels high first), THEN DIR -> output, then LATCH low
    if (!init_) { pa_.noPull(0xF); pa_.setOut(pa_.out() | 0xF); pa_.dirOut(0xF); init_ = true; }
    else if (have_ && w == last_) return;
    pa_.setOut(pa_.out() & ~1u);  // stock @0x13626 runs on EVERY call: LATCH low before shifting, so the final PA0 high is a fresh rising edge
    for (int i = 28; i >= 0; i--) {
      pa_.setOut(pa_.out() & ~4u); pa_.delay();
      uint32_t o = pa_.out() & ~8u; if (!((w >> i) & 1)) o |= 8u;  // inverted data
      pa_.setOut(o); pa_.delay();
      pa_.setOut(pa_.out() | 4u); pa_.delay(); pa_.delay();
    }
    pa_.setOut(pa_.out() | 1u); pa_.setOut(pa_.out() & ~2u);
    last_ = w; have_ = true;
  }
  static uint32_t base() { return 0x08000001u; }
  static uint32_t led(int i) { return 1u << kLedBit[i]; }
  static uint32_t digit(int d, bool dp = false) { return ((d >= 1 && d <= 9 ? kSeg[d - 1] : 0x3Fu) | (dp ? 0x80u : 0u)) << 16; }
 private:
  Pa& pa_; bool init_ = false, have_ = false; uint32_t last_ = 0;
};
}  // namespace tg
