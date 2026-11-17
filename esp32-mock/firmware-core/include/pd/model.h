// Data model: chain of blocks, scenes, presets, global settings.
// Fixed-size POD structs (no heap) so the same code fits the AC791N later.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pd {

constexpr int kPresets = 32;
constexpr int kScenes = 4;
constexpr int kMaxBlocks = 16;
constexpr int kMaxBankBlocks = 10;
constexpr int kMaxParams = 7;
constexpr int kNameLen = 24;       // 23 chars + NUL
constexpr int kSceneNameLen = 16;  // 15 chars + NUL
constexpr int kCaptureSlots = 15;
constexpr int kIrSlots = 8;

struct Block {
  uint32_t id = 0;   // runtime instance id (not persisted)
  uint8_t type = 0;  // catalog id
  char bank = 'P';   // 'P' before the amp, 'Q' after
  uint8_t lane = 0;
  bool on = true;
  uint8_t np = 0;
  int16_t p[kMaxParams] = {};
};

struct SceneBlock {
  bool on = false;
  int16_t p[kMaxParams] = {};
};

struct Scene {
  bool used = false;
  char name[kSceneNameLen] = {};
  int8_t gain_in = 0, gain_out = 0;
  bool amp_on = true;
  int16_t ampeq[6] = {};
  uint8_t nb = 0;
  SceneBlock b[kMaxBlocks];
};

struct Preset {
  char name[kNameLen] = {};
  char color[8] = {};  // "rrggbb" or empty
  bool amp_present = true, amp_on = true;
  int16_t ampeq[6] = {};
  int8_t gain_in = 0, gain_out = 0;
  int8_t capture = 0;
  uint8_t nb = 0;
  Block b[kMaxBlocks];
  Scene sc[kScenes];
};

// Cheap in-RAM index of what is on flash (PLIST without reading 32 files).
struct PresetInfo {
  bool used = false;
  char name[kNameLen] = {};
  char color[8] = {};
  uint8_t nb = 0;
  uint8_t scene_mask = 0;
  bool amp_present = true;
};

struct FsAction {
  uint8_t type = 0, a = 0, b = 255;
};
enum class Layout : uint8_t { Factory = 0, Scenes = 1, Custom = 2 };

struct FsLed {
  bool custom = false;
  char on[8] = "000000", off[8] = "000000";
};

struct LibSlot {
  bool used = false;
  char name[kNameLen] = {};
  uint32_t size = 0;
};

struct Settings {
  int16_t hpf = 80;
  int16_t geq[6] = {};
  Layout layout = Layout::Factory;
  FsAction fsa[4][3];
  bool fspress[4] = {false, false, false, false};
  // LEDCFG keys, alphabetical (same order as the reply).
  char led[7][12] = {"1", "100", "1", "0", "edit", "1", "needle"};
  FsLed fsled[4];
  LibSlot caps[kCaptureSlots];
  LibSlot irs[kIrSlots];
  int8_t last_preset = 0;
  uint8_t bank = 0;
};

void copyName(char* dst, size_t cap, const char* src);  // ASCII-sanitize + truncate
void defaultFsa(Settings& s, Layout l);

}  // namespace pd
