# firmware-tankg

Open replacement firmware for the M-VAVE Tank-G v1 (JieLi AC791N, `pi32v2`), built on the open felucca-bsp chip layer and
the shared engine in `../esp32-mock/firmware-core`.

Documentation lives in [`../docs/`](../docs/):

- [Building, packing, flashing, recovery](../docs/building.md)
- [Hardware map](../docs/hardware.md) · [Flash layout and store](../docs/flash-layout.md)
- [Serial protocol](../docs/protocol.md) · [Architecture and real-time budget](../docs/architecture.md)
- [Writing a new effect](../docs/effects.md)

## Quick start
```sh
# optional: create tools/local.env: put PI32V2_LLVM, BSP, JL_UBOOT_TOOL, JL_PYTHON here (gitignored)
. tools/env.sh
tools/build.sh                                            # build/tankg.bin
python3 -I tools/pack.py --template <your app-area backup> --out build/tankg_app_mypedal.bin
tools/flash_tankg.sh check && tools/flash_tankg.sh write  # write needs a real terminal (sudo) and asks you to type GRAVAR
```
Back up your own pedal first and never publish the backup or the packed image (see `../NOTICE.md`).

| Path | |
|---|---|
| `src/` | firmware: `main.cpp` (loop, commands), `panel.h`, `audio.h`, `dsp_fx.h`, `a2l.h`, `media.h`, `log_store.h`, `bsp_glue.c` |
| `tests/` | host tests (no hardware), `host_pedal.cpp` = the whole firmware as a host process |
| `tools/` | build, pack (`jlfs/` = JLFS container helpers), flash, bench and probe scripts |

Older working notes (Portuguese): [`../docs/firmware-notes.pt-BR.md`](../docs/firmware-notes.pt-BR.md).
