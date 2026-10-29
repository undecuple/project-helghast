#!/usr/bin/env python3
"""Resolve patch 1033's host profile (`KZ3_PROF=FROM,TO`, needs KZ3_WAIT_ACCT) to function names.

  tools/spu/prof_resolve.py LOG EXE [--top N] [--job ID] [--nm NM]

LOG is the run's app_stdout.txt with the `[prof] rva=... job=... n=...` dump; EXE the kz3.exe that ran
(unstripped MinGW build). Symbols come from `nm` (MinGW's x86_64-w64-mingw32-nm or binutils nm, both
read PE). Prints the share of samples per resident job module (ovlN as in the wacct lines), then the top
functions overall, and for --job the top functions while that module was resident.

Patch 1115: entries carry "thr=K" (and "caller=0xRVA" under KZ3_PROF_UNWIND); "[prof] thr K tries=N
NAME" names the threads (KZ3_PROF_ALL samples the unnamed ones round robin: tries = how often each was sampled).
  --threads        per thread and per class (PPU, SPU, RSX, other): busy samples (in the exe) normalized by tries
  --tls F1,F2,...  share of the TLS functions (default the MinGW emutls/mcfgthread ones) per class, and their
                   callers (caller=) per class
  --unwind-rvas    print the KZ3_PROF_UNWIND value for --tls's functions in EXE and stop (LOG is ignored)
"""
import argparse
import bisect
import re
import shutil
import struct
import subprocess
from collections import Counter, defaultdict

DONE = re.compile(r"\[prof\] done .*samples=(\d+) miss=(\d+)")
ENT = re.compile(r"\[prof\] (rva|ext)=0x([0-9a-f]+) job=(-?\d+) n=(\d+)(?: thr=(\d+))?(?: caller=(0x[0-9a-f]+|ext))?")
THR = re.compile(r"\[prof\] thr (\d+)(?: tries=(\d+))? (.*)$")
TLS_DEFAULT = "__emutls_get_address,_MCF_thread_self,__MCF_tls_table_get"


def thread_class(name, top):
    """PPU / SPU / RSX / other from the thread's name, else from its hottest function in the exe."""
    n = name.lower()
    if "ppu" in n:
        return "PPU"
    if "spurs" in n or "pm-worker" in n or "spu" in n:
        return "SPU"
    if "rsx" in n or "gcm" in n:
        return "RSX"
    t = (top or "").lower()
    if t.startswith("_z13func_") or t.startswith("func_"):
        return "PPU"
    if "rsx" in t or "gcm" in t or "d3d12" in t:
        return "RSX"
    if t.startswith("spu_") or "spu_func" in t or "job_" in t:
        return "SPU"
    return "other"


def image_base(exe):
    with open(exe, "rb") as f:
        d = f.read(4096)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    magic = struct.unpack_from("<H", d, pe + 24)[0]
    return struct.unpack_from("<Q", d, pe + 24 + 24)[0] if magic == 0x20B else struct.unpack_from("<I", d, pe + 24 + 28)[0]


