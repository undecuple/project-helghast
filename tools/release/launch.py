#!/usr/bin/env python3
"""Launcher logic behind the launcher exe (names.LAUNCHER_EXE).

The exe (installer/launcher/launcher.c) only starts the bundled `toolchain\\python\\pythonw.exe` on this file with
`--root <its own folder>` and its own arguments; everything it decides is here, so a host test can check it.

    launch.py --root DIR [--print] [game arguments...]

What it does, from what is on disk under DIR:
  first-build     never built (no build/paths.json): offer the first-run build (firstrun.py --interactive)
  missing-inputs  the dump, the update folder or the EBOOT named in paths.json is gone (unplugged drive, moved
                  folder): offer to pick them again (firstrun.py --interactive)
  resume          a build was started and did not finish (no build-stamp.json or no build/kz3.exe): offer to go on
                  (firstrun.py --bar with the recorded paths; finished steps are skipped by their markers)
  rebuild         built, but an input changed since: the EBOOT (size/time), the folders or the check's facts, the
                  lift inputs (lift + compile) or the port version (compile only; ninja rebuilds what changed)
  run             write build/kz3.ini (the player's paths, game_ini below) and start build/kz3.exe with no arguments

The build runs in its own console window (python.exe, not pythonw) and the launcher waits for it; when it ends
well the game starts. A "No" starts nothing.

Game environment (4.3), written to <root>/build/kz3.ini before each start (pass 26: kz3.exe reads it when started
with no arguments, port/kz3/kz3_ini.h; the launcher no longer passes the variables or the EBOOT): PS3_VFS_ROOT=<root>/vfs, a writable folder whose PS3_GAME is a directory junction to
<dump>/PS3_GAME (pass 17, ensure_vfs: the runtime inflates PSARC entries into <vfs root>/psarc-cache and maps
/dev_hdd1 to <vfs root>/cache, so a read-only or untouched dump as the root made every asset open fail); KZ3_UPDATE_DIR=<update>/USRDIR (read by the 1.14 build, main's default);
PS3_HDD0_ROOT=<...>/dev_hdd0 only when the update folder sits in an RPCS3 dev_hdd0/game/<id> tree;
PS3_EDAT_CACHE=<root>/build/cache/edat; RSX_SHADER_CACHE_DIR=<data>/cache/shaders (pass 5: the shader disk cache,
pso_keys.bin and pso_library.bin). Working folder <data> (where cellSaveData's ./gamedata lands): the install folder
when portable, else %APPDATA%/<names.DATA_FOLDER>. Portable is decided by data_mode (highest first): --portable /
--appdata (one run), a file portable.txt in the install root, kz3-install.ini portable=1/0, default AppData; the log
line and --open-data name the rule. --switch-data creates/removes portable.txt and writes the ini.

Version check (opt-in, kz3-install.ini [online] check_updates=1): at most once a day, version_check.py runs in the
background (no window, 8 s timeout) and leaves its line in <root>/logs/version-check.txt; the next start shows
"a new version is out" once per version (a "No" is remembered in [online] dismissed).

Crash reports (opt-in, kz3-install.ini [crash] report=1 and a dsn the build carries; crash_report.py): at start (in
the background while the game runs) and after the game exits, the newest game crash file not handled yet is sent as
one scrubbed event (at most one per session); after the game, a dialog says whether it was sent. --crash-reports
("Crash reports on or off", Start menu) shows the state and switches it; without a dsn it says "not available in this
build". Nothing else goes online.

--prepare-shaders: "Prepare shaders again" (Start menu entry): the warm-up session of shaders.py --force in a console
window; needs a finished build.

--repair: "Verify and rebuild" (Start menu entry; pass 6): first the install's own files against manifest.json (the
payload's SHA-256 list; a missing or changed file = "reinstall over this folder", nothing is rebuilt from a damaged
install), then firstrun.py --bar with the recorded paths: the check step always runs again (dump, update, EBOOT
hash), and every later step whose inputs fingerprint changed runs again (the others are kept; ninja recompiles what
changed). Never built or inputs gone = the same questions as a normal start. Ends with a message; does not start the game.

--print: show the decision, the command, the working folder and the variables it sets, as JSON; starts nothing.
"""
import argparse
import configparser
import datetime
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crash_report  # noqa: E402  (opt-in crash reports)
import firstrun as fr  # noqa: E402
import names  # noqa: E402  (the product name, file names and links: one place, names.py)

TITLE = names.PRODUCT
SAVE_FOLDER = names.DATA_FOLDER         # under %APPDATA% when not portable
FIRSTRUN_LOG = names.FIRSTRUN_LOG
ISSUES_URL = names.ISSUES_URL
# UI review 26: every failure message says where to report it (the log holds no personal folders)
REPORT = f"\n\nTo report a problem, attach this log on the issues page: {ISSUES_URL}"
GAME_VARS = ("PS3_VFS_ROOT", "KZ3_UPDATE_DIR", "PS3_HDD0_ROOT", "PS3_EDAT_CACHE", "RSX_SHADER_CACHE_DIR")


# ------------------------------------------------------------------------------------------------------- reading
def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


PORTABLE_FILE = "portable.txt"


def data_mode(root, flag=None, ini_value=None):
    """Where data goes -> (portable, rule). Precedence, highest first: a one-run flag ("portable"/"appdata"), the file
    portable.txt next to the launcher (content ignored), kz3-install.ini portable=1/0, the default (AppData)."""
    if flag in ("portable", "appdata"):
        return flag == "portable", f"--{flag} flag"
    if os.path.isfile(os.path.join(root, PORTABLE_FILE)):
        return True, PORTABLE_FILE
    if ini_value in ("0", "1"):
        return ini_value == "1", "kz3-install.ini"
    return False, "default"


def data_line(portable, rule):
    return f"data: {'portable' if portable else 'AppData'} ({rule})"


