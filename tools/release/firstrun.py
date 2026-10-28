#!/usr/bin/env python3
"""First-run build pipeline for the release installer and the portable zip.

Runs the checks, then lifts and compiles the port on the player's machine, as a table of steps (name, weight, command
builder, done-marker). Standard library only, Python 3.11+, Windows and Linux.

    firstrun.py --dump DIR --update DIR [--eboot FILE] [--root DIR] [--build DIR] [--jobs N] [--toolchain DIR]
                [--log-dir DIR] [--log-name NAME] [--dry-run]
    firstrun.py --interactive [--portable] [same options; --dump/--update/--eboot become the defaults it offers]

  --root       the install (or source tree) holding the toolkit and our tools; default: two folders above this script
  --build      where everything generated goes; default <root>/build (the dump is never written to, plan 4.4)
  --jobs       compile/lift job count; default from free RAM: (available - 8 GB) / 1.5 GB, clamped 1..8 (build-kz3.sh)
  --low-memory auto|on|off  one compile job, one lift worker, low priority (Linux: OOM score 1000; Windows: BELOW_NORMAL
               class + low memory priority) so the build is the first to give way; slower (step 9u). auto (default) = on
               when the PC has 8 GiB of physical RAM or less, said in the log; other PCs keep the free-RAM formula above
  --toolchain  an llvm-mingw folder (bin/x86_64-w64-mingw32-clang[.exe]), built with cmake/llvm-mingw.cmake; default:
               the payload's <root>/toolchain/llvm-mingw when present (pass 8), else cmake/mingw-w64.cmake (NixOS cross).
               The payload's toolchain/cmake, toolchain/ninja and (Windows) toolchain/python are used when present.
  --log-dir    folder the setup.exe / zip was started from (Inno passes {src}); not writable -> the temp folder
  --log-name   default names.SETUP_LOG (Project-Helghast-setup.log); an earlier log of the same name is renamed with its start time
  --status-file  also keep the LOG line and the latest PROGRESS/RESULT line in this file (for the Inno wizard)
  --stop-file    stop when this file appears (the wizard's Stop button): the running tool's process tree ends,
                 its step leaves no marker, RESULT step=cancelled, exit 130; the next start resumes there
  --dry-run    print the plan (steps, weights, done or pending, commands) and the log location; runs and writes nothing
  --interactive  the portable zip's text UI (run.cmd): asks for the dump, update and EBOOT
               (defaults: the arguments, else the last run's build/paths.json, else RPCS3's usual folder), runs the
               check until it passes (or the player quits), offers the one-shot version check (opt-in, asks first),
               writes kz3-install.ini, then builds with one console progress bar instead of the raw lines
  --portable   with --interactive: saves and settings stay in the install folder (kz3-install.ini portable=1)
  --bar        the console progress bar without the questions (the launcher's resume/rebuild in a console window)
  --font-dir   the player's PS3 firmware dev_flash/data/font folder (kz3.ini font_dir = KZ3_FONT_DIR; optional, pass 26;
               "-" = none, "auto" = font_guess beside the update); --interactive asks for it (default: RPCS3's
               folder beside the update when it holds the fonts)
  --font-copy  yes (default) | no: copy that folder's font files into <build>/fonts/ps3 and point kz3.ini there
               (step 9s), so the original may go; no = kz3.ini keeps pointing at the original folder
  --lang, --window-size  kz3.ini lang (KZ3_LANG) and window_size (KZ3_WINDOW_SIZE), optional
  --spu-fma    auto (default) = cpu_fma(): AVX + FMA3 -> the default build, else configure with -DKZ3_SPU_FMA=OFF
  --cleanup    ask|yes|no: delete the build intermediates (obj, recomp, spu, analysis) after a finished build;
               ask = a question with --interactive (default keep), else no. A later rebuild is then a full build.

After the build (stamp step) kz3.ini is written next to kz3.exe (launch.game_ini), so kz3.exe starts with no launch
options (pass 26; the runtime side is port/kz3/kz3_ini.h). The configure step links the EBOOT.elf it lifted into
kz3.exe (-DKZ3_EMBED_EBOOT, step 9s; port/kz3/kz3_embed.h): the exe checks its hash at start and needs no eboot line.

Machine-readable lines on stdout, for the Inno wizard's output page and the zip's console:
  LOG path=<file>
  PROGRESS step=N/M name=<step> done=X total=Y elapsed=S pct=P      (P = whole pipeline, weights applied)
  RESULT ok=1|0 step=<failed step or -> elapsed=S
The compile step's done/total come from Ninja's "[x/y]" status lines. Every other line is the step's own output.

Resume: each finished step writes <build>/.firstrun/<NN>-<name>.done (JSON: the inputs fingerprint, duration). A
cancel (Ctrl+C, the wizard's Cancel = terminate) writes nothing for the running step, so the next start resumes at
the first unfinished step. The check step (firstrun_check.py) always runs first; if its facts change (another EBOOT,
other folders) every later marker is stale and the build starts over.

Exit 0 = built, 1 = a step failed, 2 = usage, 130 = cancelled.
Pass 12 (stage B, plan 4.11): every step ran through this file on the payload's Windows Python under Proton on
the 1.00 tree (lift byte-identical to the Linux one apart from CRLF; the exe boots like main's GCC build). The 1.14
tree (main's default since 2026-10-22) went through it on native Windows 11 on 2026-10-21: the installer built the
game in 422 s on a 16-core PC and the game reached the menus (docs/PROGRESS.md).
Weights: stage B at 4 jobs (pass 12, plan 4.11, measured on the 1.00 tree; not re-measured on 1.14); the shader
step's is an estimate.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import names  # noqa: E402  (product name, file names, links: tools/release/names.py)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOG_NAME = names.SETUP_LOG
MARKER_DIR = ".firstrun"
KEEP_LOG_COPIES = 5                      # the install's logs\ keeps the last five (plan 9.11)
NINJA_RE = re.compile(r"^\[(\d+)/(\d+)\]")

# Lift constants per EBOOT version (tools/lift.sh: kz3ver.py DEFAULT, 114 on main since 2026-10-22; KZ3_EBOOT_VER=100 selects 1.00). The tree being built
# decides which one (tree_eboot_ver, pass 12): its runtime glue (port/) is written for one EBOOT's addresses, so a
# 1.14 lift under main's 1.00 glue (or the reverse) would build but not boot.
CODE_END_114 = "0xe753f4"
TOC_114 = "0x139fe40"
EBOOT_VERS = {"114": {"code_end": CODE_END_114, "toc": TOC_114, "check": "1.14"},
              "100": {"code_end": "0xdfbbd4", "toc": "0x132c960", "check": "1.00"}}
GIB = 1 << 30
# Release without debug info, at the verified build's optimisation level: the developer builds are RelWithDebInfo
# (-O2 -g); CMake's Release default is -O3, untried on the lifted chunks (more compile memory and time, ASSUMPTION).
RELEASE_FLAGS = "-O2 -DNDEBUG"
# tools/lift.sh's KZ3_LIFT_CHUNKS default (patch 3005): 64 chunks of 51-73k lines, ~1.5 GB per compile job instead of
# 4.4-6.7 GB for the lifter's default 8 (build-kz3.sh header). Pass 8: firstrun.py did not pass it before.
LIFT_CHUNKS = 64
# The lifts' worker count is capped (pass 12): stage B measured them at -j4 (PPU lift 30 s, its main process 2.3 GB;
# SPU lift 103 s, small workers); more workers were never measured and gain little on steps this short.
LIFT_JOBS_MAX = 4


# ---------------------------------------------------------------------------------------------------------- context
@dataclass
class Ctx:
    root: str
    build: str
    dump: str = ""
    update: str = ""
    eboot: str = ""
    jobs: int = 1
    toolchain: str = ""
    python: str = sys.executable
    cmake: str = "cmake"
    ninja: str = ""
    facts: dict = field(default_factory=dict)
    cache: dict = field(default_factory=dict, repr=False)
    font_dir: str = ""              # the player's firmware fonts (KZ3_FONT_DIR), optional (pass 26)
    lang: str = ""                  # KZ3_LANG, optional
    window_size: str = ""           # KZ3_WINDOW_SIZE, optional
    spu_fma: bool | None = None     # -DKZ3_SPU_FMA=ON/OFF from the CPU check (pass 26); None = CMake's default
    font_copy: bool = True          # copy font_dir into <build>/fonts/ps3 (step 9s); False = point at the original
    font_src: str = ""              # the folder the copy came from (paths.json, the log)
    low_memory: bool = False        # step 9u: one compile job, one lift worker, low priority (see low_memory_for)

    def tool(self, *rel):
        """Our tools: <root>/kz3-setup/... in the release (plan 3, ASSUMPTION), <root>/tools/... in the source tree."""
        for base in ("kz3-setup", "tools"):
            p = os.path.join(self.root, base, *rel)
            if os.path.exists(p):
                return p
        return os.path.join(self.root, "tools", *rel)

    @property
    def ps3recomp(self):
        """The patched toolkit: <root>/ps3recomp in the release, work/ps3recomp-patched in the source tree."""
        for rel in ("ps3recomp", os.path.join("work", "ps3recomp-patched")):
            p = os.path.join(self.root, rel)
            if os.path.isdir(p):
                return p
        return os.path.join(self.root, "ps3recomp")

    @property
    def py(self):
        """The Python command for the steps' scripts: UTF-8 mode (pass 12, stage B: Windows Python's default text
        encoding is the ANSI code page, so the PPU lifter wrote its comments' em dashes as cp1252 0x97 where the Linux
        lift writes UTF-8; the C was otherwise identical). PYTHONUTF8 in env() as well; -X works in the embeddable
        Python's isolated mode, where environment variables may be ignored."""
        return [self.python, "-X", "utf8"]

    @property
    def ver(self):
        """The EBOOT version this tree is built for ("100" or "114"): tree_eboot_ver, read once."""
        if "ver" not in self.cache:
            self.cache["ver"] = tree_eboot_ver(self)
        return self.cache["ver"]

    def b(self, *rel):
        return os.path.join(self.build, *rel)

    def env(self):
        """Child environment: the bundled compiler, CMake and Ninja first on PATH (release layout, plan 3)."""
        extra = [os.path.join(self.toolchain, "bin")] if self.toolchain else []
        extra += [os.path.join(self.root, "toolchain", *d) for d in (("cmake", "bin"), ("ninja",))]
        e = dict(os.environ)
        e["PATH"] = os.pathsep.join([d for d in extra if os.path.isdir(d)] + [e.get("PATH", "")])
        e["PYTHONIOENCODING"] = "utf-8"
        e["PYTHONUTF8"] = "1"
        e["KZ3_EBOOT_VER"] = self.ver                                # the tree's EBOOT version (pass 12)
        e["KZ3_EBOOT_ELF"] = self.eboot
        e["KZ3_ANALYSIS_DIR"] = self.b("analysis")
        if fixups_file(self):
            e["PPU_JT_HINTS"] = self.b("analysis", "jt_hints.txt")    # written by lift_fixups.py before the lift
        return e

    def parts(self):
        """The build's inputs as three hashes (pass 4, plan 7): `base` = the folders and the EBOOT's identity from
        the check; `lift` = what the lifts read besides the EBOOT (lift_inputs_hash); `runtime` = the port version
        (VERSION.txt). A new release that changes only the runtime glue recompiles without lifting again."""
        key = {"dump": self.dump, "update": self.update, "eboot": self.eboot,
               "eboot_elf": self.facts.get("eboot_elf"), "update_sfo": self.facts.get("update")}
        if "lift" not in self.cache:
            self.cache["lift"] = lift_inputs_hash(self)
        return {"base": _h16(json.dumps(key, sort_keys=True)), "lift": self.cache["lift"],
                "runtime": _h16(port_version(self.root))}

    def fingerprint(self, keys=()):
        """One hash of `base` plus the named extra parts ("lift", "runtime"); a step's marker stores the one for
        its Step.keys, so a lift-input change re-runs the lifts but not the load and inventory steps."""
        p = self.parts()
        return p["base"] if not keys else _h16("|".join([p["base"]] + [p[k] for k in keys]))


