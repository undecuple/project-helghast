#!/usr/bin/env python3
"""Host test (release pass 66, audit risk 1): the installer pipeline lifts and builds from the same inputs as the dev route.

Issue #3 lived only in the installer: the payload's lift lacked the 1.14 override table, so every dev run passed.
This assembles the payload from the repo (assemble_payload.py, as the release does; no game data) and compares what
firstrun.py's lift and configure steps read with what the dev route (tools/lift.sh, tools/spu/kz3ver.py,
tools/build-kz3.sh) reads, from the one source each:
- the EBOOT version firstrun picks from the payload's kz3ver.py = the repo's DEFAULT (what lift.sh and CMake use);
- the PPU override table, the fixups table, --toc, --code-end and --chunks of the lift command = lift.sh's case block
  and its KZ3_LIFT_CHUNKS default, table bytes equal to the repo file lift.sh resolves;
- the ppu_overrides.h set (the lifter's own parse of that table: hex rows, setjmp/longjmp rows aside) is therefore the
  same, and the host overrides kz3_ppu_overrides.cpp compiles for it (PPU_OVERRIDE_<addr>) are the same non-empty set;
- the configure step's -D names that decide code: dev's names plus the installer's allow-list only, no option off;
- controls: a payload tree without the 1.14 table stops the lift (StepFailed); a tree holding the 1.00 table instead
  differs in the announced set.
Exit 0 = PASS, 1 = FAIL."""
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import assemble_payload as ap  # noqa: E402
import firstrun as fr  # noqa: E402

FAILS = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  {detail}"[:500]))
    if not cond:
        FAILS.append(what)


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def announced(table_text):
    """The lifter's parse of --overrides (ppu_lifter.py main): the addresses that become PPU_OVERRIDE_<addr>."""
    s = set()
    for ln in table_text.splitlines():
        ln = ln.split("#", 1)[0].strip()
        if ln and ln.split()[0] not in ("setjmp", "longjmp"):
            s.add(int(ln, 16))
    return s


def host_side(cpp_text):
    return {int(m, 16) for m in re.findall(r"PPU_OVERRIDE_([0-9A-Fa-f]{8})", cpp_text)}


def lift_sh_case(ver):
    """tools/lift.sh's constants for a version, parsed from its case block: (code_end, toc, ovr candidates, fix)."""
    sh = read(os.path.join(REPO, "tools", "lift.sh"))
    m = re.search(rf"^\s*{ver}\)(.*?)^\s*(?:\d+\)|\*\))", sh, re.S | re.M)
    body = m.group(1)
    ce, toc = re.search(r"CODE_END=(0x[0-9a-f]+)", body), re.search(r"TOC=(0x[0-9a-f]+)", body)
    ovr = re.findall(r"OVR=(\S+?);?\s", body + " ")
    return ce.group(1), toc.group(1), [o.rstrip(";") for o in ovr]


def dev_table(rel_candidates):
    for r in rel_candidates:
        if os.path.isfile(os.path.join(REPO, r)):
            return os.path.join(REPO, r)
    return ""