def read_ini(root, flag=None):
    cp = configparser.ConfigParser()
    cp.read(os.path.join(root, "kz3-install.ini"), encoding="utf-8")
    g = lambda s, k, d="": cp.get(s, k, fallback=d).strip()  # noqa: E731
    portable, rule = data_mode(root, flag, g("install", "portable"))
    return {"portable": portable, "rule": rule, "check_updates": g("online", "check_updates", "0") == "1",
            "last_check": g("online", "last_check"), "dismissed": g("online", "dismissed")}


def write_ini(root, section, key, value):
    p = os.path.join(root, "kz3-install.ini")
    cp = configparser.ConfigParser()
    cp.read(p, encoding="utf-8")
    if not cp.has_section(section):
        cp.add_section(section)
    cp.set(section, key, value)
    with open(p, "w", encoding="utf-8") as f:
        cp.write(f)


def note(root, text):
    """One line in <root>/logs/launcher.log (what the launcher decided; pythonw has no console). Never fails."""
    try:
        d = os.path.join(root, "logs")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "launcher.log"), "a", encoding="utf-8") as f:
            f.write(f"{datetime.datetime.now().isoformat(timespec='seconds')} {text}\n")
    except OSError:
        pass


def ctx_for(root, paths):
    """A firstrun Ctx for the recorded paths, so the fingerprint parts are computed the same way as the build's."""
    return fr.Ctx(root=root, build=os.path.join(root, "build"), dump=paths.get("dump", ""),
                  update=paths.get("update", ""), eboot=paths.get("eboot", ""), facts=paths.get("facts") or {})


# ------------------------------------------------------------------------------------------------------ deciding
def decide(root):
    """-> (action, reason); action is one of first-build, missing-inputs, resume, rebuild, run."""
    build = os.path.join(root, "build")
    paths = load_json(os.path.join(build, "paths.json"))
    if not paths or not paths.get("dump"):
        return "first-build", "The port is not built on this computer yet"
    gone = [paths[k] for k in ("dump", "update", "eboot") if paths.get(k) and not os.path.exists(paths[k])]
    if gone:
        return "missing-inputs", "These game folders or files are not found any more: " + ", ".join(gone)
    stamp = load_json(os.path.join(build, "build-stamp.json"))
    if not stamp or not os.path.isfile(os.path.join(build, "kz3.exe")):
        return "resume", "The last build did not finish"
    if paths.get("eboot") and stamp.get("eboot_stat") != fr.eboot_stat(paths["eboot"]):
        return "rebuild", "The EBOOT changed since the port was built (lift and compile again)"
    now, then = ctx_for(root, paths).parts(), stamp.get("parts") or {}
    if now["base"] != then.get("base"):
        return "rebuild", "The game folders changed since the port was built (lift and compile again)"
    if now["lift"] != then.get("lift"):
        return "rebuild", "This version changes the lift (lift and compile again)"
    if now["runtime"] != then.get("runtime"):
        return "rebuild", "The port was updated (compile again; usually minutes)"
    return "run", ""


def rpcs3_hdd0(update):
    """<x>/dev_hdd0 when the update folder is <x>/dev_hdd0/game/<title id> (RPCS3's layout), else ""."""
    parent = os.path.dirname(os.path.normpath(update))
    hdd0 = os.path.dirname(parent)
    if os.path.basename(parent).lower() == "game" and os.path.basename(hdd0).lower() == "dev_hdd0":
        return hdd0
    return ""


_KNOWN_FOLDERS = {"AppData": "{3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}",       # FOLDERID_RoamingAppData
                  "LocalAppData": "{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}"}   # FOLDERID_LocalAppData


def known_folder(name):
    """The Windows known folder "AppData" or "LocalAppData" from SHGetKnownFolderPath, which holds when the
    environment variable is missing or was changed. "" when not on Windows or the call fails."""
    if sys.platform != "win32":
        return ""
    try:
        import ctypes
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("a", wintypes.DWORD), ("b", wintypes.WORD), ("c", wintypes.WORD), ("d", ctypes.c_ubyte * 8)]
        g = GUID()
        if ctypes.oledll.ole32.CLSIDFromString(_KNOWN_FOLDERS[name], ctypes.byref(g)) != 0:
            return ""
        out = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) != 0:
            return ""
        try:
            return out.value or ""
        finally:
            ctypes.windll.ole32.CoTaskMemFree(out)
    except Exception:
        return ""


def user_folder(environ, name):
    """%APPDATA% (name "AppData") or %LOCALAPPDATA% ("LocalAppData"): the Windows known folder, else the environment
    variable, else "" (not Windows)."""
    return known_folder(name) or environ.get("APPDATA" if name == "AppData" else "LOCALAPPDATA", "")


def data_dir(root, portable, environ, appdata=None):
    """Where saves, logs and caches of play go: the install folder when portable, else %APPDATA%/SAVE_FOLDER."""
    appdata = appdata if appdata is not None else user_folder(environ, "AppData")
    return root if portable or not appdata else os.path.join(appdata, SAVE_FOLDER)


def cache_dir(root, portable, environ, localappdata=None, appdata=None):
    """Where the caches go (shader cache, warm-up output): the install folder when portable, else
    %LOCALAPPDATA%/SAVE_FOLDER (not the roaming %APPDATA%, which a roaming profile would sync; pass 32).
    No %LOCALAPPDATA% (not Windows): the data folder."""
    local = localappdata if localappdata is not None else user_folder(environ, "LocalAppData")
    if portable:
        return root
    return os.path.join(local, SAVE_FOLDER) if local else data_dir(root, portable, environ, appdata)


DATA_ENTRIES = ("gamedata", "keybinds.ini", "online.ini")   # saves and settings (the rest of the data folder is ours)
CACHE_ENTRIES = ("cache",)                                  # <cache dir>/cache/shaders and /shader-warmup


def _tree(path):
    """{relative path: size} of a file or folder, for comparing a copy with its source."""
    if os.path.isfile(path):
        return {"": os.path.getsize(path)}
    out = {}
    for d, dirs, files in os.walk(path):
        rel = os.path.relpath(d, path)
        for n in dirs:
            out[os.path.normpath(os.path.join(rel, n)) + os.sep] = 0
        for n in files:
            out[os.path.normpath(os.path.join(rel, n))] = os.path.getsize(os.path.join(d, n))
    return out


