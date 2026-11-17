#!/usr/bin/env python3 -I
"""Drives tools/knob_sweep.py against a fake pedal on a pty: the harness 'turns' the knob the script asks for, skips one, and redoes one with 'b'. No hardware."""
import os, pty, sys, threading, subprocess, tempfile, json, random, hashlib, re, time
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, 'tools'))
NAMES = ['MASTER','IR CAB','RVB MIX','RVB DECAY','DLY TIME','DLY MIX','MOD SPEED','MOD FX','VOLUME','BASS','MIDDLE','TREBLE','GAIN','TYPE','NOISE GATE']
SLOT = [3, 0, 12, 5, 14, 1, 9, 2, 7, 13, 6, 8, 11, 4, 10]            # knob k -> true slot (a permutation of 15 slots; slot 15 = voltage)
state = [random.randint(100, 500) for _ in range(16)]; state[15] = 700; lock = threading.Lock()
m, s = pty.openpty(); name = os.ttyname(s)
def fake():
    buf = b''
    while True:
        try: buf += os.read(m, 256)
        except OSError: return
        while b'\n' in buf:
            l, buf = buf.split(b'\n', 1); l = l.decode().strip()
            if l.startswith('KNOBS'):
                with lock: v = [max(0, min(1023, x + random.randint(-4, 4))) for x in state]
                r = 'OK KNOBS ch=8 mux0..7 ch3(PB1)=' + ','.join(map(str, v[:8])) + ' ch8=' + ','.join(map(str, v[8:]))
            else: r = 'ERR'
            os.write(m, (r + '\r\n').encode())
threading.Thread(target=fake, daemon=True).start()
tmp = tempfile.mkdtemp(); js = os.path.join(tmp, 'k.json'); hdr = os.path.join(tmp, 'knob_map.h')
R = os.path.join(HERE, 'src/knob_map.h'); H0 = hashlib.sha256(open(R, 'rb').read()).hexdigest() if os.path.exists(R) else None
env = dict(os.environ, PYTHONUNBUFFERED='1')
p = subprocess.Popen([os.environ.get('PY', sys.executable), '-u', os.path.join(HERE, 'tools/knob_sweep.py'), '--port', name, '--json', js, '--header', hdr],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
out = ''
def wait(txt, t=30):
    global out; end = time.time() + t
    while txt not in out[-400:] and time.time() < end:
        c = p.stdout.read(1)
        if not c: break
        out += c
    return txt in out[-400:]
def say(x): p.stdin.write(x + '\n'); p.stdin.flush()
fails = 0
def chk(c, msg):
    global fails; print(('ok   ' if c else 'FAIL ') + msg); fails += not c
wait('Capturando')
redone = False
for k, n in enumerate(NAMES):
    ok = wait(f'Gire o knob  {n}')
    if not ok: chk(False, f'prompt for {n}'); break
    if n == 'IR CAB': out = ''; say('s'); continue                    # skipped
    if n == 'NOISE GATE' and not redone:                               # go back to TYPE and redo it
        out = ''; say('b'); redone = True
        ok = wait('Gire o knob  TYPE'); chk(ok, 'b went back to TYPE')
        with lock: state[SLOT[13]] = (state[SLOT[13]] + 300) % 1000    # turn TYPE again
        out = ''; wait('Pare de girar'); out = ''
        ok = wait('Gire o knob  NOISE GATE'); chk(ok, 'then NOISE GATE again')
    out = ''
    with lock: state[SLOT[k]] = (state[SLOT[k]] + 450) % 1024 if state[SLOT[k]] < 574 else state[SLOT[k]] - 450   # big move
    if not wait('Pare de girar'): chk(False, f'{n} not detected'); break
    chk(f'slot {SLOT[k]} ' in out or f'slot {SLOT[k]}  ' in out, f'{n} detected on slot {SLOT[k]}')
    out = ''
rest = p.stdout.read(); p.wait(timeout=20)
d = json.load(open(js))
chk(p.returncode == 0, 'exit 0')
chk('IR CAB' in d['skipped'] and 'IR CAB' not in d['map'], 'IR CAB skipped')
chk(all(d['map'][n]['slot'] == SLOT[k] for k, n in enumerate(NAMES) if n != 'IR CAB'), '14 knobs mapped to the right slots')
h = open(hdr).read()
exp = ','.join(str(SLOT[k] if NAMES[k] != 'IR CAB' else -1) for k in range(15))
chk(f'kKnobSlot[15] = {{{exp}}}' in h, 'kKnobSlot generated (IR CAB = -1)')
open(os.path.join(tmp, 't.cpp'), 'w').write('#include "knob_map.h"\nint main(){return tg::kKnobSlot[0]==3 && tg::kSlotKnob[3]==0 && tg::kSlotKnob[15]==-1 && tg::kKnobSlot[1]==-1 ? 0 : 1;}\n')
c = subprocess.run(['g++', '-std=c++17', '-I', tmp, os.path.join(tmp, 't.cpp'), '-o', os.path.join(tmp, 't')], capture_output=True, text=True)
chk(c.returncode == 0 and subprocess.run([os.path.join(tmp, 't')]).returncode == 0, 'generated header compiles; inverse table and -1 ok')
chk((hashlib.sha256(open(R, 'rb').read()).hexdigest() if os.path.exists(R) else None) == H0, 'real src/knob_map.h untouched by the test')
# q at the first prompt, and EOF on stdin, both must save an all -1 map and exit 0
for tag, inp in (('q', 'q\n'), ('EOF', '')):
    j2 = os.path.join(tmp, f'q{tag}.json'); h2 = os.path.join(tmp, f'q{tag}.h')
    r = subprocess.run([os.environ.get('PY', sys.executable), os.path.join(HERE, 'tools/knob_sweep.py'), '--port', name, '--json', j2, '--header', h2], input=inp, capture_output=True, text=True, timeout=60)
    chk(r.returncode == 0 and 'kKnobSlot[15] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}' in open(h2).read(), f'{tag}: exits 0 and writes an all -1 map ' + r.stderr[-100:])
print(f'{fails} failed'); sys.exit(fails != 0)
