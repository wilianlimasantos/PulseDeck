#!/usr/bin/env python3
"""Flash persistence check against the real ESP32 bench board.

  ~/.local/share/pipx/venvs/platformio/bin/python tools/bench_serial.py [/dev/ttyUSB0]

1. FACTORYRESET, build a preset + scenes + global settings, save them
2. hardware reset (RTS pulse) = power cycle of the firmware
3. check that everything came back from flash
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
fails = 0


def reset(s):
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    t = time.time()
    buf = b""
    while time.time() - t < 6:
        buf += s.read(256)
        if b"BOOT pulsedeck-bench" in buf:
            time.sleep(0.2)
            s.read(4096)
            return buf.decode(errors="replace").strip().splitlines()[-1]
    raise SystemExit("board did not boot: %r" % buf[-200:])


def cmd(s, line, end=None, timeout=5.0):
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    out, t = [], time.time()
    while time.time() - t < timeout:
        l = s.readline().decode(errors="replace").strip()
        if not l or l.startswith("["):  # ignore ESP-IDF log noise
            continue
        out.append(l)
        if end is None or l.startswith(end):
            break
    return out


def check(name, cond, info=""):
    global fails
    print(("  ok   " if cond else "  FAIL ") + name + (("   " + info) if (info and not cond) else ""))
    fails += not cond


s = serial.Serial(PORT, 115200, timeout=0.3)
s.dtr = s.rts = False
print("boot:", reset(s))
check("PING", cmd(s, "PING")[0].startswith("PONG"))
check("STORE reports flash backends", "presets=littlefs settings=nvs" in cmd(s, "STORE")[0], cmd(s, "STORE")[0])
check("FACTORYRESET", cmd(s, "FACTORYRESET")[-1] == "FACTORYRESET ok")

cmd(s, "PNEW")
fx = cmd(s, "FXA P 99 overdrive", "FX cpu")
bid = int(fx[0].split("id=")[1].split()[0])
cmd(s, "FXA Q 99 delay", "FX cpu")
cmd(s, "FXP %d 0 91" % bid)
r = cmd(s, "PSAVE 7 Flash Test")
check("PSAVE 7", r[0].startswith("PSAVE ok slot=7"), str(r))
cmd(s, "PCOLOR 00ff88")
cmd(s, "PSAVE 7 Flash Test")
cmd(s, "FXP %d 0 12" % bid)
check("SNAPSAVE 1", cmd(s, "SNAPSAVE 1")[0] == "SNAPSAVE ok 1 preset=7")
cmd(s, "FXE %d 0" % bid)
check("SNAPSAVE 3", cmd(s, "SNAPSAVE 3")[0] == "SNAPSAVE ok 3 preset=7")
cmd(s, "SNAPNAME 3 Clean Mute")
cmd(s, "HPF 140")
cmd(s, "FSMODE layout=custom")
cmd(s, "FSA 2 1 8 4 1")
check("GSAVE", cmd(s, "GSAVE")[0] == "GSAVE ok")
check("PLOAD 7 (before reboot) is current", "current=7" in cmd(s, "PLIST", "PLIST end")[-1])

print("-- hardware reset --")
print("boot:", reset(s))
pl = cmd(s, "PLIST", "PLIST end")
print("   ", pl)
check("preset 7 survived reboot", any(l.startswith("PRE 7 2 0 0 'Flash Test' s=10 col=00ff88") for l in pl), str(pl))
check("demo presets still there", any(l.startswith("PRE 0 5") for l in pl) and any(l.startswith("PRE 1 5") for l in pl))
check("current preset restored", pl[-1] == "PLIST end current=7", pl[-1])
check("chain restored (overdrive p0=91 from PSAVE)", any("overdrive on=1 p=91," in l for l in cmd(s, "FXS", "FXS peak")))
check("scenes 1,3 flagged", cmd(s, "SNAPS")[0].startswith("SNAPS 0101"), cmd(s, "SNAPS")[0])
check("SNAP 1 restores p0=12", cmd(s, "SNAP 1")[0].startswith("SNAP ok 1") and any("p=12," in l for l in cmd(s, "FXS", "FXS peak")))
check("SNAP 3 turns block off", cmd(s, "SNAP 3")[0].startswith("SNAP ok 3") and any("overdrive on=0" in l for l in cmd(s, "FXS", "FXS peak")))
check("scene name persisted", "SNAPNAME 3 'Clean Mute'" in cmd(s, "SNAPNAME", "SNAPNAME end"))
check("HPF persisted", cmd(s, "HPF")[0] == "HPF hz=140")
check("FSMODE layout persisted", "layout=custom" in cmd(s, "FSMODE")[0])
check("FSA persisted", "FSA 2 1 8 4 1" in cmd(s, "FSA", "FSA end"))

print("-- footswitch via FSDO --")
cmd(s, "FSMODE layout=scenes")
r = cmd(s, "FSDO 0 0")
check("FSDO short fs0 -> scene 0 empty (not saved)", r[-1] == "FSDO ok 0 0", str(r))
print("\n%s" % ("ALL OK" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
