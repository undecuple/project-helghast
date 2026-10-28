#!/usr/bin/env python3
"""Apply the PPU lift fixups on the player's machine.

The same work tools/lift.sh did inline in the old switch-114 branch (KZ3_EBOOT_VER=114; the branch is archived), as a step the first-run runner
can call with the build folder's paths:

    lift_fixups.py --fixups port/spu/ppu_lift_fixups-114.txt --functions analysis/functions.json \\
                   --out analysis/functions.fixed.json --jt-hints analysis/jt_hints.txt

Fixups file (one per line, '#' comments):
  end  FUNC NEW_END                        function boundary find_functions got wrong: the copy gets end = NEW_END
  jt   DISPATCHER BASE COUNT [abs|rel]     jump table the lifter's scan cannot resolve; the line minus "jt" goes to
                                           the hints file the lifter reads through PPU_JT_HINTS (ps3recomp patch 3004)
Output: --out (functions.json with the ends changed, same JSON shape), --jt-hints (always written, maybe empty), one
"fixup: func_XXXXXXXX end OLD -> NEW" line per change.

Stricter than lift.sh on purpose: on a player's machine a mismatch means the function list is not the one the
fixups were written for, so an "end" whose function is missing, a malformed line or an unknown kind fails (exit 1)
before the lift starts, with the line number. Exit 0 = written, 1 = a fixup does not apply, 2 = usage.
"""
import argparse
import json
import os
import sys


class FixupError(Exception):
    pass


def parse(path):
    """-> (ends {start: new_end}, hints [str]); raises FixupError on a malformed line."""
    ends, hints = {}, []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            w = line.split()
            try:
                if w[0] == "end" and len(w) == 3:
                    ends[int(w[1], 0)] = int(w[2], 0)
                elif w[0] == "jt" and len(w) in (4, 5) and (len(w) == 4 or w[4] in ("abs", "rel")):
                    int(w[1], 0), int(w[2], 0), int(w[3], 0)
                    hints.append(" ".join(w[1:]))
                else:
                    raise FixupError(f"{path}:{n}: expected 'end FUNC NEW_END' or 'jt DISPATCHER BASE COUNT [abs|rel]'")
            except ValueError:
                raise FixupError(f"{path}:{n}: not a number in {line!r}") from None
    return ends, hints


def apply(functions, ends):
    """Change the ends in place; -> report lines. Raises FixupError for a start not in the list."""
    by_start = {}
    for f in functions:
        by_start.setdefault(int(f["start"], 16), f)
    missing = sorted(a for a in ends if a not in by_start)
    if missing:
        raise FixupError("no function starts at " + ", ".join(f"0x{a:08X}" for a in missing)
                         + " (the function list is not the one these fixups were written for)")
    out = []
    for a in sorted(ends):
        f = by_start[a]
        new = f"0x{ends[a]:08X}"
        out.append(f"fixup: func_{a:08X} end {f['end']} -> {new}")
        f["end"] = new
    return out


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fixups", required=True)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jt-hints", required=True)
    a = ap.parse_args(argv)
    try:
        ends, hints = parse(a.fixups)
        with open(a.functions, encoding="utf-8") as f:
            functions = json.load(f)
        report = apply(functions, ends)
    except (FixupError, OSError, ValueError, KeyError) as e:
        print(f"lift fixups not applied: {e}", file=out)
        return 1
    for d in (os.path.dirname(os.path.abspath(p)) for p in (a.out, a.jt_hints)):
        os.makedirs(d, exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(functions, f)
    with open(a.jt_hints, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(h + "\n" for h in hints))
    for r in report:
        print("  " + r, file=out)
    print(f"lift fixups: {len(report)} end(s), {len(hints)} jump-table hint(s) from {os.path.basename(a.fixups)}",
          file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
