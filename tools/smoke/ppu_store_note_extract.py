#!/usr/bin/env python3
"""Patch 2082 host test helper: copy the patch-2082 blocks out of the patched toolkit so the test compiles the real
code. Usage: ppu_store_note_extract.py PATCHED_DIR OUT_DIR -> OUT_DIR/{gate,hook,lifter}.inc"""
import sys
from pathlib import Path

BEGIN, END = "/* ---- KZ3 patch 2082 begin", "/* ---- KZ3 patch 2082 end ---- */"


def blocks(text):
    out, pos = [], 0
    while (b := text.find(BEGIN, pos)) >= 0:
        e = text.index(END, b)
        out.append(text[b:e + len(END)] + "\n")
        pos = e
    return out


def main():
    pt, od = Path(sys.argv[1]), Path(sys.argv[2])
    loader = blocks((pt / "runtime/ppu/ppu_loader.cpp").read_text())
    lifter = blocks((pt / "tools/ppu_lifter.py").read_text())
    if len(loader) != 2 or len(lifter) != 1:
        sys.exit(f"expected 2 loader blocks and 1 lifter block, found {len(loader)} and {len(lifter)}")
    (od / "gate.inc").write_text(loader[0])
    (od / "hook.inc").write_text(loader[1])
    # The lifter block sits in a Python string: '\\' + newline there is a C line continuation.
    (od / "lifter.inc").write_text(lifter[0].replace("\\\\\n", "\\\n"))


if __name__ == "__main__":
    main()
