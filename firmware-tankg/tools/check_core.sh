#!/bin/sh
# Compile every firmware-core source for pi32v2 (-mcpu=r3, like the SDK builds) and report code size.
# Proves the core is portable: no STL, no libc beyond string/stdio/stdlib.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/tools/env.sh"
CORE="${CORE:-$HERE/../esp32-mock/firmware-core}"
OUT="${OUT:-$HERE/build/core}"
mkdir -p "$OUT"
FLAGS="--target=pi32v2 -mcpu=r3 -Oz -g0 -std=c++17 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections -fdata-sections -Wall -Wextra -nostdinc -isystem $HERE/compat -isystem $($PI32V2_LLVM/bin/clang --target=pi32v2 -print-resource-dir)/include -I$CORE/include"
for f in "$CORE"/src/*.cpp; do
  clang++ $FLAGS -c "$f" -o "$OUT/$(basename "$f" .cpp).o"
done
llvm-size "$OUT"/*.o
echo "--- total:"; llvm-size -t "$OUT"/*.o | tail -1
