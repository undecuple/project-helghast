#!/usr/bin/env python3
"""Private helpers for the Windows mission-0 benchmark.

Inputs and raw output may contain private paths or game data. Launcher and route
results are for the runner's private pipe; errors and analyzed results are sanitized.
"""

import argparse
from collections import Counter
import contextlib
import glob as globlib
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import threading
import time

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "release"))
sys.path.insert(0, str(REPO / "tools" / "assets"))
import profile_edit  # noqa: E402
import shaders  # noqa: E402

WINDOW_START = 265
WINDOW_END = 325
MAX_FLIP_GAP_MS = 2000
MAX_PAD_LATENESS = 2.0
MAX_PAD_ALIGNMENT_ERROR = 2.0
PAD_TIMESTAMP_PRECISION = 0.05
FLIP = re.compile(r"^\[flip\]\s+(\d+)\s+at\s+(\d+)\s+ms(?:\s+host=(\d+)\s+ms)?\s*$")
PAD_COUNT = re.compile(r"\[cellPad\]\s+PAD_SCRIPT:\s+(\d+)\s+event")
PAD_EVENT = re.compile(r"^\[cellPad\]\s+script t=(\d+(?:\.\d+)?)s\s+(press|sticks)\b")
PAD_PRESS = re.compile(r"\bpress\s+0x([0-9a-f]+)\s+for\s+(?:(\d+)\s+polls|(\d+(?:\.\d+)?)s)\b", re.I)
PAD_STICKS = re.compile(
    r"\bsticks\s+l=\((\d+),(\d+)\)\s+r=\((\d+),(\d+)\)\s+l2=(\d+)\s+r2=(\d+)"
    r"\s+for\s+(\d+(?:\.\d+)?)s\b", re.I)
PAD_ACTUAL = re.compile(r"\(at\s+(\d+\.\d+)s\)")
PAD_HOST = re.compile(r"\bhost=(\d+)\s+ms\b")
PAD_WARNING = re.compile(r"\[cellPad\]\s+PAD_SCRIPT(?:_FILE)?:\s+WARNING", re.I)
CRASH = re.compile(r"\[(?:CRASH|null-call|fatal|HardExit)\]|DEADCA7|unhandled exception|EXCEPTION_ACCESS|"
                   r"GUARDVIOLATION|SPU pool did not drain|undrained SPU workers",
                   re.I)
SHADER_FAILURE = re.compile(r"compile failed|fallback draw|pipeline (?:state )?creation failed|PSO failed", re.I)
MISSION = re.compile(r"^\[fs\]\s+open\s+'[^']*levels\.single_player\.mission_0\.", re.I)
PATCHSET = re.compile(r"^([0-9a-f]{16})-([0-9a-f]{7,40})$")


class BenchmarkError(Exception):
    """A fixed, path-free failure code safe to expose."""


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, _message):
        raise BenchmarkError("arguments_invalid")


def fail(code):
    raise BenchmarkError(code)


def _axis_byte(value, trigger):
    encoded = value * 255.0 if trigger else 128.0 + value * 128.0
    return max(0, min(255, int(encoded + 0.5)))


def _script_events(script):
    events = []
    try:
        for line in script.splitlines():
            if not line.strip():
                continue
            time_text, body = line.split(":", 1)
            scheduled = float(time_text)
            action_text, separator, duration_text = body.partition("/")
            values = [value.strip() for value in action_text.split(",")]
            if len(values) == 1:
                mask = int(values[0], 0)
                action = ("press", mask, "seconds", round(float(duration_text), 2)) if separator else (
                    "press", mask, "polls", 40)
            elif len(values) in (4, 6):
                axes = [float(value) for value in values]
                if len(axes) == 4:
                    axes.extend((0.0, 0.0))
                duration = float(duration_text) if separator else 1.0
                action = ("sticks", tuple(_axis_byte(value, index >= 4) for index, value in enumerate(axes)),
                          "seconds", round(duration, 2))
            else:
                fail("route_invalid")
            if not math.isfinite(scheduled) or scheduled < 0:
                fail("route_invalid")
            events.append({"scheduled": scheduled, "action": action})
    except (TypeError, ValueError):
        fail("route_invalid")
    return events


def _logged_action(line, kind):
    if kind == "press":
        match = PAD_PRESS.search(line)
        if not match:
            return None
        if match.group(2) is not None:
            return ("press", int(match.group(1), 16), "polls", int(match.group(2)))
        return ("press", int(match.group(1), 16), "seconds", round(float(match.group(3)), 2))
    match = PAD_STICKS.search(line)
    if not match:
        return None
    return ("sticks", tuple(int(match.group(index)) for index in range(1, 7)),
            "seconds", round(float(match.group(7)), 2))


def route():
    script, duration = shaders.route_script("profile")
    events = _script_events(script)
    if not events:
        fail("route_invalid")
    coverage_end = max(
        event["scheduled"] + (event["action"][3] if event["action"][2] == "seconds" else 0)
        for event in events
    )
    if duration < WINDOW_END or coverage_end < WINDOW_END:
        fail("route_invalid")
    return {"script": script, "duration_seconds": duration, "window_start": WINDOW_START,
            "window_end": WINDOW_END, "clock": "flip_ms_since_first_flip", "event_count": len(events),
            "mission_start": shaders.PROFILE_LEVEL_START,
            "pad_lateness_seconds": MAX_PAD_LATENESS, "pad_alignment_seconds": MAX_PAD_ALIGNMENT_ERROR,
            "sha256": hashlib.sha256(script.encode("utf-8")).hexdigest()}


def first_boot_route():
    script, duration = shaders.route_script("first-boot")
    events = _script_events(script)
    times = [event["scheduled"] for event in events]
    if not events or min(times) < 0 or max(times) >= duration:
        fail("route_invalid")
    return {"script": script, "duration_seconds": duration, "event_count": len(events),
            "sha256": hashlib.sha256(script.encode("utf-8")).hexdigest()}


def absolute_launcher_ini_paths(result, root):
    """Keep the original launcher's path base when the executable is staged elsewhere."""
    ini = result.get("ini")
    if not isinstance(ini, dict):
        return result
    base = shaders.la.ini_base(str(root))
    for key in ("eboot", "font_dir"):
        value = ini.get(key)
        if isinstance(value, str) and value and not os.path.isabs(value):
            ini[key] = os.path.normpath(os.path.join(base, value))
    return result


