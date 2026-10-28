#!/usr/bin/env python3
"""size estimate: replay a real app_stdout.txt through the router table of port/kz3/kz3_log.c (parsed from the
source, so the estimate always matches the shipped table) and report what the default (info) log would hold.
    test_kz3_log_size.py LOG [--seconds S] [--level info] [--top 12]
Duration: --seconds, else the last "[flip] N at T ms" line. Prefix overhead per emitted line: 18 bytes
("%9llu %5lu L "). Lines garbled by today's unsynchronised stdout/stderr writes (two prints interleaved character by character:
"[[mRfScX- lniusltl]]", "elems", "ist] RESUME-ELEMS") cannot be routed. They are counted apart: a garbled tag, an
untagged line that is not a continuation, or a tagged line whose text holds a '[' (a second line's tag mixed in).
The logger writes whole lines, so these vanish; the bound with them counted as info is printed too.
Exit 1 if the default log exceeds 1 MB per 10 minutes (the target)."""
import argparse, collections, re, sys

LV = {"KZ3_LOG_ERROR": 0, "KZ3_LOG_WARN": 1, "KZ3_LOG_INFO": 2, "KZ3_LOG_DEBUG": 3, "KZ3_LOG_TRACE": 4,
      "E": 0, "W": 1, "I": 2, "D": 3, "T": 4}

def load_routes(src):
    body = src[src.index("k_routes[] = {"):]
    body = body[:body.index("};")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out = []
    for m in re.finditer(r'\bR\("([^"]+)",\s*(\w)\)|\bRW\("([^"]+)",\s*"([^"]*)",\s*(\w)\)', body):
        if m.group(1): out.append((m.group(1), "", LV[m.group(2)]))
        else: out.append((m.group(3), m.group(4), LV[m.group(5)]))
    return out

def route(routes, tag, rest):
    for t, w, l in routes:
        if t.endswith("*"):
            if not tag.startswith(t[:-1]): continue
        elif t != tag: continue
        if w and not rest.startswith(w): continue
        return l
    return 2

TAG = re.compile(rb"^\[([\x20-\x5a\x5c\x5e-\x7e]{1,31})\] ?")
def continuation(l):
    if not l: return False
    c = l[0:1]
    if c in (b" ", b"\t") or c.isdigit(): return True
    if not c.isalpha() and c != b"[": return True
    m = re.match(rb"[0-9A-Fa-f]{4,}[ :]", l)
    return bool(m)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log"); ap.add_argument("--seconds", type=float); ap.add_argument("--level", default="info")
    ap.add_argument("--top", type=int, default=12); ap.add_argument("--src", default="port/kz3/kz3_log.c")
    a = ap.parse_args()
    thr = {"error": 0, "warn": 1, "info": 2, "debug": 3, "trace": 4}[a.level]
    routes = load_routes(open(a.src).read())
    tot = kept = lines = kl = garbled = gb = 0; last = (b"", 2); last_ms = 0
    per = collections.Counter(); perl = collections.Counter()
    flip = re.compile(rb"^\[flip\] \d+ at (\d+) ms")
    known = {t.rstrip("*") for t, _, _ in routes}
    with open(a.log, "rb") as f:
        for raw in f:
            l = raw.rstrip(b"\r\n"); lines += 1; tot += len(raw)
            if not l: continue
            m = TAG.match(l)
            if m:
                tag = m.group(1); rest = l[m.end():].decode("latin1")
                ts = tag.decode("latin1")
                if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_ -]*", ts) or (" " in ts and not any(ts.startswith(k) for k in known)):
                    garbled += 1; gb += len(raw) + 18; continue     # interleaved write, e.g. "[R[SmXf cn-ullils]t ]"
                elif "[" in rest:
                    garbled += 1; gb += len(raw) + 18; continue
                else:
                    lvl = route(routes, ts, rest); last = (tag, lvl)
            elif continuation(l): tag, lvl = last
            else: garbled += 1; gb += len(raw) + 18; continue
            fm = flip.match(l)
            if fm: last_ms = max(last_ms, int(fm.group(1)))
            if lvl <= thr:
                n = len(l) + 1 + 18; kept += n; kl += 1
                per[tag] += n; perl[tag] += 1
    secs = a.seconds or last_ms / 1000.0 or 1.0
    scale = 600.0 / secs
    print(f"input {lines} lines {tot/1e6:.1f} MB over {secs:.0f} s ({a.log.split('/')[-4] if a.log.count('/')>3 else a.log})")
    print(f"default '{a.level}' log: {kl} lines {kept/1e6:.3f} MB -> {kept*scale/1e6:.3f} MB per 10 min "
          f"({kl*scale:.0f} lines)")
    print(f"garbled (interleaved) lines {garbled}, {gb*scale/1e6:.3f} MB per 10 min; upper bound with them: "
          f"{(kept+gb)*scale/1e6:.3f} MB per 10 min")
    for t, n in per.most_common(a.top):
        print(f"  {perl[t]*scale:9.0f} lines {n*scale/1e3:8.1f} kB/10min  [{t.decode('latin1') or '(untagged)'}]")
    return 1 if (a.level == "info" and kept * scale > 1e6) else 0

if __name__ == "__main__":
    sys.exit(main())