@dataclass
class Step:
    name: str                                   # short id, also the marker file name
    label: str                                  # what the wizard shows ("Lifting PPU code")
    weight: float                               # share of the whole bar (normalised over the table)
    commands: Callable[[Ctx], list]             # -> list of argv lists, run in order
    always: bool = False                        # run even when its marker exists (the check)
    ninja: bool = False                         # parse "[x/y]" for progress
    clean: Callable[[Ctx], list] = lambda c: []  # folders under <build> removed before a (re)run
    after: Callable[[Ctx], None] | None = None  # in-process work after the commands (record, stamp)
    keys: tuple = ()                            # extra fingerprint parts its marker depends on (Ctx.parts)
    optional: bool = False                      # a failure is logged, the result stays ok (the port is built)


def _h16(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]


LIFT_INPUTS = [   # (base: "root" | "tools" | "toolkit", glob) : what the PPU and SPU lifts read besides the EBOOT
    ("root", "port/spu/ppu_overrides.txt"), ("root", "port/spu/ppu_lift_fixups-*.txt"),
    ("root", "re/data/spu_map_114.tsv"), ("root", "re/data/spu_seeds_114.tsv"),
    ("root", "re/data/spu_coverage_seeds.tsv"),
    ("tools", "jobmods.py"), ("tools", "elf_info.py"), ("tools", "spu/lift_spu.py"),
    ("tools", "spu/gen_spu_register.py"), ("tools", "spu/kz3ver.py"), ("tools", "release/lift_fixups.py"),
    ("toolkit", "tools/*.py"), ("toolkit", ".kz3-patchset"),
]


