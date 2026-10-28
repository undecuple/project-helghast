#!/usr/bin/env python3
"""Assemble the release payload folder from the repository, with an allowlist,
a scrub and a manifest. Standard library only, Python 3.8+.

    assemble_payload.py --out DIR [--repo DIR] [--replace] [--version X.Y.Z] [--launcher EXE] [--toolchain CACHE]
                        [--fonts DIR]
    assemble_payload.py --dry-run [--repo DIR] [--toolchain CACHE] [--fonts DIR]

What goes in (the allowlist, RULES below; nothing else is ever copied):
  kz3-setup/      tools/release/{firstrun,firstrun_check,iso9660,version_check,crash_report,launch,lift_fixups,names}.py, eboot_sha256.json and the lift tools
                  firstrun.py calls
                  (tools/jobmods.py, tools/elf_info.py, tools/spu/{lift_spu,gen_spu_register,kz3ver}.py)
  re/data/        the three SPU seed/map tables the SPU lift reads (recovered numbers)
  port/           port/kz3, port/spu, port/vfs, port/net (no tests)
  cmake/          cmake/*.cmake (the cross toolchain file firstrun.py falls back to)
  ps3recomp/      work/ps3recomp-patched (plan item 1: the patched toolkit, tools/ps3recomp-patched.sh) without
                  fw_spu/ (Sony firmware modules and code lifted from them: never shipped), scratch/, docs/, packaging/,
                  .github/
  licenses/       our LICENSE, ps3recomp's LICENSE, at3_standalone's COPYING.LGPLv2.1
  fonts/          Rajdhani-Medium.ttf and Rajdhani-Bold.ttf (SIL OFL 1.1) from --fonts DIR (default
                  <repo>/work/release/fonts, filled by installer_art.py --fetch-fonts; never committed), each checked
                  against its SHA-256 pinned in installer/art/art.json, and their OFL text as licenses/Rajdhani-OFL.txt.
                  firstrun.py and the launcher copy them to build/fonts/, next to kz3.exe: the runtime's error notice
                  loads them privately from <exe dir>/fonts (RSX pass 38). Missing or changed fonts refuse the payload.
  Project Helghast.exe  with --launcher EXE: the launcher built by tools/release/build_launcher.sh (pass 4)
  README.md, NOTICE.md (from public/), run.cmd (installer/run.cmd, the portable zip's entry; the Inno script
                  excludes it)
The toolchain (compiler, CMake, Ninja, embeddable Python) is not in the repository. With --toolchain CACHE (the
folder fetch_toolchain.py filled from tools/release/toolchain.json, pass 8) each zip entry's unpacked tree goes to
toolchain/<dest>/ (minus its "prune" globs) and the licence texts to licenses/<group>/ (the entry's license_globs
and the group's licence-file entries). Refused: a TO-FILL hash, a cache whose fetched.json does not hold the
manifest's hash for every entry, a group without a licence text. Toolchain files get the game-data name and magic
checks only (third-party binaries are large and their docs may hold example paths). manifest.json then also lists
the components (name, version, URL, SHA-256, licence).

Scrub (any hit refuses the whole payload, nothing is written): game-data names and extensions (EBOOT.*, EBOOT*.BIN,
PARAM.SFO, .psarc .elf .self .sprx .prx .edat .sdat .pkg .rap .pup .core .corestream .bik .at3), game/ELF magics at
the file start (SCE, PSAR, RTTIBin, PSF, ELF, NPD), home paths (Linux home folders, Windows user folders), private hosts,
secrets, binaries over 2 MB and any file over 8 MB. Symlinks inside the copied trees are skipped (never followed).

VERSION.txt holds --version (default 0.0.0-dev): firstrun.py shows it and the version check compares against it.
manifest.json at the payload root: every file with its size and SHA-256, the count and total size, the repository
commit and the toolkit's patchset id when known. No timestamps: the same sources give the same manifest.

Exit 0 = ok, 1 = scrub refused or a source is missing, 2 = usage.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import names as rnames  # noqa: E402  (product name, file names, links: tools/release/names.py)

HERE = os.path.dirname(os.path.abspath(__file__))
PUBLIC_DIR = "public/"             # in the private tree; the exported public repo has the same files at its root
TOOLKIT = "work/ps3recomp-patched"  # "/" on every OS: it is printed, matched in dest names, and os.path.join accepts it

# (source relative to the repo, destination in the payload, kind, options)
#   kind "file": one file; "tree": a folder, walked without following symlinks
#   options: include = filename globs (default all), exclude_dirs = folder names skipped anywhere in the tree,
#            skip_tests = drop test_* files and tests/ folders
RULES = [
    ("tools/release/firstrun.py", "kz3-setup/release/firstrun.py", "file", {}),
    ("tools/release/firstrun_check.py", "kz3-setup/release/firstrun_check.py", "file", {}),
    ("tools/release/iso9660.py", "kz3-setup/release/iso9660.py", "file", {}),   # firstrun: a decrypted disc ISO as the game source
    ("tools/release/version_check.py", "kz3-setup/release/version_check.py", "file", {}),
    ("tools/release/crash_report.py", "kz3-setup/release/crash_report.py", "file", {}),
    ("tools/release/launch.py", "kz3-setup/release/launch.py", "file", {}),
    ("tools/smoke/pso_template.py", "kz3-setup/pso_template.py", "file", {}),   # the launcher's export check
    ("tools/release/lift_fixups.py", "kz3-setup/release/lift_fixups.py", "file", {}),
    ("tools/release/eboot_sha256.json", "kz3-setup/release/eboot_sha256.json", "file", {}),
    ("tools/release/shaders.py", "kz3-setup/release/shaders.py", "file", {}),
    ("tools/release/names.py", "kz3-setup/release/names.py", "file", {}),
    ("tools/release/support.py", "kz3-setup/release/support.py", "file", {}),
    ("tools/release/support_text.py", "kz3-setup/release/support_text.py", "file", {}),
    ("tools/release/rpcs3_import.py", "kz3-setup/release/rpcs3_import.py", "file", {}),
    ("tools/release/rpcs3_text.py", "kz3-setup/release/rpcs3_text.py", "file", {}),
    ("tools/release/rpcs3_locate.py", "kz3-setup/release/rpcs3_locate.py", "file", {}),
    # the warm-up clears its own profile's continue record with it (shaders.py prepare_profile)
    ("tools/assets/profile_edit.py", "kz3-setup/assets/profile_edit.py", "file", {}),
    # profile_edit.py reads the chapter-1 level rows from <root>/re/data (its LEVELS_TSV, two folders above assets/)
    ("re/data/campaign_levels.tsv", "re/data/campaign_levels.tsv", "file", {}),
    ("tools/jobmods.py", "kz3-setup/jobmods.py", "file", {}),
    ("tools/elf_info.py", "kz3-setup/elf_info.py", "file", {}),
    ("tools/spu/lift_spu.py", "kz3-setup/spu/lift_spu.py", "file", {}),
    ("tools/spu/gen_spu_register.py", "kz3-setup/spu/gen_spu_register.py", "file", {}),
    ("tools/spu/kz3ver.py", "kz3-setup/spu/kz3ver.py", "file", {}),
    # recovered numbers the SPU lift reads from <root>/re/data (kz3ver.py ROOT = two folders above kz3-setup/spu)
    ("re/data/spu_map_114.tsv", "re/data/spu_map_114.tsv", "file", {}),
    ("re/data/spu_seeds_114.tsv", "re/data/spu_seeds_114.tsv", "file", {}),
    ("re/data/spu_coverage_seeds.tsv", "re/data/spu_coverage_seeds.tsv", "file", {}),
    ("port/kz3", "port/kz3", "tree", {"skip_tests": True}),
    ("port/spu", "port/spu", "tree", {"skip_tests": True}),
    ("port/vfs", "port/vfs", "tree", {"skip_tests": True}),
    ("port/net", "port/net", "tree", {"skip_tests": True}),   # Medius/NP client; CMakeLists ../net/*.c (pass 16)
    ("cmake", "cmake", "tree", {"include": ["*.cmake"]}),
    (TOOLKIT, "ps3recomp", "tree", {"exclude_dirs": ["fw_spu", "scratch", "docs", "packaging", ".github"]}),
    ("public/LICENSE", "licenses/LICENSE.txt", "file", {}),
    (TOOLKIT + "/LICENSE", "licenses/ps3recomp-LICENSE.txt", "file", {}),
    (TOOLKIT + "/third_party/at3_standalone/COPYING.LGPLv2.1", "licenses/at3_standalone-COPYING.LGPLv2.1.txt",
     "file", {}),
    ("public/README.md", "README.md", "file", {}),
    ("public/NOTICE.md", "NOTICE.md", "file", {}),
    ("installer/run.cmd", "run.cmd", "file", {}),
]
FONTS = ("Rajdhani-Medium.ttf", "Rajdhani-Bold.ttf")   # the runtime's notice fonts (port/kz3/main.cpp, RSX pass 38)
FONT_LICENSE = ("OFL-rajdhani.txt", "licenses/Rajdhani-OFL.txt")   # installer_art.py's name -> payload name
SDL3_DEST = ("runtime/SDL3.dll", "licenses/SDL3-LICENSE.txt")   # payload names: the DLL (firstrun.place_sdl3 copies it next to
SDL3_SRC = ("SDL3.dll", "LICENSE.txt")                          # kz3.exe) and SDL's zlib licence; names inside --sdl3 DIR
SDL3_MAX_BYTES = 16 << 20                                       # a DLL is exempt from the 2 MiB binary cap, not from this one
LAUNCHER = rnames.LAUNCHER_EXE   # --launcher: built from installer/launcher/launcher.c (pass 4), scrubbed like the rest
TEMPLATE_SRC = "installer/pso-template.bin"   # optional release input: the shared pipeline template (RSX pass 67)
TEMPLATE_DEST = "pso-template.bin"            # payload root; the launcher sets LD_PSO_TEMPLATE to it when present
TEMPLATE_CHECK = "tools/smoke/pso_template.py"
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "smoke"))
import pso_template as _pt  # noqa: E402  (format limits only; the content check is `pso_template.py check`)
TEMPLATE_MAX_BYTES = _pt.HDR.size + _pt.MAX_RECS * _pt.REC.size   # explicit cap: header + MAX_RECS records


def is_template(rel):
    """The one rule for the shipped template (assemble and pack_zip): exempt from scrub_file (a binary over MAX_BINARY is
    fine), gated instead by `pso_template.py check` (strict: no bytes after the last record) and TEMPLATE_MAX_BYTES."""
    return rel == TEMPLATE_DEST


def is_sdl3(rel):
    return rel == SDL3_DEST[0]


def pe_x64_problem(path):
    """-> "" when the file is a 64-bit Windows DLL (MZ header, PE signature, machine AMD64, DLL flag), else why not."""
    with open(path, "rb") as f:
        head = f.read(4096)
    if head[:2] != b"MZ" or len(head) < 64:
        return "not a Windows executable (no MZ header)"
    off = int.from_bytes(head[60:64], "little")
    if off + 24 > len(head) or head[off:off + 4] != b"PE\0\0":
        return "no PE header"
    if int.from_bytes(head[off + 4:off + 6], "little") != 0x8664:
        return "not a 64-bit (AMD64) PE"
    if not int.from_bytes(head[off + 22:off + 24], "little") & 0x2000:
        return "a PE but not a DLL"
    return ""


def scrub_ship(path, rel):
    """-> problems for one payload file: the template and SDL3.dll get their own gates, everything else scrub_file."""
    if is_sdl3(rel):
        size = os.path.getsize(path)
        bad = pe_x64_problem(path)
        return ([bad] if bad else []) + ([f"{size} bytes exceeds the SDL3 cap {SDL3_MAX_BYTES}"] if size > SDL3_MAX_BYTES else [])
    if is_template(rel):
        size = os.path.getsize(path)
        return [f"{size} bytes exceeds the template cap {TEMPLATE_MAX_BYTES}"] if size > TEMPLATE_MAX_BYTES else []
    return scrub_file(path, rel)
ALWAYS_SKIP_DIRS = {".git", "__pycache__", ".pytest_cache", ".ruff_cache", "build"}
ALWAYS_SKIP_FILES = ("*.pyc", "*.pyo", ".DS_Store", "*.swp", "*~")

# ---- scrub ------------------------------------------------------------------------------------------------------
DENY_NAMES = ("eboot.*", "eboot*.bin", "param.sfo", "*.psarc", "*.elf", "*.self", "*.sprx", "*.prx", "*.edat",
              "*.sdat", "*.pkg", "*.rap", "*.pup", "*.core", "*.corestream", "*.bik", "*.at3")
DENY_MAGIC = {b"SCE\0": "SELF (encrypted PS3 executable)", b"PSAR": "PSARC archive", b"RTTIBin": "RTTIBin game data",
              b"\0PSF": "PARAM.SFO", b"\x7fELF": "ELF binary", b"NPD\0": "EDAT/SDAT"}
# Patterns are written so this file never matches them itself (the public export scrubs tools/ with similar rules).
TEXT_DENY = [
    ("home path", re.compile(r"/home/[a-z][a-z0-9_.-]*/")),
    ("home path", re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+(?!Public\b|Default\b|<|%|\{|\$)[A-Za-z0-9_. -]+[\\/]", re.I)),
    ("private host", re.compile("discord(app)?\\.com/api/" "webhooks|ntfy\\.sh/")),
    ("secret", re.compile("sk-" "ant-|gh" "p_[A-Za-z0-9]{20}|github_" "pat_|BEGIN [A-Z ]*PRIVATE" " KEY|xox[bp]" "-")),
]
# The builder's own private host names stay out of this file: one per line in ~/.config/kz3/private-hosts (optional).
try:
    with open(os.path.expanduser("~/.config/kz3/private-hosts"), encoding="utf-8") as _f:
        _hosts = [h.strip() for h in _f if h.strip() and not h.startswith("#")]
    if _hosts:
        TEXT_DENY.append(("private host", re.compile("|".join(re.escape(h) for h in _hosts), re.I)))
except OSError:
    pass
MAX_BINARY = 2 << 20
TOOLCHAIN_SCHEMES = ("https",)   # URL schemes a toolchain manifest may use (the host test adds file://)
MAX_ANY = 8 << 20


def is_text(data):
    return b"\0" not in data[:8192]


def scrub_file(path, rel):
    """Problems with one source file, as a list of strings (empty = clean)."""
    bad = []
    low = os.path.basename(rel).lower()
    for pat in DENY_NAMES:
        if fnmatch.fnmatch(low, pat):
            bad.append(f"game-data name ({pat})")
            break
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        data = f.read(min(size, MAX_ANY + 1))
    for magic, what in DENY_MAGIC.items():
        if data.startswith(magic):
            bad.append(f"file starts like a {what}")
    text = is_text(data)
    if size > MAX_ANY or (not text and size > MAX_BINARY):
        bad.append(f"too big ({size:,} bytes, {'text' if text else 'binary'})")
    if text:
        s = data.decode("utf-8", "replace")
        for what, rx in TEXT_DENY:
            m = rx.search(s)
            if m:
                line = s.count("\n", 0, m.start()) + 1
                bad.append(f"{what} at line {line}")
    return bad


# ---- collection -------------------------------------------------------------------------------------------------
def skipped(name, opts, is_dir):
    if is_dir:
        if name in ALWAYS_SKIP_DIRS or name in opts.get("exclude_dirs", ()):
            return True
        return bool(opts.get("skip_tests")) and name in ("tests", "test")
    if any(fnmatch.fnmatch(name, p) for p in ALWAYS_SKIP_FILES):
        return True
    if opts.get("skip_tests") and name.startswith("test_"):
        return True
    inc = opts.get("include")
    return bool(inc) and not any(fnmatch.fnmatch(name, p) for p in inc)


def collect(repo):
    """-> (files: list of (dest, src), missing: list of source paths). Destinations are '/'-separated."""
    files, missing = {}, []
    for src, dest, kind, opts in RULES:
        sp = os.path.join(repo, *src.split("/"))
        if kind == "file":
            # the exported public repo has public/'s files at its root (LICENSE, README.md, NOTICE.md): try both
            if src.startswith(PUBLIC_DIR) and not os.path.isfile(sp):
                sp = os.path.join(repo, *src[len(PUBLIC_DIR):].split("/"))
            if os.path.isfile(sp) and not os.path.islink(sp):
                files[dest] = sp
            else:
                missing.append(src)
            continue
        if not os.path.isdir(sp):
            missing.append(src + "/")
            continue
        for d, dirs, names in os.walk(sp, followlinks=False):
            dirs[:] = sorted(x for x in dirs if not skipped(x, opts, True) and not os.path.islink(os.path.join(d, x)))
            for n in sorted(names):
                p = os.path.join(d, n)
                if os.path.islink(p) or not os.path.isfile(p) or skipped(n, opts, False):
                    continue
                rel = os.path.relpath(p, sp).replace(os.sep, "/")
                files[dest + "/" + rel] = p
    return sorted(files.items()), missing


CMAKE_LISTS = "port/kz3/CMakeLists.txt"


def cmake_sources_missing(repo, files):
    """-> payload destinations that port/kz3/CMakeLists.txt names as ../<dir>/<file> under port/ but the payload lacks
    (pass 16: the first real install failed at configure on ../net/kz3_net.c, a folder RULES did not list)."""
    path = os.path.join(repo, *CMAKE_LISTS.split("/"))
    if not os.path.isfile(path):
        return []
    have = {dest for dest, _ in files}
    base = os.path.dirname(CMAKE_LISTS)
    missing = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for tok in sorted(set(re.findall(r"\.\./[\w./-]+\.(?:c|cc|cpp|h|hpp|rc|def)\b", f.read()))):
            dest = os.path.normpath(os.path.join(base, tok)).replace(os.sep, "/")
            if dest.startswith("port/") and dest not in have:
                missing.append(f"{dest} (named in {CMAKE_LISTS} as {tok}; add its folder to RULES)")
    return missing


def collect_fonts(repo, fonts_dir):
    """-> (files [(dest, src)], problems [str]): the two notice fonts, hash-checked against installer/art/art.json's
    fonts_sha256 pins, and the family's OFL text."""
    files, bad = [], []
    try:
        with open(os.path.join(repo, "installer", "art", "art.json"), encoding="utf-8") as f:
            pins = json.load(f).get("fonts_sha256", {})
    except (OSError, ValueError) as e:
        return [], [f"installer/art/art.json: {e}"]
    for fn in FONTS:
        p = os.path.join(fonts_dir, fn)
        if not os.path.isfile(p) or os.path.islink(p):
            bad.append(f"{p} (run tools/release/installer_art.py --fetch-fonts)")
        elif not pins.get(fn):
            bad.append(f"{fn}: no SHA-256 pin in installer/art/art.json")
        elif sha256(p) != pins[fn]:
            bad.append(f"{fn}: SHA-256 differs from the pin in installer/art/art.json")
        else:
            files.append(("fonts/" + fn, p))
    lic = os.path.join(fonts_dir, FONT_LICENSE[0])
    if os.path.isfile(lic) and not os.path.islink(lic):
        files.append((FONT_LICENSE[1], lic))
    else:
        bad.append(f"{lic} (the fonts' OFL text; run tools/release/installer_art.py --fetch-fonts)")
    return files, bad