def symbols(exe, nm):
    out = subprocess.run([nm, "--defined-only", exe], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "Tt":
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("exe")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--job", type=int, action="append", default=[])
    ap.add_argument("--nm", default=shutil.which("x86_64-w64-mingw32-nm") or "nm")
    ap.add_argument("--threads", action="store_true")
    ap.add_argument("--tls", nargs="?", const=TLS_DEFAULT, default=None)
    ap.add_argument("--unwind-rvas", action="store_true")
    a = ap.parse_args()
    base = image_base(a.exe)
    addrs, names = symbols(a.exe, a.nm)
    if a.unwind_rvas:
        want = (a.tls or TLS_DEFAULT).split(",")
        print(",".join(hex(addrs[names.index(w)] - base) for w in want if w in names))
        return

    def fn(rva):
        i = bisect.bisect_right(addrs, base + rva) - 1
        return names[i] if i >= 0 else "?"

    total = 0
    by_fn, by_job = Counter(), Counter()
    by_job_fn = defaultdict(Counter)
    thr_names, thr_tries = {}, {}
    by_thr_fn = defaultdict(Counter)            # thr -> function -> samples
    tls_callers = defaultdict(Counter)          # thr -> caller function -> samples
    hdr = ""
    with open(a.log, errors="replace") as f:
        for line in f:
            if "[prof]" not in line:
                continue
            m = DONE.search(line)
            if m:
                hdr = line.strip()
                continue
            m = THR.search(line)
            if m:
                thr_names[int(m.group(1))] = m.group(3).strip()
                if m.group(2):
                    thr_tries[int(m.group(1))] = int(m.group(2))
                continue
            m = ENT.search(line)
            if not m:
                continue
            kind, v, job, n = m.group(1), int(m.group(2), 16), int(m.group(3)), int(m.group(4))
            name = fn(v) if kind == "rva" else "<outside exe>"
            total += n
            by_fn[name] += n
            by_job[job] += n
            by_job_fn[job][name] += n
            thr = int(m.group(5)) if m.group(5) else -1
            by_thr_fn[thr][name] += n
            if m.group(6):
                tls_callers[thr][fn(int(m.group(6), 16)) if m.group(6) != "ext" else "<outside exe>"] += n
    if not total:
        print("no [prof] samples")
        return
    print(hdr)
    print(f"samples {total}")
    print("-- by resident job module (0 = none: policy module / kernel)")
    for job, n in by_job.most_common(15):
        print(f"  ovl{job:<5} {n:7d} {100.0 * n / total:5.1f}%")
    print(f"-- top {a.top} functions")
    for name, n in by_fn.most_common(a.top):
        print(f"  {n:7d} {100.0 * n / total:5.1f}%  {name}")
    if a.threads or a.tls:
        ticks = max(thr_tries.values()) if thr_tries else 1
        tls = set((a.tls or TLS_DEFAULT).split(","))
        cls_busy, cls_tls, cls_call = Counter(), Counter(), defaultdict(Counter)
        rows = []
        for thr, c in by_thr_fn.items():
            w = ticks / max(1, thr_tries.get(thr, ticks))            # round-robin weight
            busy = sum(v for k, v in c.items() if k != "<outside exe>")
            top = next((k for k, _ in c.most_common() if k != "<outside exe>"), "")
            cl = thread_class(thr_names.get(thr, ""), top)
            t = sum(v for k, v in c.items() if k in tls)
            cls_busy[cl] += busy * w; cls_tls[cl] += t * w
            for k, v in tls_callers[thr].items():
                cls_call[cl][k] += v * w
            rows.append((busy * w, thr, cl, thr_names.get(thr, "?"), sum(c.values()), t, top))
        allb = sum(cls_busy.values()) or 1
        print(f"-- by class (busy = samples in the exe, x ticks/tries; {ticks} ticks)")
        for cl in ("PPU", "SPU", "RSX", "other"):
            b = cls_busy[cl]
            print(f"  {cl:5} busy {b:9.0f} {100.0 * b / allb:5.1f}%  tls {cls_tls[cl]:8.0f} = {100.0 * cls_tls[cl] / (b or 1):4.1f}% of its busy, "
                  f"{100.0 * cls_tls[cl] / allb:4.2f}% of all busy")
        if a.threads:
            print("-- threads (weighted busy, thr, class, name, raw samples, tls samples, top function)")
            for r in sorted(rows, reverse=True)[:a.top]:
                print(f"  {r[0]:9.0f} t{r[1]:<4} {r[2]:5} {r[3][:36]:36} raw {r[4]:7d} tls {r[5]:6d}  {r[6][:60]}")
        if a.tls:
            for cl in ("PPU", "SPU", "RSX", "other"):
                tot = sum(cls_call[cl].values())
                if not tot:
                    continue
                print(f"-- {cl}: TLS callers ({tot:.0f} weighted samples with a caller)")
                for k, v in cls_call[cl].most_common(a.top):
                    print(f"  {v:8.0f} {100.0 * v / tot:5.1f}%  {k[:90]}")
    for job in a.job:
        t = sum(by_job_fn[job].values()) or 1
        print(f"-- ovl{job}: {t} samples, top {a.top}")
        for name, n in by_job_fn[job].most_common(a.top):
            print(f"  {n:7d} {100.0 * n / t:5.1f}%  {name}")


if __name__ == "__main__":
    main()
