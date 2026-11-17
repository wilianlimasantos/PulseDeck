# Architecture

```
firmware-tankg (pi32v2)                      PulseDeck (desktop, Tauri)
 ├─ felucca-bsp (MPL-2.0): chip, USB, CPU1    ├─ crates/pulsedeck-core: protocol, session, .nam -> A2L1, compaction, uploads
 ├─ firmware-core (shared C++17, no STL)      ├─ src-tauri: commands, state sync
 │   Engine: presets, scenes, Live mode,      └─ src: React UI
 │   footswitch gestures, knobs, protocol
 ├─ panel.h: footswitches, knobs, LEDs
 ├─ audio.h / dsp_fx.h: effects, amp, cab
 ├─ a2l.h: capture (WaveNet) player
 ├─ media.h: uploaded captures / IRs
 └─ log_store.h: flash store
```

## Modes
- **Preset mode** (boot): A..D load presets 0..3; tapping the active one = bypass; holding = save.
- **Live mode** (B+C tapped together; decimal point lit): A reverb, B delay, C modulation, D amp on/off; knobs edit the first
  active block of their kind with soft takeover (LED blinks until the knob crosses the stored value). MASTER and NOISE GATE
  (absolute position, <= 2 % = off) work in both modes.

## Audio path
`input -> gain -> P effects -> amp (open amp stage or capture network) -> Q effects -> gain -> MASTER -> output`,
integer DSP (Q23 samples), 128-frame blocks in the ALNK0 interrupt. Parameters are copied into a double-buffered snapshot
by the main loop every 20 ms, so the interrupt never sees a half-edited chain.

## Dual core
With CPU1 running, the block is a three-stage pipeline (+2 blocks, 5.3 ms latency): CPU1 runs the Q effects of block n-2 and
the P effects of block n while CPU0 runs the amp/capture of block n-1. Host test: identical output to the single-core path,
256 samples later.

## Real-time budget (measured on the author's unit, 2026-11-15)
| Item | Cost |
|---|---|
| block period | 2666 us |
| preset with drive + delay + reverb + cab (two cores) | ~680 us on CPU0 |
| capture network | ~95..100 ns per multiply-accumulate (memory bound: ~35 ns per SRAM read) |
| capture limit reported by `CAPS` | ~170 MACs/sample |

Stock NAM "A2 Lite" captures need ~1730 MACs/sample; the editor distils them into a smaller network of the same family that
fits the measured limit (`crates/pulsedeck-core/src/a2l/distill.rs`) and shows the remaining error (ESR).
