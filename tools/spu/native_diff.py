#!/usr/bin/env python3
"""Native gate vs lifted function (SPU pass 122; KZ2's native_diff.py design).

    nix develop .#re --command tools/sandbox.sh -- python3 tools/spu/native_diff.py \
        [--slot 0,2,4] [--trials 400] [--records DIR] [--lift work/spu/r122/lift114/lift]

For each slot of re/spu/native_gates.tsv (default: all rows whose image is in the lift tree) build
tools/spu/native_diff_harness.c with the image's lifted spu_recomp.c and port/spu/kz3_native_post.c/kz3_native_dof.c
and compare the lifted function with the native kernel on synthetic states and on records written by
KZ3_NATIVE_REC (DIR/s<slot>_*.kz3nj): the whole local store and r0, r1, r80-r127 must be identical. The image's
constant pool must hash as the native kernel expects. SKIP (exit 0) without game data. Exit 1 on any difference.
"""
import argparse, glob, json, os, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kz3ver  # noqa: E402

PT = os.environ.get("HARNESS_PT", os.path.join(kz3ver.SPU_OUT, "ps3recomp-patched"))


def rows():
    out = []
    for line in open(os.path.join(ROOT, "re/spu/native_gates.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        out.append(dict(image=f[0], addr=int(f[1], 16), slot=int(f[2]), job=f[3]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", default="")
    ap.add_argument("--trials", type=int, default=400)
    ap.add_argument("--seed", default="0x4B5A33")
    ap.add_argument("--records", default="")
    ap.add_argument("--lift", default=os.environ.get("HARNESS_LIFT", os.path.join(kz3ver.SPU_OUT, "lift")))
    ap.add_argument("--ps3recomp", default=PT)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    if not os.path.exists(kz3ver.EBOOT) or not os.path.exists(os.path.join(kz3ver.SPU_OUT, "spu_lift.json")):
        print("SKIP: no game data / lift")
        return 0
    pt = os.path.abspath(a.ps3recomp)
    R = {r["name"]: r for r in json.load(open(os.path.join(kz3ver.SPU_OUT, "spu_lift.json")))}
    want = {int(x) for x in a.slot.split(",") if x}
    eboot = open(kz3ver.EBOOT, "rb").read()
    bad = 0
    with tempfile.TemporaryDirectory(dir=kz3ver.SPU_OUT) as t:
        built = {}
        for row in rows():
            name = row["image"]
            if (want and row["slot"] not in want) or name not in R or not os.path.exists(os.path.join(a.lift, name, "spu_recomp.c")):
                continue
            r = R[name]
            if name not in built:
                img = os.path.join(t, name + ".bin")
                open(img, "wb").write(eboot[r["file_offset"]:r["file_offset"] + r["size"]])
                exe = os.path.join(t, name)
                inc = ["-I", os.path.join(ROOT, "tools/spu"), "-I", os.path.join(ROOT, "port/spu"),
                       "-I", os.path.join(pt, "runtime/spu"), "-I", os.path.join(pt, "include"),
                       "-I", os.path.join(pt, "runtime"), "-I", os.path.join(a.lift, name)]
                subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-msse4.1", "-w", "-fno-strict-aliasing",
                                "-ffp-contract=off", f"-DHARNESS_REGISTER={name}_spu_recomp_register", *inc,
                                os.path.join(ROOT, "tools/spu/native_diff_harness.c"),
                                os.path.join(ROOT, "port/spu/kz3_native_post.c"), os.path.join(ROOT, "port/spu/kz3_native_dof.c"), os.environ.get("NATIVE_DIFF_LP") or os.path.join(ROOT, "port/spu/kz3_native_lp.c"),
                                os.path.join(pt, "runtime/spu/spu_interp.c"), os.path.join(pt, "runtime/spu/spu_drain.c"),
                                os.path.join(a.lift, name, "spu_recomp.c"), "-lm", "-o", exe], check=True)
                built[name] = (exe, img)
            exe, img = built[name]
            recs = sorted(glob.glob(os.path.join(a.records, f"s{row['slot']:02d}_*.kz3nj"))) if a.records else []
            p = subprocess.run([exe, img, str(r["base"]), str(row["slot"]), a.seed, str(a.trials), *recs],
                               capture_output=True, text=True)
            print(f"{name} 0x{row['addr']:X} {row['job']}: {p.stdout.strip() or p.stderr.strip()[-300:]}  (records {len(recs)})")
            if p.returncode:
                bad += 1
    print("FAIL" if bad else "PASS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
