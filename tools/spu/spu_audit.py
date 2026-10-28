#!/usr/bin/env python3
"""Audit the SPU lift: unhandled markers per mnemonic, `.word` sites split into reachable/unreachable,
instruction histogram across all KZ3 SPU code, per-image table.

    spu_audit.py work/spu/spu_lift.json [--md]

Input is written by tools/spu/lift_spu.py. `--md` prints the per-image table as Markdown (for the RE notes).
"""
import json, struct, sys


def fclass(raw):
    """Rough guess at what a non-instruction word is."""
    if raw == 0:
        return "zero"
    b = struct.pack(">I", raw)
    if all(32 <= c < 127 or c in (0, 9, 10) for c in b) and sum(32 <= c < 127 for c in b) >= 2:
        return "ascii"
    f = struct.unpack(">f", struct.pack(">I", raw))[0]
    if 1e-6 < abs(f) < 1e7:
        return "float"
    if raw < 0x40000:
        return "small int / LS address"
    return "other"


def main():
    R = json.load(open(sys.argv[1]))
    md = "--md" in sys.argv
    if md:
        print("| Image | Kind | Bytes | Code end | Functions | Instructions | `.word` (reachable) |")
        print("|---|---|---|---|---|---|---|")
        for r in R:
            w = r.get("words", [])
            kind = r["kind"]
            if r.get("wraps_jobmod"):
                kind += " (job module inside)"
            if r.get("inside_elf"):
                kind += " (= audio ELF payload)"
            ce = f"{r['code_end']:#x}" if r.get("code_end") else "-"
            print(f"| `{r['name']}` | {kind} | {r['size']:,} | {ce} | {r.get('functions', 0):,} | "
                  f"{r.get('insns', 0):,} | {len(w)} ({sum(1 for x in w if x[3])}) |")
        return
    unsup, hist = {}, {}
    for r in R:
        for k, v in r.get("unsupported", {}).items():
            unsup[k] = unsup.get(k, 0) + v
        for k, v in r.get("hist", {}).items():
            hist[k] = hist.get(k, 0) + v
    words = [(r["name"],) + tuple(w) for r in R for w in r.get("words", [])]   # name, addr, raw, func, reach, ref
    reach = [w for w in words if w[4]]
    print(f"images: {len(R)} ({sum(r['kind'] == 'elf' for r in R)} ELF, {sum(r['kind'] == 'job' for r in R)} job modules)"
          f"  failed: {[r['name'] for r in R if not r['ok']]}")
    print(f"functions lifted: {sum(r.get('functions', 0) for r in R):,}  instructions lifted: {sum(hist.values()):,}")
    print("unsupported markers per mnemonic: " +
          (", ".join(f"{k} {v}" for k, v in sorted(unsup.items(), key=lambda kv: -kv[1])) or "none"))
    print(f"`.word` sites: {len(words)}; reachable from a function start by fall-through/direct branch: {len(reach)}")
    cls = {}
    for w in words:
        c = fclass(w[2])
        cls[c] = cls.get(c, 0) + 1
    print("  value classes: " + ", ".join(f"{k} {v}" for k, v in sorted(cls.items(), key=lambda kv: -kv[1])))
    # Data evidence: ASCII text, or an ila/lqr/lqa/stqr target within 0x200 bytes before the word (a constant
    # pool or table the code points at). A reachable word with neither needs a look by hand.
    ref = [w for w in words if w[5]]
    print(f"  in a table the code references (ila/lqr/lqa target <= 0x200 before): {len(ref)}")
    odd = [w for w in reach if fclass(w[2]) != "ascii" and not w[5]]
    print(f"  reachable, not ASCII, not near a data reference: {len(odd)}")
    for w in odd:
        print(f"    {w[0]} @{w[1]:#x} = {w[2]:#010x} [{fclass(w[2])}] (function {w[3]:#x})")
    print("\n== instruction histogram (all lifted code, top 80)")
    tot = sum(hist.values())
    for i, (k, v) in enumerate(sorted(hist.items(), key=lambda kv: -kv[1])[:80]):
        print(f"{i + 1:3} {k:10} {v:8,} {100 * v / tot:5.2f}%")
    print(f"distinct mnemonics: {len(hist)}")
    print("all: " + " ".join(f"{k}:{v}" for k, v in sorted(hist.items(), key=lambda kv: -kv[1])))
    print("\n== per-image")
    for r in R:
        w = r.get("words", [])
        print(f"{r['name']:14} {r['kind']:3} bytes={r['size']:7} code_end={r.get('code_end', 0):#7x} "
              f"funcs={r.get('functions', 0):5} insns={r.get('insns', 0):6} word={len(w):4} "
              f"reach={sum(1 for x in w if x[3]):3}")


if __name__ == "__main__":
    main()