def lift_inputs_hash(c):
    """SHA-256 (16 hex) over the lift inputs' relative names and contents; a missing file counts as missing."""
    import glob
    tools = os.path.dirname(os.path.dirname(c.tool("release", "firstrun.py")))
    bases = {"root": c.root, "tools": tools, "toolkit": c.ps3recomp}
    h = hashlib.sha256()
    for base, pat in LIFT_INPUTS:
        hits = sorted(glob.glob(os.path.join(glob.escape(bases[base]), pat)))
        h.update(f"{base}:{pat}:{len(hits)}\n".encode())
        for p in hits:
            h.update(os.path.relpath(p, bases[base]).replace(os.sep, "/").encode() + b"\0")
            with open(p, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()[:16]


# ------------------------------------------------------------------------------------------------- the step table
def cmd_check(c):
    a = [*c.py, c.tool("release", "firstrun_check.py"), "--dump", c.dump, "--json",
         "--target", EBOOT_VERS[c.ver]["check"]]
    if c.update:
        a += ["--update", c.update]
    if c.eboot:
        a += ["--eboot", c.eboot]
    return [a]


STAGED_EBOOT = "EBOOT.elf"   # the name the loader's outputs (EBOOT.imports.json, EBOOT.functions.json) and the lifts assume


def stage_eboot(c):
    """-> the path of the player's EBOOT under the fixed name EBOOT.elf in <build>/analysis (a copy; a hard link is
    not used, the source may be on another drive). ppu_loader names its outputs after the input file and the later
    steps read EBOOT.*.json, so a dump's "EBOOT (1).elf" or EBOOT-114.elf failed the lift (pass 53). c.eboot itself
    stays the player's file (hash, embed, KZ3_EBOOT_ELF)."""
    an = c.b("analysis")
    dst = os.path.join(an, STAGED_EBOOT)
    if os.path.abspath(c.eboot) == os.path.abspath(dst) or not os.path.isfile(c.eboot):
        return c.eboot   # (dry run: a placeholder; a missing file is the check step's error, not ours)
    os.makedirs(an, exist_ok=True)
    shutil.copyfile(c.eboot, dst)
    return dst


def cmd_load(c):
    pt, an = c.ps3recomp, c.b("analysis")
    staged = stage_eboot(c)
    return [[*c.py, os.path.join(pt, "tools", "ppu_loader.py"), staged, "-o", an + os.sep],
            [*c.py, os.path.join(pt, "tools", "find_functions.py"), c.eboot,
             "--seed-json", os.path.join(an, "EBOOT.functions.json"), "--output", os.path.join(an, "functions.json")]]


def cmd_inventory(c):
    an = c.b("analysis")
    return [[*c.py, c.tool("jobmods.py"), c.eboot, "--out", os.path.join(an, "jobmods"),
             "--tsv", os.path.join(an, "jobmods.tsv")],
            [*c.py, c.tool("elf_info.py"), c.eboot, "--imports", os.path.join(an, "imports.tsv"),
             "--spu-dir", os.path.join(an, "spu_images")]]


def tree_eboot_ver(c):
    """The EBOOT version the tree's glue is written for: `DEFAULT = "100"|"114"` in its tools/spu/kz3ver.py (the
    one-word switch tools/lift.sh reads too), else "114" ('s target).
    Pass 12: firstrun.py assumed 114 everywhere, so on a 1.00 tree it lifted the wrong EBOOT layout."""
    try:
        with open(c.tool("spu", "kz3ver.py"), encoding="utf-8") as f:
            m = re.search(r'^DEFAULT = "(100|114)"', f.read(), re.M)
        return m.group(1) if m else "114"
    except OSError:
        return "114"


def fixups_file(c):
    """port/spu/ppu_lift_fixups-<ver>.txt (the file tools/lift.sh uses), or "" when this tree has none."""
    p = os.path.join(c.root, "port", "spu", f"ppu_lift_fixups-{c.ver}.txt")
    return p if os.path.isfile(p) else ""


def lift_jobs(c):
    """Lift workers: the job count capped at LIFT_JOBS_MAX; one in the low-memory mode."""
    return 1 if c.low_memory else min(c.jobs, LIFT_JOBS_MAX)


def cmd_ppu_lift(c):
    # The 1.14 lift fixups (plan 10.3, pass 4): lift_fixups.py writes functions.fixed.json and jt_hints.txt (the
    # lifter reads the hints through PPU_JT_HINTS, set in Ctx.env); without a fixups file the list is lifted as is.
    an = c.b("analysis")
    funcs, cmds = os.path.join(an, "functions.json"), []
    if fixups_file(c):
        cmds.append([*c.py, c.tool("release", "lift_fixups.py"), "--fixups", fixups_file(c), "--functions", funcs,
                     "--out", os.path.join(an, "functions.fixed.json"), "--jt-hints", os.path.join(an, "jt_hints.txt")])
        funcs = os.path.join(an, "functions.fixed.json")
    return cmds + [[*c.py, os.path.join(c.ps3recomp, "tools", "ppu_lifter.py"), c.eboot,
                    "--functions", funcs,
                    "--hle-stubs", os.path.join(an, "EBOOT.imports.json"), "--toc", EBOOT_VERS[c.ver]["toc"],
                    "--code-end", EBOOT_VERS[c.ver]["code_end"],
                    "--overrides", os.path.join(c.root, "port", "spu", "ppu_overrides.txt"),
                    "-j", str(lift_jobs(c)), "--chunks", str(LIFT_CHUNKS), "--output", c.b("recomp")]]


def cmd_spu_lift(c):
    # The SPU tools read their inputs through tools/spu/kz3ver.py; KZ3_EBOOT_VER=114, KZ3_EBOOT_ELF and
    # KZ3_ANALYSIS_DIR (Ctx.env) point it at this build's EBOOT and analysis folder. ASSUMPTION: kz3ver.py reads the
    # last two once the requested SPU change lands (pass 4); until then it reads
    # work/analysis-114.
    return [[*c.py, c.tool("spu", "lift_spu.py"), "--ps3recomp", c.ps3recomp, "--out", c.b("spu"),
             "-j", str(lift_jobs(c))],
            [*c.py, c.tool("spu", "gen_spu_register.py"), "--lift", c.b("spu"), "--out", c.b("spu", "gen")]]


def payload_tools(root):
    """The release's bundled tools that exist (plan 3: <root>/toolchain/...; fetched by fetch_toolchain.py and copied
    by assemble_payload.py --toolchain): llvm_mingw (folder), cmake, ninja, python (Windows only: python.exe)."""
    exe = ".exe" if os.name == "nt" else ""
    t = os.path.join(root, "toolchain")
    found = {}
    lm = os.path.join(t, "llvm-mingw")
    if os.path.isfile(os.path.join(lm, "bin", "x86_64-w64-mingw32-clang" + exe)):
        found["llvm_mingw"] = lm
    for k, p in (("cmake", os.path.join(t, "cmake", "bin", "cmake" + exe)), ("ninja", os.path.join(t, "ninja", "ninja" + exe)),
                 ("python", os.path.join(t, "python", "python.exe"))):
        if os.path.isfile(p) and (k != "python" or os.name == "nt"):
            found[k] = p
    return found


def cm_path(p, sep=os.sep):
    """A path for a CMake -D value: forward slashes on Windows (pass 11). A backslash in a -D value that a CMake
    file later expands inside a string (LLVM_MINGW_ROOT -> CMAKE_C_COMPILER, RECOMP_DIR) can be read as an escape
    (ASSUMPTION: not seen yet; stage A runs the Windows CMake); forward slashes work on every Windows CMake.
    A no-op on Linux."""
    return p.replace("\\", "/") if sep == "\\" else p


def cmd_configure(c):
    a = [c.cmake, "-S", os.path.join(c.root, "port", "kz3"), "-B", c.b("obj"), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_FLAGS_RELEASE={RELEASE_FLAGS}",
         f"-DCMAKE_CXX_FLAGS_RELEASE={RELEASE_FLAGS}", f"-DPS3RECOMP_DIR={cm_path(c.ps3recomp)}",
         f"-DRECOMP_DIR={cm_path(c.b('recomp'))}", f"-DKZ3_SPU_DIR={cm_path(c.b('spu'))}",
         f"-DPython3_EXECUTABLE={cm_path(c.python)}"]
    if c.ninja:
        a.append(f"-DCMAKE_MAKE_PROGRAM={cm_path(c.ninja)}")
    if c.toolchain:     # llvm-mingw: the release's compiler, cmake/llvm-mingw.cmake (pass 8)
        a += [f"-DCMAKE_TOOLCHAIN_FILE={cm_path(os.path.join(c.root, 'cmake', 'llvm-mingw.cmake'))}",
              f"-DLLVM_MINGW_ROOT={cm_path(c.toolchain)}"]
    else:
        a += [f"-DCMAKE_TOOLCHAIN_FILE={cm_path(os.path.join(c.root, 'cmake', 'mingw-w64.cmake'))}"]
    if c.eboot and embed_supported(c.root):   # step 9s: the EBOOT this build lifted, linked into kz3.exe (port/kz3/kz3_eboot_blob.c, .incbin)
        a.append(f"-DKZ3_EMBED_EBOOT={cm_path(c.eboot)}")
    if c.spu_fma is not None:   # patch 1097: -mfma for the lifted SPU code needs AVX + FMA3 (cpu_fma below)
        a.append(f"-DKZ3_SPU_FMA={'ON' if c.spu_fma else 'OFF'}")
    return [a]


def _clang(toolchain):
    for n in ("x86_64-w64-mingw32-clang.exe", "clang.exe", "x86_64-w64-mingw32-clang", "clang"):
        p = os.path.join(toolchain, "bin", n)
        if toolchain and os.path.isfile(p):
            return p
    return ""


def cpu_fma(toolchain="", run=subprocess.run, system=None, cpuinfo="/proc/cpuinfo", pf=None):
    """Does this CPU (and OS) run AVX + FMA3 code? -> (True/False/None, how). Standard library only.

    1. the bundled compiler's own host probe: `clang -march=native -dM -E -` defines __AVX__ and __FMA__ only when
       the CPU has them and the OS saves the AVX state (LLVM checks XGETBV); the same facts kz3_cpu_check.c asks for.
    2. Windows without it: IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE = 40). ASSUMPTION: AVX2 implies
       FMA3 (true of every Intel and AMD CPU with AVX2); an AVX + FMA3 CPU without AVX2 (AMD Piledriver) gets the
       slower OFF build, which still runs.
    3. elsewhere: the avx and fma flags of /proc/cpuinfo.
    None = unknown: CMake's default (ON) stays and kz3.exe's own check says so if the CPU lacks them."""
    cc = _clang(toolchain)
    if cc:
        try:
            r = run([cc, "-march=native", "-dM", "-E", "-x", "c", "-"], input="", capture_output=True, text=True,
                    timeout=120)
            if r.returncode == 0 and "__x86_64__" in r.stdout:
                d = set(re.findall(r"#define (__\w+__) ", r.stdout))
                return ("__AVX__" in d and "__FMA__" in d), f"{os.path.basename(cc)} -march=native"
        except (OSError, subprocess.SubprocessError):
            pass
    system = system or platform.system()
    if system == "Windows":
        try:
            if pf is None:
                import ctypes
                pf = ctypes.windll.kernel32.IsProcessorFeaturePresent
            return bool(pf(40)), "IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)"
        except (OSError, AttributeError, ImportError):
            return None, "unknown (no compiler probe, no IsProcessorFeaturePresent)"
    try:
        with open(cpuinfo, encoding="utf-8", errors="replace") as f:
            flags = next((ln.split(":", 1)[1].split() for ln in f if ln.startswith("flags")), None)
        if flags is not None:
            return ("avx" in flags and "fma" in flags), cpuinfo
    except OSError:
        pass
    return None, "unknown"


SOURCES_RECORD = ("_firstrun_sources.json",)   # under <build>: the payload hashes of the last finished build


def payload_hashes(root):
    """{path: sha256} of the payload's own files from <root>/manifest.json (assemble_payload.py); {} for a source
    tree (no manifest)."""
    try:
        with open(os.path.join(root, "manifest.json"), encoding="utf-8") as f:
            return {e["path"]: e["sha256"] for e in json.load(f).get("files", [])}
    except (OSError, ValueError, KeyError, TypeError):
        return {}


def prepare_sources(c):
    """configure's clean hook (pass 18, plan 4.17). Installing over an earlier version keeps each payload file's
    own time stamp (Inno), so a source that changed can be older than the object built from the old one and ninja
    skips it (pass 18 under Proton: a changed port/kz3/main.cpp, "ninja: no work to do"). So, against the hashes
    recorded by the last finished build: every changed payload file gets the current time (ninja then rebuilds what
    depends on it), and a changed toolchain/ file empties obj/ (ninja does not track the compiler binary).
    No record (first build, a source tree) touches nothing. -> folders to clean."""
    cur = payload_hashes(c.root)
    try:
        with open(c.b(*SOURCES_RECORD), encoding="utf-8") as f:
            prev = json.load(f)
    except (OSError, ValueError):
        return []
    if not cur or not isinstance(prev, dict):
        return []
    now, touched, tool = time.time(), [], False
    for path, h in sorted(cur.items()):
        if prev.get(path) == h:
            continue
        if path.startswith("toolchain/"):
            tool = True
            continue
        f = os.path.join(c.root, *path.split("/"))
        if os.path.isfile(f):
            os.utime(f, (now, now))
            touched.append(path)
    c.touched = touched
    return [c.b("obj")] if tool else []


def record_sources(c):
    cur = payload_hashes(c.root)
    if cur:
        with open(c.b(*SOURCES_RECORD), "w", encoding="utf-8") as f:
            json.dump(cur, f, indent=0, sort_keys=True)


def cmd_compile(c):
    return [[c.cmake, "--build", c.b("obj"), "-j", str(c.jobs)]]


FONT_EXTS = (".ttf", ".ttc", ".otf", ".pgf")   # what a firmware dev_flash/data/font folder holds


def ps3_font_dest(build):
    return os.path.join(build, "fonts", "ps3")


def copy_ps3_fonts(src, dest):
    """Mirror the font files at the top of src (an already-extracted firmware font folder, never PS3UPDAT.PUP) into
    dest: copies a missing or different-size file, removes font files src no longer has. -> (copied, removed)."""
    want = {n for n in os.listdir(src) if n.lower().endswith(FONT_EXTS) and os.path.isfile(os.path.join(src, n))}
    os.makedirs(dest, exist_ok=True)
    copied = removed = 0
    for n in sorted(want):
        a, b = os.path.join(src, n), os.path.join(dest, n)
        if os.path.isfile(b) and os.path.getsize(a) == os.path.getsize(b):
            continue
        shutil.copyfile(a, b)
        copied += 1
    for n in os.listdir(dest):
        if n not in want and n.lower().endswith(FONT_EXTS):
            os.remove(os.path.join(dest, n))
            removed += 1
    return copied, removed


def place_ps3_fonts(c):
    """Step 9s: the chosen font folder copied into the install (<build>/fonts/ps3), kz3.ini then points there and
    the original may go; with font_copy off, kz3.ini keeps the original. A folder that is already the copy (a resume
    reads it back from paths.json) is left as is. -> the folder kz3.ini names."""
    if not c.font_dir:
        return ""
    dest = ps3_font_dest(c.build)
    if os.path.normcase(os.path.abspath(c.font_dir)) == os.path.normcase(os.path.abspath(dest)):
        return dest
    c.font_src = c.font_dir
    if not c.font_copy:
        return c.font_dir
    copied, removed = copy_ps3_fonts(c.font_dir, dest)
    print(f"firmware fonts: {copied} copied, {removed} removed -> {dest}", flush=True)
    return dest


def after_record(c):
    """Write paths.json (the launcher reads it): the three paths and the check's facts."""
    os.makedirs(c.build, exist_ok=True)
    try:
        c.font_dir = place_ps3_fonts(c)
    except OSError as e:   # a font folder that cannot be read: the fonts are optional, keep the original
        print(f"firmware fonts: not copied ({e.strerror}); kz3.ini points at the original folder", flush=True)
    with open(c.b("paths.json"), "w", encoding="utf-8") as f:
        json.dump({"dump": c.dump, "update": c.update, "eboot": c.eboot, "facts": c.facts, "font_dir": c.font_dir,
                   "font_src": c.font_src, "lang": c.lang, "window_size": c.window_size}, f, indent=2)


def eboot_stat(path):
    """[size, whole-second mtime] of the EBOOT, or None: the launcher's cheap "did it change since the build"."""
    try:
        st = os.stat(path)
        return [st.st_size, int(st.st_mtime)]
    except OSError:
        return None


NOTICE_FONTS = ("Rajdhani-Medium.ttf", "Rajdhani-Bold.ttf")


SDL3_PAYLOAD = "runtime/SDL3.dll"


def place_sdl3(root, build):
    """Copy <root>/runtime/SDL3.dll (shipped in the payload, assemble_payload SDL3_DEST) next to <build>/kz3.exe: the game
    loads it from its own folder for native pads (patch 4010; without it pads fall back to XInput). Copies only a missing
    or different file (size, then hash); -> True when it copied. Called after every build and by the launcher."""
    src, dst = os.path.join(root, *SDL3_PAYLOAD.split("/")), os.path.join(build, "SDL3.dll")
    if not os.path.isfile(src):
        return False
    if os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src) and file_sha256(dst) == file_sha256(src):
        return False
    os.makedirs(build, exist_ok=True)
    shutil.copyfile(src, dst)
    return True


