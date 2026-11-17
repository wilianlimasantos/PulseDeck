// dB <-> linear helpers shared by the DSP and the media loader (integer only, table based).
#pragma once
#include <stdint.h>
namespace tg {
// dB -30..12 -> Q12 linear (4096 = unity)
inline int32_t dbQ12(int db) {
  static const int32_t t[55] = {130,145,163,183,205,230,258,290,325,365,410,460,516,579,649,728,817,917,1029,1154,1295,1453,1631,1830,2053,2303,2584,2900,3254,3651,4096,4596,5157,5786,6492,7284,8173,9170,10289,11544,12953,14533,16306,18296,20529,23034,25844,28997,32536,36506,40960,45958,51566,57858,64917};
  return t[db < -30 ? 0 : db > 24 ? 54 : db + 30];
}
// dB (any, -90..+40) -> linear Q8, built from the Q12 table (the table stops at +24 dB, so larger gains are chained)
inline int32_t gainQ8(int db) {
  if (db <= 24) return dbQ12(db) >> 4 ? dbQ12(db) >> 4 : 1;
  return (dbQ12(24) >> 4) * dbQ12(db - 24 > 24 ? 24 : db - 24) >> 12;
}
// level in dB (-90..0) -> 24 bit amplitude (full scale = 0 dB)
inline int32_t dbToAmp(int db) {
  int d = db > 0 ? 0 : db, steps = 0;
  while (d < -30) { d += 30; steps++; }
  int32_t a = 2047 * dbQ12(d);
  while (steps-- > 0) a = a * 130 / 4096;
  return a < 1 ? 1 : a;
}
// MASTER knob (0..1023): squared (audio-taper-like) linear gain, Q12; 1023 = unity, 0 = silence. Not stored in presets.
inline int32_t masterQ12(uint16_t raw) { uint32_t r = raw > 1023 ? 1023 : raw; return (int32_t)(r * r * 4096u / (1023u * 1023u)); }
// milli-dB (-30000..24000) -> Q12 linear, interpolated between the integer-dB entries
inline int32_t mdbQ12(long mdb) {
  if (mdb <= -30000) return dbQ12(-30);
  if (mdb >= 24000) return dbQ12(24);
  long d = mdb >= 0 ? mdb / 1000 : -((-mdb + 999) / 1000), f = mdb - d * 1000;   // floor
  int32_t a = dbQ12((int)d), b = dbQ12((int)d + 1); return a + (int32_t)((int64_t)(b - a) * f / 1000);
}
}  // namespace tg