def _hashes(path):
    import hashlib
    files = [path] if os.path.isfile(path) else [os.path.join(d, n) for d, _, fs in os.walk(path) for n in fs]
    out = []
    for f in sorted(files):
        h = hashlib.sha256()
        with open(f, "rb") as fh:
            for c in iter(lambda: fh.read(1 << 20), b""):
                h.update(c)
        out.append(h.hexdigest())
    return out


def move_entries(entries, src, dst, hashed=True, copy=None):
    """Move the named entries of folder src into folder dst without any chance of losing data: copy each to a
    temporary name, verify (same files and sizes; SHA-256 too when hashed), rename into place, and only when every entry
    is in place remove the sources. An entry already in dst, or a failed copy or check, undoes the copies made and
    leaves src untouched. -> (ok, message). `copy` is replaceable for tests."""
    import shutil
    copy = copy or (lambda a, b: shutil.copytree(a, b) if os.path.isdir(a) else shutil.copy2(a, b))
    todo = [n for n in entries if os.path.exists(os.path.join(src, n))]
    clash = [n for n in todo if os.path.exists(os.path.join(dst, n))]
    if clash:
        return False, f"{', '.join(clash)} already exist(s) in {dst}; nothing was moved"
    made, tmps = [], []

    def rm(p):
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        elif os.path.exists(p):
            os.remove(p)
    try:
        os.makedirs(dst, exist_ok=True)
        for n in todo:
            a, t = os.path.join(src, n), os.path.join(dst, n + ".copying")
            tmps.append(t)
            copy(a, t)
            if _tree(a) != _tree(t) or (hashed and _hashes(a) != _hashes(t)):
                raise OSError(f"the copy of {n} does not match the original")
        for n, t in zip(todo, list(tmps)):
            os.rename(t, os.path.join(dst, n))
            tmps.remove(t)
            made.append(os.path.join(dst, n))
    except OSError as e:
        for p in tmps + made:
            rm(p)
        return False, f"could not copy to {dst} ({e}); your files are unchanged"
    left = []
    for n in todo:
        p = os.path.join(src, n)
        try:
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        except OSError:
            left.append(n)
    return True, (f"moved {len(todo)} item(s) to {dst}" + (f"; could not remove the old copy of {', '.join(left)} "
                                                           f"(delete it by hand)" if left else ""))


def migrate_caches(environ, appdata=None, localappdata=None):
    """Once, non-portable: the shader caches earlier versions kept in the roaming %APPDATA%/SAVE_FOLDER/cache move
    to %LOCALAPPDATA%/SAVE_FOLDER/cache. -> a line for launcher.log, or "" when there is nothing to move."""
    appdata = appdata if appdata is not None else user_folder(environ, "AppData")
    local = localappdata if localappdata is not None else user_folder(environ, "LocalAppData")
    if not appdata or not local:
        return ""
    src, dst = os.path.join(appdata, SAVE_FOLDER), os.path.join(local, SAVE_FOLDER)
    if os.path.normcase(src) == os.path.normcase(dst) or not os.path.exists(os.path.join(src, "cache")):
        return ""
    if os.path.exists(os.path.join(dst, "cache")):   # both: the old one is only a cache, drop it
        import shutil
        shutil.rmtree(os.path.join(src, "cache"), ignore_errors=True)
        return "cache: dropped the old roaming cache (a local one exists)"
    ok, msg = move_entries(CACHE_ENTRIES, src, dst, hashed=False)
    return "cache: " + (f"moved the roaming cache to %LOCALAPPDATA% ({msg})" if ok else msg)


def switch_data(root, to_portable, environ, appdata=None, localappdata=None):
    """Move saves, settings and caches between the install folder (portable) and %APPDATA% / %LOCALAPPDATA%, then record
    the mode in kz3-install.ini. Copy, verify, then remove: a failure at any step leaves the old data and the old mode.
    -> (ok, message)."""
    cur = read_ini(root)["portable"]
    marker = os.path.join(root, PORTABLE_FILE)
    if cur == to_portable and os.path.isfile(marker) == to_portable:
        return False, "Already " + ("portable." if to_portable else "using %APPDATA%.")
    new = to_portable
    d_src, d_dst = data_dir(root, cur, environ, appdata), data_dir(root, new, environ, appdata)
    c_src = cache_dir(root, cur, environ, localappdata, appdata)
    c_dst = cache_dir(root, new, environ, localappdata, appdata)
    if os.path.normcase(d_src) == os.path.normcase(d_dst):
        return False, "%APPDATA% is not available on this computer."
    ok, m1 = move_entries(DATA_ENTRIES, d_src, d_dst, hashed=True)
    if not ok:
        return False, m1
    m2 = "unchanged"
    if os.path.normcase(c_src) != os.path.normcase(c_dst):
        ok, m2 = move_entries(CACHE_ENTRIES, c_src, c_dst, hashed=False)
        if not ok:
            m2 = "left where they were and rebuilt later (" + m2 + ")"   # a cache is never worth failing for
    had = os.path.isfile(marker)
    try:
        if new and not had:
            open(marker, "w").close()
        elif not new and had:
            os.remove(marker)
        write_ini(root, "install", "portable", "1" if new else "0")
    except OSError as e:
        try:
            if had and not os.path.isfile(marker):
                open(marker, "w").close()
            elif not had and os.path.isfile(marker):
                os.remove(marker)
        except OSError:
            pass
        move_entries(DATA_ENTRIES, d_dst, d_src, hashed=True)   # the mode did not change: put the saves back
        return False, f"could not record the new setting ({e}); your files are unchanged"
    return True, f"{m1}. Caches: {m2}"