def place_fonts(root, build):
    """Copy <root>/fonts/Rajdhani-*.ttf (shipped in the payload, assemble_payload FONTS) to <build>/fonts/, next to
    kz3.exe: the runtime's error notice loads them privately from <exe dir>\\fonts (RSX pass 38; without them it
    falls back to the Windows UI font). Copies only a missing or different-size file; -> names copied. Called after
    every build and by the launcher before each start (an install from before the fonts shipped gets them too)."""
    done = []
    for fn in NOTICE_FONTS:
        src, dst = os.path.join(root, "fonts", fn), os.path.join(build, "fonts", fn)
        if not os.path.isfile(src):
            continue
        if os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        done.append(fn)
    return done


def embed_supported(root):
    """The payload's runtime can link the EBOOT into kz3.exe (step 9s: port/kz3/kz3_eboot_blob.c). An older payload
    cannot; its exe still needs kz3.ini's eboot line."""
    return os.path.isfile(os.path.join(root, "port", "kz3", "kz3_eboot_blob.c"))


def cache_root_supported(root):
    """The payload's runtime reads kz3.ini cache_root (KZ3_CACHE_ROOT): the dump is the VFS root, read in place, and
    every write goes to the cache root, so no directory junction is needed (port/kz3/kz3_ini.h)."""
    try:
        with open(os.path.join(root, "port", "kz3", "kz3_ini.h"), encoding="utf-8", errors="replace") as f:
            return "KZ3_CACHE_ROOT" in f.read()
    except OSError:
        return False


def file_sha256(path):
    """-> the file's SHA-256 (hex), or "" (no file): build-stamp.json's eboot_embedded, the EBOOT linked into kz3.exe."""
    if not path or not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def after_stamp(c):
    exe = c.b("obj", "kz3.exe")
    if os.path.isfile(exe):
        shutil.copy2(exe, c.b("kz3.exe"))
    place_fonts(c.root, c.build)
    place_sdl3(c.root, c.build)
    record_sources(c)
    stamp = {"inputs": c.fingerprint(("lift", "runtime")), "parts": c.parts(), "version": port_version(c.root),
             "eboot_stat": eboot_stat(c.eboot), "facts": c.facts, "jobs": c.jobs,
             "toolchain": c.toolchain or "cross", "spu_fma": c.spu_fma, "eboot_embedded": file_sha256(c.eboot) if embed_supported(c.root) else "",
             "cache_root": cache_root_supported(c.root),
             "built": datetime.datetime.now().isoformat(timespec="seconds")}
    pf = os.path.join(c.ps3recomp, ".kz3-patchset")
    if os.path.isfile(pf):
        with open(pf, encoding="utf-8", errors="replace") as f:
            stamp["patchset"] = f.read().strip()
    with open(c.b("build-stamp.json"), "w", encoding="utf-8") as f:
        json.dump(stamp, f, indent=2)
    write_game_ini(c)


def write_game_ini(c):
    """kz3.ini next to kz3.exe (pass 26): kz3.exe then starts with no launch options (a double-click, a Steam
    shortcut). The same values the launcher writes before each start (launch.game_ini); here too so the exe works
    before the launcher ever ran. In an install (no .git beside it) the writable VFS folder is made as well."""
    import launch as la   # here, not at the top: launch imports this module
    paths = la.load_json(c.b("paths.json")) or {"dump": c.dump, "update": c.update, "eboot": c.eboot}
    if not os.path.exists(os.path.join(c.root, ".git")):
        _, err = la.ensure_vfs(c.root, c.dump, la.link_free(c.root, c.build))
        print(f"vfs folder: {err or 'ok'}", flush=True)
    p = la.write_game_ini(c.root, la.game_ini(c.root, paths, la.read_ini(c.root)["portable"], os.environ,
                                                   build=c.build), build=c.build)
    print(f"kz3.ini: {p}", flush=True)


STEPS = [   # weights: % of stage B at 4 jobs (pass 12, plan 4.11): load 20 s, ppu 30, SPU pass 103, compile 312 of 469 s
    Step("check", "Checking the game files", 1, cmd_check, always=True, after=after_record),
    Step("load", "Loading the EBOOT", 4, cmd_load, clean=lambda c: [c.b("analysis")]),
    Step("inventory", "Finding SPU programs", 1, cmd_inventory,
         clean=lambda c: [c.b("analysis", "jobmods"), c.b("analysis", "spu_images")]),
    Step("ppu-lift", "Lifting PPU code", 7, cmd_ppu_lift, clean=lambda c: [c.b("recomp")], keys=("lift",)),
    Step("spu-lift", "Lifting SPU code", 22, cmd_spu_lift, clean=lambda c: [c.b("spu")], keys=("lift",)),
    # configure re-runs in place (no clean of obj\): a runtime-only update then recompiles only what changed
    # (ninja, with prepare_sources' touch of the changed payload files); a changed toolchain empties obj\.
    Step("configure", "Configuring the build", 1, cmd_configure, clean=prepare_sources, keys=("lift", "runtime")),
    Step("compile", "Compiling", 63, cmd_compile, ninja=True, keys=("lift", "runtime")),
    Step("stamp", "Finishing", 1, lambda c: [], after=after_stamp, keys=("lift", "runtime")),
]


def cmd_shaders(c):
    """shaders.py decides itself whether the cache is warm for this exe and version."""
    return [[*c.py, c.tool("release", "shaders.py"), "--root", c.root]]


# Optional, after the build (wizard checkbox / --interactive question, kz3-install.ini [shaders] prepare): one
# unattended warm-up session; its own "[elapsed/total]" lines drive the bar. Weight 280: measured in release pass 17
# (plan 4.16) the first-boot session takes 1,020 s against ~360 s for load + lifts + compile at 16 jobs (weights 100).
SHADER_STEP = Step("shaders", "Preparing shaders", 280, cmd_shaders, always=True, ninja=True, optional=True)


def shaders_wanted(root, choice):
    """--shaders on|off|auto; auto = kz3-install.ini [shaders] prepare (default off since pass 17, as the wizard's
    "Warm it while playing" choice: plan 4.16)."""
    if choice in ("on", "off"):
        return choice == "on"
    import configparser
    cp = configparser.ConfigParser()
    cp.read(os.path.join(root, "kz3-install.ini"), encoding="utf-8")
    return cp.get("shaders", "prepare", fallback="0").strip() == "1"


def steps_for(root, choice, steps=None):
    steps = list(STEPS if steps is None else steps)
    return steps + [SHADER_STEP] if shaders_wanted(root, choice) else steps


# ----------------------------------------------------------------------------------------------------- helpers
def available_ram():
    """Available RAM in bytes, or None. /proc/meminfo on Linux, GlobalMemoryStatusEx on Windows."""
    try:
        if os.name == "nt":
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                            ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
            m = MS()
            m.len = ctypes.sizeof(MS)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
                return m.avail
            return None
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, AttributeError):
        pass
    return None


