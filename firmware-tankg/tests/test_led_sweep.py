#!/usr/bin/env python3 -I
"""Drives tools/led_sweep.py against a fake firmware on a pty (no hardware) with scripted answers; works on a COPY of led_sr.h."""
import os, pty, sys, threading, subprocess, shutil, json, re, tempfile
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m, s = pty.openpty(); name = os.ttyname(s); seen = []
def fake():
    buf = b''
    while True:
        try: buf += os.read(m, 256)
        except OSError: return
        while b'\n' in buf:
            l, buf = buf.split(b'\n', 1); l = l.decode().strip(); seen.append(l)
            if l == 'PING': r = 'PONG tankg_fw M5.26 rel amx ble v1 Nov 12 2026 10:00:00'
            elif l.startswith('LEDBIT '): n = int(l.split()[1]); r = f'OK LEDBIT {n} word={1<<n:08x}'
            elif l.startswith('LEDBASE'): r = 'OK LEDBASE ' + l.split()[1]
            elif l.startswith('LEDRAW'): r = 'OK LEDRAW word=00000000'
            else: r = 'ERR'
            os.write(m, (r + '\r\n').encode())
threading.Thread(target=fake, daemon=True).start()
ans = ['n']
for fs in range(1, 5):
    ans += [f'FS{fs} R', f'fs{fs} verde', f'fs {fs} azul']                      # bits 0..11
ans += ['fs1 amarelo', 'n', 'seg a', 'r', 'seg b', 'seg c', 'b', 'seg c',        # 12 13 14 | 15 (repeat) | 16, back, redo 16
        'seg d', 'seg e', 'seg f', 'seg g', 'dp', 'algo estranho'] + ['n'] * 6   # 17..22, 23..28
ans += ['s']                                                                      # save to header
import hashlib; H0 = hashlib.sha256(open(os.path.join(HERE, 'src/led_sr.h'),'rb').read()).hexdigest()
tmp = tempfile.mkdtemp(); hdr = os.path.join(tmp, 'led_sr.h'); js = os.path.join(tmp, 'sw.json')
shutil.copy(os.path.join(HERE, 'src/led_sr.h'), hdr)
p = subprocess.run([os.environ.get('PY', sys.executable), os.path.join(HERE, 'tools/led_sweep.py'), '--port', name, '--fresh', '--header', hdr, '--json', js],
                   input='\n'.join(ans) + '\n', capture_output=True, text=True, timeout=120)
fails = 0
def chk(c, msg):
    global fails; print(('ok   ' if c else 'FAIL ') + msg); fails += not c
chk(p.returncode == 0, f'script exit 0 (stderr: {p.stderr.strip()[:120]})')
chk('LEDBASE 08000001' in seen, 'sweep sets the stock base word first')
chk('LEDRAW 0' in seen and seen[-1] == 'LEDRAW 0', 'pedal left with LEDRAW 0 at the end')
chk([x for x in seen if x.startswith('LEDBIT')].count('LEDBIT 15') == 2, 'bit 15 sent twice (repeat command)')
chk([x for x in seen if x.startswith('LEDBIT')].count('LEDBIT 16') == 2, 'bit 16 sent twice (back command)')
d = json.load(open(js)); chk(len(d['bits']) == 29, f'29 bits recorded ({len(d["bits"])})')
chk(d['baseline'] == 'n', 'baseline recorded')
h = open(hdr).read()
chk('{{0,1,2}, {3,4,5}, {6,7,8}, {9,10,11}}' in h, 'kFsColorBit = fs1..4 x R,G,B -> bits 0..11 (accepts "FS1 R", "fs1 verde", "fs 3 azul")')
chk('kSegBit[7] = {14,15,16,17,18,19,20}' in h, 'kSegBit a..g -> 14..20')
chk('kDpBit = 21' in h, 'kDpBit = 21')
chk('"fs1 amarelo"' in h and '"algo estranho"' in h, 'free-text labels kept')
chk(os.path.exists(hdr + '.bak'), 'backup of led_sr.h made')
chk(hashlib.sha256(open(os.path.join(HERE, 'src/led_sr.h'),'rb').read()).hexdigest() == H0, 'real src/led_sr.h untouched by the test')
open(os.path.join(tmp, 't.cpp'), 'w').write('#include "led_sr.h"\nint main(){ return tg::fsColor(2,1)==(1u<<7) && tg::fsColor(0,0)==1u && tg::fsColor(9,0)==0 ? 0 : 1; }\n')
c = subprocess.run(['g++', '-std=c++17', '-Wall', '-I', tmp, os.path.join(tmp, 't.cpp'), '-o', os.path.join(tmp, 't')], capture_output=True, text=True)
chk(c.returncode == 0, 'generated header compiles' + (' ' + c.stderr[:150] if c.returncode else ''))
chk(subprocess.run([os.path.join(tmp, 't')]).returncode == 0, 'fsColor() returns the mapped bits')
print(f'{fails} failed'); sys.exit(fails != 0)