def migrate_data(environ, old, new=None, appdata=None):
    """One-time move of the data folder %APPDATA%/<old> (a name it had before a rename: saves in gamedata,
    keybinds.ini, online.ini, caches) to %APPDATA%/<new> (default SAVE_FOLDER). -> a line for launcher.log, or ""
    when there is nothing to move.
    The new folder missing: the old one is renamed (same volume, one step). Both present: each entry of the old folder
    the new one lacks is moved, an entry present in both is left in the old folder (never overwritten), and the old
    folder is removed when it ends up empty. Portable installs keep their data in the install folder: not called."""
    appdata = appdata if appdata is not None else user_folder(environ, "AppData")
    if not appdata:
        return ""
    new = new or SAVE_FOLDER
    if not old or old.lower() == new.lower():
        return ""
    oname, nname = old, new
    old, new = os.path.join(appdata, old), os.path.join(appdata, new)
    if not os.path.isdir(old):
        return ""
    try:
        if not os.path.exists(new):
            os.rename(old, new)
            return f"data: moved {oname} to {nname} (under %APPDATA%)"
        moved, kept = [], []
        for name in sorted(os.listdir(old)):
            if os.path.exists(os.path.join(new, name)):
                kept.append(name)
            else:
                os.rename(os.path.join(old, name), os.path.join(new, name))
                moved.append(name)
        if not kept:
            os.rmdir(old)
        return (f"data: moved {len(moved)} entries of {oname} into {nname}"
                + (f"; kept in the old folder (already in the new one): {', '.join(kept)}" if kept else ""))
    except OSError as e:
        return f"data: could not move {oname} to {nname} ({e}); the old folder is unchanged or partly moved"


def shader_dir(data):
    """The shader disk cache + pso_keys.bin + pso_library.bin (patch 2031), via the runtime's RSX_SHADER_CACHE_DIR
    (rsx_live_draw.c shader_disk_cache_prepare; default <cwd>/scratch/rsx_shader_cache_v2). The runtime creates only
    the last folder level, so the launcher creates it first."""
    return os.path.join(data, "cache", "shaders")


VFS = "vfs"                 # <root>/vfs: the game's writable VFS root (pass 17)
VFS_LINK = "link.json"      # what <root>/vfs/PS3_GAME was made to point at


def vfs_root(root):
    return os.path.join(root, VFS)


def _is_dir_link(p):
    """A symlink or (Windows) a directory junction: removing it never touches its target."""
    if os.path.islink(p):
        return True
    isj = getattr(os.path, "isjunction", None)   # Python 3.12+
    return bool(isj and isj(p))


def _make_dir_link(target, link):
    """Directory junction on Windows (no admin rights or developer mode needed), else a symlink. -> error or ""."""
    errs = []
    if os.name == "nt":
        try:
            import _winapi
            _winapi.CreateJunction(target, link)
            return ""
        except (ImportError, AttributeError, OSError) as e:
            errs.append(f"junction: {e}")
    try:
        os.symlink(target, link, target_is_directory=True)
        return ""
    except (OSError, NotImplementedError) as e:
        errs.append(f"symlink: {e}")
    return "; ".join(errs)


def link_free(root, build=None):
    """True when the installed kz3.exe takes the game folder and the writable folder as two settings (kz3.ini
    vfs_root = the dump, cache_root = <root>/vfs; build-stamp.json "cache_root", set by firstrun.after_stamp when the
    payload's port/kz3/kz3_ini.h has the key). Then nothing is linked: the game reads the dump in place. An exe from an
    older payload needs the junction (ensure_vfs, pass 17) and keeps getting it. Why (round KZ3-03): the first native
    Windows install could not open anything through the junction."""
    stamp = load_json(os.path.join(build or os.path.join(root, "build"), "build-stamp.json")) or {}
    return bool(stamp.get("cache_root"))


def ensure_vfs(root, dump, no_link=False):
    """Make <root>/vfs with PS3_GAME -> <dump>/PS3_GAME. -> (vfs folder, error text or "").

    no_link (link_free): only make <root>/vfs (the writable cache root) and drop the link an older start left in it
    (removing a link never touches its target; a real PS3_GAME folder there is left alone).

    The player's dump is read in place and never written: the runtime's writes (psarc-cache, /dev_hdd1 cache) land
    in <root>/vfs. A real folder already named PS3_GAME there is never removed (error instead); a link to another
    dump (the player picked a new folder) is replaced; removing a link leaves its target alone."""
    v = vfs_root(root)
    link = os.path.join(v, "PS3_GAME")
    target = os.path.join(dump, "PS3_GAME")
    rec = os.path.join(v, VFS_LINK)
    os.makedirs(v, exist_ok=True)
    if no_link:
        if _is_dir_link(link):
            try:
                try:
                    os.unlink(link)
                except OSError:
                    os.rmdir(link)
            except OSError as e:
                return v, f"Could not remove the old link {link} ({e})"
            try:
                os.remove(rec)
            except OSError:
                pass
        return v, ""
    if _is_dir_link(link) or (os.path.isdir(link) and (load_json(rec) or {}).get("link") == link):
        if (load_json(rec) or {}).get("target") == target and os.path.isdir(link):
            return v, ""
        try:
            try:
                os.unlink(link)
            except OSError:
                os.rmdir(link)      # a junction on older Pythons; RemoveDirectory removes the link only
        except OSError as e:
            return v, f"Could not replace the old link {link} ({e})"
    elif os.path.lexists(link):
        return v, f"{link} is a real folder, not a link to the game folder; move it away and start again"
    err = _make_dir_link(target, link)
    if err:
        return v, f"Could not link {target} into {v} ({err})"
    with open(rec, "w", encoding="utf-8") as f:
        json.dump({"link": link, "target": target}, f, indent=1)
    return v, ""