def jobs_for(avail, clang=False, cores=None):
    """Compile jobs from available RAM. GCC (the source tree's mingw-w64 cross build): build-kz3.sh's 64-chunk rule,
    (available - 8 GB) / 1.5 GB, 1..8 (GCC peak 1.32 GB per job, pass 9). llvm-mingw clang (the release's compiler,
    pass 12 from measured peaks: 785 MB per job Linux-hosted, 738 / 741 MB per job Windows-hosted under Proton in
    stages A and B; the lift's largest process 2.3 GB; Wine's own processes ~0.6 GB): (available - 3 GiB) / 1 GiB,
    1..min(cores, 16). The 3 GiB covers the lift (one process, before the compile) and the link; 1 GiB per job is
    the measured 0.74 GB plus ~35 %."""
    if not avail:
        return 1
    if clang:
        return max(1, min(cores or 8, 16, int((avail - 3 * GIB) / GIB)))
    return max(1, min(8, int((avail - 8 * GIB) / (1.5 * GIB))))


def total_ram():
    """Physical RAM in bytes, or None (GlobalMemoryStatusEx ullTotalPhys on Windows, /proc/meminfo MemTotal)."""
    try:
        if os.name == "nt":
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                            ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
            m = MS()
            m.len = ctypes.sizeof(MS)
            return m.total if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, AttributeError):
        pass
    return None


LOW_MEMORY_RAM = 8 * GIB


def low_memory_for(choice, total):
    """(on, why) for --low-memory auto|on|off: auto turns it on at <= 8 GiB of physical RAM (an 8 GB PC reports
    about 7.9 GiB), and stays off when the RAM is unknown."""
    if choice == "on":
        return True, "--low-memory on"
    if choice == "off":
        return False, "--low-memory off"
    if total is None:
        return False, "auto: physical RAM unknown"
    if total <= LOW_MEMORY_RAM:
        return True, f"auto: {total / GIB:.1f} GiB of RAM is 8 GiB or less"
    return False, f"auto: {total / GIB:.1f} GiB of RAM is more than 8 GiB"


def lower_priority(run=None):
    """Make this process and every build process it starts the system's first victim (low-memory mode). Linux: the
    OOM killer's score 1000 and nice 10, both inherited. Windows: BELOW_NORMAL priority class and low memory priority
    (its pages are trimmed before other programs'), both inherited by the children. Returns what was set."""
    done = []
    try:
        if os.name == "nt":
            import ctypes
            k = ctypes.WinDLL("kernel32", use_last_error=True)
            k.GetCurrentProcess.restype = ctypes.c_void_p   # the 64-bit pseudo handle must not be cut to an int
            h = ctypes.c_void_p(k.GetCurrentProcess())
            k.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            k.SetProcessInformation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]
            if k.SetPriorityClass(h, 0x4000):          # BELOW_NORMAL_PRIORITY_CLASS
                done.append("BELOW_NORMAL priority")
            prio = ctypes.c_ulong(2)                    # MEMORY_PRIORITY_LOW (ProcessMemoryPriority = 0)
            if k.SetProcessInformation(h, 0, ctypes.byref(prio), 4):
                done.append("low memory priority")
        else:
            with open("/proc/self/oom_score_adj", "w") as f:
                f.write("1000")
            done.append("oom_score_adj 1000")
            os.nice(10)
            done.append("nice 10")
    except (OSError, AttributeError):
        pass
    return done


# Any Windows user folder, also in CMake's object-path form (C_/Users/name/..., ':' and spaces become '_'): the user
# name is replaced, so a dump in another account's folder or a mangled build path does not put a name in the log
# (2026-10-21: a tester's log showed both after the home folder itself had been redacted).
_USERS_RX = re.compile(r"(\b[A-Za-z][:_][\\/]Users[\\/])(?!%USERPROFILE%)([^\\/\r\n\"']+)", re.IGNORECASE)


def redactor():
    """Replace the user's home folder in log text with %USERPROFILE% (both slash styles, any case on Windows, and
    CMake's mangled object-path form), then any other user folder's name with <user>."""
    homes = {h for h in (os.environ.get("USERPROFILE"), os.path.expanduser("~")) if h and len(h) > 3}
    pats = []
    for h in sorted(homes, key=len, reverse=True):
        h = h.rstrip("\\/")
        mangled = h.replace(":", "_").replace(" ", "_").replace("\\", "/")
        for v in {h, h.replace("\\", "/"), h.replace("/", "\\"), mangled}:
            pats.append(re.escape(v))
    rx = re.compile("|".join(pats), re.IGNORECASE if os.name == "nt" else 0) if pats else None
    def red(s):
        if rx is not None:
            s = rx.sub("%USERPROFILE%", s)
        return _USERS_RX.sub(lambda m: m.group(1) + "<user>", s)
    return red


def writable(d):
    try:
        os.makedirs(d, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=d, prefix=".kz3w", delete=True):
            pass
        return True
    except OSError:
        return False


def would_write(d):
    """writable() without creating anything (for --dry-run): the folder, or its nearest existing parent."""
    d = os.path.abspath(d)
    while not os.path.exists(d) and os.path.dirname(d) != d:
        d = os.path.dirname(d)
    return os.path.isdir(d) and os.access(d, os.W_OK)


def pick_log(log_dir, name, probe=None):
    """The log file: <log_dir>/<name>, else <temp>/<name> (TEMP on Windows, TMPDIR on Linux)."""
    if log_dir and (probe or writable)(log_dir):
        return os.path.join(log_dir, name), False
    # fallback flag only when a folder was asked for (pass 12: without --log-dir the header claimed "not writable")
    return os.path.join(tempfile.gettempdir(), name), bool(log_dir)


def started_at(path):
    """Start time of an earlier log: its '# started <iso>' first line, else its mtime."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.match(r"# started (\S+)", f.readline())
        if m:
            return datetime.datetime.fromisoformat(m.group(1))
    except (OSError, ValueError):
        pass
    return datetime.datetime.fromtimestamp(os.path.getmtime(path))


def rotate(path):
    """Rename an earlier log to <stem>-<YYYYmmdd-HHMMSS><ext> (its start time), so each run keeps one file."""
    if not os.path.exists(path):
        return None
    stem, ext = os.path.splitext(path)
    new = f"{stem}-{started_at(path):%Y%m%d-%H%M%S}{ext}"
    n = 1
    while os.path.exists(new):
        new = f"{stem}-{started_at(path):%Y%m%d-%H%M%S}-{n}{ext}"
        n += 1
    os.replace(path, new)
    return new


class Log:
    def __init__(self, path, redact):
        self.path, self.redact = path, redact
        self.f = open(path, "w", encoding="utf-8", newline="\n")

    def write(self, line):
        self.f.write(self.redact(line.rstrip("\r\n")) + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def copy_to_install_logs(log_path, logs_dir):
    """A copy in <install>/logs, keeping the last KEEP_LOG_COPIES firstrun-*.log."""
    try:
        os.makedirs(logs_dir, exist_ok=True)
        dst = os.path.join(logs_dir, f"firstrun-{datetime.datetime.now():%Y%m%d-%H%M%S}.log")
        shutil.copy2(log_path, dst)
        old = sorted(n for n in os.listdir(logs_dir) if n.startswith("firstrun-") and n.endswith(".log"))
        for n in old[:-KEEP_LOG_COPIES]:
            os.remove(os.path.join(logs_dir, n))
    except OSError:
        pass


def marker_path(ctx, i, step):
    return ctx.b(MARKER_DIR, f"{i + 1:02d}-{step.name}.done")


def is_done(ctx, i, step):
    try:
        with open(marker_path(ctx, i, step), encoding="utf-8") as f:
            return json.load(f).get("inputs") == ctx.fingerprint(step.keys)
    except (OSError, ValueError):
        return False


def mark_done(ctx, i, step, secs):
    p = marker_path(ctx, i, step)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump({"inputs": ctx.fingerprint(step.keys), "seconds": round(secs, 1)}, f)


def safe_clean(ctx, d):
    """Remove a step's old outputs, only inside <build> (never the dump or anything else)."""
    b = os.path.realpath(ctx.build)
    r = os.path.realpath(d)
    if r != b and r.startswith(b + os.sep) and os.path.isdir(r):
        shutil.rmtree(r)


# ------------------------------------------------------------------------------------------------------ runner
class Cancelled(Exception):
    pass