def readonly_launcher(root):
    """Run the real launcher's --print decision without its moved-install repair."""
    launcher, firstrun = shaders.la, shaders.fr
    original = firstrun.heal_moved_paths

    def refuse_repair(install, build):
        context = firstrun.Ctx(root=install, build=build)
        if firstrun.root_moved(context)[0] or firstrun.old_root(context):
            fail("launcher_moved_root")
        return ""

    firstrun.heal_moved_paths = refuse_repair
    output = io.StringIO()
    try:
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            rc = launcher.main(["--root", str(root), "--print"])
        if rc:
            fail("launcher_failed")
        result = json.loads(output.getvalue())
        if not isinstance(result, dict):
            fail("launcher_output_invalid")
        return absolute_launcher_ini_paths(result, root)
    finally:
        firstrun.heal_moved_paths = original


def _under(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _no_links(root):
    """Refuse symlinks and Windows junctions before copying an entire profile."""
    try:
        info = Path(root).lstat()
    except OSError:
        fail("profile_scan_failed")
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        fail("profile_contains_link")
    profiles = []

    def scan_error(_error):
        fail("profile_scan_failed")

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=scan_error):
        if any(name.lower() == "vfs" for name in dirs):
            fail("profile_source_invalid")
        for name in dirs + files:
            p = Path(directory) / name
            try:
                info = p.lstat()
            except OSError:
                fail("profile_scan_failed")
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                fail("profile_contains_link")
            if name.upper() == "PROFILE.DAT":
                if not stat.S_ISREG(info.st_mode):
                    fail("profile_invalid")
                profiles.append(p)
    return profiles


def _no_link_ancestors(path, *, allow_missing=False):
    for ancestor in reversed((path, *path.parents)):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            if allow_missing:
                return False
            fail("profile_missing")
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            fail("profile_contains_link")
    return True


def _checked_path(path, kind, missing, link, *, forbid_vfs=False):
    path = Path(os.path.abspath(os.fspath(path)))
    if forbid_vfs and any(part.lower() == "vfs" for part in path.parts):
        fail("link_free_launcher_required")
    for ancestor in reversed((path, *path.parents)):
        try:
            info = ancestor.lstat()
        except OSError:
            fail(missing)
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            fail(link)
    if (kind == "file" and not stat.S_ISREG(info.st_mode)) or (kind == "directory" and not stat.S_ISDIR(info.st_mode)):
        fail(missing)
    return path


def _existing_no_links(path):
    path = Path(os.path.abspath(os.fspath(path)))
    if any(part.lower() == "vfs" for part in path.parts):
        fail("link_free_launcher_required")
    for ancestor in reversed((path, *path.parents)):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            return False
        except OSError:
            fail("developer_path_invalid")
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            fail("developer_path_link")
    return True


def _checked_optional_file(path):
    if _existing_no_links(path):
        _checked_path(path, "file", "developer_install_invalid", "developer_path_link", forbid_vfs=True)


def _guard_hand_paths(root):
    launcher = shaders.la
    build = os.path.join(root, "build")
    old = launcher.read_game_ini(os.path.join(build, launcher.GAME_INI))
    recorded = launcher.load_json(os.path.join(build, launcher.INI_WRITTEN))
    if recorded is not None and not isinstance(recorded, dict):
        fail("developer_install_invalid")
    for key in launcher.INI_PATH_KEYS:
        value = old.get(key)
        if not value or (recorded is not None and recorded.get(key) == value):
            continue
        if recorded is None and launcher.is_abs(value):
            continue
        path = os.path.normpath(value if launcher.is_abs(value) else os.path.join(launcher.ini_base(root), value))
        if key in ("vfs_root", "update_dir", "hdd0_root", "data_dir"):
            _checked_path(path, "directory", "developer_recorded_paths_invalid", "developer_path_link",
                          forbid_vfs=True)
        else:
            _checked_path(Path(path).parent, "directory", "developer_recorded_paths_invalid", "developer_path_link",
                          forbid_vfs=True)
            _existing_no_links(path)