def game_env(root, paths, portable, environ, appdata=None, localappdata=None, build=None):
    """-> (argv, cwd, env, added): the game command for the recorded paths.
    Link-free exe: PS3_VFS_ROOT = the dump itself (read in place), KZ3_CACHE_ROOT = <root>/vfs (every write)."""
    cwd = data_dir(root, portable, environ, appdata)
    added = {"PS3_VFS_ROOT": vfs_root(root), "PS3_EDAT_CACHE": os.path.join(root, "build", "cache", "edat"),
             "RSX_SHADER_CACHE_DIR": shader_dir(cache_dir(root, portable, environ, localappdata, appdata))}
    if link_free(root, build) and paths.get("dump"):
        added["PS3_VFS_ROOT"] = paths["dump"]
        added["KZ3_CACHE_ROOT"] = vfs_root(root)
    if paths.get("update"):
        added["KZ3_UPDATE_DIR"] = os.path.join(paths["update"], "USRDIR")
        hdd0 = rpcs3_hdd0(paths["update"])
        if hdd0:   # ASSUMPTION (plan 4.3): trophies + update resolve; cellSaveData keeps ./gamedata (cwd), not this
            added["PS3_HDD0_ROOT"] = hdd0
    env = dict(environ)
    env.update(added)
    return [os.path.join(root, "build", "kz3.exe"), paths["eboot"]], cwd, env, added


GAME_INI = "kz3.ini"         # next to build\kz3.exe: the game starts with no launch options (pass 26)
# (section, key) in file order; the runtime's mapping to environment variables is port/kz3/kz3_ini.h.
GAME_INI_KEYS = (("game", "eboot"), ("game", "vfs_root"), ("game", "cache_root"), ("game", "update_dir"), ("game", "hdd0_root"),
                 ("game", "edat_cache"), ("game", "shader_cache"), ("game", "data_dir"), ("game", "font_dir"),
                 ("game", "lang"), ("display", "window_size"))
# Player choices: from paths.json when the first run recorded one, else the value already in kz3.ini (hand edits stay).
GAME_INI_CHOICES = ("font_dir", "lang", "window_size")
GAME_INI_HEAD = f"""; {names.PRODUCT}: what kz3.exe reads when it is started with no arguments (written by the first-run
; build and the launcher; the keys below the paths are yours to edit). An environment variable that is set wins:
; vfs_root = PS3_VFS_ROOT (the folder that holds PS3_GAME: your dump, read only), cache_root = KZ3_CACHE_ROOT (the
; writable folder for the unpacked archive and the cache; empty = vfs_root), update_dir = KZ3_UPDATE_DIR, hdd0_root = PS3_HDD0_ROOT, edat_cache = PS3_EDAT_CACHE,
; shader_cache = RSX_SHADER_CACHE_DIR, font_dir = KZ3_FONT_DIR (your PS3 firmware's dev_flash/data/font folder,
; optional), lang = KZ3_LANG (optional; default: the system language), window_size = KZ3_WINDOW_SIZE (WxH or
; 720p/900p/1080p/1440p/2160p/4k; empty = 1280x720). data_dir is the working folder (saves).
"""


def read_game_ini(path):
    """kz3.ini as {key: value} (keys lower-case, sections ignored as the runtime does, kz3_ini.h: the first non-empty
    value of a key counts)."""
    out = {}
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return out
    text = raw.decode("utf-8-sig") if raw.startswith(b"\xef\xbb\xbf") else raw.decode("utf-8", "replace")
    for line in text.splitlines():
        s = line.strip()
        if not s or s[0] in ";#[" or "=" not in s:
            continue
        k, v = (x.strip() for x in s.split("=", 1))
        if len(v) >= 2 and v[0] == v[-1] == '"':
            v = v[1:-1]
        if v:
            out.setdefault(k.lower(), v)
    return out


def game_ini(root, paths, portable, environ, appdata=None, build=None, localappdata=None):
    """-> {key: value} for kz3.ini: the same game paths game_env gives as variables, plus the EBOOT, the working
    folder and the player's choices (paths.json, else what kz3.ini already holds)."""
    _, cwd, _, added = game_env(root, paths, portable, environ, appdata, localappdata, build)
    old = read_game_ini(os.path.join(build or os.path.join(root, "build"), GAME_INI))
    # Step 9s: an exe with the EBOOT linked in (build-stamp.json eboot_embedded) gets no eboot line; kz3.exe still
    # reads one from an older kz3.ini, and an exe built before 9s keeps needing it.
    stamp = load_json(os.path.join(build or os.path.join(root, "build"), "build-stamp.json")) or {}
    v = {"eboot": "" if stamp.get("eboot_embedded") else paths.get("eboot", ""), "vfs_root": added["PS3_VFS_ROOT"],
         "cache_root": added.get("KZ3_CACHE_ROOT", ""), "update_dir": added.get("KZ3_UPDATE_DIR", ""), "hdd0_root": added.get("PS3_HDD0_ROOT", ""),
         "edat_cache": added["PS3_EDAT_CACHE"], "shader_cache": added["RSX_SHADER_CACHE_DIR"], "data_dir": cwd}
    for k in GAME_INI_CHOICES:
        v[k] = paths.get(k) or old.get(k, "")
    return v


def write_game_ini(root, values, build=None):
    """Write <root>/build/kz3.ini; -> its path. The runtime opens files with the ANSI code page (fopen), so on Windows
    the file is in that code page ("mbcs"); a path it cannot encode falls back to UTF-8 (such a path would not open
    either way: ASSUMPTION, non-ANSI install paths are untested)."""
    p = os.path.join(build or os.path.join(root, "build"), GAME_INI)
    lines, sec = [GAME_INI_HEAD.rstrip("\n")], None
    for s, k in GAME_INI_KEYS:
        if k == "eboot" and not values.get(k):
            continue   # step 9s: the EBOOT is inside kz3.exe
        if s != sec:
            lines += ["", f"[{s}]"]
            sec = s
        lines.append(f"{k} = {values.get(k, '')}")
    text = "\n".join(lines) + "\n"
    try:
        data = text.encode("mbcs" if os.name == "nt" else "utf-8")
    except UnicodeEncodeError:
        data = text.encode("utf-8")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data.replace(b"\n", b"\r\n") if os.name == "nt" else data)
    return p


TEMPLATE_SHIPPED = "pso-template.bin"        # in the install folder when the release carries one (assemble_payload.py)
TEMPLATE_EXPORT = "pso-template-export.bin"   # in <data>, written by "Export pipeline template"
TEMPLATE_TOOL = os.path.join("kz3-setup", "pso_template.py")


