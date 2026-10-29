#!/usr/bin/env python3
"""Per-gate native call counts by 10-second window, split into load phase and steady state (SPU pass 125).

    python3 tools/spu/native_counts.py LOG [--split SECONDS]

The exe (KZ3_NATIVE_JOB_* on) prints, at every power of two and at exit, one line per gate slot:
    [native] slot 12 job_0116d380 0x047F0 calls=N fallback=M win10s(window:calls[/fallback]):  3:120 4:2000/1 ...
(window w covers seconds 10w..10w+10 since the process start). This reads the last line of each slot and prints the
calls before and from --split (default: the first window in which slot 12 or 14, the steady-state blend, is called,
else no split). Output: slot, total, load calls, steady calls, steady calls per second, fallbacks.
"""
import re, argparse

WIN = 10
LINE = re.compile(r"\[native\] slot (\d+) (\S+) 0x([0-9A-Fa-f]+) calls=(\d+) fallback=(\d+) win(\d+)s\(window:calls\[/fallback\]\):(.*)$")


def parse(text):
    last = {}
    for ln in text.splitlines():
        m = LINE.search(ln)
        if not m:
            continue
        slot = int(m.group(1))
        wins = {}
        for tok in m.group(7).split():
            w, _, rest = tok.partition(":")
            c, _, f = rest.partition("/")
            wins[int(w)] = (int(c), int(f or 0))
        last[slot] = dict(image=m.group(2), addr=int(m.group(3), 16), calls=int(m.group(4)), fb=int(m.group(5)),
                          win=int(m.group(6)), wins=wins)
    return last


def split_report(last, split_s=None):
    if split_s is None:
        for s in (12, 14):
            if s in last and last[s]["wins"]:
                split_s = min(last[s]["wins"]) * WIN
                break
    rows = []
    for slot in sorted(last):
        d = last[slot]
        sw = None if split_s is None else split_s // WIN
        load = sum(c for w, (c, f) in d["wins"].items() if sw is not None and w < sw)
        steady = sum(c for w, (c, f) in d["wins"].items() if sw is None or w >= sw)
        nw = len([w for w in d["wins"] if sw is None or w >= sw])
        rows.append((slot, d["image"], d["addr"], d["calls"], load, steady, nw, d["fb"]))
    return split_s, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--split", type=float, default=None)
    a = ap.parse_args()
    last = parse(open(a.log, errors="replace").read())
    s, rows = split_report(last, a.split)
    print("split at %s s" % ("-" if s is None else int(s)))
    print("slot image addr total load steady steady_windows fallback")
    for r in rows:
        print("%d %s 0x%X %d %d %d %d %d" % r)


if __name__ == "__main__":
    main()
