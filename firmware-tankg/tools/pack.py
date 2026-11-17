#!/usr/bin/env python3 -I
"""Pack build/tankg.bin into a valid encrypted JLFS container (JieLi JLFS layout), using the verified tools/jlfs/repack_app.py pipeline.
READ-ONLY on the template; writes only --out (+ .sha256). Nothing here touches hardware.

  python3 -I tools/pack.py [--bin build/tankg.bin] [--template <your app area backup>] [--out build/tankg_app.bin]

The app.bin entry keeps its exact size (repack_app requirement): tankg.bin is padded with 0xFF. The JLFS entry table,
cfg_tool.bin, app_area_head and every CRC16/XMODEM are rebuilt; the result is decrypted again and all CRCs re-checked."""
import sys, os, argparse, hashlib
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap.add_argument('--bin', default=os.path.join(here, 'build/tankg.bin'))
ap.add_argument('--template', required=True, help="YOUR pedal's own app area (flash 0x4000..0x7B000) from your read-back backup; never part of this repo")
ap.add_argument('--out', default=os.path.join(here, 'build/tankg_app.bin'))
ap.add_argument('--jlfs', default=os.path.join(here, 'tools', 'jlfs'))
a = ap.parse_args()
sys.path.insert(0, a.jlfs)
from repack_app import repack, check_dir
from decrypt_app import decrypt
tpl = open(a.template, 'rb').read(); fw = open(a.bin, 'rb').read()
if os.path.abspath(a.out) == os.path.abspath(a.template): sys.exit('refusing to overwrite the template')
img = decrypt(tpl, 0x980F, 0); by, probs = check_dir(img)
if probs: sys.exit('template not clean: ' + ', '.join(probs))
size = by['app.bin']['size']
if fw[:4] != bytes.fromhex('04818000'): sys.exit('tankg.bin does not start with goto+4;rts')
if len(fw) > size: sys.exit(f'tankg.bin {len(fw)} B > app.bin slot {size} B')
inner = fw + b'\xff' * (size - len(fw))
out = repack(tpl, inner, (), 0x980F, 0)
# independent re-check from the written bytes
back = decrypt(out, 0x980F, 0); by2, p2 = check_dir(back); e = by2['app.bin']
assert not p2 and back[e['off']:e['off'] + len(fw)] == fw and len(out) == len(tpl)
open(a.out, 'wb').write(out)
h = hashlib.sha256(out).hexdigest(); open(a.out + '.sha256', 'w').write(f'{h}  {os.path.basename(a.out)}\n')
print(f'wrote {a.out}: {len(out)} B (app.bin {len(fw)} B + {size-len(fw)} B pad), CRCs OK, sha256 {h}')
print('template untouched:', hashlib.sha256(open(a.template,'rb').read()).hexdigest() == hashlib.sha256(tpl).hexdigest())
