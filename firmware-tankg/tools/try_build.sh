#!/bin/sh
# Compile-check C glue (felucca-bsp) + C++ main (firmware-core) for pi32v2. Does not link/pack (needs app.ld, crt0, package builder).
HERE=$(cd "$(dirname "$0")/.." && pwd); . "$HERE/tools/env.sh"
BSP=${BSP:-$HERE/../third_party/felucca-bsp}   # set in tools/local.env or the environment
CORE=${CORE:-$HERE/../esp32-mock/firmware-core}
# Default: flash persistence ON in the verified-free slot 0x75000..0x7B000 (see HW_MAP_TANKG_M548.md). EXTRA=" " (a space) gives the RAM-only build.
EXTRA=${EXTRA:--DTG_FLASH_STORE -DTG_STORE_OFF=0x75000u}
mkdir -p "$HERE/build"; F="--target=pi32v2 -mcpu=r3 -Os -ffreestanding"
clang $F $EXTRA -I"$BSP/chip" -I"$HERE/src" -c "$HERE/src/bsp_glue.c" -o "$HERE/build/bsp_glue.o" || exit 1
clang++ $(echo $F | sed s/-Os/-O2/) -std=c++17 -fno-exceptions -fno-rtti -nostdinc -isystem "$HERE/compat" -isystem "$(clang --target=pi32v2 -print-resource-dir)/include" $EXTRA -I"$HERE/src" -I"$CORE/include" -c "$HERE/src/main.cpp" -o "$HERE/build/main.o" || exit 1
llvm-size "$HERE"/build/*.o
