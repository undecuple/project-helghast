#!/usr/bin/env python3
"""Host test for tools/spu/kz3ver.py's release overrides (KZ3_EBOOT_ELF, KZ3_ANALYSIS_DIR): no game data needed.

Each case imports kz3ver in a fresh interpreter with a given environment and checks EBOOT / ANALYSIS / SPU_DIR /
JOBTSV, and that the SPU image name kz3ver's users build (spu_<file offset, 8 hex>.elf) is the one elf_info.py
--spu-dir writes. Exit 0 = pass."""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def probe(env):
    e = {k: v for k, v in os.environ.items() if not k.startswith("KZ3_")}
    e.update(env)
    code = ("import json, kz3ver as k; print(json.dumps({'EBOOT': k.EBOOT, 'ANALYSIS': k.ANALYSIS, "
            "'SPU_DIR': k.SPU_DIR, 'JOBTSV': k.JOBTSV, 'VER': k.VER, 'SPU_OUT': k.SPU_OUT}))")
    out = subprocess.run([sys.executable, "-c", code], cwd=HERE, env=e, capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(f"FAIL: kz3ver import failed with {env}: {out.stderr.strip()[-300:]}")
    return json.loads(out.stdout)


fails = 0


def check(what, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += not ok


rel = probe({"KZ3_EBOOT_VER": "114", "KZ3_EBOOT_ELF": "/p/EBOOT.elf", "KZ3_ANALYSIS_DIR": "/p/analysis"})
check("KZ3_EBOOT_ELF sets EBOOT", rel["EBOOT"] == "/p/EBOOT.elf")
check("KZ3_ANALYSIS_DIR sets ANALYSIS", rel["ANALYSIS"] == "/p/analysis")
check("SPU_DIR = ANALYSIS/spu_images", rel["SPU_DIR"] == os.path.join("/p/analysis", "spu_images"))
check("JOBTSV under ANALYSIS", rel["JOBTSV"] == os.path.join("/p/analysis", "jobmods.tsv"))
for ver in ("100", "114"):
    d = probe({"KZ3_EBOOT_VER": ver})
    check(f"{ver} without overrides keeps work/", "/p/" not in d["EBOOT"] + d["ANALYSIS"] + d["SPU_DIR"])
# Per-version lift dirs (pass 144, swapped in pass 149 = series step 6): 1.14 work/recomp + work/spu, 1.00
# work/recomp-100 + work/spu-100; KZ3_SPU_OUT overrides the SPU one. DEFAULT is 114.
check("SPU_OUT 114 = work/spu", probe({"KZ3_EBOOT_VER": "114"})["SPU_OUT"] == os.path.join(ROOT, "work/spu"))
check("SPU_OUT 100 = work/spu-100", probe({"KZ3_EBOOT_VER": "100"})["SPU_OUT"] == os.path.join(ROOT, "work/spu-100"))
check("default version = 114", probe({})["VER"] == "114")
check("empty KZ3_EBOOT_VER = default", probe({"KZ3_EBOOT_VER": ""})["VER"] == "114")
check("SPU_OUT default version = 114's dir", probe({})["SPU_OUT"] == os.path.join(ROOT, "work/spu"))


def cli(env, *args):
    e = {k: v for k, v in os.environ.items() if not k.startswith("KZ3_")}
    e.update(env)
    r = subprocess.run([sys.executable, os.path.join(HERE, "kz3ver.py"), *args], env=e, capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


check("CLI default: 114 114 work/recomp work/spu", cli({}, "ver", "default", "recomp", "spu") == (0, "114 114 work/recomp work/spu"))
check("CLI 100: 100 work/recomp-100 work/spu-100", cli({"KZ3_EBOOT_VER": "100"}, "ver", "recomp", "spu") == (0, "100 work/recomp-100 work/spu-100"))
check("CLI KZ3_SPU_OUT", cli({"KZ3_SPU_OUT": "work/y"}, "spu") == (0, "work/y"))
check("CLI bad version: exit 2", cli({"KZ3_EBOOT_VER": "113"}, "ver")[0] == 2)
check("CLI bad key: exit 2", cli({}, "nope")[0] == 2)
check("CLI lift-ver unknown dir = selected version", cli({"KZ3_EBOOT_VER": "100"}, "lift-ver", "/nonexistent/x") == (0, "100"))
check("CLI lift-ver recomp-100 / recomp / recomp-114 by name",
      [cli({}, "lift-ver", f"/nonexistent/{d}")[1] for d in ("recomp-100", "recomp", "recomp-114")] == ["100", "114", "114"])
# firstrun.py reads the tree's DEFAULT with this regex (tree_eboot_ver): the line must keep its form.
check("DEFAULT line keeps the form firstrun.py reads",
      re.search(r'^DEFAULT = "(100|114)"', open(os.path.join(HERE, "kz3ver.py")).read(), re.M) is not None)
check("KZ3_SPU_OUT relative", probe({"KZ3_EBOOT_VER": "114", "KZ3_SPU_OUT": "work/x"})["SPU_OUT"] == os.path.join(ROOT, "work/x"))
check("KZ3_SPU_OUT absolute", probe({"KZ3_SPU_OUT": "/q/s"})["SPU_OUT"] == "/q/s")
# The name elf_info.py --spu-dir writes, read from its source, against the one gen_spu_register.py opens.
src = open(os.path.join(ROOT, "tools/elf_info.py")).read()
gen = open(os.path.join(HERE, "gen_spu_register.py")).read()
check("elf_info.py writes spu_{off:08x}.elf", 'f"spu_{off:08x}.elf"' in src)
check("gen_spu_register.py opens spu_{file_offset:08x}.elf", re.search(r'spu_\{r\[.file_offset.\]:08x\}\.elf', gen) is not None)
print("PASS" if not fails else f"FAIL ({fails})")
sys.exit(1 if fails else 0)