def template_env(root, cwd, environ, export=False):
    """The pipeline-template variables for the game (RSX pass 67): LD_PSO_TEMPLATE = the shipped template when the install
    has one, LD_PSO_TEMPLATE_OUT = <data>/pso-template-export.bin for the export run. A variable the player set wins."""
    add = {}
    shipped = os.path.join(root, TEMPLATE_SHIPPED)
    if os.path.isfile(shipped):
        add["LD_PSO_TEMPLATE"] = shipped
    if export:
        add["LD_PSO_TEMPLATE_OUT"] = os.path.join(cwd, TEMPLATE_EXPORT)
    return {k: v for k, v in add.items() if not environ.get(k)}


def game_argv(root, paths, extra=()):
    """The launcher's game command: kz3.exe alone (it reads kz3.ini); with extra game arguments, the EBOOT first as
    before (kz3.exe's argv[1] is the EBOOT)."""
    exe = os.path.join(root, "build", "kz3.exe")
    return [exe, paths["eboot"], *extra] if extra else [exe]


def firstrun_cmd(root, action, paths, portable, python):
    """The build command for an action other than run."""
    a = [python, os.path.join(HERE, "firstrun.py"), "--root", root, "--log-dir", root, "--log-name", FIRSTRUN_LOG]
    if action in ("first-build", "missing-inputs") or not paths:
        return a + ["--interactive"] + (["--portable"] if portable else [])
    a += ["--bar", "--dump", paths["dump"]]
    for k in ("update", "eboot"):
        if paths.get(k):
            a += [f"--{k}", paths[k]]
    return a


def shaders_cmd(root, python):
    """The launcher's "Prepare shaders again": always runs (--force)."""
    return [python, os.path.join(HERE, "shaders.py"), "--root", root, "--force"]


def verify_install(root):
    """The payload files against <root>/manifest.json (assemble_payload.py): -> (checked, [problems]).
    No manifest (a source tree, a developer run) -> (0, [])."""
    man = load_json(os.path.join(root, "manifest.json"))
    if not man or not isinstance(man.get("files"), list):
        return 0, []
    import hashlib
    bad, n = [], 0
    for e in man["files"]:
        p = os.path.join(root, *e["path"].split("/"))
        if e.get("portable_only") and not os.path.exists(p):
            continue   # run.cmd: the zip's entry point; the installer leaves it out (the launcher replaces it)
        n += 1
        try:
            if os.path.getsize(p) != e["size"]:
                bad.append(f"{e['path']} (changed)")
                continue
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            if h.hexdigest() != e["sha256"]:
                bad.append(f"{e['path']} (changed)")
        except OSError:
            bad.append(f"{e['path']} (missing)")
    return n, bad


def repair(root, action, reason, paths, ini, ui, run):
    """--repair: verify the install, then re-run the checks and rebuild what changed (docstring)."""
    checked, bad = verify_install(root)
    if bad:
        more = f"\n... and {len(bad) - 5} more" if len(bad) > 5 else ""
        ui.info(f"{len(bad)} of the {checked} files of this install are missing or changed:\n" + "\n".join(bad[:5]) +
                more + "\n\nRun the setup again (or unpack the zip again) over this folder; your saves stay.")
        return 1
    if action in ("first-build", "missing-inputs"):
        q = f"{reason}.\n\n{QUESTION[action]}"
    else:
        q = ("Check your game files again and rebuild what changed? Steps whose inputs did not change are kept, "
             "so this takes from a minute to a full build; a console window shows the progress.")
    if not ui.ask(q):
        return 0
    flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
    rc = run(firstrun_cmd(root, "rebuild" if action not in ("first-build", "missing-inputs") else action, paths,
                          ini["portable"], console_python()), creationflags=flags)
    after, _ = decide(root)
    if rc != 0 or after != "run":
        ui.info(f"{did_not_finish(rc, 'The check or the build did not finish')}. {log_note(root)}")
        return rc or 1
    ui.info(f"Verified: {checked} install files intact, your game files pass the checks and the port is up to date.")
    return 0


def check_due(ini, today):
    return ini["check_updates"] and ini["last_check"] != today.isoformat()


def newer_from_result(path, dismissed):
    """(latest, url) when the last background check found a newer release not dismissed yet, else None."""
    try:
        with open(path, encoding="utf-8") as f:
            line = f.readline().strip()
    except OSError:
        return None
    kv = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
    if line.startswith("VERSION ") and kv.get("status") == "newer" and kv.get("latest") not in (None, "-", dismissed):
        return kv["latest"], kv.get("url", "-")
    return None


# ------------------------------------------------------------------------------------------------------- the UI
class UI:
    """Message boxes on Windows (pythonw has no console), the terminal elsewhere (developer runs)."""

    def ask(self, text):
        if os.name == "nt":
            import ctypes
            return ctypes.windll.user32.MessageBoxW(None, text, TITLE, 0x4 | 0x20) == 6   # YESNO|QUESTION -> IDYES
        return input(f"{text} [y/N] ").strip().lower().startswith("y")

    def info(self, text):
        if os.name == "nt":
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, text, TITLE, 0x40)
        else:
            print(text)

    def open_folder(self, path):
        os.makedirs(path, exist_ok=True)
        if os.name == "nt":
            os.startfile(path)
        else:
            print(path)

    def open_url(self, url):
        import webbrowser
        webbrowser.open(url)


def console_python():
    """python.exe beside pythonw.exe (the build needs a console window), else this interpreter."""
    exe = sys.executable
    if os.path.basename(exe).lower() == "pythonw.exe":
        alt = os.path.join(os.path.dirname(exe), "python.exe")
        if os.path.isfile(alt):
            return alt
    return exe


def run_wait(argv, **kw):
    return subprocess.call(argv, **kw)


CONSOLE_CLOSED = (0xC000013A, 0x40010004, 3221225786 - 2**32, 0xC000013A - 2**32)   # STATUS_CONTROL_C_EXIT, DBG_TERMINATE_PROCESS (as signed too)


