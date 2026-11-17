#!/usr/bin/env python3
"""Offline repack of a JieLi JLFS application container (e.g. the app area of your own Tank-G backup) around a new app.bin.   READ-ONLY on every input; writes only --out.

  python3 -I tools/jlfs/repack_app.py --template your_app_area.bin [--inner patched_inner.bin] [--patch OFF:OLDHEX=NEWHEX ...] --out OUT.bin
  python3 -I tools/jlfs/repack_app.py --template your_app_area.bin --selftest        # byte-exact round trip proof

Pipeline (all verified against the shipped images):
  1. decrypt template      : ENC cipher, 32-byte blocks, key = chip_key ^ (offset>>2), chip_key 0x980F, base 0 (decrypt_app.py)
  2. parse JLFS directory  : 32-byte entries at 0x00..; every CRC must already be valid, otherwise abort
  3. splice inner          : app.bin payload at entry.off (0x120), same length required
  4. CRC16/XMODEM (poly 0x1021, init 0) in this order:
        app.bin.data_crc  = crc16(app.bin payload)
        app.bin.hdr_crc   = crc16(entry[2:32])
        area_head.data_crc= crc16(image[0x20 : area_head.size])      (covers the entry table + app.bin + cfg_tool.bin)
        area_head.hdr_crc = crc16(entry[2:32])
  5. re-encrypt (XOR cipher is its own inverse), then re-decrypt the result and re-check every CRC.
Unknowns not handled here: anything outside the app area (SPL, VM, USR) is never touched; the image must keep its exact size."""
import sys, os, struct, hashlib, argparse
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from decrypt_app import decrypt          # decrypt == encrypt (self-inverse)
from jlfs import crc16, entries

def check_dir(img):
    es = entries(img); by = {e['name']: e for e in es}
    for n in ('app_area_head', 'app.bin', 'cfg_tool.bin'):
        if n not in by: raise SystemExit(f"JLFS entry {n} missing - wrong key/base?")
    problems = []
    for e in es[:3]:
        if crc16(img[e['pos'] + 2:e['pos'] + 32]) != e['hdr']: problems.append(f"{e['name']}: header CRC")
    a = by['app.bin']; c = by['cfg_tool.bin']; h = by['app_area_head']
    if crc16(img[a['off']:a['off'] + a['size']]) != a['dcrc']: problems.append("app.bin: data CRC")
    if crc16(img[c['off']:c['off'] + c['size']]) != c['dcrc']: problems.append("cfg_tool.bin: data CRC")
    if crc16(img[0x20:h['size']]) != h['dcrc']: problems.append("app_area_head: data CRC")
    return by, problems

def fix_crcs(img):
    img = bytearray(img); by, _ = check_dir(img)
    a, h = by['app.bin'], by['app_area_head']
    struct.pack_into('<H', img, a['pos'] + 2, crc16(img[a['off']:a['off'] + a['size']]))
    struct.pack_into('<H', img, a['pos'], crc16(img[a['pos'] + 2:a['pos'] + 32]))
    struct.pack_into('<H', img, h['pos'] + 2, crc16(img[0x20:h['size']]))
    struct.pack_into('<H', img, h['pos'], crc16(img[h['pos'] + 2:h['pos'] + 32]))
    return bytes(img)

def repack(template_enc, inner=None, patches=(), key=0x980F, base=0):
    img = decrypt(template_enc, key, base)
    by, probs = check_dir(img)
    if probs: raise SystemExit("template is not a clean JLFS image: " + ", ".join(probs))
    a = by['app.bin']; img = bytearray(img)
    cur = bytes(img[a['off']:a['off'] + a['size']])
    new = bytearray(cur if inner is None else inner)
    if len(new) != a['size']: raise SystemExit(f"inner size {len(new)} != app.bin size {a['size']} (must match exactly)")
    for p in patches:
        off, rest = p.split(':', 1); old, newb = rest.split('=')
        off = int(off, 0); old = bytes.fromhex(old); newb = bytes.fromhex(newb)
        if len(old) != len(newb): raise SystemExit("patch old/new length differ: " + p)
        if bytes(new[off:off + len(old)]) != old: raise SystemExit(f"patch {p}: bytes at 0x{off:x} are {bytes(new[off:off+len(old)]).hex()}, expected {old.hex()}")
        new[off:off + len(old)] = newb
    img[a['off']:a['off'] + a['size']] = new
    img = fix_crcs(bytes(img))
    _, probs = check_dir(img)
    if probs: raise SystemExit("internal error, CRCs invalid after fix: " + ", ".join(probs))
    out = decrypt(img, key, base)
    # independent verification: decrypt what we are about to write and re-check
    back = decrypt(out, key, base); _, p2 = check_dir(back)
    if back != img or p2: raise SystemExit("verification failed")
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--template', required=True); ap.add_argument('--inner'); ap.add_argument('--out')
    ap.add_argument('--patch', action='append', default=[], help='OFFSET_IN_INNER:OLDHEX=NEWHEX (old bytes are checked)')
    ap.add_argument('--key', type=lambda x: int(x, 0), default=0x980F); ap.add_argument('--base', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    tpl = open(a.template, 'rb').read()
    if a.selftest:
        img = decrypt(tpl, a.key, a.base); by, probs = check_dir(img)
        e = by['app.bin']; inner = img[e['off']:e['off'] + e['size']]
        print("JLFS CRC problems in original:", probs or "none")
        out = repack(tpl, inner, (), a.key, a.base)
        ok = out == tpl
        print("round trip repack(decrypt(original)) == original:", ok, "| sha256", hashlib.sha256(out).hexdigest())
        print("original sha256                                :", hashlib.sha256(tpl).hexdigest())
        return 0 if ok else 1
    if not a.out: ap.error("--out required")
    if os.path.abspath(a.out) == os.path.abspath(a.template): ap.error("refusing to overwrite the template")
    inner = open(a.inner, 'rb').read() if a.inner else None
    out = repack(tpl, inner, a.patch, a.key, a.base)
    open(a.out, 'wb').write(out)
    print(f"wrote {a.out}: {len(out)} bytes sha256 {hashlib.sha256(out).hexdigest()}")

if __name__ == "__main__":
    sys.exit(main())