def developer_launcher(root, build_dir, repo):
    """Resolve the recorded install through launcher helpers without changing its build."""
    launcher = shaders.la
    root = _checked_path(root, "directory", "developer_install_invalid", "developer_path_link", forbid_vfs=True)
    build_dir = _checked_path(build_dir, "directory", "developer_build_dir_invalid", "developer_path_link",
                              forbid_vfs=True)
    repo = _checked_path(repo, "directory", "developer_source_invalid", "developer_path_link", forbid_vfs=True)
    exe = _checked_path(build_dir / "kz3.exe", "file", "developer_executable_missing", "developer_path_link",
                        forbid_vfs=True)
    source = _checked_path(repo / "port" / "kz3", "directory", "developer_source_invalid", "developer_path_link",
                           forbid_vfs=True)
    cache = _checked_path(build_dir / "CMakeCache.txt", "file", "developer_cache_invalid", "developer_path_link",
                          forbid_vfs=True)
    values, cache_hash = _cmake_cache(cache)
    eboot = _developer_eboot(values, source, build_dir)
    install_build = _checked_path(root / "build", "directory", "developer_install_invalid", "developer_path_link",
                                  forbid_vfs=True)
    for name in ("paths.json", "build-stamp.json", "kz3.exe"):
        _checked_path(install_build / name, "file", "developer_install_invalid", "developer_path_link",
                      forbid_vfs=True)
    for name in ("kz3.ini", launcher.INI_WRITTEN, os.path.join("obj", "CMakeCache.txt")):
        _checked_optional_file(install_build / name)
    for name in ("kz3-install.ini", launcher.PORTABLE_FILE, launcher.TEMPLATE_SHIPPED,
                 os.path.join("port", "kz3", "kz3_ini.h")):
        _checked_optional_file(root / name)
    paths = launcher.load_json(str(install_build / "paths.json"))
    if not isinstance(paths, dict):
        fail("developer_recorded_paths_invalid")
    for name, kind in (("dump", "directory"), ("update", "directory"), ("eboot", "file")):
        value = paths.get(name)
        if not isinstance(value, str) or not value or not os.path.isabs(value) or re.search(r"[\x00\r\n]", value):
            fail("developer_recorded_paths_invalid")
        _checked_path(value, kind, "developer_recorded_paths_invalid", "developer_path_link", forbid_vfs=True)
    _checked_path(Path(paths["update"]) / "USRDIR", "directory", "developer_recorded_paths_invalid",
                  "developer_path_link", forbid_vfs=True)
    for relative in ("PARAM.SFO", os.path.join("USRDIR", "EBOOT.BIN"),
                     os.path.join("USRDIR", launcher.fc.UPDATE_EDAT)):
        _checked_path(Path(paths["update"]) / relative, "file", "developer_recorded_paths_invalid",
                      "developer_path_link", forbid_vfs=True)
    if paths.get("font_dir"):
        _checked_path(paths["font_dir"], "directory", "developer_recorded_paths_invalid",
                      "developer_path_link", forbid_vfs=True)
    eboot_hash = _file_sha256(paths["eboot"])
    if eboot["mode"] == "embedded" and eboot["sha256"] != eboot_hash:
        fail("developer_eboot_mismatch")
    _guard_hand_paths(str(root))
    decision = readonly_launcher(str(root))
    action = decision.get("action")
    if action not in ("run", "rebuild"):
        fail("launcher_not_runnable")
    _checked_path(repo / "port" / "kz3" / "kz3_ini.h", "file", "developer_link_free_unsupported",
                  "developer_path_link", forbid_vfs=True)
    if not shaders.fr.cache_root_supported(str(repo)):
        fail("developer_link_free_unsupported")
    ini = launcher.read_ini(str(root))
    environ = dict(os.environ)
    _, cwd, _, added = launcher.game_env(str(root), paths, ini["portable"], environ)
    hand = launcher.hand_paths(str(root))
    if not launcher.link_free(str(root)):
        if "vfs_root" not in hand:
            added["PS3_VFS_ROOT"] = paths["dump"]
        if "cache_root" not in hand:
            added["KZ3_CACHE_ROOT"] = launcher.vfs_root(str(root))
    for name in ("PS3_VFS_ROOT", "KZ3_UPDATE_DIR", "PS3_HDD0_ROOT"):
        if name in added:
            _checked_path(added[name], "directory", "developer_recorded_paths_invalid", "developer_path_link",
                          forbid_vfs=True)
    _checked_path(cwd, "directory", "developer_recorded_paths_invalid", "developer_path_link", forbid_vfs=True)
    game_ini = launcher.game_ini(str(root), paths, ini["portable"], environ)
    game_ini["eboot"] = paths["eboot"]
    for key, name in (("vfs_root", "PS3_VFS_ROOT"), ("cache_root", "KZ3_CACHE_ROOT")):
        if key not in hand:
            game_ini[key] = launcher.ini_rel(added[name], str(root))
    argv = [str(exe)] + ([paths["eboot"]] if eboot["mode"] == "argument" else [])
    result = {"action": action, "argv": argv, "cwd": cwd, "env": added, "ini": game_ini,
              "eboot_mode": eboot["mode"], "eboot_sha256": eboot_hash, "cmake_cache_sha256": cache_hash,
              "template_env": launcher.template_env(str(root), cwd, environ)}
    result = absolute_launcher_ini_paths(result, str(root))
    if result["ini"].get("font_dir"):
        _checked_path(result["ini"]["font_dir"], "directory", "developer_recorded_paths_invalid",
                      "developer_path_link", forbid_vfs=True)
    _developer_resource_sources(root, repo)
    return result


def check_profile_source(source, session, *, allow_missing=False):
    source, session = Path(os.path.abspath(source)), Path(os.path.abspath(session))
    if _under(source, session) or _under(session, source):
        fail("profile_source_overlap")
    if any(part.lower() == "vfs" for part in source.parts):
        fail("profile_source_invalid")
    present = _no_link_ancestors(source, allow_missing=allow_missing)
    if present and not source.is_dir():
        fail("profile_missing")
    resolved_source, resolved_session = source.resolve(), session.resolve()
    if _under(resolved_source, resolved_session) or _under(resolved_session, resolved_source):
        fail("profile_source_overlap")
    return {"ok": True}


def _profiles(source):
    return _no_links(source)


def inspect_profile(source, session):
    source = Path(source)
    check_profile_source(source, session, allow_missing=True)
    profiles = _profiles(source) if source.is_dir() else []
    if len(profiles) > 1:
        fail("profile_count")
    return {"ok": True, "profile_count": len(profiles)}


def prepare_seed(source, destination):
    source, destination = Path(source), Path(destination)
    check_profile_source(source, destination.parent)
    work = (REPO / "work" / "helsari-p0").resolve()
    if not _under(destination.resolve(), work) or destination.exists():
        fail("seed_destination_invalid")
    profiles = _profiles(source)
    if len(profiles) != 1:
        fail("profile_count")
    relative = profiles[0].relative_to(source)
    parts = relative.parts
    if (len(parts) != 6 or parts[0].lower() != "dev_hdd0" or parts[1].lower() != "home"
            or parts[3].lower() != "savedata"):
        fail("profile_layout")
    data = bytearray(profiles[0].read_bytes())
    if len(data) != profile_edit.SIZE or struct.unpack_from(">I", data)[0] != profile_edit.VERSION_WORD:
        fail("profile_invalid")
    profile_edit.edit(data, clear_continue=True)
    destination.mkdir(parents=True)
    shutil.copytree(source, destination / "gamedata")
    (destination / "gamedata" / relative).write_bytes(data)
    return {"ok": True, "profile": "copied_and_continue_cleared"}


