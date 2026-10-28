#!/usr/bin/env python3
"""Closed-loop menu driver for a Proton run (PAD_FILE): presses buttons when the game's log shows a page.

A timed PAD_SCRIPT drifts when the boot is slower (trophy check, GPU shared with another run: 3 s seen), so this
watches work/proton/NAME/app/app_stdout.txt for the page names the game logs (coverscreen.gum, preintro.gum,
titlescreen.gum, mainmenu.gum, common/mainsettings.gum ...) and writes the next button into the PAD_FILE the
runtime polls ("mask [polls]", pressed once, emptied; third_party/ps3recomp cellPad.c).

  PAD_FILE=padin.txt tools/spu/run.sh NAME SECS ... &      # the run (cwd = work/proton/NAME/app)
  python3 tools/release/padloop.py NAME tools/release/pad/boot-to-menu.pad [more.pad ...]

Scenario lines (# comments):  wait PAGE [TIMEOUT]   block until a new log line contains PAGE (default timeout 60 s)
                              key X|O|UP|DOWN|LEFT|RIGHT|START|SQUARE|TRIANGLE|L1|R1 [POLLS]   press (default 40 polls)
                              sleep SECONDS         pause   ·   mark TEXT   write a line to the action log
The action log is work/proton/NAME/padloop.log: "<seconds since the run's stdout appeared> <action>", the same
clock as KZ3_VIDEO's times.txt (clip start offset = the KZ3_VIDEO start value), so frames can be found by action.
"""
import os, sys, time
MASK = dict(SELECT=1, L3=2, R3=4, START=8, UP=0x10, RIGHT=0x20, DOWN=0x40, LEFT=0x80, L2=0x100, R2=0x200, L1=0x400,
            R1=0x800, TRIANGLE=0x1000, O=0x2000, CIRCLE=0x2000, X=0x4000, CROSS=0x4000, SQUARE=0x8000)

def main():
    name = sys.argv[1]; steps = []
    for p in sys.argv[2:]:
        for ln in open(p):
            ln = ln.split('#')[0].strip()
            if ln: steps.append(ln.split(None, 2))
    run = os.path.join('work', 'proton', name, 'app'); out = os.path.join(run, 'app_stdout.txt')
    pf = os.path.join(run, 'padin.txt')
    while not os.path.exists(out): time.sleep(0.5)
    t0 = time.time(); pos = 0
    open(pf, 'w').close()
    log = open(os.path.join('work', 'proton', name, 'padloop.log'), 'w')
    def say(s):
        log.write('%.1f %s\n' % (time.time() - t0, s)); log.flush()
    def read_new():
        nonlocal pos
        with open(out, 'rb') as f:
            f.seek(pos); d = f.read(); pos += len(d)
        return d.decode('latin1')
    for st in steps:
        op = st[0]
        if op == 'wait':
            page = st[1]; to = float(st[2]) if len(st) > 2 else 60; t = time.time(); ok = False
            while time.time() - t < to:
                d = read_new(); i = d.find(page)
                if i >= 0:                       # consume only up to the match: a later page in the same chunk still counts
                    pos -= len(d.encode('latin1')) - len(d[:i + len(page)].encode('latin1')); ok = True; break
                time.sleep(0.3)
            say('wait %s %s' % (page, 'ok' if ok else 'TIMEOUT'))
            if not ok: return 1
        elif op == 'key':
            while os.path.getsize(pf) > 0: time.sleep(0.1)       # previous press not yet taken
            m = MASK[st[1].upper()]
            with open(pf, 'w') as f: f.write('0x%04x %s\n' % (m, st[2] if len(st) > 2 else 40))
            say('key %s' % st[1])
        elif op == 'sleep': time.sleep(float(st[1])); say('sleep %s' % st[1])
        elif op == 'mark': say('mark ' + ' '.join(st[1:]))
        else: print('bad step', st); return 2
    say('done'); return 0
sys.exit(main())