def did_not_finish(rc, what):
    """The dialog line for a first-run/rebuild child that ended with rc != 0 (release pass 51: X on the console window)."""
    if rc in CONSOLE_CLOSED:
        what += " (its window was closed before it finished; finished steps are kept, open the program again to continue)"
    return f"{what} (code {rc})" if rc not in CONSOLE_CLOSED else what


def log_note(root):
    """The dialog's log sentence: the first-run log's path and how to report it, or, when the window was closed before
    the first check wrote it (VM test 2026-10-27), that no log exists yet."""
    log = os.path.join(root, FIRSTRUN_LOG)
    if os.path.isfile(log):
        return f"The log is {log}.{REPORT}"
    return "No log was written yet: it starts with the first check."


def spawn(argv, **kw):
    return subprocess.Popen(argv, **kw)


QUESTION = {
    "first-build": "Build it now? The first build lifts and compiles the port from your own game files and takes "
                   "5 to 30 minutes; a console window shows the progress.",
    "missing-inputs": "Pick the folders again and build? (your saves stay where they are)",
    "resume": "Continue the build now? Finished steps are skipped.",
    "rebuild": "Build again now?",
}


def support_package(root, ini, environ, out, ui, today):
    """--support-package: see support.py. Returns the exit code."""
    import support
    import support_text as st
    if not out:
        home = environ.get("USERPROFILE") or os.path.expanduser("~")
        desk = os.path.join(home, "Desktop")
        out = desk if os.path.isdir(desk) else root
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    try:
        dest, skipped = support.build(root, data_dir(root, ini["portable"], environ), out, fr.redactor(), stamp)
    except OSError as e:
        ui.info(st.FAILED.format(why=e.strerror or "the folder cannot be written"))
        return 1
    note(root, "support package: " + (os.path.basename(dest) if dest else "nothing to collect")
         + (f" ({'; '.join(skipped)})" if skipped else ""))
    if not dest:
        ui.info(st.NOTHING)
        return 1
    ui.info(st.DONE.format(path=dest, issues=ISSUES_URL))
    ui.open_folder(os.path.dirname(dest))
    return 0


def crash_check(session, root):
    """session.check() that never raises (a crash report is never worth a launcher failure)."""
    try:
        return session.check()
    except Exception as e:  # noqa: BLE001
        note(root, f"crash report: skipped ({type(e).__name__}: {e})")
        return None


def crash_toggle(root, ui):
    """--crash-reports: say whether crash reports are on and offer to switch (writes [crash] report=1|0). Turning
    them on records the crash files already there so they are never sent."""
    available, on, text = crash_report.toggle_text(root)
    if not available:
        ui.info(text)
        return 0
    if ui.ask(text):
        if not on:
            crash_report.Session(root, log=lambda t: note(root, t)).mark_existing()
        crash_report.set_report(root, not on)
        note(root, f"crash reports: {'off' if on else 'on'}")
        ui.info(f"Crash reports are {'off' if on else 'on'}.")
    return 0