def _tracked_translation(repo):
    repo = _checked_path(repo, "directory", "developer_source_invalid", "developer_path_link", forbid_vfs=True)
    relative = Path("port") / "text" / "port_ui.tsv"
    source = _checked_path(repo / relative, "file", "developer_resource_missing", "developer_path_link",
                           forbid_vfs=True)
    git = shutil.which("git")
    if not git:
        fail("git_missing")
    index = subprocess.run([git, "-C", str(repo), "ls-files", "--stage", "--", str(relative)],
                           capture_output=True, text=True, timeout=30, check=False)
    lines = index.stdout.splitlines()
    if index.returncode or len(lines) != 1 or "\t" not in lines[0]:
        fail("developer_translation_untracked")
    metadata, name = lines[0].split("\t", 1)
    if (not re.fullmatch(r"100(?:644|755) [0-9a-f]{40,64} 0", metadata)
            or os.path.normcase(os.path.normpath(name)) != os.path.normcase(str(relative))):
        fail("developer_translation_untracked")
    return source


def _developer_resource_sources(install, repo):
    root = _checked_path(install, "directory", "developer_install_invalid", "developer_path_link", forbid_vfs=True)
    pairs = [(root / "runtime" / "SDL3.dll", Path("SDL3.dll"))]
    pairs.extend((root / "fonts" / name, Path("fonts") / name) for name in shaders.fr.NOTICE_FONTS)
    for source, _ in pairs:
        _checked_path(source, "file", "developer_resource_missing", "developer_path_link", forbid_vfs=True)
    pairs.append((_tracked_translation(repo), Path("lang") / "port_ui.tsv"))
    return pairs


def stage_resources(install, app, *, required=False, repo=None):
    app = Path(app)
    if not _under(app.resolve(), (REPO / "work" / "helsari-p0").resolve()) or not app.is_dir():
        fail("app_destination_invalid")
    if required:
        if not repo:
            fail("arguments_invalid")
        install = _checked_path(install, "directory", "developer_install_invalid", "developer_path_link",
                                forbid_vfs=True)
        _checked_path(app, "directory", "app_destination_invalid", "developer_path_link", forbid_vfs=True)
        resources = [(source, app / relative) for source, relative in _developer_resource_sources(install, repo)]
        for source, destination in resources:
            if _existing_no_links(destination.parent):
                _checked_path(destination.parent, "directory", "app_destination_invalid", "developer_path_link",
                              forbid_vfs=True)
            if _existing_no_links(destination):
                _checked_path(destination, "file", "app_destination_invalid", "developer_path_link", forbid_vfs=True)
        for source, destination in resources:
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.is_file() or _file_sha256(source) != _file_sha256(destination):
                shutil.copyfile(source, destination)
            if _file_sha256(source) != _file_sha256(destination):
                fail("developer_resource_mismatch")
        return {"ok": True, "ui_translations_source": "repository_tracked",
                "ui_translations_sha256": _file_sha256(app / "lang" / "port_ui.tsv")}
    shaders.fr.place_fonts(str(install), str(app))
    shaders.fr.place_lang(str(install), str(app))
    shaders.fr.place_sdl3(str(install), str(app))
    return {"ok": True}


def patch_digest(patches):
    files = sorted(Path(patches).glob("*.patch"), key=lambda p: p.name)
    if not files:
        fail("patchset_missing")
    h = hashlib.sha256()
    for path in files:
        with path.open("rb") as f:
            for block in iter(lambda: f.read(1024 * 1024), b""):
                h.update(block)
    return h.hexdigest()[:16]


def validate_patch_stamp(stamp, build_stamp, digest, gitlink):
    match = PATCHSET.fullmatch(stamp)
    if (not match or stamp != build_stamp or match.group(1) != digest
            or not re.fullmatch(r"[0-9a-f]{40,64}", gitlink)
            or not gitlink.startswith(match.group(2))):
        fail("patchset_mismatch")
    return stamp


def checkout_identity(repo):
    repo = Path(repo)
    git = shutil.which("git")
    if not git:
        fail("git_missing")
    gitpath = os.path.join("third_party", "ps3recomp")
    command = [git, "-C", str(repo)]
    index = subprocess.run([*command, "ls-files", "--stage", "--", gitpath],
                           capture_output=True, text=True, timeout=30, check=False)
    match = re.fullmatch(r"160000 ([0-9a-f]{40,64}) 0\t[^\r\n]+[\r\n]*", index.stdout)
    if index.returncode or not match:
        fail("patchset_gitlink_missing")
    head = subprocess.run([*command, "rev-parse", "--verify", "HEAD"], capture_output=True, text=True,
                          timeout=30, check=False)
    revision = head.stdout.strip()
    if head.returncode or not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        fail("git_head_missing")
    for extra in ([], ["--cached"]):
        diff = subprocess.run([*command, "diff", "--quiet", "--ignore-submodules=none", *extra, "--", gitpath],
                              capture_output=True, timeout=30, check=False)
        if diff.returncode:
            fail("patchset_submodule_changed")
    return match.group(1), revision


def verify_build(repo, install):
    repo, install = Path(repo), Path(install)
    gitlink, revision = checkout_identity(repo)
    toolkit = install / "ps3recomp"
    if not toolkit.is_dir():
        toolkit = install / "work" / "ps3recomp-patched"
    stamp = (toolkit / ".kz3-patchset").read_text(encoding="ascii").strip()
    build = json.loads((install / "build" / "build-stamp.json").read_text(encoding="utf-8"))
    verified = validate_patch_stamp(stamp, build.get("patchset", ""), patch_digest(repo / "port" / "patches"),
                                    gitlink)
    return {"ok": True, "patchset": verified, "git_head": revision}


def _file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _cmake_cache(path):
    wanted = {"CMAKE_HOME_DIRECTORY", "CMAKE_CACHEFILE_DIR", "ps3recomp_runtime_SOURCE_DIR",
              "PS3RECOMP_DIR", "CMAKE_GENERATOR", "KZ3_EMBED_EBOOT"}
    values = {}
    content = Path(path).read_bytes()
    for line in content.decode("utf-8", errors="strict").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key, value = line.split("=", 1)
        name, _ = key.split(":", 1)
        if name in wanted:
            if name in values:
                fail("developer_cache_invalid")
            values[name] = value
    if not {"CMAKE_HOME_DIRECTORY", "CMAKE_CACHEFILE_DIR", "ps3recomp_runtime_SOURCE_DIR",
            "CMAKE_GENERATOR", "KZ3_EMBED_EBOOT"} <= values.keys():
        fail("developer_cache_invalid")
    return values, hashlib.sha256(content).hexdigest()


