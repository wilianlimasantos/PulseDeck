# PulseDeck + open Tank-G firmware

An open-source **desktop editor** (PulseDeck) and an open-source **replacement firmware** (`firmware-tankg`) for the
M-VAVE Tank-G v1 guitar multi-effects pedal (JieLi AC791N).

> Independent project. Not affiliated with or endorsed by M-VAVE, JieLi or any other vendor. Flashing custom firmware
> replaces the factory firmware: make a full read-back backup of **your** pedal first and keep it private. See `NOTICE.md`.

## What it does
- **Firmware** — presets with a power-cut-safe flash store, Preset / Live modes (B+C), soft-takeover knobs, RGB rings and
  7-segment display, integer DSP (gate, drives, 3-band amp stage, 8 built-in cabinets, uploaded IRs, GEQ, chorus, flanger,
  phaser, tremolo, vibrato, rotary, envelope filter, compressor, octaver, delay, reverb), an on-chip player for NAM "A2" WaveNet
  captures, and a second-core audio pipeline.
- **PulseDeck** — Tauri 2 + Rust + React editor: live chain editing, presets, uploads of `.nam` / `.a2l` captures and `.wav`
  IRs. Heavy NAM captures are automatically **compacted** (distilled into a smaller network) to the limit the pedal measures
  on its own CPU.

## Repository layout
| Path | |
|---|---|
| `firmware-tankg/` | the pedal firmware (C/C++, pi32v2) and its tools |
| `esp32-mock/firmware-core/` | the portable engine shared by the firmware and the ESP32 test bench (C++17, no STL) |
| `crates/pulsedeck-core/` | protocol, serial session, NAM conversion and compaction, uploads, simulator |
| `src-tauri/`, `src/` | the desktop app |
| `protocol.json` | machine-readable protocol description |
| `docs/` | hardware map, flash layout, protocol, architecture, build guide, effect guide |

## Quick start
```sh
# editor
npm install
npm run tauri dev                    # pedal connected
PULSEDECK_SIM=1 npm run tauri dev    # simulated pedal

# firmware (see docs/building.md for the toolchain)
cd firmware-tankg && . tools/env.sh && tools/build.sh

# tests, no hardware needed
make -C esp32-mock/firmware-core test
firmware-tankg/tools/host_pedal.sh && cargo test --workspace && npm test
```
Packing and flashing (`tools/pack.py`, `tools/flash_tankg.sh`) and recovery: **[docs/building.md](docs/building.md)**.

## Architecture in one paragraph
The ALNK0 interrupt processes 128-frame blocks at 48 kHz: input -> P effects -> amp (open amp stage or capture network) ->
Q effects -> MASTER. With the second core running, the effects move to CPU1 and CPU0 runs only the capture network
(three-stage pipeline, +5.3 ms). The main loop owns the protocol, the panel and the flash; it hands the DSP immutable parameter
snapshots. Details and measured costs: [docs/architecture.md](docs/architecture.md).

## Status
Tested on one unit (the author's). Measured: audio path, panel, footswitches, knobs, RGB, flash store, uploads, second core,
real-time limits. Captures run at ~170 multiply-accumulates per sample on this chip, so stock NAM captures are compacted and
sound approximate. Not done: codec identification, Bluetooth, SDRAM.

## Contributing / Writing custom effects
Contributions are welcome — new effects, better DSP, measurements on other units, the Tank-B, translations.

1. Read [docs/effects.md](docs/effects.md): catalogue entry -> integer DSP in `dsp_fx.h` -> wiring in `audio.h` -> host test.
2. Keep it real-time: integer only, no heap, measure with `AUDIO STAT` (`missed=0`).
3. **Cite your sources** (paper, textbook, URL, or "measured on hardware") in a header comment.
4. Do not submit code, text or data from proprietary firmware or from projects whose licence is incompatible with MIT
   (details in `NOTICE.md`). Never commit firmware images, flash dumps or backups.
5. Run all host tests before opening a pull request.

## License
MIT (`LICENSE`), © 2026 Wilian de Lima Santos. Third-party components (felucca-bsp under MPL-2.0, toolchain, dependencies)
keep their own licences: see `NOTICE.md`.
