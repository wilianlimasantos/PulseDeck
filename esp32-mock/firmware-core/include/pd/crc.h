// CRC helpers shared by upload validation and on-flash record integrity.
// Pure C++17, no platform dependencies.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pd {

// CRC-32/ISO-HDLC (zlib). Streaming use: c = kCrc32Init; c = crc32_update(c, ...); final = c ^ 0xFFFFFFFF.
constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);
inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }
inline uint32_t crc32(const uint8_t* d, size_t n) { return crc32_final(crc32_update(kCrc32Init, d, n)); }

// CRC16/XMODEM (the JieLi flash container uses it; kept here for the future port).
uint16_t crc16_xmodem(const uint8_t* data, size_t len, uint16_t crc = 0);

}  // namespace pd
