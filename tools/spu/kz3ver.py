"""EBOOT version selection for the SPU tools: `KZ3_EBOOT_VER=100|114`, the variable tools/lift.sh uses.

Default 114 (the 1.14 update) since series step 6 (the RE notes s.36.4, RE pass 149). DEFAULT below is the one
place that decides it: tools/lift.sh, build-kz3.sh, kz3-stage.sh, tools/q, tools/sql/pick_views.py, tools/assets/lift_table.sh,
runprofile_kz3.py and tools/release/firstrun.py (regex on the `DEFAULT = "..."` line, keep its form) all read it, the shell
scripts through the command line below. Every SPU image key in the tools is written once with its v1.00 name/offset; for
114 it is translated through re/data/spu_map_114.tsv (RE pass 44, the RE notes section 17).

Lift directories per version (LIFT_DIRS, fixed, not derived from DEFAULT, so editing DEFAULT never re-points data on disk):
  114: work/recomp      + work/spu        (the plain names: every build dir, lane link and tool that names them gets 1.14)
  100: work/recomp-100  + work/spu-100    (the disc v1.00 lift, kept as the A/B reference)
Command line (for the shell scripts):  python3 tools/spu/kz3ver.py KEY...   KEY = ver | default | recomp | spu
prints the values for $KZ3_EBOOT_VER (else DEFAULT), space-separated;  `kz3ver.py lift-ver DIR` prints the version of
the PPU lift in DIR (its .lift-stamp, else the directory name, else the selected version). Exit 2 on a bad version.

Inputs per version:
  100: work/bin/EBOOT.elf,     work/bin/spu/spu_<fo>.elf,                work/analysis/jobmods.tsv,     re/data/spu_coverage_seeds.tsv
  114: work/bin/EBOOT-114.elf, work/analysis-114/spu_images/spu_<fo>.elf, work/analysis-114/jobmods.tsv, re/data/spu_seeds_114.tsv
A lane worktree has no work/analysis-114 link; the main checkout's copy ($KZ3_MAIN, set by tools/sandbox.sh) is used.
Release first-run overrides: KZ3_EBOOT_ELF (the EBOOT) and KZ3_ANALYSIS_DIR (jobmods.tsv, functions.json and
spu_images/ under it), set by tools/release/firstrun.py; tools/spu/test_kz3ver.py checks them.
"""
import os
import re
import sys

DEFAULT = "114"
VER = os.environ.get("KZ3_EBOOT_VER") or DEFAULT
if VER not in ("100", "114"):
    print(f"KZ3_EBOOT_VER must be 100 or 114, not {VER!r}", file=sys.stderr)
    sys.exit(2)
# PPU and SPU lift directory of each version (RE pass 149, series step 6; before it 1.00 had the plain names and
# 1.14 work/recomp-114 + work/spu-114). tools/lift.sh writes RECOMP_REL, tools/spu/lift_spu.sh SPU_OUT_REL.
LIFT_DIRS = {"114": ("work/recomp", "work/spu"), "100": ("work/recomp-100", "work/spu-100")}
RECOMP_REL = LIFT_DIRS[VER][0]

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAP_TSV = os.path.join(ROOT, "re/data/spu_map_114.tsv")


def work(rel):
    """Path of a shared work/ input: this checkout's, else the main checkout's (lane worktrees)."""
    local = os.path.join(ROOT, rel)
    main = os.environ.get("KZ3_MAIN")
    if os.path.exists(local) or not main:
        return local
    other = os.path.join(main, rel)
    return other if os.path.exists(other) else local


if VER == "100":
    EBOOT = work("work/bin/EBOOT.elf")
    ANALYSIS = work("work/analysis")
    SPU_DIR = work("work/bin/spu")
    SEEDS = os.path.join(ROOT, "re/data/spu_coverage_seeds.tsv")
    TOC = 0x132C960
    PPU_CODE_END = 0xDF5710
else:
    EBOOT = work("work/bin/EBOOT-114.elf")
    ANALYSIS = work("work/analysis-114")
    SPU_DIR = os.path.join(ANALYSIS, "spu_images")
    SEEDS = os.path.join(ROOT, "re/data/spu_seeds_114.tsv")
    TOC = 0x139FE40           # tools/lift.sh (KZ3_EBOOT_VER=114)
    # ASSUMPTION: v1.00 used 0xDF5710 (a little below lift.sh's --code-end 0xDFBBD4); for 1.14 the lift's code end
    # is used. Only bounds job_refs.py's scan for `lis`/`addi` pairs; a few late hits more or less.
    PPU_CODE_END = 0xE753F4
