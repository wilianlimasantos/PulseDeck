#!/usr/bin/env python3
"""Interactive sweep of the Tank-G LED/7-segment word (LEDBIT 0..28) over USB CDC; saves the map into src/led_sr.h.
  ~/.local/share/pipx/venvs/platformio/bin/python tools/led_sweep.py [--port /dev/ttyACM0] [--from N] [--fresh]
Run it in a REAL terminal (it asks you questions). Needs pyserial (the PlatformIO venv has it). UNTESTED ON HARDWARE.

For every bit the pedal lights exactly that bit; you type what you SEE. Accepted answers (case/space tolerant):
  fs1 r | fs2 g | fs3 b | fs4 azul        footswitch 1..4 + colour (r/g/b, vermelho/verde/azul, red/green/blue)
  fs2 amarelo | fs3 branco ...             any other colour: stored as label only (does not fill the R/G/B table)
  seg a .. seg g                           7-segment display segment
  dp                                       decimal point
  n                                        nothing visible
  any other text                           stored as free label
Commands:  r repeat the bit   b back one bit   s skip   ? help   q save and quit
Results are written after every answer to build/led_sweep.json (resume with the same command)."""
import argparse, json, os, re, sys, time, datetime
try:
    import serial
except ImportError:
    sys.exit("pyserial missing -> run with ~/.local/share/pipx/venvs/platformio/bin/python")
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON = os.path.join(HERE, 'build', 'led_sweep.json'); HDR = os.path.join(HERE, 'src', 'led_sr.h')
COL = {'r': 0, 'red': 0, 'vermelho': 0, 'vm': 0, 'g': 1, 'green': 1, 'verde': 1, 'vd': 1, 'b': 2, 'blue': 2, 'azul': 2, 'az': 2}

class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.3)
        try: self.s.dtr = True      # needed by the real CDC port; a pty used in tests does not support it
        except OSError: pass
        time.sleep(0.3); self.s.reset_input_buffer()
    def cmd(self, line, want=('OK', 'ERR', 'PONG'), t=2.0):
        self.s.write((line + '\n').encode()); end = time.time() + t; last = ''
        while time.time() < end:
            r = self.s.readline().decode(errors='replace').strip()
            if r:
                last = r
                if r.startswith(want): return r
        return last

def parse(ans):
    a = re.sub(r'\s+', ' ', ans.strip().lower())
    if a in ('n', 'nada', 'none', '-'): return {'kind': 'none'}
    if a in ('dp', 'ponto', 'point'): return {'kind': 'dp'}
    m = re.fullmatch(r'(?:7?seg)[ -]?([a-g])', a)
    if m: return {'kind': 'seg', 'seg': 'abcdefg'.index(m.group(1))}
    m = re.fullmatch(r'fs ?([1-4]) ?([a-zç]+)', a)
    if m:
        c = COL.get(m.group(2))
        return {'kind': 'fs', 'fs': int(m.group(1)) - 1, 'rgb': c, 'color': m.group(2)} if c is not None else {'kind': 'fs-other', 'fs': int(m.group(1)) - 1, 'color': m.group(2)}
    return {'kind': 'text'}

def load():
    try: return json.load(open(JSON))
    except Exception: return {'bits': {}, 'baseline': None}
def save(d):
    os.makedirs(os.path.dirname(JSON), exist_ok=True); json.dump(d, open(JSON, 'w'), indent=1, ensure_ascii=False)

def gen_block(d):
    lab = ['""'] * 29; fsb = [[-1] * 3 for _ in range(4)]; seg = [-1] * 7; dp = -1; conflicts = []
    for k, v in sorted(d['bits'].items(), key=lambda x: int(x[0])):
        n = int(k); p = v.get('parsed', {}); lab[n] = json.dumps(v['answer'], ensure_ascii=False)
        def put(arr, i, name):
            if arr[i] != -1: conflicts.append(f'{name} already bit {arr[i]}, also bit {n}')
            arr[i] = n
        if p.get('kind') == 'fs': put(fsb[p['fs']], p['rgb'], f"fs{p['fs']+1}/{'RGB'[p['rgb']]}")
        elif p.get('kind') == 'seg':
            if seg[p['seg']] != -1: conflicts.append(f"seg {p['seg']} twice")
            seg[p['seg']] = n
        elif p.get('kind') == 'dp': dp = n
    stamp = datetime.date.today().isoformat()
    b = [f'// BEGIN GENERATED LED MAP (tools/led_sweep.py, {stamp}) -- observed by a human on the pedal, -1 = not observed',
         'constexpr const char* kBitLabel[29] = {' + ', '.join(lab) + '};',
         'constexpr int8_t kFsColorBit[4][3] = {' + ', '.join('{' + ','.join(map(str, r)) + '}' for r in fsb) + '};  // [footswitch 0..3][R,G,B] -> word bit',
         'constexpr int8_t kSegBit[7] = {' + ','.join(map(str, seg)) + '};  // segments a..g -> word bit',
         f'constexpr int8_t kDpBit = {dp};', '// END GENERATED LED MAP']
    return '\n'.join(b), conflicts