def collect_sdl3(sdl3_dir):
    """-> (files [(dest, src)], problems [str]): SDL3.dll (pads, patch 4010) and SDL's licence from stage_sdl3.py's folder.
    Both are required: a payload without the DLL would silently fall back to XInput on every player's machine."""
    files, bad = [], []
    for want, dest in zip(SDL3_SRC, SDL3_DEST):
        p = os.path.join(sdl3_dir, want)
        if os.path.isfile(p) and not os.path.islink(p):
            files.append((dest, p))
        else:
            bad.append(f"{p} (SDL3 for the pads; run tools/release/stage_sdl3.py in `nix develop .#win`)")
    return files, bad


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit(repo):
    try:
        r = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], capture_output=True, text=True, timeout=20)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def patchset(repo):
    p = os.path.join(repo, TOOLKIT, ".kz3-patchset")
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().strip() or None
    except OSError:
        return None


PATCHES = "port/patches"


def toolkit_stale(repo):
    """-> a message when the toolkit's .kz3-patchset was made from another port/patches set than the repo's, else
    "". The stamp's first field is tools/ps3recomp-patched.sh's sha256 of the patch files in name order (16 hex
    digits). Pass 16: a payload from a toolkit two merges old linked kz3.exe against missing runtime symbols."""
    pd = os.path.join(repo, *PATCHES.split("/"))
    stamp = patchset(repo)
    if not os.path.isdir(pd) or not stamp:
        return ""
    h = hashlib.sha256()
    for n in sorted(os.listdir(pd)):
        if n.endswith(".patch"):
            with open(os.path.join(pd, n), "rb") as f:
                h.update(f.read())
    want = h.hexdigest()[:16]
    if stamp.split("-")[0] != want:
        return (f"{TOOLKIT}/.kz3-patchset {stamp} was made from another {PATCHES} set (now {want}-...): "
                "run tools/ps3recomp-patched.sh first")
    return ""


