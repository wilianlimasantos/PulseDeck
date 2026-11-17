#!/usr/bin/env python3
"""Find which GPIO bits change when you press each footswitch, using the firmware's read-only PORTS command.
  ~/.local/share/pipx/venvs/platformio/bin/python tools/port_probe.py [--port /dev/ttyACM0] [--raw]
Needs a terminal (asks you to press things). UNTESTED ON HARDWARE. 'PORTS PU' briefly enables input + pull-up on every pin (never PA0..PA3)
and restores the previous config; use --raw for plain register reads without touching any config.
Bits that change on press are reported as e.g. 'H bit 7'. Bits that are noisy at rest are listed and ignored."""
import argparse, re, sys, time
try: import serial
except ImportError: sys.exit('pyserial missing -> run with ~/.local/share/pipx/venvs/platformio/bin/python')
ap = argparse.ArgumentParser(); ap.add_argument('--port', default='/dev/ttyACM0'); ap.add_argument('--raw', action='store_true'); ap.add_argument('--n', type=int, default=8)
a = ap.parse_args(); cmd = 'PORTS' if a.raw else 'PORTS PU'
s = serial.Serial(a.port, 115200, timeout=0.4)
try: s.dtr = True
except OSError: pass
time.sleep(0.3); s.reset_input_buffer()
def rd():
    s.write((cmd + '\n').encode()); end = time.time() + 2
    while time.time() < end:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('OK PORTS'):
            return {k: int(v, 16) for k, v in re.findall(r'([A-H])=([0-9a-f]{4})', l)}
    sys.exit('no PORTS reply (is the new firmware running?)')
def sample():
    r = [rd() for _ in range(a.n)]; return r
def agg(rs):  # per port: (always-1 mask, always-0 mask)
    one = {p: 0xFFFF for p in 'ABCDEFGH'}; zero = {p: 0xFFFF for p in 'ABCDEFGH'}
    for r in rs:
        for p in one: one[p] &= r[p]; zero[p] &= ~r[p] & 0xFFFF
    return one, zero
bits = lambda m: [i for i in range(16) if m >> i & 1]
print('Solte tudo (nada pisado). Lendo repouso...'); rest = sample(); one, zero = agg(rest)
noisy = {p: 0xFFFF & ~(one[p] | zero[p]) for p in one}
print('Repouso estável: ' + ' '.join(f'{p}={one[p]:04x}' for p in sorted(one)))
for p in sorted(noisy):
    if noisy[p]: print(f'  bits instáveis em repouso (ignorados) {p}: {bits(noisy[p])}')
res = {}
for name in ['FS1', 'FS2', 'FS3', 'FS4', 'o 5º botão/tap, se houver (Enter para pular)']:
    t = input(f'\nPise e SEGURE {name}, depois Enter: ')
    o2, z2 = agg(sample()); ch = []
    for p in sorted(one):
        for b in bits((one[p] & z2[p]) | (zero[p] & o2[p])):
            if not noisy[p] >> b & 1: ch.append(f'{p}{b}({"1->0" if one[p]>>b&1 else "0->1"})')
    res[name] = ch; print('  mudou:', ', '.join(ch) if ch else '(nada)')
    input('  Solte e Enter: ')
print('\n=== resumo ===')
for k, v in res.items(): print(f'{k}: {v}')