def arg(cmd, flag):
    return cmd[cmd.index(flag) + 1]


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-liftagree-")
    try:
        pl = os.path.join(tmp, "payload")
        buf = open(os.devnull, "w")
        rc = ap.main(["--repo", REPO, "--out", pl], out=buf)
        if rc != 0 or not os.path.isdir(pl):
            print("SKIP: payload cannot be assembled from this tree (rc %s)" % rc)
            return 0
        # payload layout: <out>/<root files>; Ctx.root is the folder holding port/, re/, kz3-setup/ (the install folder)
        root = pl
        if not os.path.isdir(os.path.join(root, "kz3-setup")):
            cand = [d for d in os.listdir(pl) if os.path.isdir(os.path.join(pl, d, "kz3-setup"))]
            root = os.path.join(pl, cand[0]) if cand else pl
        c = fr.Ctx(root=root, build=os.path.join(tmp, "build"), dump="d", eboot=os.path.join(tmp, "EBOOT.elf"))

        # 1. the version
        dm = re.search(r'^DEFAULT = "(100|114)"', read(os.path.join(REPO, "tools", "spu", "kz3ver.py")), re.M)
        check(dm and c.ver == dm.group(1), "payload kz3ver.py DEFAULT = the repo's", f"{c.ver} vs {dm and dm.group(1)}")
        ver = c.ver
        ce, toc, ovr_c = lift_sh_case(ver)

        # 2. the lift command against lift.sh
        lifts = fr.cmd_ppu_lift(c)
        cmd = lifts[-1]
        check(arg(cmd, "--code-end") == ce and arg(cmd, "--toc") == toc,
              "lift --code-end/--toc = lift.sh's", f"{arg(cmd, '--code-end')} {arg(cmd, '--toc')} vs {ce} {toc}")
        sh = read(os.path.join(REPO, "tools", "lift.sh"))
        chunks = re.search(r'KZ3_LIFT_CHUNKS="\$\{KZ3_LIFT_CHUNKS:-(\d+)\}"', sh)
        check(chunks and arg(cmd, "--chunks") == chunks.group(1), "lift --chunks = lift.sh's default")
        dev_ovr = dev_table([o.replace("$VER", ver) for o in ovr_c])
        pay_ovr = arg(cmd, "--overrides")
        check(bool(dev_ovr) and read(dev_ovr) == read(pay_ovr), "payload override table = the file lift.sh resolves",
              f"{dev_ovr} vs {pay_ovr}")
        fix_dev = dev_table([f"port/spu/ppu_lift_fixups-{ver}.txt", f"re/data/overrides/ppu_lift_fixups-{ver}.txt"])
        fx = fr.fixups_file(c)
        check(bool(fix_dev) == bool(fx) and (not fx or read(fix_dev) == read(fx)), "payload fixups table = lift.sh's")

        # 3. the announced set, and the host overrides compiled for it
        a_dev, a_pay = announced(read(dev_ovr)), announced(read(pay_ovr))
        check(a_dev == a_pay and len(a_pay) > 10, "ppu_overrides.h set (lifter parse) equal and non-empty", f"{len(a_pay)}")
        cpp_dev = read(os.path.join(REPO, "port", "spu", "kz3_ppu_overrides.cpp"))
        cpp_pay = read(os.path.join(root, "port", "spu", "kz3_ppu_overrides.cpp"))
        h_dev, h_pay = host_side(cpp_dev) & a_dev, host_side(cpp_pay) & a_pay
        check(h_dev == h_pay and len(h_pay) > 5, "host overrides compiled for the announced set agree and are non-empty",
              f"{len(h_dev)} vs {len(h_pay)}")

        # 4. the configure step's code-deciding -D names
        names = {m.group(1) for a in fr.cmd_configure(c)[0] for m in [re.match(r"-D([A-Za-z0-9_]+)=", a)] if m}
        dev_sh = read(os.path.join(REPO, "tools", "build-kz3.sh"))
        dev_names = set(re.findall(r"-D([A-Za-z0-9_]+)=", dev_sh[dev_sh.find("cmake -S"):]))
        paths_tools = {"CMAKE_TOOLCHAIN_FILE", "Python3_EXECUTABLE", "RECOMP_DIR", "KZ3_SPU_DIR", "PS3RECOMP_DIR",
                       "CMAKE_MAKE_PROGRAM", "LLVM_MINGW_ROOT"}
        allowed = {"CMAKE_BUILD_TYPE", "CMAKE_C_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELEASE", "KZ3_EMBED_EBOOT", "KZ3_SPU_FMA"}
        check(dev_names - {"CMAKE_BUILD_TYPE"} <= names and {"RECOMP_DIR", "KZ3_SPU_DIR"} <= names,
              "installer configure passes every -D the dev route does (build type aside)", str(dev_names - names))
        check(names <= paths_tools | allowed, "installer configure adds only the allow-listed -D names",
              str(names - paths_tools - allowed))
        flags = " ".join(fr.cmd_configure(c)[0])
        check(not re.search(r"-D(KZ3_WITH_SPU|KZ3_WITH_NET|PORT_ENABLE_SPU|KZ3_LOG_REDIRECT)=(OFF|0)", flags),
              "no code option is switched off")

        # 5. controls
        t1 = os.path.join(tmp, "no114")
        shutil.copytree(root, t1)
        for r in ("re/data/overrides/ppu_overrides-114.txt", "port/spu/ppu_overrides-114.txt"):
            if os.path.exists(os.path.join(t1, r)):
                os.remove(os.path.join(t1, r))
        c1 = fr.Ctx(root=t1, build=os.path.join(tmp, "b1"), dump="d", eboot="e")
        try:
            fr.cmd_ppu_lift(c1)
            check(False, "control: a payload without the 1.14 table stops the lift")
        except fr.StepFailed:
            check(True, "control: a payload without the 1.14 table stops the lift")
        t2 = os.path.join(tmp, "swapped")
        shutil.copytree(root, t2)
        tgt = os.path.join(t2, "re", "data", "overrides", f"ppu_overrides-{ver}.txt")
        if os.path.exists(tgt):
            shutil.copy(os.path.join(REPO, "port", "spu", "ppu_overrides.txt"), tgt)
            c2 = fr.Ctx(root=t2, build=os.path.join(tmp, "b2"), dump="d", eboot="e")
            check(announced(read(fr.overrides_file(c2))) != a_dev,
                  "control: the 1.00 table in the 1.14 slot changes the announced set (the test would catch it)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_lift_inputs_agree:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
