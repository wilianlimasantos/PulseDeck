// Tank-G RGB LED stream encoder, reimplemented from stock M5.48 fn +0x12ED8. UNTESTED ON HARDWARE.
// 12 pixels (4 footswitches x 3, order unknown), RGB in -> SPI-DMA byte stream out: 36 zero bytes (reset gap), then per pixel 24 bytes in G,R,B order,
// MSB first, one SPI byte per WS2812 bit: 0x7E for a 1, 0x60 for a 0 (values and loop structure read from the stock code). Total 324 = 0x144 bytes.
#pragma once
#include <stdint.h>
namespace tg {
constexpr int kWsPixels = 12, kWsReset = 36, kWsBytes = kWsReset + kWsPixels * 24;  // 324
// one/zero = SPI byte used for a WS bit 1 / 0. Stock: 0x7E / 0x60. Adjustable because the real SPI clock of this firmware may differ from the stock one.
inline void wsEncode(const uint8_t* rgb /* 36 bytes R,G,B per pixel */, uint8_t* out /* kWsBytes */, uint8_t one = 0x7E, uint8_t zero = 0x60) {
  for (int i = 0; i < kWsReset; i++) out[i] = 0;
  uint8_t* o = out + kWsReset;
  for (int p = 0; p < kWsPixels; p++) {
    const uint8_t c[3] = {rgb[p * 3 + 1], rgb[p * 3 + 0], rgb[p * 3 + 2]};  // G, R, B
    for (int k = 0; k < 3; k++) for (int b = 7; b >= 0; b--) *o++ = ((c[k] >> b) & 1) ? one : zero;
  }
}
}  // namespace tg