# Release first-run: the player's build folder instead of work/ (tools/release/firstrun.py sets
# both, plus KZ3_EBOOT_VER=114). The SPU images keep elf_info.py --spu-dir's names, spu_<file offset, 8 hex>.elf.
EBOOT = os.environ.get("KZ3_EBOOT_ELF") or EBOOT
if os.environ.get("KZ3_ANALYSIS_DIR"):
    ANALYSIS = os.environ["KZ3_ANALYSIS_DIR"]
    SPU_DIR = os.path.join(ANALYSIS, "spu_images")
# Where this version's SPU lift lives (RE pass 144, swapped in pass 149): one directory per version so a 1.14 and a
# 1.00 lift (and the builds that glob them) do not clobber each other; the default version (1.14) has work/spu, which
# every existing tool, build dir and lane link names. KZ3_SPU_OUT overrides (relative to the repo, or absolute).
# tools/spu/lift_spu.sh, gen_spu_register.py, job_refs.py, test_spu_register.py and the job harnesses default to SPU_OUT.
SPU_OUT_REL = os.environ.get("KZ3_SPU_OUT") or LIFT_DIRS[VER][1]
SPU_OUT = SPU_OUT_REL if os.path.isabs(SPU_OUT_REL) else os.path.join(ROOT, SPU_OUT_REL)
JOBTSV = os.path.join(ANALYSIS, "jobmods.tsv")
FUNCTIONS = os.path.join(ANALYSIS, "functions.json")


def _load_map():
    m = {}
    for line in open(MAP_TSV).read().splitlines():
        f = line.split("\t")
        if line.startswith("#") or len(f) < 9 or f[0] == "kind":
            continue
        m[f[2]] = f[7]
    return m


_MAP = _load_map() if VER == "114" else {}


def name(n100):
    """Image name (`elf_<fo>`, `job_<fo>`, `pm_<fo>`) for the selected version, from its v1.00 name."""
    return n100 if VER == "100" else _MAP[n100]


def off(o100, kind):
    """EBOOT file offset of an image for the selected version, from its v1.00 offset and kind (elf/job/pm)."""
    return int(name(f"{kind}_{o100:08x}").split("_")[1], 16)


def coverage_seeds(skip=()):
    """{image name: [LS seed]} for the selected version. v1.00 file: module, seed_ls_addr; 1.14 file:
    module100, seed100, module114, seed114, note (all 164 seeds keep their LS address)."""
    out = {}
    if not os.path.exists(SEEDS):
        return out
    lines = open(SEEDS).read().splitlines()
    head = lines[0].split("\t")
    mi, si = (head.index("module114"), head.index("seed114")) if "module114" in head else (0, 1)
    for line in lines[1:]:
        f = line.split("\t")
        if len(f) > si and f[si].startswith("0x") and f[mi] not in skip:
            out.setdefault(f[mi], []).append(int(f[si], 16))
    return out


def check_lift(lift_dir):
    """Stop when the lift in `lift_dir` (lift_spu.py --out) was made for another EBOOT version."""
    p = os.path.join(lift_dir, "eboot_ver")
    got = open(p).read().strip() if os.path.exists(p) else "100"   # lifts before pass 23 were all v1.00
    if got != VER:
        sys.exit(f"{lift_dir} holds the {got} lift, KZ3_EBOOT_VER is {VER}: re-run tools/spu/lift_spu.sh")


def lift_version(recomp_dir):
    """EBOOT version of the PPU lift in `recomp_dir`: its .lift-stamp (tools/lift.sh writes `elf=...EBOOT-114.elf` or
    `elf=...EBOOT.elf`), else the directory name (LIFT_DIRS, or the pre-round-149 name recomp-114), else VER."""
    stamp = os.path.join(recomp_dir or "", ".lift-stamp")
    if recomp_dir and os.path.isfile(stamp):
        m = re.search(r"elf=(\S*)", open(stamp).read())
        if m:
            return "114" if "EBOOT-114" in m.group(1) else "100"
    base = os.path.basename(os.path.normpath(recomp_dir)) if recomp_dir else ""
    for v, (r, _) in LIFT_DIRS.items():
        if base == os.path.basename(r):
            return v
    return "114" if base == "recomp-114" else VER


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["lift-ver"] and len(args) == 2:
        print(lift_version(args[1]))
        sys.exit(0)
    vals = {"ver": VER, "default": DEFAULT, "recomp": RECOMP_REL, "spu": SPU_OUT_REL}
    if not args or any(a not in vals for a in args):
        print("usage: kz3ver.py KEY... (ver default recomp spu) | kz3ver.py lift-ver DIR", file=sys.stderr)
        sys.exit(2)
    print(" ".join(vals[a] for a in args))