class Runner:
    def __init__(self, ctx, steps, log, out=sys.stdout, status_file=None):
        self.ctx, self.steps, self.log, self.out = ctx, steps, log, out
        self.status_file, self.status_head = status_file, ""
        self.t0 = time.monotonic()
        tot = sum(s.weight for s in steps) or 1.0
        self.frac = [s.weight / tot for s in steps]
        self.proc = None
        self.stop_requested = False

    def watch_stop(self, path, poll=0.25):
        """--stop-file: a daemon thread that ends the running tool once the file appears; run_cmd then raises
        Cancelled (reading the child's pipe cannot be interrupted, so the child has to go first)."""
        def loop():
            while not os.path.exists(path):
                time.sleep(poll)
            self.stop_requested = True
            self.stop_child()
        threading.Thread(target=loop, name="stop-file", daemon=True).start()

    def emit(self, line, to_log=True):
        print(line, file=self.out, flush=True)
        if to_log and self.log:
            self.log.write(line)
        if self.status_file and line.startswith(("LOG ", "PROGRESS ", "RESULT ")):
            self.status(line)

    def status(self, line):
        """--status-file: the LOG line plus the latest PROGRESS/RESULT line, replaced atomically (the Inno wizard
        polls it; a reader holding it open only costs one update)."""
        if line.startswith("LOG "):
            self.status_head = line
        tmp = self.status_file + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(self.status_head + "\n" + (line if line != self.status_head else "") + "\n")
            os.replace(tmp, self.status_file)
        except OSError:
            pass

    def progress(self, i, done, total):
        part = (done / total) if total else 0.0
        pct = 100.0 * (sum(self.frac[:i]) + self.frac[i] * min(1.0, part))
        self.emit(f"PROGRESS step={i + 1}/{len(self.steps)} name={self.steps[i].name} done={done} total={total} "
                  f"elapsed={int(time.monotonic() - self.t0)} pct={pct:.1f}")

    def run_cmd(self, i, argv):
        step = self.steps[i]
        self.log.write(f"$ {subprocess.list2cmdline(argv)}")
        t = time.monotonic()
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {}
        captured = []
        try:
            self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                         encoding="utf-8", errors="replace", bufsize=1, env=self.ctx.env(), **kw)
        except OSError as e:
            self.emit(f"cannot start {argv[0]}: {e}")
            return 127, captured
        try:
            for line in self.proc.stdout:
                line = line.rstrip("\r\n")
                captured.append(line)
                self.emit(line)
                m = NINJA_RE.match(line) if step.ninja else None
                if m:
                    self.progress(i, int(m.group(1)), int(m.group(2)))
            rc = self.proc.wait()
            if self.stop_requested:
                raise Cancelled()
        except KeyboardInterrupt:
            self.stop_child()
            raise Cancelled()
        finally:
            self.proc = None
        self.log.write(f"# exit {rc} after {time.monotonic() - t:.1f} s")
        return rc, captured

    def stop_child(self):
        p = self.proc
        if p and p.poll() is None:
            if os.name == "nt":   # the whole tree: cmake -> ninja -> clang would outlive a terminate()
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()

    def run(self):
        ctx, n = self.ctx, len(self.steps)
        try:
            for i, step in enumerate(self.steps):
                if self.stop_requested:
                    raise Cancelled()
                if not step.always and is_done(ctx, i, step):
                    self.log.write(f"## step {i + 1}/{n} {step.name}: done earlier, skipped")
                    self.progress(i, 1, 1)
                    continue
                self.log.write(f"## step {i + 1}/{n} {step.name}: {step.label}")
                t = time.monotonic()
                for d in step.clean(ctx):
                    safe_clean(ctx, d)
                cmds = step.commands(ctx)
                self.progress(i, 0, len(cmds) or 1)
                failed = False
                for k, argv in enumerate(cmds):
                    rc, out = self.run_cmd(i, argv)
                    if rc != 0 and step.optional:
                        self.emit(f"step {step.name} did not finish (exit {rc}); optional, the port is built")
                        failed = True
                        break
                    if rc != 0:
                        self.emit(f"step {step.name} failed: exit {rc}")
                        return self.result(False, step.name)
                    if step.name == "check":
                        self.take_check(out)
                        bad = self.version_mismatch()
                        if bad:
                            self.emit(bad)
                            return self.result(False, step.name)
                    if not step.ninja:
                        self.progress(i, k + 1, len(cmds))
                if step.after and not failed:
                    step.after(ctx)
                if not cmds or step.ninja or failed:
                    self.progress(i, 1, 1)
                if failed:
                    continue
                self.log.write(f"## step {step.name} done in {time.monotonic() - t:.1f} s")
                mark_done(ctx, i, step, time.monotonic() - t)
            return self.result(True, "-")
        except (Cancelled, KeyboardInterrupt):
            self.stop_child()
            self.log.write("## cancelled: the running step left no marker, the next start resumes here")
            self.emit(f"RESULT ok=0 step=cancelled elapsed={int(time.monotonic() - self.t0)}")
            return 130

    def take_check(self, out):
        """Keep the check's facts (EBOOT size/hash) so a changed input invalidates the later markers."""
        try:
            j = json.loads("\n".join(out))
            self.ctx.facts = j.get("facts", {})
            if not self.ctx.eboot:   # the checker picked it (update's EBOOT.elf, else the dump's)
                self.ctx.eboot = self.ctx.facts.get("eboot_elf", {}).get("path", "")
        except (ValueError, AttributeError):
            pass

    def version_mismatch(self):
        """A message when the decrypted EBOOT is another version than the one this release is built for (pass 12):
        the lift would succeed and the game would not start. "" when it matches or the check did not say."""
        got = (self.ctx.facts.get("eboot_elf") or {}).get("version")
        want = EBOOT_VERS[self.ctx.ver]["check"]
        if not got or got == want:
            return ""
        return (f"the decrypted EBOOT is the {got} one, this release is built for the {want} EBOOT: "
                f"choose the {want} EBOOT.elf (the decrypted EBOOT.BIN of that version)")

    def result(self, ok, step):
        self.emit(f"RESULT ok={int(ok)} step={step} elapsed={int(time.monotonic() - self.t0)}")
        return 0 if ok else 1


def header(ctx, log_path, fallback):
    du = shutil.disk_usage(ctx.build if os.path.isdir(ctx.build) else ctx.root)
    ram = available_ram()
    return [f"# started {datetime.datetime.now().isoformat(timespec='seconds')}",
            f"# firstrun.py, Python {platform.python_version()}, {platform.platform()}",
            f"# cores {os.cpu_count()}, available RAM {ram / GIB:.1f} GiB" if ram else f"# cores {os.cpu_count()}",
            f"# free disk {du.free / GIB:.1f} GiB on the build drive, jobs {ctx.jobs}",
            f"# root {ctx.root}", f"# build {ctx.build}",
            f"# toolchain {ctx.toolchain or 'cmake/mingw-w64.cmake (cross)'}, cmake {ctx.cmake}, "
            f"ninja {ctx.ninja or '(PATH)'}, python {ctx.python}",
            f"# dump {ctx.dump}", f"# update {ctx.update}", f"# eboot {ctx.eboot or '(auto)'}",
            f"# tree built for the {EBOOT_VERS[ctx.ver]['check']} EBOOT"] + \
        ([f"# log folder not writable, using {os.path.dirname(log_path)}"] if fallback else [])


def dry_run(ctx, steps, log_path, fallback, out=sys.stdout):
    tot = sum(s.weight for s in steps) or 1.0
    if not ctx.eboot:
        ctx.eboot = "<EBOOT.elf the check picks>"
    print(f"LOG path={log_path}{' (fallback)' if fallback else ''}", file=out)
    for i, s in enumerate(steps):
        state = "always" if s.always else ("done" if is_done(ctx, i, s) else "pending")
        print(f"{i + 1}/{len(steps)} {s.name:10} {100 * s.weight / tot:5.1f}%  {state:7} {s.label}", file=out)
        for argv in s.commands(ctx):
            print("    " + subprocess.list2cmdline(argv), file=out)
    return 0


# ------------------------------------------------------------------------------------- portable zip: text UI
NOTICE_TEXT = f"""{names.PRODUCT}: first run
This checks your own Killzone 3 disc dump (BCUS98234), the 1.14 update and the decrypted EBOOT, then lifts and
compiles the game on this computer (5-30 minutes; Ctrl+C stops it, the next start resumes). Nothing is downloaded
and nothing of the game is copied; the files it generates are your own and must not be shared."""
UPDATE_HELP = """The 1.14 update is required: use the update from your own console, or the one installed in RPCS3 (see
RPCS3's documentation). The port also needs EBOOT.elf, the decrypted
form of the update's USRDIR/EBOOT.BIN, made from your own copy (this project includes no decryption tools)."""
# ASSUMPTION: RPCS3's usual game folders (Linux XDG config; Windows: RPCS3 is portable, so only a guess next to the
# player's profile). Shown as a default only if it exists.
RPCS3_UPDATE_GUESSES = [os.path.join("~", ".config", "rpcs3", "dev_hdd0", "game", "BCUS98234"),
                        os.path.join("~", "RPCS3", "dev_hdd0", "game", "BCUS98234")]


def port_version(root):
    """The payload's VERSION.txt (assemble_payload.py --version), else 0.0.0-dev (a source tree)."""
    try:
        with open(os.path.join(root, "VERSION.txt"), encoding="utf-8") as f:
            return f.read().strip() or "0.0.0-dev"
    except OSError:
        return "0.0.0-dev"


def write_install_ini(root, portable, check_updates, shaders=None):
    """kz3-install.ini, the same keys the Inno script writes ([install] portable, [online] check_updates,
    [shaders] prepare)."""
    import configparser
    cp = configparser.ConfigParser()
    p = os.path.join(root, "kz3-install.ini")
    cp.read(p, encoding="utf-8")
    for sec in ("install", "online", "shaders"):
        if not cp.has_section(sec):
            cp.add_section(sec)
    cp.set("install", "portable", "1" if portable else "0")
    cp.set("online", "check_updates", "1" if check_updates else "0")
    if shaders is not None:
        cp.set("shaders", "prepare", "1" if shaders else "0")
    with open(p, "w", encoding="utf-8") as f:
        cp.write(f)
    return p


def _check_files(dump, update, eboot):
    """Run firstrun_check in-process; -> (exit code, its text lines)."""
    import contextlib
    import io
    sys.path.insert(0, HERE)
    import firstrun_check
    buf = io.StringIO()
    args = ["--dump", dump] + (["--update", update] if update else []) + (["--eboot", eboot] if eboot else [])
    with contextlib.redirect_stdout(buf):
        rc = firstrun_check.main(args)
    return rc, buf.getvalue()


def _version_check(current):
    sys.path.insert(0, HERE)
    import version_check
    return version_check.check(current)


