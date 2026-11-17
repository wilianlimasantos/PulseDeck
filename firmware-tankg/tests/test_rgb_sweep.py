#!/usr/bin/env python3 -I
"""Drives tools/rgb_sweep.py against a fake pedal on a pty with scripted answers (no hardware); output goes to temp files."""
import os, pty, sys, threading, subprocess, tempfile, json, hashlib
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m, s = pty.openpty(); name = os.ttyname(s); seen = []
def fake():
    buf = b''
    while True:
        try: buf += os.read(m, 256)
        except OSError: return
        while b'\n' in buf:
            l, buf = buf.split(b'\n', 1); l = l.decode().strip(); seen.append(l)
            r = 'PONG fake' if l == 'PING' else 'OK ' + l if l.split(' ')[0] in ('RGBPAT', 'RGBDIV', 'RGBOFF', 'RGB') else 'ERR'
            os.write(m, (r + '\r\n').encode())
threading.Thread(target=fake, daemon=True).start()
ans = ['1 verde 3 laranja', 'r']                       # calibration: 2nd candidate (div 8) is good
ans += [f'fs{i // 3 + 1} {"abc"[i % 3]}' for i in range(12)]   # pixel i -> fs(i//3+1) pos a/b/c
ans += ['verde', 'azul', 's']                          # channel order, write header
_R = os.path.join(HERE, 'src/rgb_map.h'); M0 = hashlib.sha256(open(_R,'rb').read()).hexdigest() if os.path.exists(_R) else None
tmp = tempfile.mkdtemp(); js = os.path.join(tmp, 'r.json'); hdr = os.path.join(tmp, 'rgb_map.h')
H0 = hashlib.sha256(open(os.path.join(HERE, 'tools/rgb_sweep.py'), 'rb').read()).hexdigest()
p = subprocess.run([os.environ.get('PY', sys.executable), os.path.join(HERE, 'tools/rgb_sweep.py'), '--port', name, '--fresh', '--json', js, '--header', hdr],
                   input='\n'.join(ans) + '\n', capture_output=True, text=True, timeout=120)
fails = 0
def chk(c, msg):
    global fails; print(('ok   ' if c else 'FAIL ') + msg); fails += not c
chk(p.returncode == 0, 'exit 0 ' + p.stderr[-150:])
chk('RGBDIV 9' in seen and 'RGBDIV 8' in seen and 'RGBDIV 10' not in seen, 'calibration stopped at the first good candidate (div 8)')
chk(all(f'RGB {i} 100000' in seen for i in range(12)), 'every pixel lit alone once at level 0x10')
chk('RGB 0 001000' in seen and 'RGB 0 000010' in seen, 'channel order probes sent')
chk(seen[-1] == 'RGBOFF', 'pedal left dark')
d = json.load(open(js)); chk(d['good'] == {'div': 8, 'one': '7e', 'zero': '60'}, 'good settings saved')
h = open(hdr).read(); chk('{{0,1,2},{3,4,5},{6,7,8},{9,10,11}}' in h.replace(' ', ''), 'kRgbPixel generated')
open(os.path.join(tmp, 't.cpp'), 'w').write('#include "rgb_map.h"\nint main(){return tg::kRgbPixel[3][2]==11?0:1;}\n')
c = subprocess.run(['g++', '-std=c++17', '-I', tmp, os.path.join(tmp, 't.cpp'), '-o', os.path.join(tmp, 't')], capture_output=True, text=True)
chk(c.returncode == 0 and subprocess.run([os.path.join(tmp, 't')]).returncode == 0, 'generated header compiles and indexes correctly')
R = os.path.join(HERE, 'src/rgb_map.h'); chk((hashlib.sha256(open(R,'rb').read()).hexdigest() if os.path.exists(R) else None) == M0, 'real src/rgb_map.h untouched by the test')
print(f'{fails} failed'); sys.exit(fails != 0)
