# Writing a new effect

1. **Catalogue** — add an `FxDef` to `esp32-mock/firmware-core/src/catalog.cpp`: id, name, category, CPU cost (ticks of a 1000
   budget), memory, and up to 7 parameters `{name, min, max, default}`. Run `make -C esp32-mock/firmware-core test`
   (`tools/check_catalog.py` checks consistency). The editor reads the catalogue from the pedal (`FXT`), so it appears in
   "+ Add block" without UI changes.
2. **DSP** — write the processing in `firmware-tankg/src/dsp_fx.h` as an `inline int32_t myRun(State&, const int16_t* p, int32_t x)`:
   integer only (no libm, no heap, no float), Q23 samples, `clamp24` the output. Big buffers go in `FxState` (watch the RAM:
   `llvm-size -A build/tankg.elf`).
3. **Wire it** — in `firmware-tankg/src/audio.h`: normalise its parameters by name in `buildParams` (so the DSP never depends on
   catalogue positions) and add a `case <id>:` in `AudioDsp::fx`.
4. **Knobs (optional)** — map Live-mode knobs to its parameter names in `esp32-mock/firmware-core/src/live.cpp` (`kMap`).
5. **Test** — add a case to `firmware-tankg/tests/test_dsp.cpp`: the block must change the signal, be transparent when off, stay
   bounded at full scale and do what its parameters say.
6. **Measure** — on a pedal: `AUDIO STAT` before/after (`load=`, `missed=` must stay 0).

**Provenance:** cite the algorithm (paper, textbook, URL) in a header comment. No code from proprietary firmware or from
projects whose licence is incompatible with MIT (see `NOTICE.md`).