def interactive(a, root, log_path, notes, ask=input, out=sys.stdout, check=_check_files, vcheck=_version_check):
    """Fill a.dump/a.update/a.eboot from the player's answers; True = build now, False = quit. Lines for the install
    log go to `notes` (main writes them, redacted, after the log header)."""
    def say(*t):
        print(*t, file=out, flush=True)

    def yes(q, default):
        r = ask(f"{q} [{'Y/n' if default else 'y/N'}] ").strip().lower()
        return default if not r else r.startswith("y")

    def folder(q, default, need_dir=True):
        while True:
            r = ask(f"{q}" + (f" [{default}]" if default else "") + ": ").strip().strip('"')
            r = os.path.expanduser(r or default or "")
            if not r and not need_dir:
                return ""
            if r and (os.path.isdir(r) if need_dir else os.path.isfile(r)):
                return os.path.abspath(r)
            say(f"  not found: {r or '(empty)'}")

    prev = {}
    try:
        with open(os.path.join(os.path.abspath(a.build or os.path.join(root, "build")), "paths.json"),
                  encoding="utf-8") as f:
            prev = json.load(f)
    except (OSError, ValueError):
        pass
    guess = next((os.path.expanduser(g) for g in RPCS3_UPDATE_GUESSES if os.path.isdir(os.path.expanduser(g))), "")
    say(NOTICE_TEXT)
    say(f"Log: {log_path}")
    say("")
    imp, rp = _rpcs3_offer(a, say, yes, notes)
    dump = a.dump or rp.get("disc", "") or prev.get("dump", "")
    upd = a.update or rp.get("update", "") or prev.get("update", "") or guess
    elf = a.eboot or ""
    while True:
        dump = folder("Disc dump folder (the one holding PS3_GAME)", dump)
        upd = folder("1.14 update folder (dev_hdd0/game/BCUS98234)", upd)
        auto = os.path.join(upd, "USRDIR", "EBOOT.elf")
        elf = elf or (prev.get("eboot", "") if prev.get("update") == upd else "") or \
            (auto if os.path.isfile(auto) else "")
        elf = folder("Decrypted EBOOT (EBOOT.elf; Enter = find it)", elf, need_dir=False)
        while True:
            rc, text = check(dump, upd, elf)
            say(text.rstrip())
            notes.append("# interactive check")
            notes.extend(text.rstrip().splitlines())
            if rc == 0:
                break
            guide = _rpcs3_eboot_guide(upd) if "E1" in text else ""
            if guide:
                say("")
                say(guide)
                notes.append("# RPCS3 decrypt guidance shown")
            elif "U0" in text or "U1" in text or "U2" in text or "E1" in text:
                say("")
                say(UPDATE_HELP)
            r = ask(RPCS3_TEXT.CHECK_AGAIN if guide else
                    "Fix the FAIL lines, then Enter = check again, c = change the folders, q = quit: ").strip().lower()
            if r.startswith("q"):
                return False
            if r.startswith("c"):
                break
        if rc == 0:
            break
    if not elf and os.path.isfile(auto):      # made by RPCS3 while the check was repeated
        elf = auto
    a.dump, a.update, a.eboot = dump, upd, elf
    say("")
    if imp is not None and rp.get("saves"):
        c, k = imp.import_saves(rp, _save_root(a, root), yes, say)
        notes.append(f"# RPCS3 saves: copied {c}, kept {k}")
        say("")
    fd = getattr(a, "font_dir", "") or prev.get("font_dir", "") or font_guess(upd)
    while True:
        r = ask("Your PS3 firmware's dev_flash/data/font folder, e.g. from an emulator install (optional: the game's "
                "text then uses the PS3's own fonts; Enter = " + (f"{fd}" if fd else "skip") + ", - = none): ")
        r = r.strip().strip('"')
        r = "" if r == "-" else os.path.expanduser(r or fd)
        if not r or has_fonts(r):
            break
        say(f"  no SCE-PS3-*.TTF fonts in: {r}")
        fd = ""
    a.font_dir = os.path.abspath(r) if r else ""
    a.font_dir_set = True     # "-" / no answer = none, not the last run's folder
    notes.append(f"# firmware fonts: {'given' if a.font_dir else 'none'}")
    online = False
    if yes("Check GitHub for a newer version of the port now? (one anonymous request; nothing about you or the game "
           "is sent)", False):
        r = vcheck(port_version(root))
        notes.append(f"# version check (player asked): status={r['status']} current={r['current']} "
                     f"latest={r['latest'] or '-'} url={r['url'] or '-'} {r.get('detail', '')}".rstrip())
        say({"newer": f"  A newer version {r['latest']} is out: {r['url']} (download it there if you want it)",
             "current": f"  This is the latest version ({r['current']})."}.get(
            r["status"], "  Could not reach GitHub; carrying on (one line in the log)."))
        online = yes("Check like this at every start (at most once a day)?", False)
    choice = getattr(a, "shaders", "auto")
    shaders = choice != "off" and (choice == "on" or yes(
        "Warm the shader cache now? The game runs minimised and plays its opening and the start of the first level "
        "by itself (about 17 minutes the first time). n warms it while you play: the first minutes stutter", False))
    write_install_ini(root, a.portable, online, shaders)
    a.shaders = "on" if shaders else "off"
    say("Saves and settings stay in this folder (portable)." if a.portable else "")
    return yes("Start the build now? (usually 5-30 minutes)", True)


try:
    import rpcs3_text as RPCS3_TEXT   # every player-facing string of the RPCS3 import
except ImportError:                   # a bundle without the import files keeps the plain prompts
    RPCS3_TEXT = None


def _rpcs3_import():
    try:
        import rpcs3_import
        return rpcs3_import
    except ImportError:
        return None


def _rpcs3_offer(a, say, yes, notes):
    """(module, found) when --rpcs3 asks for it (off/absent = nothing) and an RPCS3 folder holds the game; the player
    confirms the folders as the starting answers. (None, {}) otherwise: the plain prompts stay as they are."""
    want = getattr(a, "rpcs3", "off") or "off"
    imp = _rpcs3_import() if want != "off" else None
    if imp is None:
        return None, {}
    found = imp.discover(None if want == "auto" else want)
    if not found["ok"]:
        return None, {}
    for line in imp.describe(found):
        say(line)
    notes.append(f"# RPCS3 folder found: disc={bool(found['disc'])} update={bool(found['update'])} "
                 f"saves={len(found['saves'])}")
    if (found["disc"] or found["update"]) and not yes(RPCS3_TEXT.USE_RPCS3, True):
        found = dict(found, disc="", update="")
    say("")
    return imp, found


def _rpcs3_eboot_guide(upd):
    imp = _rpcs3_import()
    return imp.eboot_guidance(upd) if imp and upd else ""


def _save_root(a, root):
    """Where the port keeps saves and settings (the same rule as launch.data_dir)."""
    appdata = os.environ.get("APPDATA", "")
    return root if getattr(a, "portable", False) or not appdata else os.path.join(appdata, names.DATA_FOLDER)


def has_fonts(d):
    """The folder holds the firmware's SCE-PS3-*.TTF (what patch 5005 loads from KZ3_FONT_DIR)."""
    try:
        return any(n.upper().startswith("SCE-PS3-") and n.upper().endswith(".TTF") for n in os.listdir(d))
    except OSError:
        return False


def font_guess(update):
    """An RPCS3 update folder <rpcs3>/dev_hdd0/game/<id> -> <rpcs3>/dev_flash/data/font when it holds the fonts."""
    if not update:
        return ""
    base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(update))))
    d = os.path.join(base, "dev_flash", "data", "font")
    return d if has_fonts(d) else ""


CLEAN_DIRS = ("obj", "recomp", "spu", "analysis")     # the lift and the objects; kz3.exe is already copied out
CLEAN_STEPS = ("load", "inventory", "ppu-lift", "spu-lift", "configure", "compile")   # their markers go with them


def tree_size(d):
    n = 0
    for base, _, files in os.walk(d):
        for fn in files:
            try:
                n += os.lstat(os.path.join(base, fn)).st_size
            except OSError:
                pass
    return n


def cleanup_size(ctx):
    return sum(tree_size(ctx.b(d)) for d in CLEAN_DIRS)


def cleanup_build(ctx, steps):
    """Delete the build intermediates after a finished build (pass 26, optional): -> bytes freed. Kept: kz3.exe,
    kz3.ini, paths.json, build-stamp.json, the sources record, fonts, cache, logs and the check/stamp markers, i.e.
    what the launcher's start and rebuild checks read. The markers of the steps whose outputs went are removed, so a
    later rebuild (a new port version, "Verify and rebuild") runs them again from the start: a full build."""
    if not os.path.isfile(ctx.b("kz3.exe")):
        return 0
    freed = cleanup_size(ctx)
    for d in CLEAN_DIRS:
        safe_clean(ctx, ctx.b(d))
    for i, st in enumerate(steps):
        if st.name in CLEAN_STEPS:
            try:
                os.remove(marker_path(ctx, i, st))
            except OSError:
                pass
    stamp = load_stamp(ctx)
    if stamp is not None:
        stamp["cleaned"] = datetime.datetime.now().isoformat(timespec="seconds")
        with open(ctx.b("build-stamp.json"), "w", encoding="utf-8") as f:
            json.dump(stamp, f, indent=2)
    return freed


