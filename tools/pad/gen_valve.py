#!/usr/bin/env python3
"""Valve-section scripts for the Flooded Pyrrhus burning-gate valve (network pass 16, patches 4030/4031; run plan in
the RE notes s.15.6). All times are seconds on the pad clock (the first cellPadGetData), the one
PAD_SCRIPT, KZ3_KB_SCRIPT and KZ3_MOUSE_STICK_FEED share.

  python3 tools/pad/gen_valve.py --use T [--cycles N] [--combo-at S] -o PREFIX

writes PREFIX.pad (PAD_SCRIPT_FILE: Circle = Use at T to mount, then for the mouse run the R1 (GrabRightHand, the default
Mouse1 binding) holds, or for the combo run L3+R3 held) and PREFIX.feed (the KZ3_MOUSE_STICK_FEED string: counts per second for whole seconds, the grammar of KZ3_MOUSE_FEED).
  mouse run : PAD_SCRIPT_FILE=PREFIX.pad KZ3_MOUSE_STICK_FEED="$(cat PREFIX.feed)"
  combo run : PAD_SCRIPT_FILE=PREFIX-combo.pad  + keybinds.ini [motion] autocomplete = hold   (no feed)
The mouse gesture is the original one: stick to one side with the grab released, then the stick across with the grab
held (a +-35 degree hand limit makes the hand sweep 70 degrees per push), grab released; set A first (right push), set B
(mirrored) after N cycles, since which hand direction lowers the angle is not known statically (ASSUMPTION).
"""
import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--use", type=float, required=True, help="second of the Use press (Circle) that mounts the valve")
    ap.add_argument("--cycles", type=int, default=12, help="cycles per set (2 s each)")
    ap.add_argument("--combo-at", type=float, default=3.0, help="seconds after Use that L3+R3 starts (combo run)")
    ap.add_argument("-o", required=True)
    a = ap.parse_args()
    t0 = a.use + 2.0
    pad = ["# network pass 16: Use mounts the valve, R1 = GrabRightHand (Standard/Alternate scheme; the default Mouse1 binding)",
           f"{a.use:.2f}:0x2000/1.0   # CIRCLE: Use"]
    feed = []
    k = 0
    for sets in (1, -1):   # A: left, then right with the grab; B: mirrored
        for _ in range(a.cycles):
            b = t0 + 2.0 * k
            feed.append(f"{b:.2f}:{-200 * sets},0*1")          # stick to the first side, grab released (200 counts: 100 = full travel)
            feed.append(f"{b + 1.0:.2f}:{200 * sets},0*1")      # across, grab held
            pad.append(f"{b + 1.0:.2f}:0x0800/0.9")             # R1 held over the sweep, released before the next side
            k += 1
    open(a.o + ".pad", "w").write("\n".join(pad) + "\n")
    open(a.o + ".feed", "w").write(" ".join(feed) + "\n")
    combo = [pad[0], f"{a.use + a.combo_at:.2f}:0x0006/{max(10.0, 2.0 * k):.1f}   # L3 + R3 held (packet bits 1 and 2)"]
    open(a.o + "-combo.pad", "w").write("\n".join(combo) + "\n")
    print(f"{a.o}.pad {a.o}-combo.pad {a.o}.feed: {2 * a.cycles} cycles, last at {t0 + 2.0 * (k - 1):.1f} s")


main()
