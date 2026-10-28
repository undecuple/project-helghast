#!/usr/bin/env python3
"""Check the generated SPU registration against the runtime's own fingerprint code.

    nix develop .#re --command tools/sandbox.sh -- python3 tools/spu/test_spu_register.py

Takes spu_workload_fingerprint() verbatim out of <ps3recomp>/runtime/spu/spu_workload.c, compiles it with the
host C compiler into a small program, and runs it over the bytes each registered image is looked up by:
  * policy modules: `size` bytes at the module's EBOOT offset (what cellSpurs.c spurs_resolve_pm hashes at the
    workload's pm EA; the first Proton boot printed fp=0xFE229D83A0B8B05D size=12160 for the WWS job manager);
  * SPU ELFs: spu_elf_image_size() bytes of the ELF (computed by the C code too, taken from the same file).
Every value must equal the one in work/spu/gen/kz3_spu_manifest.json, and the WWS job manager must hash to
the value the runtime logged. Exit status 0 = all match.
"""
import json, os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kz3ver  # noqa: E402  KZ3_EBOOT_VER=100|114
from kz3ver import EBOOT, SPU_DIR, SPU_OUT, name as vname  # noqa: E402
PT = os.path.join(SPU_OUT, "ps3recomp-patched")
MANIFEST = os.path.join(SPU_OUT, "gen/kz3_spu_manifest.json")
# Values the runtime printed under Proton: PM from kz3-boot1 ("PM NOT LIFTED"), Edge zlib from the maintainer's
# post-FIOS-fix boot ("async dispatch MISS fp=0x089D7CA6345D885F size=36324").
# 1.14: both images are byte-identical (the RE notes 17.1), same values under the 1.14 names.
LOGGED = {vname("pm_012af080"): (0xFE229D83A0B8B05D, 12160), vname("elf_01000900"): (0x089D7CA6345D885F, 36324)}
# Recorded registration per version (name, kind, image id, fingerprint or job signature, size): any drift in the
# lift/generator or the inputs shows up as a diff. `--record` writes work/spu/spu_register_<ver>.tsv from the current
# manifest (the sandbox mounts the repo read-only); copy it to re/spu/ outside the sandbox.
EXPECTED = os.path.join(ROOT, f"re/spu/spu_register_{kz3ver.VER}.tsv")
RECORD = os.path.join(SPU_OUT, f"spu_register_{kz3ver.VER}.tsv")


def expected_rows(M):
    return [(m["name"], m["kind"], str(m["image_id"]), m.get("fp") or m.get("sig") or "-", str(m["size"]))
            for m in M if m["kind"] != "elfseg"]


def check_expected(M, record):
    rows = expected_rows(M)
    if record:
        with open(RECORD, "w") as f:
            f.write("# name\tkind\timage_id\tfp_or_sig\tsize   (tools/spu/test_spu_register.py --record, "
                    f"KZ3_EBOOT_VER={kz3ver.VER})\n")
            f.writelines("\t".join(r) + "\n" for r in rows)
        print(f"recorded {len(rows)} rows in {os.path.relpath(RECORD, ROOT)} (copy to {os.path.relpath(EXPECTED, ROOT)})")
        return 0
    if not os.path.exists(EXPECTED):
        print(f"no {os.path.relpath(EXPECTED, ROOT)} (run with --record)")
        return 1
    want = [tuple(line.split("\t")) for line in open(EXPECTED).read().splitlines() if not line.startswith("#")]
    diff = sorted(set(want) ^ set(rows))
    for r in diff[:20]:
        print(("expected " if r in set(want) else "got      ") + " ".join(r))
    print(f"{len(rows)} registered images vs {os.path.relpath(EXPECTED, ROOT)}: "
          f"{'MATCH' if not diff else f'{len(diff)} rows differ'}")
    return 1 if diff else 0


def extract(src, name):
    """Return the full definition of C function `name` (from its signature line to the matching brace)."""
    m = re.search(r"^[^\n;]*\b" + name + r"\s*\([^;{]*\)\s*\{", src, re.M)
    if not m:
        sys.exit(f"{name} not found")
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.start():i]


def main():
    record = "--record" in sys.argv[1:]
    kz3ver.check_lift(os.path.dirname(os.path.dirname(MANIFEST)))
    src = open(os.path.join(PT, "runtime/spu/spu_workload.c")).read()
    fns = [extract(src, n) for n in ("rd_be16", "rd_be32", "spu_workload_fingerprint", "spu_elf_image_size")]
    prog = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
""" + "\n".join(fns) + r"""
int main(int argc, char** argv) {
    /* lines: <path> <offset> <size or 0 = ELF image size> */
    char path[4096]; unsigned long off, size;
    while (scanf("%4095s %lx %lu", path, &off, &size) == 3) {
        FILE* f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) return 2; fclose(f);
        if (!size) size = spu_elf_image_size(b + off, n - off);
        printf("%016llX %lu\n", (unsigned long long)spu_workload_fingerprint(b + off, size), size);
        free(b);
    }
    return 0;
}
"""
    M = json.load(open(MANIFEST))
    rows = [m for m in M if m["kind"] in ("pm", "elf")]
    with tempfile.TemporaryDirectory(dir=SPU_OUT) as t:
        c, exe = os.path.join(t, "fp.c"), os.path.join(t, "fp")
        open(c, "w").write(prog)
        # The repo path may contain a space: the nix dev shell's NIX_LDFLAGS carries an unquoted
        # "-rpath <repo>/outputs/out/lib", and the helper reads whitespace-separated paths. Drop the former and
        # pass the latter relative to the repo root.
        env = dict(os.environ)
        env["NIX_LDFLAGS"] = env.get("NIX_LDFLAGS", "").replace(f"-rpath {ROOT}/outputs/out/lib", "")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wno-unused-function", "-o", exe, c], check=True, env=env)
        # Inputs may live in the main checkout ("Killzone 3", a space): link them into the temp dir first.
        lines = []
        for i, m in enumerate(rows):
            src_path = EBOOT if m["kind"] == "pm" else os.path.join(SPU_DIR, "spu_%08x.elf" % (m["ea"] - 0x10000))
            ln = os.path.join(t, f"in{i}")
            os.symlink(src_path, ln)
            lines.append(f"{os.path.relpath(ln, ROOT)} {(m['ea'] - 0x10000) if m['kind'] == 'pm' else 0:x} "
                         f"{m['size'] if m['kind'] == 'pm' else 0}")
        out = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True, cwd=ROOT)
    bad = 0
    for m, line in zip(rows, out.stdout.split("\n")):
        fp_c, size_c = line.split()
        fp_c, size_c = int(fp_c, 16), int(size_c)
        ok = fp_c == int(m["fp"], 16) and size_c == m["size"]
        if m["name"] in LOGGED:
            ok = ok and (fp_c, size_c) == LOGGED[m["name"]]
            print(f"{m['name']}: runtime C fingerprint 0x{fp_c:016X} over {size_c} bytes; "
                  f"boot log 0x{LOGGED[m['name']][0]:016X} size {LOGGED[m['name']][1]}: {'MATCH' if ok else 'DIFF'}")
        if not ok:
            bad += 1
            print(f"MISMATCH {m['name']}: C 0x{fp_c:016X}/{size_c}, manifest {m['fp']}/{m['size']}")
    print(f"{len(rows) - bad}/{len(rows)} registered fingerprints match the runtime's spu_workload_fingerprint")
    return (1 if bad else 0) | check_expected(M, record)


if __name__ == "__main__":
    sys.exit(main())