def load_stamp(ctx):
    try:
        with open(ctx.b("build-stamp.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def cpu_changed(ctx, steps):
    """The last build's -DKZ3_SPU_FMA differs from this CPU's answer (an install moved to another PC): drop the
    configure/compile/stamp markers so they run again. -> True when it did."""
    stamp = load_stamp(ctx)
    if not stamp or "spu_fma" not in stamp or ctx.spu_fma is None or stamp["spu_fma"] == ctx.spu_fma:
        return False
    for i, st in enumerate(steps):
        if st.name in ("configure", "compile", "stamp"):
            try:
                os.remove(marker_path(ctx, i, st))
            except OSError:
                pass
    return True


class ConsoleBar:
    """File-like sink for the runner's lines (portable zip): PROGRESS lines become one bar with the step's label,
    the count for the compile and the elapsed time; RESULT ends it; the steps' own output goes only to the log."""
    WIDTH = 30

    def __init__(self, out=sys.stdout, steps=None, tty=None):
        self.out, self.buf, self.last, self.step = out, "", "", None
        self.labels = {s.name: s.label for s in (steps or STEPS + [SHADER_STEP])}
        self.tty = out.isatty() if tty is None else tty
        self.shown_pct = -10.0

    def write(self, s):
        self.buf += s
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.line(line)
        return len(s)

    def flush(self):
        self.out.flush()

    def line(self, ln):
        kv = dict(x.split("=", 1) for x in ln.split()[1:] if "=" in x)
        if ln.startswith("LOG "):
            self.out.write(f"Log: {kv.get('path', '')}\n")
        elif ln.startswith("PROGRESS "):
            pct, name = float(kv.get("pct", 0)), kv.get("name", "")
            el = int(kv.get("elapsed", 0))
            label = self.labels.get(name, name)
            if name == "compile" and kv.get("total", "1") not in ("0", "1"):
                label += f" {int(kv['done']):,} / {int(kv['total']):,}"
            elif name == "shaders" and kv.get("total", "1") not in ("0", "1"):
                left = max(0, int(kv["total"]) - int(kv["done"]))
                label += f" (about {left // 60}:{left % 60:02d} left)"
            fill = int(self.WIDTH * pct / 100)
            text = f"[{'#' * fill}{'.' * (self.WIDTH - fill)}] {pct:5.1f}%  {label}  {el // 60}:{el % 60:02d}"
            if self.tty:
                self.out.write("\r" + text.ljust(len(self.last)))
            elif name != self.step or pct - self.shown_pct >= 5 or pct >= 100:
                self.out.write(text + "\n")
                self.shown_pct = pct
            self.last, self.step = text, name
        elif ln.startswith("RESULT "):
            el = int(kv.get("elapsed", 0))
            end = "\n" if self.tty and self.last else ""
            if kv.get("ok") == "1":
                self.out.write(f"{end}Done in {el // 60}:{el % 60:02d}: the port is built.\n")
            elif kv.get("step") == "cancelled":
                self.out.write(f"{end}Stopped. Run it again to resume where it stopped.\n")
            else:
                self.out.write(f"{end}The build stopped at step '{kv.get('step')}'. The log has the details; attach "
                               f"it to an issue: {names.ISSUES_URL}\n")
        self.out.flush()


def _ask_cleanup(ctx, ask, out):
    mib = cleanup_size(ctx) / 2**20
    r = ask(f"Delete the build's intermediate files ({mib:.0f} MiB)? The game keeps working; a later port update or "
            "\"Verify and rebuild\" then builds from the start (usually 5-30 minutes). [y/N] ").strip().lower()
    return r.startswith("y")


def _interrupt(*_):
    raise KeyboardInterrupt()


def main(argv=None, steps=None, out=sys.stdout, ask=input):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", default="")
    ap.add_argument("--update", default="")
    ap.add_argument("--eboot", default="")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--build")
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--low-memory", choices=("auto", "on", "off"), default="auto",
                    help="one compile job and one lift worker, low priority; auto = on at 8 GiB of RAM or less")
    ap.add_argument("--toolchain", default="")
    ap.add_argument("--log-dir", default="")
    ap.add_argument("--log-name", default=DEFAULT_LOG_NAME)
    ap.add_argument("--status-file", default="")
    ap.add_argument("--stop-file", default="")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--rpcs3", default="auto", metavar="DIR|auto|off",
                    help="with --interactive: look in the player's RPCS3 folder (auto = the usual places) for the disc, "
                         "the 1.14 update and saves, and offer them as the answers; off = the plain prompts")
    ap.add_argument("--portable", action="store_true")
    ap.add_argument("--bar", action="store_true")
    ap.add_argument("--font-dir", default="", help="the PS3 firmware's dev_flash/data/font folder (optional)")
    ap.add_argument("--font-copy", choices=("yes", "no"), default="yes",
                    help="copy the font folder into the install (default) or keep pointing at it")
    ap.add_argument("--lang", default="", help="KZ3_LANG for kz3.ini (optional)")
    ap.add_argument("--window-size", default="", help="KZ3_WINDOW_SIZE for kz3.ini (optional)")
    ap.add_argument("--spu-fma", choices=("auto", "on", "off"), default="auto",
                    help="-DKZ3_SPU_FMA; auto = this CPU has AVX + FMA3 (cpu_fma)")
    ap.add_argument("--cleanup", choices=("ask", "yes", "no"), default="ask",
                    help="delete the build intermediates after a finished build; ask = only with --interactive (default no)")
    ap.add_argument("--shaders", choices=("auto", "on", "off"), default="auto",
                    help="the optional Prepare shaders step after the build (auto = kz3-install.ini, default off)")
    a = ap.parse_args(argv)
    if a.font_dir == "-":     # the wizard's empty font page: none, not the last build's folder
        a.font_dir, a.font_dir_set = "", True
    elif a.font_dir == "auto":   # the wizard's Recommended type (step 9s): RPCS3's folder beside the update, if any
        a.font_dir, a.font_dir_set = font_guess(a.update), True
    if not a.dump and not a.interactive:
        ap.error("--dump is required (or --interactive)")
    root = os.path.abspath(a.root)
    notes, quit_early = [], False
    if a.interactive and not a.dry_run:
        if not interactive(a, root, pick_log(a.log_dir or None, a.log_name)[0], notes, ask=ask, out=out):
            quit_early = True
    steps = steps_for(root, a.shaders) if steps is None else steps
    if (a.interactive or a.bar) and not a.dry_run:
        out = ConsoleBar(out, steps)
    bundled = payload_tools(root)
    toolchain = os.path.abspath(a.toolchain) if a.toolchain else bundled.get("llvm_mingw", "")
    ctx = Ctx(root=root, build=os.path.abspath(a.build or os.path.join(root, "build")),
              dump=os.path.abspath(a.dump), update=os.path.abspath(a.update) if a.update else "",
              eboot=os.path.abspath(a.eboot) if a.eboot else "",
              jobs=a.jobs or jobs_for(available_ram(), clang=bool(toolchain), cores=os.cpu_count()),
              toolchain=toolchain,
              python=bundled.get("python", sys.executable), cmake=bundled.get("cmake", "cmake"),
              ninja=bundled.get("ninja", ""), font_dir=os.path.abspath(a.font_dir) if a.font_dir else "",
              lang=a.lang, window_size=a.window_size, font_copy=a.font_copy == "yes")
    ctx.low_memory, low_why = low_memory_for(a.low_memory, total_ram())
    if ctx.low_memory:
        ctx.jobs = 1
        low_why += "; one compile job, one lift worker"
        if not a.dry_run:
            low_why += "; " + (", ".join(lower_priority()) or "no priority change possible")
    notes.append(f"# low-memory build: {'ON' if ctx.low_memory else 'off'} ({low_why})")
    if a.spu_fma == "auto":
        ctx.spu_fma, cpu_how = cpu_fma(toolchain)
    else:
        ctx.spu_fma, cpu_how = a.spu_fma == "on", "--spu-fma"
    notes.append(f"# cpu: AVX + FMA3 {dict([(True, 'yes'), (False, 'no'), (None, 'unknown')])[ctx.spu_fma]} "
                 f"({cpu_how}) -> " + ("KZ3_SPU_FMA=OFF (slower SPU code, runs on this CPU)" if ctx.spu_fma is False
                                       else "the default build (KZ3_SPU_FMA=ON)"))
    try:   # the player's choices of the last run (a resume or rebuild passes only the three game paths)
        with open(ctx.b("paths.json"), encoding="utf-8") as f:
            p = json.load(f)
        for k in ("font_dir", "font_src", "lang", "window_size"):
            if not getattr(ctx, k) and p.get(k) and not (k in ("font_dir", "font_src") and getattr(a, "font_dir_set", False)):
                setattr(ctx, k, p[k])
    except (OSError, ValueError):
        pass
    try:   # facts of the last check, so --dry-run and resume see the same fingerprint before the check re-runs
        with open(ctx.b("paths.json"), encoding="utf-8") as f:
            p = json.load(f)
        if (p.get("dump"), p.get("update")) == (ctx.dump, ctx.update) and (not ctx.eboot or ctx.eboot == p.get("eboot")):
            ctx.facts, ctx.eboot = p.get("facts", {}), p.get("eboot", "")
    except (OSError, ValueError):
        pass
    log_path, fallback = pick_log(a.log_dir or None, a.log_name, would_write if a.dry_run else None)
    if a.dry_run:
        return dry_run(ctx, steps, log_path, fallback, out)
    rotate(log_path)
    log = Log(log_path, redactor())
    r = Runner(ctx, steps, log, out, a.status_file or None)
    r.emit(f"LOG path={log_path}", to_log=False)
    if a.stop_file:
        r.watch_stop(a.stop_file)
    for line in header(ctx, log_path, fallback) + notes:
        log.write(line)
    if quit_early:
        log.write("# the player quit before the build")
        log.close()
        return 1
    if os.name != "nt":
        signal.signal(signal.SIGTERM, _interrupt)
    if cpu_changed(ctx, steps):
        log.write("# the CPU's AVX + FMA3 answer changed since the last build: configure and compile run again")
    try:
        rc = r.run()
        clean = a.cleanup == "yes" or (a.cleanup == "ask" and a.interactive and rc == 0 and _ask_cleanup(ctx, ask, out))
        if rc == 0 and clean:
            freed = cleanup_build(ctx, steps)
            r.emit(f"cleanup: removed the build intermediates, {freed / 2**20:.0f} MiB freed")
    except Exception as e:   # always end with a RESULT line: the Inno wizard waits for one
        r.emit(f"internal error: {e!r}")
        rc = r.result(False, "internal")
    finally:
        log.close()
    copy_to_install_logs(log_path, os.path.join(root, "logs"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
