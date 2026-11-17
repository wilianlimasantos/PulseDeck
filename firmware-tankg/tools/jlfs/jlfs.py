"""JLFS helpers shared by repack_app.py (table-driven CRC16/XMODEM, entry parsing). Read-only utilities."""
import struct
_T = []
for i in range(256):
    c = i << 8
    for _ in range(8): c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    _T.append(c)
def crc16(b, c=0):
    for x in b: c = ((c << 8) & 0xFFFF) ^ _T[(c >> 8) ^ x]
    return c
def entries(d, n=32):
    out = []
    for o in range(0, 32 * n, 32):
        e = d[o:o + 32]
        if e[2:4] == b'\xff\xff' and e[0:2] == b'\xff\xff': break
        h, dc, off, sz, at, rs, ix = struct.unpack('<HHIIBBH', e[:16])
        out.append(dict(pos=o, hdr=h, dcrc=dc, off=off, size=sz, attr=at, idx=ix, name=e[16:].split(b'\0')[0].decode('latin1')))
    return out
