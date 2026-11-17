#include "pd/crc.h"

namespace pd {

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

uint16_t crc16_xmodem(const uint8_t* data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)(data[i] << 8);
    for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

}  // namespace pd