def main(argv=None, ui=None, run=run_wait, start=spawn, today=None, environ=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--print", action="store_true")
    ap.add_argument("--prepare-shaders", action="store_true",
                    help='"Prepare shaders again": the warm-up session (shaders.py --force) in a console window')
    ap.add_argument("--repair", action="store_true",
                    help='"Verify and rebuild": check the install and the game files again, rebuild what changed')
    ap.add_argument("--open-data", action="store_true",
                    help='"Open saves folder": the folder with saves and settings in Explorer')
    ap.add_argument("--switch-data", action="store_true",
                    help='"Move saves and settings": between the install folder (portable) and %%APPDATA%%')
    ap.add_argument("--portable", action="store_true", help="this run only: data in the install folder (changes no file)")
    ap.add_argument("--appdata", action="store_true", help="this run only: data in %%APPDATA%% (changes no file)")
    ap.add_argument("--support-package", action="store_true",
                    help='"Make a support package": one zip of logs and settings (user names removed) for a bug report')
    ap.add_argument("--crash-reports", action="store_true",
                    help='"Crash reports on or off": show whether crash reports are sent and switch them')
    ap.add_argument("--out", default="", help="with --support-package: the folder for the zip (default: the Desktop)")
    ap.add_argument("--export-template", action="store_true",
                    help='"Export pipeline template": run the game with LD_PSO_TEMPLATE_OUT in the data folder, then '
                         'show where the file went')
    a, extra = ap.parse_known_args(argv)
    root = os.path.abspath(a.root)
    ui = ui or UI()
    environ = dict(os.environ if environ is None else environ)
    today = today or datetime.date.today()
    flag = "portable" if a.portable else "appdata" if a.appdata else None
    ini = read_ini(root, flag)
    action, reason = decide(root)
    paths = load_json(os.path.join(root, "build", "paths.json")) or {}

    if a.open_data:
        folder = data_dir(root, ini["portable"], environ)
        note(root, data_line(ini["portable"], ini["rule"]))
        ui.info(f"{data_line(ini['portable'], ini['rule'])}\n{folder}")
        ui.open_folder(folder)
        return 0

    if a.crash_reports:
        return crash_toggle(root, ui)
    if a.support_package:
        return support_package(root, ini, environ, a.out, ui, today)

    if a.switch_data:
        ini = read_ini(root)   # a one-run flag does not decide what is switched
        to = not ini["portable"]
        src, dst = data_dir(root, ini["portable"], environ), data_dir(root, to, environ)
        if not ui.ask(f"Move your saves and settings to {dst}?\n\nThey are copied, checked, and only then removed from "
                      f"{src}. Close the game first. Caches (shaders) move too, to "
                      f"{cache_dir(root, to, environ)}."):
            return 0
        ok, msg = switch_data(root, to, environ)
        note(root, f"switch-data to {'portable' if to else '%APPDATA%'}: {msg}")
        ui.info(("Done. " if ok else "Not changed. ") + msg + ("" if ok else REPORT))
        return 0 if ok else 1

    if a.print:
        rep = {"action": action, "reason": reason, "portable": ini["portable"], "data_rule": ini["rule"],
               "version_check_due": check_due(ini, today)}
        if action == "run":
            _, cwd, _, added = game_env(root, paths, ini["portable"], environ)
            rep["template_env"] = template_env(root, cwd, environ, a.export_template)
            rep.update(argv=game_argv(root, paths, extra), cwd=cwd, env=added,
                       ini=game_ini(root, paths, ini["portable"], environ))
        else:
            rep["build"] = firstrun_cmd(root, action, paths, ini["portable"], console_python())
        if a.repair:
            checked, bad = verify_install(root)
            rep["repair"] = {"checked": checked, "problems": bad,
                             "build": firstrun_cmd(root, action if action in ("first-build", "missing-inputs")
                                                   else "rebuild", paths, ini["portable"], console_python())}
        print(json.dumps(rep, indent=1))
        return 0

    logs = os.path.join(root, "logs")
    note(root, f"start: {action}" + (f" ({reason})" if reason else ""))
    note(root, data_line(ini["portable"], ini["rule"]))
    if not ini["portable"]:
        for old in names.OLD_DATA_FOLDERS:
            moved = migrate_data(environ, old)
            if moved:
                note(root, moved)
        moved = migrate_caches(environ)
        if moved:
            note(root, moved)
    nv = newer_from_result(os.path.join(logs, "version-check.txt"), ini["dismissed"])
    if nv:
        if ui.ask(f"{TITLE} {nv[0]} is out. Open its release page?"):
            if nv[1].startswith("https://"):
                ui.open_url(nv[1])
        else:
            write_ini(root, "online", "dismissed", nv[0])
    if check_due(ini, today):
        os.makedirs(logs, exist_ok=True)
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        start([sys.executable, os.path.join(HERE, "version_check.py"), "--current", fr.port_version(root),
               "--timeout", "8", "--result-file", os.path.join(logs, "version-check.txt"),
               "--log", os.path.join(logs, "version-check.log")], creationflags=flags)
        write_ini(root, "online", "last_check", today.isoformat())

    if a.repair:
        return repair(root, action, reason, paths, ini, ui, run)

    if a.prepare_shaders:
        if action != "run":
            ui.info(f"{reason}. Start {TITLE} once to build it, then prepare the shaders.")
            return 1
        if not ui.ask("Prepare shaders again? The game runs minimised and plays the start of the first level by "
                      "itself for about 7 minutes (17 the first time), so shaders compile now instead of while you play."):
            return 0
        flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
        rc = run(shaders_cmd(root, console_python()), creationflags=flags)
        if rc != 0:
            ui.info(f"Preparing shaders did not finish (code {rc}). The game's output is in "
                    f"{os.path.join(cache_dir(root, ini['portable'], environ), 'cache', 'shader-warmup')}.{REPORT}")
        return rc

    if action != "run":
        if not ui.ask(f"{reason}.\n\n{QUESTION[action]}"):
            return 0
        flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
        rc = run(firstrun_cmd(root, action, paths, ini["portable"], console_python()), creationflags=flags)
        if rc != 0:
            ui.info(f"{did_not_finish(rc, 'The build did not finish')}. {log_note(root)}")
            return rc
        ini = read_ini(root, flag)   # --interactive may have written portable/check_updates
        action, reason = decide(root)
        paths = load_json(os.path.join(root, "build", "paths.json")) or {}
        if action != "run":
            ui.info(f"{reason}. {log_note(root)}")
            return 1

    _, cwd, _, added = game_env(root, paths, ini["portable"], environ)
    _, err = ensure_vfs(root, paths["dump"], link_free(root))
    note(root, f"vfs: {err or 'ok'}")
    if err:
        ui.info(f"{err}.{REPORT}")
        return 1
    gargv = game_argv(root, paths, extra)
    note(root, f"kz3.ini: {write_game_ini(root, game_ini(root, paths, ini['portable'], environ))}")
    note(root, "game: " + " ".join(gargv) + f" (cwd {cwd}; the game's log is in build\\logs)")
    fr.place_fonts(root, os.path.dirname(gargv[0]))     # the notice fonts next to kz3.exe (RSX pass 38)
    fr.place_sdl3(root, os.path.dirname(gargv[0]))      # SDL3.dll next to kz3.exe: native pads (patch 4010)
    os.makedirs(cwd, exist_ok=True)
    os.makedirs(added["PS3_EDAT_CACHE"], exist_ok=True)
    os.makedirs(added["RSX_SHADER_CACHE_DIR"], exist_ok=True)
    tenv = template_env(root, cwd, environ, a.export_template)
    if tenv:
        note(root, "pipeline template: " + " ".join(f"{k}={v}" for k, v in tenv.items()))
    # the paths are in kz3.ini (pass 26); the player's own variables win
    out = tenv.get("LD_PSO_TEMPLATE_OUT") or environ.get("LD_PSO_TEMPLATE_OUT", "")
    if a.export_template and out and os.path.isfile(out):
        os.remove(out)   # an export left by an earlier run must not pass as this run's result
    crash = crash_report.Session(root, environ, data_dir(root, ini["portable"], environ), lambda t: note(root, t))
    before = threading.Thread(target=crash_check, args=(crash, root), daemon=True)
    before.start()   # a crash from a run the launcher did not see end: sent while the game runs
    rc = run(gargv, cwd=cwd, env={**environ, **tenv})
    before.join(crash_report.TIMEOUT + 5)
    msg = crash_report.notice(crash_check(crash, root))
    if msg:
        ui.info(msg)
    if a.export_template:
        tool = os.path.join(root, TEMPLATE_TOOL)
        if rc == 0 and out and os.path.isfile(out) and run([console_python(), tool, "check", out]) == 0:
            note(root, f"template exported: {out}")
            ui.info(f"Your pipeline template is saved as\n{out}\n\nIt holds only hashes and render-state numbers, no game "
                    f"data. To share it, attach it to an issue: {ISSUES_URL}")
            ui.open_folder(os.path.dirname(out))
        else:
            ui.info(f"No pipeline template was written (the game ended with code {rc}). Play for a few minutes, "
                    f"then quit from the menu so the file is saved.{REPORT}")
            return rc or 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
