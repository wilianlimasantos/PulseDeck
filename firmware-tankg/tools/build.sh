#!/bin/sh
# Full build: compile -> link (felucca app.ld, XIP 0x02000120) -> raw image build/tankg.bin. NOT TESTED ON HARDWARE.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); . "$HERE/tools/env.sh"
BSP=${BSP:-$HERE/../third_party/felucca-bsp}   # set in tools/local.env or the environment
"$HERE/tools/check_core.sh" >/dev/null 2>&1 || "$HERE/tools/check_core.sh"
"$HERE/tools/try_build.sh" >/dev/null
B=$HERE/build; L=$B/l; mkdir -p $L
for s in crt0 fm1_vec fm1_isr fm1_cpu1; do clang --target=pi32v2 -c $BSP/chip/$s.S -o $L/$s.o; done
clang --target=pi32v2 -mcpu=r3 -Os -ffreestanding -c $HERE/src/libc_min.c -o $L/libc_min.o
# RAM: the BSP example reserves 96 K; the AC79 SRAM is 0x01C00000..0x01C7FD50 (felucca docs/BOARD_IO.md) and the stacks start at 0x01C74000,
# so the data region is widened to 320 K (ends 0x01C58000) for the capture player, the IR and the effect delay lines.
sed 's/RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K/RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 320K/' $BSP/examples/hwtest/app.ld > $B/app.ld
grep -q "LENGTH = 320K" $B/app.ld || { echo "app.ld RAM line not found"; exit 1; }
ld.lld -T $B/app.ld --gc-sections $L/crt0.o $L/fm1_vec.o $L/fm1_isr.o $L/fm1_cpu1.o $L/libc_min.o $B/bsp_glue.o $B/main.o $B/core/*.o -o $B/tankg.elf
for sec in text data ram_text; do llvm-objcopy -O binary -j .$sec $B/tankg.elf $L/$sec.bin 2>/dev/null || : > $L/$sec.bin; done
# the RAM copy of the capture network must not call anything (a call from RAM into the flash would fault while the flash is busy, and is out of range)
llvm-objdump -d --section=.ram_text $B/tankg.elf | awk '/<a2lRunRam>:/{f=1;next} /^$/{f=0} f' > $L/a2l_ram.s
grep -q . $L/a2l_ram.s || { echo "a2lRunRam not in .ram_text"; exit 1; }
if grep -qE "call|jsr|goto [^.]*<[^a]" $L/a2l_ram.s; then grep -E "call|jsr" $L/a2l_ram.s | head; echo "a2lRunRam calls out of RAM"; exit 1; fi
python3 -I - "$B" <<'PY'
import re,subprocess,sys
b=sys.argv[1]
sy=subprocess.run(['llvm-nm',f'{b}/tankg.elf'],capture_output=True,text=True,check=True).stdout
g=lambda n:int(re.search(r'^([0-9a-f]+) . '+n+'$',sy,re.M).group(1),16)
assert g('_start')==0x02000120,hex(g('_start'))
img=bytearray(open(f'{b}/l/text.bin','rb').read())
for at,part in sorted([(g('_rt_load'),open(f'{b}/l/ram_text.bin','rb').read()),(g('_data_load'),open(f'{b}/l/data.bin','rb').read())]):
    if part:
        assert at-0x02000120>=len(img),'overlap'
        img+=b'\xff'*(at-0x02000120-len(img))+part
img+=b'\xff'*(-len(img)%4)
open(f'{b}/tankg.bin','wb').write(img); print('tankg.bin',len(img),'bytes')
PY
