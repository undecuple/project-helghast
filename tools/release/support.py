#!/usr/bin/env python3
"""The support package (release pass 34): one zip a player attaches to a bug report. Idea only from the OpenGOAL
launcher's support package; no code taken.

    launch.py --support-package [--out DIR]      (Start menu: "Make a support package")

Holds, all text, all through firstrun.redactor() (user names in paths become <user>, also CMake's mangled form):
the newest 3 game logs (Killzone3-PC-*.log), the first-run logs, the setup log, launcher.log, kz3-install.ini,
display.ini and keybinds.ini (kz3-install.ini's crash-report dsn reduced to <set>), and facts.txt (port version and commit from VERSION.txt / manifest.json, Windows version,
GPU name when a game log has it, EBOOT version from build/paths.json facts only, what was shortened).
Never included: anything outside the install folder and the data folder (%APPDATA%\\Project Helghast, or the install
folder when portable), saves, game files, the EBOOT, environment dumps. Only files named .log/.txt/.ini are read; a
name on assemble_payload's game-data deny list is refused. Over CAP bytes: the oldest lines of the largest logs go first.
Zip name: names.FILE_BASE-support-YYYYmmdd-HHMMSS.zip.
"""
import fnmatch
import glob
import json
import os
import platform
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import names  # noqa: E402
import support_text as T  # noqa: E402

CAP = 20 << 20                   # bytes of text in the zip (uncompressed)
KEEP_RUNS = 3
OK_EXT = (".log", ".txt", ".ini")
# kz3-install.ini [crash] dsn: the crash-report address never goes into a package (players attach them to public
# issues); the package says only whether one is set.
DSN_RX = re.compile(r"(?im)^(\s*dsn\s*[=:][ \t]*)(.*)$")
# game-data names, repeated from assemble_payload.DENY_NAMES (importing it pulls in the whole payload builder)
DENY_NAMES = ("eboot.*", "eboot*.bin", "param.sfo", "*.psarc", "*.elf", "*.self", "*.sprx", "*.prx", "*.edat",
              "*.sdat", "*.pkg", "*.rap", "*.pup", "*.core", "*.corestream", "*.bik", "*.at3")
GPU_RX = re.compile(r"(?i)(adapter|gpu|device)[^\n]*?(nvidia|geforce|rtx|gtx|amd|radeon|intel|arc\b|warp)[^\n]*")


def refused(name):
    n = os.path.basename(name).lower()
    return n.endswith(".sav") or any(fnmatch.fnmatch(n, p) for p in DENY_NAMES) or not n.endswith(OK_EXT)


def zip_name(stamp):
    return f"{names.FILE_BASE}-support-{stamp}.zip"


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def newest(patterns, n):
    files = {p for pat in patterns for p in glob.glob(pat)}
    files = [p for p in files if os.path.isfile(p)]
    return sorted(files, key=lambda p: (os.path.getmtime(p), p), reverse=True)[:n]


def candidates(root, data):
    """-> [(archive name, path)] in priority order; only files inside the install folder or the data folder."""
    out = []
    games = newest([os.path.join(d, "logs", "Killzone3-PC-*.log") for d in {data, root}] +
                   [os.path.join(root, "build", "**", "logs", "Killzone3-PC-*.log")], KEEP_RUNS)
    for p in games:
        out.append(("logs/" + os.path.basename(p), p))
    for p in newest([os.path.join(root, names.FIRSTRUN_LOG), os.path.join(root, names.FILE_BASE + "-firstrun-*.log")], 2):
        out.append((os.path.basename(p), p))
    out.append((names.SETUP_LOG, os.path.join(root, names.SETUP_LOG)))
    out.append(("launcher.log", os.path.join(root, "logs", "launcher.log")))
    out.append(("kz3-install.ini", os.path.join(root, "kz3-install.ini")))
    for n in ("display.ini", "keybinds.ini"):
        out.append((n, os.path.join(data, n)))
    return out


def inside(path, *folders):
    p = os.path.realpath(path)
    return any(p == os.path.realpath(f) or p.startswith(os.path.realpath(f) + os.sep) for f in folders)


def jget(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def facts(root, texts, trimmed, red, cap=CAP):
    ver = "unknown"
    try:
        ver = read_text(os.path.join(root, "VERSION.txt")).strip() or ver
    except OSError:
        pass
    man = jget(os.path.join(root, "manifest.json"))
    f = (jget(os.path.join(root, "build", "paths.json")).get("facts") or {})
    eboot = (f.get("eboot_elf") or {}).get("version") or "unknown"
    gpu = "not in the logs"
    for n, t in texts.items():
        if n.startswith("logs/"):
            m = GPU_RX.search(t)
            if m:
                gpu = m.group(0).strip()[:160]
                break
    lines = [f"{names.PRODUCT} support package", f"Port version: {ver}", f"Port commit: {man.get('repo_commit') or 'unknown'}",
             f"Windows: {platform.platform()}", f"GPU: {gpu}", f"EBOOT version: {eboot}",
             T.FACTS_TRIMMED.format(mb=cap >> 20, names=", ".join(trimmed)) if trimmed else T.FACTS_NOT_TRIMMED,
             "Files: " + ", ".join(sorted(texts))]
    return red("\n".join(lines) + "\n")


def trim(texts, cap):
    """Drop the oldest lines of the largest entries until the total fits. -> names of the shortened entries."""
    trimmed = []
    size = lambda: sum(len(t.encode("utf-8")) for t in texts.values())
    while size() > cap:
        n = max(texts, key=lambda k: len(texts[k]))
        t = texts[n]
        cut = max(len(t) // 4, size() - cap)
        nl = t.find("\n", min(cut, len(t) - 1))
        marker = "[earlier lines removed to keep the package small]\n"
        texts[n] = marker + (t[nl + 1:] if nl >= 0 else "")
        if n not in trimmed:
            trimmed.append(n)
        if len(texts[n]) <= len(marker) and all(len(v) <= len(marker) for v in texts.values()):
            break
    return trimmed


def build(root, data, out_dir, red, stamp, cap=CAP):
    """-> (zip path, [skipped reasons]); zip path "" when there was nothing to collect."""
    texts, skipped = {}, []
    for arc, path in candidates(root, data):
        if not os.path.isfile(path):
            continue
        if refused(arc) or refused(path) or not inside(path, root, data):
            skipped.append(f"refused {arc}")
            continue
        try:
            texts[arc] = red(read_text(path))
            if arc == "kz3-install.ini":
                texts[arc] = DSN_RX.sub(lambda m: m.group(1) + ("<set>" if m.group(2).strip() else ""), texts[arc])
        except OSError:
            skipped.append(f"unreadable {arc}")
    if not texts:
        return "", skipped
    trimmed = trim(texts, cap - 4096)     # room for facts.txt
    texts["facts.txt"] = facts(root, texts, trimmed, red, cap)
    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, zip_name(stamp))
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
        for arc in ["facts.txt"] + [k for k in texts if k != "facts.txt"]:
            z.writestr(arc, texts[arc])
    return dest, skipped