def scrub_toolchain_file(path, rel):
    """Game-data names and magics only: never a PS3 file inside the bundled toolchain."""
    bad = []
    low = os.path.basename(rel).lower()
    if any(fnmatch.fnmatch(low, pat) for pat in DENY_NAMES):
        bad.append("game-data name")
    with open(path, "rb") as f:
        head = f.read(8)
    for magic, what in DENY_MAGIC.items():
        if head.startswith(magic):
            bad.append(f"file starts like a {what}")
    return bad


def collect_toolchain(cache, manifest_path=None):
    """-> (files [(dest, src)], problems [str], components [dict]) for the payload's toolchain/ and licenses/."""
    sys.path.insert(0, HERE)
    import fetch_toolchain as ft
    manifest_path = manifest_path or ft.MANIFEST
    try:
        m = ft.load(manifest_path)
    except (OSError, ValueError) as ex:
        return [], [f"toolchain manifest {manifest_path}: {ex}"], []
    problems = [f"toolchain manifest: {p}" for p in ft.validate(m, TOOLCHAIN_SCHEMES)]
    problems += [f"toolchain: {c.get('name')} sha256 is TO-FILL (fetch_toolchain.py --fill first)"
                 for c in m.get("components", []) if c.get("sha256") == ft.TO_FILL]
    if problems:
        return [], problems, []
    fetched = ft.read_fetched(cache)
    files, comps, lic_groups = {}, [], set()
    for raw in m["components"]:
        e = ft.expand(raw)
        name, tree = e["name"], os.path.join(cache, "trees", e["name"])
        if fetched.get(name, {}).get("sha256") != e["sha256"] or not os.path.isdir(tree):
            problems.append(f"toolchain: {name} not fetched with the manifest's hash (run fetch_toolchain.py)")
            continue
        if e.get("stage_only"):
            continue   # staged by stage_sdl3.py (runtime/SDL3.dll), not part of toolchain/
        comps.append({k: e.get(k) for k in ("name", "group", "version", "url", "sha256", "license")})
        if e["archive"] == "file":
            src = os.path.join(tree, e["license_file"])
            if os.path.isfile(src):
                files[f"licenses/{e['group']}/{e['license_file']}"] = src
                lic_groups.add(e["group"])
            else:
                problems.append(f"toolchain: {name}: {e['license_file']} missing in the cache")
            continue
        prune, globs = e.get("prune", []), e.get("license_globs", [])
        for d, dirs, names in os.walk(tree, followlinks=False):
            dirs[:] = sorted(x for x in dirs if not os.path.islink(os.path.join(d, x)))
            for n in sorted(names):
                p = os.path.join(d, n)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                rel = os.path.relpath(p, tree).replace(os.sep, "/")
                if any(fnmatch.fnmatchcase(rel, g) for g in globs):
                    files[f"licenses/{e['group']}/{name}-tree-{rel.replace('/', '_')}"] = p   # -tree-: never collides, case-folded, with a license_file
                    lic_groups.add(e["group"])
                if not any(fnmatch.fnmatchcase(rel, g) for g in prune):
                    files[f"toolchain/{e['dest']}/{rel}"] = p
    for g in sorted({c["group"] for c in m["components"] if not c.get("stage_only")} - lic_groups):
        problems.append(f"toolchain: group {g} has no licence text in the cache (check its license_globs)")
    return sorted(files.items()), problems, comps


