# Technical documentation

| File | Contents |
|---|---|
| [hardware.md](hardware.md) | M-VAVE Tank-G v1 board: AC791N pins, panel shift register, 7-segment, footswitches, RGB rings, knobs, audio interface |
| [flash-layout.md](flash-layout.md) | SPI flash map, the window the firmware may write, the power-cut-safe store and its keys (presets, captures, IRs) |
| [protocol.md](protocol.md) | USB CDC text protocol: framing, the commands the open firmware implements, uploads, diagnostics |
| [architecture.md](architecture.md) | Firmware structure, audio path, dual-core pipeline, capture player, real-time budget and measured costs |
| [building.md](building.md) | Toolchain, build, packing, flashing, recovery |
| [effects.md](effects.md) | How to write a new DSP effect |

Confidence tags used throughout: **[measured]** = observed on the author's own pedal; **[code]** = read from the stock
firmware for interoperability (a hardware fact recorded as a fact, not as code, see `NOTICE.md`); **[inferred]** = not verified.