def _same_path(actual, expected):
    return bool(actual and os.path.isabs(actual) and
                os.path.normcase(os.path.normpath(actual)) == os.path.normcase(os.path.normpath(str(expected))))


def _developer_eboot(values, source, build_dir):
    if not _same_path(values["CMAKE_HOME_DIRECTORY"], source):
        fail("developer_source_mismatch")
    if not _same_path(values["CMAKE_CACHEFILE_DIR"], build_dir):
        fail("developer_build_dir_mismatch")
    value = values["KZ3_EMBED_EBOOT"]
    header = build_dir / "kz3_embed_gen" / "kz3_eboot_embed.h"
    if not value:
        if _existing_no_links(header):
            fail("developer_eboot_header_mismatch")
        return {"mode": "argument", "sha256": None, "outputs": []}
    if re.search(r'[\x00\r\n"]', value):
        fail("developer_eboot_path_invalid")
    configured = Path(value) if os.path.isabs(value) else source / value
    configured = _checked_path(configured, "file", "developer_eboot_missing", "developer_path_link",
                               forbid_vfs=True)
    header = _checked_path(header, "file", "developer_eboot_header_missing", "developer_path_link", forbid_vfs=True)
    if header.stat().st_size > 65536:
        fail("developer_eboot_header_invalid")
    text = header.read_text(encoding="utf-8", errors="strict")
    paths = re.findall(r'^[ \t]*#define[ \t]+KZ3_EMBED_EBOOT_FILE[ \t]+"([^"\r\n]+)"[ \t]*$', text, re.M)
    hashes = re.findall(r'^[ \t]*#define[ \t]+KZ3_EMBED_EBOOT_SHA256[ \t]+"([0-9a-f]{64})"[ \t]*$', text, re.M)
    if len(paths) != 1 or len(hashes) != 1:
        fail("developer_eboot_header_invalid")
    if not _same_path(paths[0], configured):
        fail("developer_eboot_mismatch")
    digest = _file_sha256(configured)
    if digest != hashes[0]:
        fail("developer_eboot_mismatch")
    return {"mode": "embedded", "sha256": digest, "outputs": [header, configured]}