def check_template(repo, path):
    """-> list of problems for the template at path: pso_template.py check (header, layout, record count)."""
    tool = os.path.join(repo, *TEMPLATE_CHECK.split("/"))
    if not os.path.isfile(tool):
        return [f"{TEMPLATE_CHECK} is missing, cannot check the template"]
    r = subprocess.run([sys.executable, tool, "check", path], capture_output=True, text=True)
    return [] if r.returncode == 0 else [(r.stderr or r.stdout).strip() or "pso_template.py check failed"]


PORTABLE_ONLY = ("run.cmd",)   # in the zip, not in the installed copy (installer/kz3.iss Excludes it): entries marked "portable_only"


def build_manifest(repo, files, version, toolchain=None):
    entries = [{"path": dest, "size": os.path.getsize(src), "sha256": sha256(src),
                **({"portable_only": True} if dest in PORTABLE_ONLY else {})} for dest, src in files]
    m = {"format": 1, "version": version, "repo_commit": git_commit(repo), "ps3recomp_patchset": patchset(repo),
         "count": len(entries), "total_bytes": sum(e["size"] for e in entries), "files": entries}
    if toolchain:
        m["toolchain"] = toolchain
    return m


def write_payload(out, files, manifest, replace, version):
    if os.path.lexists(out):
        if not os.path.isdir(out) or os.path.islink(out):
            raise SystemExit(f"--out {out} exists and is not a folder")
        if os.listdir(out):
            if not (replace and os.path.isfile(os.path.join(out, "manifest.json"))):
                raise SystemExit(f"--out {out} is not empty (--replace replaces an earlier payload, one holding "
                                 "manifest.json)")
    part = out.rstrip("/\\") + ".partial"
    if os.path.lexists(part):
        shutil.rmtree(part)
    os.makedirs(part)
    for dest, src in files:
        d = os.path.join(part, *dest.split("/"))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(src, d)
    with open(os.path.join(part, "VERSION.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(version + "\n")
    with open(os.path.join(part, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    if os.path.lexists(out):
        shutil.rmtree(out)
    os.replace(part, out)


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(HERE)))
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--out")
    g.add_argument("--dry-run", action="store_true")
    ap.add_argument("--replace", action="store_true")
    ap.add_argument("--version", default="0.0.0-dev")
    ap.add_argument("--launcher", default="", help="the built launcher, names.LAUNCHER_EXE (tools/release/build_launcher.sh)")
    ap.add_argument("--toolchain", default="", help="fetch_toolchain.py's cache (work/release/toolchain-cache)")
    ap.add_argument("--toolchain-manifest", default="", help=argparse.SUPPRESS)
    ap.add_argument("--template", default="", help=f"the shared pipeline template (default {TEMPLATE_SRC} when present)")
    ap.add_argument("--sdl3", default="", help="stage_sdl3.py's folder, SDL3.dll + LICENSE.txt (default work/release/sdl3)")
    ap.add_argument("--fonts", default="", help="installer_art.py --fetch-fonts' folder (default work/release/fonts)")
    a = ap.parse_args(argv)
    repo = os.path.abspath(a.repo)
    files, missing = collect(repo)
    ffiles, fmissing = collect_fonts(repo, os.path.abspath(a.fonts or os.path.join(repo, "work", "release", "fonts")))
    sfiles, smissing = collect_sdl3(os.path.abspath(a.sdl3 or os.path.join(repo, "work", "release", "sdl3")))
    files, missing = sorted(files + ffiles + sfiles), missing + fmissing + smissing
    if a.launcher:
        if os.path.isfile(a.launcher):
            files = sorted(files + [(LAUNCHER, os.path.abspath(a.launcher))])
        else:
            missing.append(a.launcher)
    tpl = os.path.abspath(a.template) if a.template else os.path.join(repo, *TEMPLATE_SRC.split("/"))
    tpl_bad = []
    if os.path.isfile(tpl):
        tpl_bad = check_template(repo, tpl)
        files = sorted(files + [(TEMPLATE_DEST, tpl)])
    elif a.template:
        missing.append(a.template)
    missing += cmake_sources_missing(repo, files)
    stale = toolkit_stale(repo)
    if stale:
        missing.append(stale)
    problems = [(dest, why) for dest, src in files for why in scrub_ship(src, dest)]
    problems += [(TEMPLATE_DEST, why) for why in tpl_bad]
    components = None
    if a.toolchain:
        tfiles, tprob, components = collect_toolchain(os.path.abspath(a.toolchain), a.toolchain_manifest or None)
        missing += tprob
        problems += [(dest, why) for dest, src in tfiles for why in scrub_toolchain_file(src, dest)]
        files = sorted(dict(files + tfiles).items())
    total = sum(os.path.getsize(src) for _, src in files)
    if a.dry_run:
        for dest, src in files:
            print(f"{os.path.getsize(src):>10,}  {dest}", file=out)
    for m in missing:
        print(f"MISSING {m}" + ("  (run tools/ps3recomp-patched.sh first)" if m.startswith(TOOLKIT) else ""),
              file=out)
    for dest, why in problems:
        print(f"SCRUB FAIL {dest}: {why}", file=out)
    ok = not missing and not problems
    print(f"payload: {len(files)} files, {total:,} bytes ({total / (1 << 20):.1f} MiB); "
          f"{'scrub clean' if not problems else f'{len(problems)} scrub problem(s)'}"
          f"{'' if not missing else f', {len(missing)} missing source(s)'}", file=out)
    if not ok:
        print("payload refused: fix the sources (or the allowlist in RULES) and re-run", file=out)
        return 1
    if a.dry_run:
        return 0
    if not re.fullmatch(r"\d+(\.\d+){1,3}(-[0-9A-Za-z.]+)?", a.version):
        print(f"--version {a.version!r}: expected X.Y.Z[-suffix]", file=out)
        return 2
    manifest = build_manifest(repo, files, a.version, components)
    write_payload(os.path.abspath(a.out), files, manifest, a.replace, a.version)
    print(f"wrote {a.out} (manifest.json: {manifest['count']} files, {manifest['total_bytes']:,} bytes)", file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
