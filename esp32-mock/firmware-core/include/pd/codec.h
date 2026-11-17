// Binary on-flash formats (explicit little-endian, versioned, CRC32-protected).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "pd/model.h"

namespace pd {

enum class CodecResult { Ok, Truncated, BadMagic, BadVersion, BadCrc, BadField };
const char* codecResultName(CodecResult r);

constexpr size_t kPresetMaxBytes = 4096;
constexpr size_t kSettingsMaxBytes = 2048;

// Encoders write into a caller buffer and return the byte count (0 = does not fit).
size_t encodePreset(const Preset& p, uint8_t* out, size_t cap);
// Decodes into p in place (no large temporaries: the AC791N stack is small).
// On error the contents of p are undefined; callers decode into scratch storage.
CodecResult decodePreset(const uint8_t* d, size_t n, Preset& p);
void infoFromPreset(const Preset& p, PresetInfo& i);

size_t encodeSettings(const Settings& s, uint8_t* out, size_t cap);
CodecResult decodeSettings(const uint8_t* d, size_t n, Settings& s);

}  // namespace pd
