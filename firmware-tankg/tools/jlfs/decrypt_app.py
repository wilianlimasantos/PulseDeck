#!/usr/bin/env python3
"""JieLi SFC/ENC flash descrambler (read-only on input). Algorithm per kagaimiq/jielie periph/sfc.md + misc/cipher.md:
  data scrambled in 32-byte blocks; block key = chip_key ^ ((addr_of_block) >> 2); stream = CRC16-CCITT LFSR, byte ^= key&0xff."""
import sys, collections, math
def enc(data, key):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= key & 0xFF
        key = ((key << 1) & 0xFFFF) ^ (0x1021 if key & 0x8000 else 0)
    return bytes(out)
def decrypt(buf, chip_key, base, blk=32, addr_shift=2):
    out = bytearray()
    for o in range(0, len(buf), blk):
        k = (chip_key ^ ((base + o) >> addr_shift)) & 0xFFFF
        out += enc(buf[o:o+blk], k)
    return bytes(out)
def score(b):
    c = collections.Counter(b); n = len(b)
    e = -sum(v/n*math.log2(v/n) for v in c.values())
    return e, (c[0]+c[255])/n
if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    chip_key = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x980F
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x4000
    d = open(src, 'rb').read()
    out = decrypt(d, chip_key, base)
    open(dst, 'wb').write(out)
    print("entropy %.3f  zero/ff %.3f" % score(out[:0x10000]))