def write_header(d):
    blk, conf = gen_block(d); s = open(HDR).read()
    pat = re.compile(r'// BEGIN GENERATED LED MAP.*?// END GENERATED LED MAP', re.S)
    if not pat.search(s): sys.exit('markers not found in led_sr.h')
    open(HDR + '.bak', 'w').write(s); open(HDR, 'w').write(pat.sub(lambda m: blk, s))
    print(f'\nSaved map into {HDR} (previous copy: led_sr.h.bak)')
    for c in conf: print('  WARNING:', c)

def summary(d):
    print('\n--- summary ---')
    for k in range(29):
        v = d['bits'].get(str(k)); print(f'bit {k:2d} (word {1<<k:08x}): ' + (v['answer'] if v else '(not tested)'))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/ttyACM0'); ap.add_argument('--from', dest='start', type=int); ap.add_argument('--fresh', action='store_true')
    ap.add_argument('--header', default=HDR); ap.add_argument('--json', default=JSON)
    ap.add_argument('--base', default='08000001', help='hex word OR-ed into every LEDBIT (stock always sends 0x08000001); 0 = none')
    ap.add_argument('--check', action='store_true', help='only PING + LEDBIT 0 + LEDRAW 0, no questions')
    a = ap.parse_args()
    globals()['HDR'], globals()['JSON'] = a.header, a.json
    try: dev = Dev(a.port)
    except Exception as e: sys.exit(f'cannot open {a.port}: {e}  (is another program, e.g. PulseDeck, using it?)')
    r = dev.cmd('PING'); print('PING ->', r)
    if not r.startswith('PONG'): sys.exit('no PONG: is the Tank-G firmware running on this port?')
    if a.check:
        print(dev.cmd('LEDBIT 0')); print(dev.cmd('LEDRAW 0')); return 0
    d = {'bits': {}, 'baseline': None} if a.fresh else load()
    print('Anotações: digite o que VÊ aceso. Comandos: r repetir | b voltar | s pular | ? ajuda | q salvar e sair.\n')
    print('LEDBASE ->', dev.cmd('LEDBASE ' + a.base))
    print('LEDRAW 0 ->', dev.cmd('LEDRAW 0'))
    if d['baseline'] is None:
        d['baseline'] = input('Com todos os bits em 0 (LEDRAW 0), o que está aceso? (Enter = nada): ').strip() or 'nada'; save(d)
    n = a.start if a.start is not None else next((i for i in range(29) if str(i) not in d['bits']), 29)
    while 0 <= n < 29:
        resp = dev.cmd(f'LEDBIT {n}')
        if not resp.startswith(f'OK LEDBIT {n} '): print('  unexpected reply:', resp)
        ans = input(f'[bit {n:2d}/28] {resp}\n   o que acendeu? ').strip()
        low = ans.lower()
        if low == 'q': break
        if low == 'r': continue
        if low == 'b': n = max(0, n - 1); continue
        if low == 's': n += 1; continue
        if low in ('?', 'h', 'help'): print(__doc__); continue
        if not ans: print('   (digite algo; "n" = nada)'); continue
        d['bits'][str(n)] = {'answer': ans, 'parsed': parse(ans), 'reply': resp}; save(d); n += 1
    print('LEDRAW 0 ->', dev.cmd('LEDRAW 0'))
    summary(d)
    if input('\nGravar o mapa em src/led_sr.h? [s/N] ').strip().lower() in ('s', 'y', 'sim'): write_header(d)
    return 0
if __name__ == '__main__': sys.exit(main())