def _verify_build_graph_fresh(build_dir, exe, generator):
    if generator not in ("Ninja", "Ninja Multi-Config"):
        fail("developer_freshness_unsupported")
    ninja = shutil.which("ninja")
    if not ninja:
        fail("ninja_missing")
    try:
        target = exe.relative_to(build_dir).as_posix()
    except ValueError:
        fail("developer_executable_invalid")
    verify_globs = build_dir / "CMakeFiles" / "VerifyGlobs.cmake"
    if _existing_no_links(verify_globs):
        verify_globs = _checked_path(verify_globs, "file", "developer_configure_output_missing",
                                     "developer_path_link", forbid_vfs=True)
        if verify_globs.stat().st_size > 8 * 1024 * 1024:
            fail("developer_freshness_check_failed")
        try:
            glob_text = verify_globs.read_text(encoding="utf-8", errors="strict")
        except (OSError, UnicodeError):
            fail("developer_freshness_check_failed")
        sections = re.findall(
            r'file\(GLOB NEW_GLOB LIST_DIRECTORIES true "([^"]+)"\)\s*'
            r'set\(OLD_GLOB[ \t]*\n(.*?)^[ \t]*\)',
            glob_text, re.S | re.M,
        )
        if "file(GLOB" in glob_text and not sections:
            fail("developer_freshness_check_failed")
        for pattern, old_block in sections:
            old = re.findall(r'^\s*"([^"]+)"\s*$', old_block, re.M)
            current = globlib.glob(pattern)
            old_normalized = sorted(os.path.normcase(os.path.normpath(value)) for value in old)
            current_normalized = sorted(os.path.normcase(os.path.normpath(value)) for value in current)
            if current_normalized != old_normalized:
                fail("developer_build_graph_stale")
    try:
        result = subprocess.run(
            [ninja, "-n", "-d", "explain", "-C", str(build_dir), target],
            capture_output=True, text=True, timeout=120, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        fail("developer_freshness_check_failed")
    if result.returncode:
        fail("developer_freshness_check_failed")
    pending = []
    for line in (result.stdout + "\n" + result.stderr).splitlines():
        stripped = line.strip()
        if not stripped or re.fullmatch(r"ninja:\s+(?:Entering directory .+|no work to do\.)", stripped, re.I):
            continue
        if re.fullmatch(r"ninja explain: .+[\\/]CMakeFiles[\\/]"
                        r"(?:VerifyGlobs\.cmake_force|cmake\.verify_globs) is dirty", stripped):
            continue
        if re.fullmatch(r"\[\d+/\d+\]\s+(?:Re-checking globbed directories\.\.\.|Re-running CMake\.\.\.)",
                        stripped):
            continue
        pending.append(stripped)
    if pending:
        fail("developer_build_graph_stale")


def verify_developer_build(repo, build_dir):
    repo = _checked_path(repo, "directory", "developer_source_invalid", "developer_path_link", forbid_vfs=True)
    source = _checked_path(repo / "port" / "kz3", "directory", "developer_source_invalid", "developer_path_link",
                           forbid_vfs=True)
    _checked_path(source / "CMakeLists.txt", "file", "developer_source_invalid", "developer_path_link", forbid_vfs=True)
    toolkit = _checked_path(repo / "work" / "ps3recomp-patched", "directory", "developer_toolkit_invalid",
                            "developer_path_link", forbid_vfs=True)
    toolkit_stamp = _checked_path(toolkit / ".kz3-patchset", "file", "developer_toolkit_invalid",
                                  "developer_path_link", forbid_vfs=True)
    patches = _checked_path(repo / "port" / "patches", "directory", "patchset_missing", "developer_path_link",
                            forbid_vfs=True)
    for path in patches.glob("*.patch"):
        _checked_path(path, "file", "patchset_missing", "developer_path_link", forbid_vfs=True)
    build_dir = _checked_path(build_dir, "directory", "developer_build_dir_invalid", "developer_path_link",
                              forbid_vfs=True)
    exe = _checked_path(build_dir / "kz3.exe", "file", "developer_executable_missing", "developer_path_link",
                        forbid_vfs=True)
    cache = _checked_path(build_dir / "CMakeCache.txt", "file", "developer_cache_invalid", "developer_path_link",
                          forbid_vfs=True)
    values, cache_hash = _cmake_cache(cache)
    if not _same_path(values["CMAKE_HOME_DIRECTORY"], source):
        fail("developer_source_mismatch")
    if not _same_path(values["ps3recomp_runtime_SOURCE_DIR"], toolkit):
        fail("developer_toolkit_mismatch")
    if values.get("PS3RECOMP_DIR") and not _same_path(values["PS3RECOMP_DIR"], toolkit):
        fail("developer_toolkit_mismatch")
    if not _same_path(values["CMAKE_CACHEFILE_DIR"], build_dir):
        fail("developer_build_dir_mismatch")
    eboot = _developer_eboot(values, source, build_dir)
    generator = values["CMAKE_GENERATOR"]
    if generator in ("Ninja", "Ninja Multi-Config"):
        manifest = build_dir / "build.ninja"
    elif generator in ("NMake Makefiles", "MinGW Makefiles", "Unix Makefiles"):
        manifest = build_dir / "Makefile"
    else:
        fail("developer_generator_unsupported")
    manifest = _checked_path(manifest, "file", "developer_configure_output_missing", "developer_path_link",
                             forbid_vfs=True)
    gitlink, revision = checkout_identity(repo)
    stamp = toolkit_stamp.read_text(encoding="ascii").strip()
    verified = validate_patch_stamp(stamp, stamp, patch_digest(patches), gitlink)
    config_stamp = build_dir / ".kz3-toolkit-stamp"
    configured = "not_recorded"
    outputs = [cache, manifest, *eboot["outputs"]]
    if _existing_no_links(config_stamp):
        _checked_path(config_stamp, "file", "developer_configure_stamp_invalid", "developer_path_link",
                      forbid_vfs=True)
        if config_stamp.read_text(encoding="ascii").strip() != verified:
            fail("developer_configure_stamp_mismatch")
        outputs.append(config_stamp)
        configured = "matched"
    for name in (os.path.join("CMakeFiles", "rules.ninja"), os.path.join("CMakeFiles", "cmake.check_cache"),
                 os.path.join("CMakeFiles", "TargetDirectories.txt")):
        path = build_dir / name
        if _existing_no_links(path):
            outputs.append(_checked_path(path, "file", "developer_configure_output_missing", "developer_path_link",
                                         forbid_vfs=True))
    with exe.open("rb") as stream:
        if stream.read(2) != b"MZ":
            fail("developer_executable_invalid")
    exe_time = exe.stat().st_mtime_ns
    if any(exe_time <= path.stat().st_mtime_ns for path in [toolkit_stamp, *outputs]):
        fail("developer_build_stale")
    _verify_build_graph_fresh(build_dir, exe, generator)
    return {"ok": True, "patchset": verified, "git_head": revision, "exe_sha256": _file_sha256(exe),
            "cmake_cache_sha256": cache_hash, "cmake_generator": generator,
            "eboot_mode": eboot["mode"], "eboot_sha256": eboot["sha256"],
            "configure_toolkit_stamp": configured, "freshness": "ninja_dry_run_clean"}


def _scan(path):
    flips, registered, fired, flip_origins = [], [], [], []
    warning = crashed = shader_failure = mission = False
    flip_timestamp_missing = False
    last_flip = None
    with Path(path).open(encoding="utf-8", errors="replace") as f:
        for line in f:
            warning |= bool(PAD_WARNING.search(line))
            crashed |= bool(CRASH.search(line))
            shader_failure |= bool(SHADER_FAILURE.search(line))
            count = PAD_COUNT.search(line)
            if count:
                registered.append(int(count.group(1)))
            event = PAD_EVENT.match(line)
            if event:
                actual = PAD_ACTUAL.search(line)
                if not actual:
                    fail("pad_timestamp_missing")
                scheduled, fired_at = float(event.group(1)), float(actual.group(1))
                if not math.isfinite(scheduled) or not math.isfinite(fired_at):
                    fail("pad_timestamp_invalid")
                host = PAD_HOST.search(line)
                entry = {"scheduled": scheduled, "actual": fired_at, "kind": event.group(2),
                         "action": _logged_action(line, event.group(2)),
                         "host_ms": int(host.group(1)) if host else None}
                fired.append(entry)
            flip = FLIP.match(line)
            if flip:
                point = int(flip.group(2)), int(flip.group(1))
                if not flips or point != flips[-1]:
                    flips.append(point)
                if flip.group(3) is None:
                    flip_timestamp_missing = True
                else:
                    flip_origins.append(int(flip.group(3)) - point[0])
                last_flip = point
            if (MISSION.match(line) and last_flip is not None
                    and 20000 <= last_flip[0] <= WINDOW_START * 1000):
                mission = True
    return {"flips": flips, "registered": registered, "fired": fired, "warning": warning,
            "crashed": crashed, "shader_failure": shader_failure, "mission": mission,
            "flip_origins": flip_origins, "flip_timestamp_missing": flip_timestamp_missing}


def _validate_pad_timing(scan, spec):
    events = scan["fired"]
    scripted = _script_events(spec["script"])
    scheduled = [event["scheduled"] for event in scripted]
    expected = Counter(t for t in scheduled if t < WINDOW_END)
    observed = Counter(event["scheduled"] for event in events)
    if any(observed[t] < count for t, count in expected.items()):
        fail("pad_incomplete")
    all_expected = Counter(scheduled)
    if any(count > all_expected[t] for t, count in observed.items()):
        fail("pad_event_unexpected")
    expected_actions = Counter((event["scheduled"], event["action"]) for event in scripted)
    if any(event["action"] is None for event in events):
        fail("pad_action_invalid")
    observed_actions = Counter((event["scheduled"], event["action"]) for event in events)
    if any(count > expected_actions[event] for event, count in observed_actions.items()):
        fail("pad_action_mismatch")
    for event in events:
        scheduled_at, actual = event["scheduled"], event["actual"]
        if actual + PAD_TIMESTAMP_PRECISION < scheduled_at:
            fail("pad_event_time_invalid")
        if scheduled_at < WINDOW_END and actual + PAD_TIMESTAMP_PRECISION > scheduled_at + MAX_PAD_LATENESS:
            fail("pad_event_late")

    relevant = [e for e in events if e["scheduled"] < WINDOW_END]
    mission_events = [e for e in relevant if e["scheduled"] >= spec["mission_start"]]
    if len(mission_events) < 2:
        fail("pad_alignment_missing")
    if scan["flip_timestamp_missing"] or not scan["flip_origins"]:
        fail("shared_clock_timestamp_missing")
    if max(scan["flip_origins"]) - min(scan["flip_origins"]) > 2:
        fail("shared_clock_unstable")
    flip_origin = sorted(scan["flip_origins"])[len(scan["flip_origins"]) // 2]
    offsets = []
    for event in mission_events:
        if event["host_ms"] is None:
            fail("shared_clock_timestamp_missing")
        aligned = (event["host_ms"] - flip_origin) / 1000.0
        expected_at = event["scheduled"]
        offsets.append(aligned - event["actual"])
        if expected_at < WINDOW_START:
            if aligned >= WINDOW_START:
                fail("pad_outside_window")
        elif aligned < WINDOW_START or aligned >= WINDOW_END:
            fail("pad_outside_window")
        if abs(aligned - expected_at) > MAX_PAD_ALIGNMENT_ERROR:
            fail("pad_clock_unaligned")
    if max(offsets) - min(offsets) > MAX_PAD_ALIGNMENT_ERROR:
        fail("pad_clock_unaligned")
    return {"method": "shared_monotonic_timestamp", "event_count": len(mission_events),
            "offset_lower_seconds": round(min(offsets) - PAD_TIMESTAMP_PRECISION, 4),
            "offset_upper_seconds": round(max(offsets) + PAD_TIMESTAMP_PRECISION, 4)}


def _interpolate(flips, target):
    for (ta, na), (tb, nb) in zip(flips, flips[1:]):
        if ta <= target <= tb and tb > ta:
            return na + (nb - na) * (target - ta) / (tb - ta)
    fail("incomplete_window")


def analyze(combined):
    stream = _scan(combined)
    if stream["crashed"]:
        fail("crash_log")
    if stream["shader_failure"]:
        fail("shader_failure")
    if stream["warning"]:
        fail("pad_warning")
    spec = route()
    counts = stream["registered"]
    if not counts or any(n != spec["event_count"] for n in counts):
        fail("pad_registration")
    if not stream["mission"]:
        fail("mission_not_observed")
    flips = stream["flips"]
    if len(flips) < 2:
        fail("incomplete_window")
    for (ta, na), (tb, nb) in zip(flips, flips[1:]):
        if tb < ta or nb <= na:
            fail("flip_sequence_invalid")
        if tb > WINDOW_START * 1000 and ta < WINDOW_END * 1000 and tb - ta > MAX_FLIP_GAP_MS:
            fail("flip_gap")
    first = _interpolate(flips, WINDOW_START * 1000)
    last = _interpolate(flips, WINDOW_END * 1000)
    if last <= first:
        fail("incomplete_window")
    alignment = _validate_pad_timing(stream, spec)
    return {"valid": True, "reason": "ok",
            "fps_mean_flip_approx": round((last - first) / (WINDOW_END - WINDOW_START), 4),
            "pad_clock_alignment": alignment,
            "gpu_time_status": "unavailable"}


def analyze_first_boot(combined):
    stream = _scan(combined)
    if stream["crashed"]:
        fail("crash_log")
    if stream["shader_failure"]:
        fail("shader_failure")
    if stream["warning"]:
        fail("pad_warning")
    spec = first_boot_route()
    if not stream["registered"] or any(n != spec["event_count"] for n in stream["registered"]):
        fail("pad_registration")
    scripted = _script_events(spec["script"])
    expected_times = Counter(event["scheduled"] for event in scripted)
    observed_times = Counter(event["scheduled"] for event in stream["fired"])
    if any(count > expected_times[event] for event, count in observed_times.items()):
        fail("pad_event_unexpected")
    if observed_times != expected_times:
        fail("pad_incomplete")
    if any(event["action"] is None for event in stream["fired"]):
        fail("pad_action_invalid")
    expected = Counter((event["scheduled"], event["action"]) for event in scripted)
    observed = Counter((event["scheduled"], event["action"]) for event in stream["fired"])
    if observed != expected:
        fail("pad_action_mismatch")
    if any(event["host_ms"] is None for event in stream["fired"]):
        fail("shared_clock_timestamp_missing")
    previous_scheduled = previous_actual = -math.inf
    for event in stream["fired"]:
        scheduled, actual = event["scheduled"], event["actual"]
        if scheduled < previous_scheduled or actual < previous_actual:
            fail("pad_order_invalid")
        if actual + PAD_TIMESTAMP_PRECISION < scheduled:
            fail("pad_event_time_invalid")
        if actual + PAD_TIMESTAMP_PRECISION > scheduled + MAX_PAD_LATENESS:
            fail("pad_event_late")
        previous_scheduled, previous_actual = scheduled, actual
    return {"valid": True, "reason": "ok", "route_sha256": spec["sha256"]}


def _close_process_windows(pid):
    if os.name != "nt":
        fail("windows_required")
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL
    posted = []

    @callback_type
    def close_window(hwnd, _):
        window_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == pid and user32.IsWindowVisible(hwnd):
            posted.append(bool(user32.PostMessageW(hwnd, 0x0010, 0, 0)))
        return True

    if not user32.EnumWindows(close_window, 0) or not any(posted):
        fail("close_window_unavailable")


def _process_creation_filetime(pid):
    if os.name != "nt":
        fail("windows_required")
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
                                         ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                         ctypes.POINTER(wintypes.FILETIME)]
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        fail("process_tracking_failed")
    created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
    got_times = kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                         ctypes.byref(kernel), ctypes.byref(user))
    closed = kernel32.CloseHandle(handle)
    if not got_times or not closed:
        fail("process_tracking_failed")
    return str((created.dwHighDateTime << 32) | created.dwLowDateTime)


