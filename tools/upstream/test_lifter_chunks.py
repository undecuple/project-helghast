#!/usr/bin/env python3
"""Focused test for ps3recomp patch 3005 (PPU lifter: address-keyed chunks, changed files only, ppu_overrides.h).

Runs the output stage of the lifter in the tree under test (default work/ps3recomp-patched; NEW_TREE overrides) on
200 synthetic functions and checks, without a compiler:
  1. --chunks N writes N chunk files plus ppu_recomp_table; every function is defined exactly once; the table lists all.
  2. ppu_recomp.h has no PPU_OVERRIDE_ line; ppu_overrides.h declares <name>__lifted and defines PPU_OVERRIDE_<addr>
     for each override; the chunk defines <name>__lifted for an overridden function.
  3. A second run with one function's body changed rewrites exactly that function's chunk; every other file keeps its
     content and mtime (write_if_changed). Adding one override rewrites that function's chunk and ppu_overrides.h
     only; ppu_recomp.h is untouched.
  4. The chunk assignment is a function of the address list alone: the same functions emitted with longer bodies
     land in the same files.
The upstream tree (third_party/ps3recomp) has no --chunks, so it is not run. Prints one PASS/FAIL line."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

_, new_t = X.trees()
sys.path.insert(0, os.path.join(new_t, "tools"))
import ppu_lifter  # noqa: E402

N_CHUNKS = 8


def make_lifter(bodies=None, overrides=(), long=False):
    lf = ppu_lifter.PPULifter()
    lf.header_name = "ppu_recomp.h"
    addr = 0x10000
    for i in range(200):
        size = 0x20 + (i % 7) * 0x40          # uneven guest sizes, like a real image
        body = [f"ctx->gpr[3] = {i};"] + ([f"ctx->gpr[{k}] = 0;" for k in range(4, 20)] if long else [])
        if bodies and i in bodies:
            body = bodies[i]
        lf.functions.append(ppu_lifter.LiftedFunction(name=f"func_{addr:08X}", start_addr=addr, end_addr=addr + size,
                                                      body_lines=body + ["return;"]))
        addr += size
    lf.overrides = {lf.functions[i].start_addr for i in overrides}
    return lf


def write(lf, out):
    paths = lf.write_source_files(out, base="ppu_recomp", chunks=N_CHUNKS)
    lf.write_if_changed(os.path.join(out, "ppu_recomp.h"), lf.emit_header())
    lf.write_if_changed(os.path.join(out, "ppu_overrides.h"), lf.emit_overrides_header())
    return paths


def snapshot(out):
    return {f: (os.stat(os.path.join(out, f)).st_mtime_ns, open(os.path.join(out, f)).read())
            for f in sorted(os.listdir(out))}


def chunk_of(snap, name):
    return [f for f, (_, txt) in snap.items() if f.startswith("ppu_recomp_") and f"void {name}(" in txt or
            f.startswith("ppu_recomp_") and f"void {name}__lifted(" in txt]


ok = True
def check(cond, what):
    global ok
    print(("ok   " if cond else "FAIL ") + what)
    ok = ok and bool(cond)


with tempfile.TemporaryDirectory() as out:
    lf = make_lifter(overrides=(5,))
    paths = write(lf, out)
    s1 = snapshot(out)
    chunks = [f for f in s1 if f.startswith("ppu_recomp_") and f != "ppu_recomp_table.cpp"]
    check(len(chunks) == N_CHUNKS and "ppu_recomp_table.cpp" in s1, f"{N_CHUNKS} chunks + table written ({len(paths)} paths)")
    defs = sum(txt.count("\nvoid func_") for f, (_, txt) in s1.items() if f in chunks)
    check(defs == 200, f"every function defined once ({defs})")
    table = s1["ppu_recomp_table.cpp"][1]
    check(table.count("ULL, func_") == 200 and "function_table_count = 200" in table, "table lists all 200")
    f5 = lf.functions[5].name
    check("PPU_OVERRIDE_" not in s1["ppu_recomp.h"][1] and f"void {f5}(ppu_context* ctx);" in s1["ppu_recomp.h"][1],
          "ppu_recomp.h: plain declarations, no override lines")
    ov = s1["ppu_overrides.h"][1]
    check(f"void {f5}__lifted(ppu_context* ctx);" in ov and f"#define PPU_OVERRIDE_{lf.functions[5].start_addr:08X} 1" in ov
          and ov.count("PPU_OVERRIDE_") == 1, "ppu_overrides.h announces the one override")
    check(len(chunk_of(s1, f5)) == 1 and f"void {f5}__lifted(" in s1[chunk_of(s1, f5)[0]][1], "chunk defines the __lifted body")

    # Relift with one body changed: one chunk rewritten, the rest untouched (content and mtime).
    os.utime  # noqa (mtime resolution: ns, so an unchanged file is detectable even within the same second)
    lf2 = make_lifter(bodies={120: ["ctx->gpr[3] = 999;", "ctx->gpr[4] = 1;"]}, overrides=(5,))
    write(lf2, out)
    s2 = snapshot(out)
    changed = [f for f in s2 if s2[f] != s1.get(f)]
    target = chunk_of(s1, lf.functions[120].name)
    check(changed == target, f"one body change rewrites only its chunk: changed={changed}, expected={target}")

    # Add an override: its chunk and ppu_overrides.h change, ppu_recomp.h and the table do not.
    lf3 = make_lifter(bodies={120: ["ctx->gpr[3] = 999;", "ctx->gpr[4] = 1;"]}, overrides=(5, 77))
    write(lf3, out)
    s3 = snapshot(out)
    changed = sorted(f for f in s3 if s3[f] != s2.get(f))
    target = sorted(chunk_of(s2, lf.functions[77].name) + ["ppu_overrides.h"])
    check(changed == target, f"one new override rewrites its chunk + ppu_overrides.h: changed={changed}")
    check(s3["ppu_recomp.h"][0] == s1["ppu_recomp.h"][0], "ppu_recomp.h mtime unchanged across both relifts")

    # Stability: longer bodies everywhere, same chunk membership.
    with tempfile.TemporaryDirectory() as out2:
        write(make_lifter(long=True, overrides=(5,)), out2)
        s4 = snapshot(out2)
        same = all(chunk_of(s4, f.name) == chunk_of(s1, f.name) for f in lf.functions)
        check(same, "chunk membership depends on addresses only (longer bodies, same files)")

    # Stale removal: a file from an older cut is not part of the set (the lifter's main() removes it; here only
    # the returned path set is checked).
    check(all(os.path.basename(p) in s3 for p in paths), "returned paths are the files on disk")

print(("PASS" if ok else "FAIL") + " 3005 PPU lifter chunking")
sys.exit(0 if ok else 1)
