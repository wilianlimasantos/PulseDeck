# NOTICE

PulseDeck (desktop editor, `firmware-core`, `firmware-tankg`, ESP32 bench) is © 2026 Wilian de Lima Santos and released under the **MIT License** (`LICENSE`).
All of its code was written for this project. The firmware is built on the open **felucca-bsp** chip layer (MPL-2.0) and open tools (LLVM pi32v2, jl-uboot-tool); no code of the factory firmware or of any closed-source or non-commercial project is included.
It is an independent project. It is **not affiliated with, endorsed by or supported by** M-VAVE / Cuvave / Sinco, JieLi, or the author of UltraRig.
"M-VAVE", "Tank-G", "Tank-B", "UltraRig" and the effect/brand names used to describe a sound (Tube Screamer, Klon, RAT, Big Muff, Fuzz Face, Marshall, Fender...) belong to their owners and are used only to identify hardware, compatibility or a sonic character.

Flashing custom firmware replaces the factory firmware and may void the warranty. Do it at your own risk, with a full read-back backup first. Backups contain data unique to one unit (Bluetooth keys, OTP, calibration): **never publish or share them.**

## Implementation policy (clean room)

All code in this repository is written from the sources below and from nothing else.

**Allowed sources**
1. Behavioural specifications: what a control, command or effect does, its parameter names, ranges and defaults (`protocol.json`, `PROTOCOL_SPEC.md`, user-visible documentation read as a description of behaviour, never copied as text).
2. Public algorithms and papers, re-implemented from the description: Freeverb (public domain), Schroeder all-pass and comb filters, feedback delay networks (Stautner/Puckette), chorus/flanger/phaser/tremolo/rotary textbook designs, biquad filters (RBJ cookbook), YIN pitch detection (de Cheveigne and Kawahara), standard waveshaping.
3. Measurements and tests on hardware owned by the contributor (pin maps, register sequences needed to make the chip work), and open vendor SDK documentation/headers whose licence allows it (see the third-party table).
4. Our own earlier work in this repository.

**Not allowed**
- Any code, text or assets from software under the PolyForm Noncommercial licence (including UltraRig's `web/tankg.html`, `web/nam2chip.js`, installer and docs), or from any other source whose licence is incompatible with MIT. Running such software as a black box to compare outputs in a private test is fine; copying it or shipping its output is not.
- DSP or other algorithms obtained by translating a disassembly of a proprietary firmware image. A disassembly may be read to learn hardware facts needed for interoperability (register addresses, pin assignments, message formats), recorded as a fact, not as code.
- Proprietary data: M-VAVE factory captures, impulse responses or presets (for example the factory USR area, `AM4` capture data), full flash dumps and backups, encryption keys of a specific unit.

**Provenance rule for contributors:** a new effect or protocol feature must cite its source (spec section, paper/URL, or "measured on hardware") in the commit or a header comment. If you cannot cite one, do not add it.

**Known provenance of what exists today**
- The text protocol and the effect catalogue (`protocol.json`, `firmware-core/src/catalog.cpp`) were derived by static analysis of a publicly distributed firmware image, for interoperability with hardware the author owns. Parameter names are the protocol's own (mostly Spanish). Catalogue CPU/RAM costs are our estimates, except where marked, until measured.
- `firmware-tankg` hardware facts (panel shift register, footswitch, RGB, knob, audio pin and register settings) come from measurements on the author's pedal and from the felucca-bsp headers; where a value is only inferred the source says so.

## What is not in this repository
- No factory firmware image, flash dump, backup, factory preset, capture or impulse response (`.gitignore` blocks `*.bin`, `*.uf2`, `*.ota`, `*.fwsc`, `*.am*`, `backup_tankg/`).
- Images built by `tools/pack.py` are packed into the container read from the user's own pedal: they are personal and must not be published.

## Third-party components

| Component | Licence | Use |
|---|---|---|
| felucca-bsp (chip headers, USB device, board headers) | MPL-2.0 | Linked into `firmware-tankg`; file-level copyleft: keep its notices and publish changes to its files. Not modified here. |
| LLVM / clang `pi32v2` toolchain | Apache-2.0 with LLVM exception | Build tool only. |
| jl-uboot-tool (kagaimiq) | MIT | Flash tool (`tools/flash_tankg.sh`). |
| JieLi `wl82loader.bin` | no explicit licence | Used by jl-uboot-tool at flash time; not redistributed here. |
| Tauri, React, Tailwind, `serialport`, `btleplug` and other npm/cargo dependencies | see `Cargo.lock`, `package-lock.json` | Desktop app. |
