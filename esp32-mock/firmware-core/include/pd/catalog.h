// Effect catalog (reported by FXT). Cost/memory feed the CPU/RAM budget.
#pragma once
#include <stdint.h>

namespace pd {

struct ParamDef {
  const char* name;
  int16_t min, max, def;
};
struct FxDef {
  uint8_t id;
  const char* name;
  const char* cat;
  uint16_t cost;  // CPU ticks
  uint32_t mem;   // bytes of effect arena
  uint8_t np;
  ParamDef p[7];  // kMaxParams
};

int fxCount();
const FxDef* fxAt(int i);
const FxDef* fxById(uint8_t id);
const FxDef* fxByName(const char* name);

}  // namespace pd
