# Flash layout and storage

1 MiB SPI NOR, absolute offsets. Directory read from the JLFS table of a read-back of the author's unit [code]:

| Range | Content | Written by this firmware? |
|---|---|---|
| 0x00000..0x04000 | boot loader (SPL) | **never** |
| 0x04000..0x7B000 | application window (JLFS container: header, `app.bin`, `cfg_tool.bin`) | by `tools/flash_tankg.sh write` only |
| 0x75000..0x7B000 | **inside** the window, past every CRC'd range, erased (0xFF) in the stock image: the firmware's store | yes, at run time (saves only) |
| 0x7B000..0xBD000 | VM (vendor runtime data) | never |
| 0xBD000 | BTIF (Bluetooth identity, unique per unit) | never |
| 0xBE000 | OTP (unique per unit) | never |
| 0xBF000..0xFF000 | USR (vendor presets, captures, IRs) | never |

`app_area_head` CRCs cover only `[0x4000, 0x7364B)`; PRCT/VM/BTIF/OTP/USR carry no data CRC. That the boot loader checks
nothing beyond `app_area_head` is **[inferred]**, consistent with every boot so far.

Re-flashing the application rewrites the whole window and therefore erases the store (factory presets are re-seeded).

## The store (`firmware-tankg/src/log_store.h`)
Two banks of 12 KiB (3 sectors each). Bank header `PDB2` + sequence + CRC32; records `PDR2 {len, klen, flags, CRC32}` + key +
data, appended; the last valid record of a key wins; a full bank is compacted into the other one and committed by writing its
header last. A power cut at any programmed byte leaves the old or the new value (host test with a cut at every byte).

| Key | Content |
|---|---|
| `p00`..`p31` | presets (~250..300 B) |
| `gset` | global settings (~1 KB) |
| `fver` | factory preset revision |
| `c00`..`c14` | uploaded capture blobs (A2L1, 0.5..4 KB) |
| `i0`..`i7` | uploaded impulse responses (<= 512 taps at 24 kHz, ~1 KB) |

12 KiB is shared by everything: a save that does not fit answers `ERR ... flash write failed` and the item keeps playing until
power-off. All stored media are mirrored in RAM at boot, so the audio never reads the flash.
