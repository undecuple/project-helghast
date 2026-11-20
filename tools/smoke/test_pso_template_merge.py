#!/usr/bin/env python3
"""Host test for the deterministic v2 template merge (pso_template.py merge): no game data, no GPU.
  python3 tools/smoke/test_pso_template_merge.py
Checks: merge in two input orders gives identical bytes; the same without normalisation (negative control) differs;
a clash in a pipeline field still fails; alpha_ref is zeroed and the clash minimum vp_start kept; a singleton keeps vp_start."""
import contextlib
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pso_template as P  # noqa: E402

FAILS = 0


def check(ok, what):
    global FAILS
    print(("ok   " if ok else "FAIL ") + what)
    FAILS += 0 if ok else 1


def rec(key, vp_start=0, alpha=(0, 0), **kw):
    t = list(P.REC.unpack(bytes(P.REC.size)))
    t[0] = key
    t[P.NAMES.index("vp_hash")] = 0x1111 * key
    t[P.NAMES.index("vp_start")] = vp_start
    t[P.RS0 + 2], t[P.RS0 + 3] = alpha
    for n, v in kw.items():
        t[P.NAMES.index(n) if n in P.NAMES else P.RS0 + int(n[2:])] = v
    return P.REC.pack(*t)


def wr(path, recs, flags=P.F_BUFFERED):
    P.write(path, flags, recs)


def run_merge(out, ins, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return P.merge(out, ins, **kw)


def main():
    with tempfile.TemporaryDirectory() as d:
        a, b, c = (os.path.join(d, n) for n in ("a.bin", "b.bin", "c.bin"))
        wr(a, [rec(5, 9, (0x80, 1)), rec(1, 0), rec(3, 40)])
        wr(b, [rec(5, 4, (0x20, 2)), rec(2, 7, (3, 3)), rec(3, 40)])
        wr(c, [rec(5, 6, (0, 0)), rec(1, 2)])
        o1, o2, o3 = (os.path.join(d, n) for n in ("o1", "o2", "o3"))
        run_merge(o1, [a, b, c])
        run_merge(o2, [c, b, a])
        s1, s2 = open(o1, "rb").read(), open(o2, "rb").read()
        check(s1 == s2, "merge in two input orders gives identical bytes")
        _, recs = P.read(o1, strict=True)
        f = {P.key_of(r): P.REC.unpack(r) for r in recs}
        check(list(f) == [1, 2, 3, 5], "records sorted by key")
        check(f[5][P.VP_START] == 4 and f[1][P.VP_START] == 0 and f[3][P.VP_START] == 40 and f[2][P.VP_START] == 7,
              "clash keeps minimum vp_start, singletons keep theirs")
        check(all(t[P.RS0 + 2] == 0 and t[P.RS0 + 3] == 0 for t in f.values()), "alpha_ref_raw/_format zeroed")
        # negative control: without normalisation the first file wins and the bytes depend on the order
        n1, n2 = os.path.join(d, "n1"), os.path.join(d, "n2")
        run_merge(n1, [a, b, c], normalise_fields=False)
        run_merge(n2, [c, b, a], normalise_fields=False)
        check(open(n1, "rb").read() != open(n2, "rb").read(), "negative control: without normalisation the bytes differ")
        # a pipeline-field clash still fails and writes nothing
        bad = os.path.join(d, "bad.bin")
        wr(bad, [rec(5, 4, rs14=7)])
        try:
            run_merge(o3, [a, bad])
            check(False, "pipeline-field clash (render state word) fails")
        except ValueError as e:
            check("rs[14]" in str(e) and not os.path.exists(o3), f"pipeline-field clash (render state word) fails: {e}")
        bad2 = os.path.join(d, "bad2.bin")
        wr(bad2, [rec(5, 4, vp_branch_bits=1)])
        try:
            run_merge(o3, [a, bad2])
            check(False, "pipeline-field clash (vp_branch_bits) fails")
        except ValueError as e:
            check("vp_branch_bits" in str(e), "pipeline-field clash (vp_branch_bits) fails")
        # a non-buffered input keeps its alpha_ref (it is render state there)
        nb = os.path.join(d, "nb.bin")
        wr(nb, [rec(8, 0, (0x55, 1))], flags=0)
        run_merge(o3, [nb])
        _, r3 = P.read(o3)
        check(P.REC.unpack(r3[0])[P.RS0 + 2] == 0x55, "non-buffered template keeps alpha_ref")
    print("FAILED" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
