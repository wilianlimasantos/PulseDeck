#!/usr/bin/env python3
"""Interactive ALNK sweep: for each (CLK_CON2[11:8], CON1, CON0, CON3) combo, send AUDIO SET ... + AUDIO TONE and ask what you hear.
Usage: audio_sweep.py [/dev/ttyACM0]   Answers: c = clean 440 Hz tone, n = noise/silence, p = pitch/odd, s = skip, q = quit.
Writes build/audio_sweep.json. UNTESTED on hardware."""
import itertools, json, os, sys, time
import serial

def cmd(ser, line, wait=0.25):
    ser.reset_input_buffer(); ser.write((line + "\r\n").encode()); time.sleep(wait)
    return ser.read(ser.in_waiting or 1).decode(errors="replace").strip().splitlines()[-1:] or [""]

def combos():
    for clk, con1, con0, con3 in itertools.product([1, 0, 2, 3, 4, 5, 8], ["d5", "11d5"], ["180", "1c0", "100"], ["83", "03"]):
        yield clk, con1, con0, con3

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    ser = serial.Serial(port, 115200, timeout=0.3); res = []
    print(cmd(ser, "PING")); print(cmd(ser, "AUDIO OFF"))
    for clk, con1, con0, con3 in combos():
        for nm, v in (("CLK", "%x" % clk), ("CON1", con1), ("CON0", con0), ("CON3", con3)):
            r = cmd(ser, "AUDIO SET %s %s" % (nm, v), 0.05)
        print(cmd(ser, "AUDIO TONE", 0.5)[0], cmd(ser, "AUDIO STAT", 0.2)[0])
        a = input("CLK=%x CON1=%s CON0=%s CON3=%s  [c/n/p/s/q] > " % (clk, con1, con0, con3)).strip().lower()
        cmd(ser, "AUDIO OFF", 0.1)
        if a == "q": break
        if a != "s": res.append(dict(clk=clk, con1=con1, con0=con0, con3=con3, heard=a))
        if a == "c": print("CLEAN TONE with", res[-1]); break
    os.makedirs("build", exist_ok=True); json.dump(res, open("build/audio_sweep.json", "w"), indent=1); print("saved build/audio_sweep.json")

if __name__ == "__main__":
    main()