def supervise():
    process = None
    failure = None
    parent_gone = threading.Event()
    try:
        if os.name != "nt":
            fail("windows_required")
        spec = json.loads(sys.stdin.readline())
        log = Path(spec["log"])
        work = (REPO / "work" / "helsari-p0").resolve()
        if not _under(log.resolve(), work) or not log.parent.is_dir():
            fail("log_destination_invalid")
        _no_link_ancestors(log.parent)
        duration, grace = float(spec["duration_seconds"]), float(spec["grace_seconds"])
        if not (math.isfinite(duration) and math.isfinite(grace) and 0 < duration <= 3600 and 0 < grace <= 60):
            fail("process_timeout_invalid")
        if spec["close_mode"] not in ("window", "stdin"):
            fail("process_close_invalid")
        try:
            output = log.open("xb", buffering=0)
        except FileExistsError:
            fail("log_destination_exists")
        except OSError:
            fail("log_destination_invalid")
        with output:
            try:
                process = subprocess.Popen(spec["command_line"], executable=spec["executable"], cwd=spec["cwd"],
                                           env=spec["environment"], stdin=subprocess.PIPE, stdout=output,
                                           stderr=subprocess.STDOUT)
            except Exception:
                fail("process_start_failed")
            created = _process_creation_filetime(process.pid)
            print(json.dumps({"event": "started", "pid": process.pid, "created_filetime": created},
                             separators=(",", ":")), flush=True)

            def watch_parent():
                try:
                    for _ in sys.stdin:
                        pass
                finally:
                    parent_gone.set()

            threading.Thread(target=watch_parent, daemon=True).start()
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                if parent_gone.is_set():
                    fail("supervisor_parent_lost")
                code = process.poll()
                if code is not None:
                    fail("process_crashed" if code else "process_ended_early")
                time.sleep(min(0.1, max(0, deadline - time.monotonic())))
            code = process.poll()
            if code is not None:
                fail("process_crashed" if code else "process_ended_early")
            if spec["close_mode"] == "window":
                _close_process_windows(process.pid)
            else:
                try:
                    process.stdin.write(b"\n")
                    process.stdin.flush()
                except (BrokenPipeError, OSError):
                    code = process.poll()
                    fail("process_crashed" if code else "process_ended_early")
            deadline = time.monotonic() + grace
            while process.poll() is None:
                if parent_gone.is_set():
                    fail("supervisor_parent_lost")
                if time.monotonic() >= deadline:
                    fail("shutdown_timeout")
                time.sleep(0.05)
            if process.returncode:
                fail("process_crashed")
    except BenchmarkError as e:
        failure = str(e)
    except Exception:
        failure = "process_supervisor_failed"
    finally:
        if process is not None:
            try:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                if process.stdin:
                    process.stdin.close()
            except Exception:
                failure = "process_cleanup_failed"
    result = {"event": "finished", "ok": failure is None, "reason": failure or "ok"}
    print(json.dumps(result, separators=(",", ":")), flush=True)
    return 0 if failure is None else 1


