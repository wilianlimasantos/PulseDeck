#include <stdio.h>
#include <string.h>
#include "../src/ws_enc.h"
int main() {
  uint8_t rgb[36] = {0}, o[tg::kWsBytes]; int f = 0;
  auto ck = [&](bool c, const char* m) { printf("%s %s\n", c ? "ok  " : "FAIL", m); f += !c; };
  rgb[0] = 0xFF; rgb[4] = 0x80; rgb[35] = 0x01;           // pixel0 R=FF, pixel1 G=80, pixel11 B=01
  tg::wsEncode(rgb, o);
  ck(tg::kWsBytes == 324 && tg::kWsBytes == 0x144, "length 0x144");
  bool z = true; for (int i = 0; i < 36; i++) z &= o[i] == 0; ck(z, "36 reset bytes are zero");
  bool g = true; for (int i = 0; i < 8; i++) g &= o[36 + i] == 0x60; ck(g, "pixel0 G byte-group = 8x 0x60 (G sent first)");
  bool r = true; for (int i = 0; i < 8; i++) r &= o[36 + 8 + i] == 0x7E; ck(r, "pixel0 R byte-group = 8x 0x7E");
  bool b = true; for (int i = 0; i < 8; i++) b &= o[36 + 16 + i] == 0x60; ck(b, "pixel0 B = 8x 0x60");
  ck(o[36 + 24] == 0x7E && o[36 + 25] == 0x60, "pixel1 G=0x80 MSB first: 7E then 60s");
  ck(o[36 + 11 * 24 + 23] == 0x7E && o[36 + 11 * 24 + 22] == 0x60, "pixel11 B=0x01 LSB last");
  ck(o[323] == 0x7E, "last byte is the B LSB of pixel 11");
  printf("%d failed\n", f); return f != 0;
}