def main(argv=None):
    ap = SafeArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    route_parser = sub.add_parser("route")
    route_parser.add_argument("--kind", choices=("profile", "first-boot"), default="profile")
    launch = sub.add_parser("launcher")
    launch.add_argument("--install", required=True)
    launch.add_argument("--developer-build")
    launch.add_argument("--repo")
    seed = sub.add_parser("seed")
    seed.add_argument("--source", required=True)
    seed.add_argument("--destination", required=True)
    profile = sub.add_parser("check-profile")
    profile.add_argument("--source", required=True)
    profile.add_argument("--session", required=True)
    profile.add_argument("--allow-missing", action="store_true")
    inventory = sub.add_parser("inspect-profile")
    inventory.add_argument("--source", required=True)
    inventory.add_argument("--session", required=True)
    resources = sub.add_parser("stage-resources")
    resources.add_argument("--install", required=True)
    resources.add_argument("--app", required=True)
    resources.add_argument("--required", action="store_true")
    resources.add_argument("--repo")
    stamp = sub.add_parser("verify-build")
    stamp.add_argument("--repo", required=True)
    stamp.add_argument("--install", required=True)
    developer = sub.add_parser("verify-developer-build")
    developer.add_argument("--repo", required=True)
    developer.add_argument("--build", required=True)
    logs = sub.add_parser("analyze")
    logs.add_argument("--log", required=True)
    first_boot = sub.add_parser("analyze-first-boot")
    first_boot.add_argument("--log", required=True)
    sub.add_parser("supervise")
    try:
        a = ap.parse_args(argv)
    except BenchmarkError as e:
        print(json.dumps({"ok": False, "valid": False, "reason": str(e)}, separators=(",", ":")))
        return 1
    if a.command == "supervise":
        return supervise()
    try:
        if a.command == "route":
            result = first_boot_route() if a.kind == "first-boot" else route()
        elif a.command == "launcher":
            if (a.developer_build is None) != (a.repo is None) or a.developer_build == "" or a.repo == "":
                fail("arguments_invalid")
            result = (developer_launcher(a.install, a.developer_build, a.repo) if a.developer_build is not None
                      else readonly_launcher(a.install))
        elif a.command == "seed":
            result = prepare_seed(a.source, a.destination)
        elif a.command == "check-profile":
            result = check_profile_source(a.source, a.session, allow_missing=a.allow_missing)
        elif a.command == "inspect-profile":
            result = inspect_profile(a.source, a.session)
        elif a.command == "stage-resources":
            if a.required != (a.repo is not None) or a.repo == "":
                fail("arguments_invalid")
            result = stage_resources(a.install, a.app, required=a.required, repo=a.repo)
        elif a.command == "verify-build":
            result = verify_build(a.repo, a.install)
        elif a.command == "verify-developer-build":
            result = verify_developer_build(a.repo, a.build)
        elif a.command == "analyze-first-boot":
            result = analyze_first_boot(a.log)
        else:
            result = analyze(a.log)
        print(json.dumps(result, separators=(",", ":")))
        return 0
    except BenchmarkError as e:
        print(json.dumps({"ok": False, "valid": False, "reason": str(e)}, separators=(",", ":")))
        return 1
    except Exception:
        print('{"ok":false,"valid":false,"reason":"input_or_tool_error"}')
        return 1


if __name__ == "__main__":
    sys.exit(main())
