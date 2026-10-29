# Copy of the RPCS3 locator shared with the Killzone 2 port's tools (do not edit here; change the shared module and copy it again). Used by rpcs3_import.py.
#!/usr/bin/env python3
"""Find what a player already has in RPCS3 (disc, update, saves, fonts, decrypted EBOOT).

Shared by the PS3 port projects' installers.  Python 3.11+, standard library only.

Rules: read-only (the one exception is copy_save with dry_run=False); never opens
*.rap, act.dat, exdata/ or cache/; no wide scans (known places and caller roots
only); no decryption and no keys.  Errors are returned as dicts
({"ok": False, "code": ..., "text": ...}), never raised.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
from pathlib import Path

_MAX_SFO = 1 << 20


def _err(code, text, **extra):
    d = {"ok": False, "code": code, "text": text}
    d.update(extra)
    return d


# --- the one read funnel -------------------------------------------------

# RPCS3 roots seen so far (root, dev_hdd0 in use). "cache" and "exdata" are
# forbidden only relative to these, so a game under D:\\cache\\... is fine.
_ROOTS: list[tuple[str, str]] = []


def _nc(p) -> str:
    return os.path.normcase(os.path.abspath(str(p)))


def _register(root, hdd0):
    t = (_nc(root), _nc(hdd0))
    if t not in _ROOTS:
        _ROOTS.append(t)


def _under(p: str, base: str):
    """Parts of p below base, or None."""
    if p == base:
        return []
    pre = base.rstrip(os.sep) + os.sep
    return p[len(pre):].split(os.sep) if p.startswith(pre) else None


def _forbidden(path) -> bool:
    name = os.path.basename(str(path).replace("\\", "/")).lower()
    if name.endswith(".rap") or name == "act.dat":
        return True
    p = _nc(path)
    for root, hdd0 in _ROOTS:
        rel = _under(p, os.path.join(root, "cache"))
        if rel is not None:
            return True
        rel = _under(p, os.path.join(hdd0, "home"))
        if rel is not None and len(rel) >= 2 and rel[1].lower() == "exdata":
            return True
    return False


def _open_ro(path, mode="rb"):
    """Every read goes through here. Refuses licence/key files and caches."""
    if "r" not in mode or any(c in mode for c in "wax+"):
        raise PermissionError("read-only module")
    if _forbidden(path):
        raise PermissionError(f"refusing to open {path}")
    return open(path, mode)


def _read_text(path):
    try:
        with _open_ro(path, "rb") as f:
            return f.read(_MAX_SFO * 4).decode("utf-8", "replace")
    except OSError:
        return None


# --- minimal YAML --------------------------------------------------------

def _unquote(s: str) -> str:
    s = s.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "'\"":
        return s[1:-1]
    return s


def parse_kv(text: str) -> dict:
    """`key: value` lines only. Splits at the first ':' followed by space or end,
    so Windows paths (C:\\x) in values and '/dev_hdd0/' keys work."""
    out = {}
    for line in text.splitlines():
        line = line.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.search(r":(\s|$)", line)
        if not m:
            continue
        key = _unquote(line[:m.start()])
        val = _unquote(line[m.end():]) if m.end() <= len(line) else ""
        out[key] = val
    return out


def _expand(value: str, root: str) -> str:
    return value.replace("$(EmulatorDir)", root.rstrip("/\\") + os.sep)


def _config_dir(root: Path) -> Path:
    c = root / "config"
    return c if c.is_dir() else root


def _norm(p: str) -> str:
    """Strip trailing slashes/backslashes, then normalise."""
    if not p:
        return p
    q = p.rstrip("/\\")
    return os.path.normpath(q) if q else p


# --- finding RPCS3 -------------------------------------------------------

def _is_windows() -> bool:
    return os.name == "nt"


def _wine_home():
    v = os.environ.get("WINEHOMEDIR")
    if not v:
        return None
    if v.startswith("\\??\\"):
        v = v[4:]
    return v


_PORTABLE_MAX = 40   # folders examined per base, so a huge Downloads folder cannot stall the wizard


_SHELL_FOLDERS = ("Desktop", "Personal", "{374DE290-123F-4565-9164-39C4925E467B}")   # Desktop, Documents, Downloads


def _expand_percent(v):
    """Expand %NAME% (case-insensitive, any OS; an unknown name stays and the value is then dropped)."""
    env = {k.upper(): x for k, x in os.environ.items()}
    return re.sub(r"%([^%]+)%", lambda m: env.get(m.group(1).upper(), m.group(0)), v)


def _shell_folders(winreg=None):
    """The user's real Desktop / Documents / Downloads from HKCU Explorer "User Shell Folders" (moved, localised or
    OneDrive-redirected folders). REG_EXPAND_SZ values get their environment variables expanded. [] off Windows or on
    any registry error; `winreg` is injectable for the host test."""
    if winreg is None:
        try:
            import winreg
        except ImportError:
            return []
    out = []
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders")
    except OSError:
        return []
    try:
        for name in _SHELL_FOLDERS:
            try:
                v, _t = winreg.QueryValueEx(key, name)
            except OSError:
                continue
            if isinstance(v, str) and v.strip():
                v = _expand_percent(v.strip())
                if "%" not in v:
                    out.append(Path(v))
    finally:
        try:
            winreg.CloseKey(key)
        except Exception:
            pass
    return out


def _portable_bases(winreg=None):
    """Folders in which a portable RPCS3 (rpcs3.exe beside its own config/ and dev_hdd0/, unzipped anywhere) is usually
    put: Downloads, Desktop, Documents, the home folder, the program-files and games roots, the drive roots, and
    beside the installer / current folder. Windows only; nothing outside these is scanned (pyrrhus issue #9 report,
    adapted: a portable RPCS3 was found only through an explicit hint)."""
    bases = list(_shell_folders(winreg))
    home = Path.home()
    for n in ("Downloads", "Desktop", "Documents", "Games", "Emulators"):
        bases.append(home / n)
    bases.append(home)
    for var in ("ProgramFiles", "ProgramFiles(x86)", "PUBLIC"):
        v = os.environ.get(var)
        if v:
            bases.append(Path(v))
    for d in "CDEFGH":
        for n in ("", "Games", "Emulators", "Emulator", "PS3"):
            bases.append(Path(f"{d}:\\") / n)
    for p in (os.path.dirname(os.path.abspath(sys.argv[0])) if sys.argv and sys.argv[0] else "", os.getcwd()):
        if p:
            bases.append(Path(p))
            bases.append(Path(p).parent)
    seen, out = set(), []
    for b in bases:
        k = os.path.normcase(os.path.normpath(str(b)))
        if k not in seen:
            seen.add(k)
            out.append(b)
    return out


def _portable_candidates(bases):
    """-> [(folder, "portable")]: each base itself, and its direct subfolders whose name contains 'rpcs3', where
    rpcs3.exe sits beside a config/ or dev_hdd0/ folder or a vfs.yml/games.yml (a portable layout). Never recursive."""
    out, seen = [], set()
    for b in bases:
        try:
            if not b.is_dir():
                continue
            subs = [b] + [d for d in sorted(b.iterdir())[:_PORTABLE_MAX * 20] if "rpcs3" in d.name.lower() and d.is_dir()]
        except OSError:
            continue
        for d in subs[:_PORTABLE_MAX]:
            try:
                ok = (d / "rpcs3.exe").is_file() and any((d / n).exists() for n in ("config", "dev_hdd0", "vfs.yml", "games.yml"))
            except OSError:
                ok = False
            k = os.path.normcase(str(d))
            if ok and k not in seen:
                seen.add(k)
                out.append((d, "portable"))
    return out


def _candidate_paths(hint, portable_bases=None):
    c = []
    if hint:
        h = Path(hint)
        if h.is_file():
            h = h.parent
        if h.name.lower() in ("config", "dev_hdd0", "dev_flash") and (h.parent / "rpcs3.exe").is_file():
            h = h.parent   # the player picked the portable folder's config/ or dev_hdd0/ child
        c.append((h, "hint"))
    if _is_windows():
        for var, sub, src in (("LOCALAPPDATA", ("Programs", "rpcs3"), "windows-localappdata"),
                              ("APPDATA", ("rpcs3",), "windows-appdata")):
            base = os.environ.get(var)
            if base:
                c.append((Path(base).joinpath(*sub), src))
    if _is_windows() or portable_bases is not None:
        c.extend(_portable_candidates(_portable_bases() if portable_bases is None else portable_bases))
    home = Path.home()
    if not _is_windows():
        xdg = os.environ.get("XDG_CONFIG_HOME")
        if xdg:
            c.append((Path(xdg) / "rpcs3", "xdg-config"))
        c.append((home / ".config" / "rpcs3", "linux-config"))
        c.append((home / ".var/app/net.rpcs3.RPCS3/config/rpcs3", "flatpak"))
    else:
        wh = _wine_home()  # e.g. Z:\home\user under Wine/Proton
        if wh:
            c.append((Path(wh + "\\.config\\rpcs3"), "wine-z-linux-config"))
            c.append((Path(wh + "\\.var\\app\\net.rpcs3.RPCS3\\config\\rpcs3"), "wine-z-flatpak"))
    return c


def is_rpcs3(root) -> bool:
    """A folder is RPCS3 when rpcs3.exe is beside it or vfs.yml/games.yml is in it or in config/."""
    r = Path(root)
    try:
        if (r / "rpcs3.exe").is_file():
            return True
        return any((d / n).is_file() for d in (r / "config", r) for n in ("vfs.yml", "games.yml"))
    except OSError:
        return False


def _make_candidate(root: Path, source: str) -> dict:
    cfg = _config_dir(root)
    _register(root, root / "dev_hdd0")
    vfs = parse_kv(_read_text(cfg / "vfs.yml") or "")
    r = str(root)

    def pick(key, default):
        v = vfs.get(key, "")
        return _norm(_expand(v, r)) if v else str(root / default)

    hdd0 = pick("/dev_hdd0/", "dev_hdd0")
    _register(root, hdd0)
    return {"root": r, "config_dir": str(cfg),
            "dev_hdd0": hdd0,
            "dev_flash": pick("/dev_flash/", "dev_flash"),
            "source": source}


def find_rpcs3(hint=None, portable_bases=None) -> list[dict]:
    seen, out = set(), []
    for path, source in _candidate_paths(hint, portable_bases):
        try:
            ok = path.is_dir()
        except OSError:
            ok = False
        key = os.path.normcase(str(path))
        if not ok or key in seen:
            continue
        seen.add(key)
        if source == "hint" and not is_rpcs3(path):
            out.append(_err("not_rpcs3", f"{path} is not an RPCS3 folder "
                            "(no rpcs3.exe, vfs.yml or games.yml)", hint=str(path), source="hint"))
            continue
        out.append(_make_candidate(path, source))
    if hint and not any(c.get("source") == "hint" for c in out):
        out.insert(0, _err("not_rpcs3", f"{hint} is not an existing RPCS3 folder",
                           hint=str(hint), source="hint"))
    return out


# --- PARAM.SFO -----------------------------------------------------------

def param_sfo(path) -> dict:
    p = Path(path)
    if p.is_dir():
        p = p / "PARAM.SFO"
    try:
        with _open_ro(p, "rb") as f:
            data = f.read(_MAX_SFO + 1)
    except FileNotFoundError:
        return _err("not_found", f"no PARAM.SFO at {p}")
    except OSError as e:
        return _err("unreadable", f"cannot read {p}: {e}")
    if len(data) > _MAX_SFO or len(data) < 20 or data[:4] != b"\0PSF":
        return _err("bad_sfo", f"{p} is not a PARAM.SFO")
    _ver, ktab, dtab, count = struct.unpack_from("<IIII", data, 4)
    keys = {}
    try:
        for i in range(count):
            koff, fmt, ln, _mx, doff = struct.unpack_from("<HHIII", data, 20 + 16 * i)
            kstart = ktab + koff
            kend = data.index(b"\0", kstart)
            name = data[ktab + koff:kend].decode("ascii", "replace")
            raw = data[dtab + doff:dtab + doff + ln]
            if fmt in (0x0204, 0x0004):
                keys[name] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
            elif fmt == 0x0404 and len(raw) >= 4:
                keys[name] = struct.unpack("<i", raw[:4])[0]
    except (struct.error, ValueError):
        return _err("bad_sfo", f"{p} is truncated or malformed")
    return {"ok": True, "path": str(p), "keys": keys}


# --- game lookups ---------------------------------------------------------

def _cand_for(root):
    """Accept a root path or a candidate dict."""
    c = root if isinstance(root, dict) else _make_candidate(Path(root), "given")
    # Register any root a caller passes in, so the cache/exdata guard covers it without find_rpcs3().
    _register(c["root"], c["dev_hdd0"])
    return c


def game_path(root, serial) -> dict:
    c = _cand_for(root)
    text = _read_text(Path(c["config_dir"]) / "games.yml")
    if text is None:
        return _err("not_found", "config/games.yml not found", serial=serial)
    games = parse_kv(text)
    if serial not in games:
        return _err("not_found", f"{serial} is not in games.yml", serial=serial)
    d = _norm(_expand(games[serial], c["root"]))
    dp = Path(d)
    if not dp.is_dir():
        return _err("missing_disc", f"games.yml points at {d}, which is not a folder",
                    serial=serial, disc_dir=d)
    ps3 = (dp / "PS3_GAME").is_dir()
    psn = (not ps3) and (dp / "USRDIR").is_dir()
    res = {"ok": True, "serial": serial, "disc_dir": d, "has_ps3_game": ps3,
           "has_psn_game": psn, "title_id": None, "version": None, "app_ver": None}
    sfo = param_sfo(dp / "PS3_GAME" / "PARAM.SFO" if ps3 else dp / "PARAM.SFO")
    if sfo["ok"]:
        k = sfo["keys"]
        res.update(title_id=k.get("TITLE_ID"), version=k.get("VERSION"), app_ver=k.get("APP_VER"))
    return res


def _game_root(c):
    return Path(c["dev_hdd0"]) / "game"


def _is_update_sfo(keys) -> bool:
    return keys.get("CATEGORY") == "GD" or "APP_VER" in keys


def install_dirs(root, serial) -> list[dict]:
    c = _cand_for(root)
    g = _game_root(c)
    out = []
    try:
        names = sorted(n for n in os.listdir(g) if n.startswith(serial))
    except OSError:
        return out
    for n in names:
        p = g / n
        if not p.is_dir():
            continue
        e = {"name": n, "path": str(p), "app_ver": None, "title_id": None, "kind": "game_data"}
        s = param_sfo(p)
        if s["ok"]:
            e["app_ver"] = s["keys"].get("APP_VER")
            e["title_id"] = s["keys"].get("TITLE_ID")
            if _is_update_sfo(s["keys"]):
                e["kind"] = "update"
        elif s["code"] != "not_found":
            e["sfo_error"] = {"code": s["code"], "text": s["text"]}
        out.append(e)
    return out


def _ver_key(v):
    try:
        return tuple(int(x) for x in str(v).strip().split("."))
    except ValueError:
        return None


def _same_ver(a, b):
    ka, kb = _ver_key(a), _ver_key(b)
    return ka == kb if ka is not None and kb is not None else str(a).strip() == str(b).strip()


def update_dir(root, serial, want=None) -> dict:
    c = _cand_for(root)
    p = _game_root(c) / serial
    if not p.is_dir():
        return _err("missing_update", f"no update folder {p}", serial=serial, path=str(p))
    s = param_sfo(p)
    if not s["ok"] and s["code"] == "not_found" or s["ok"] and not _is_update_sfo(s["keys"]):
        return _err("game_data_only", f"{p} is game data, not an update (no update PARAM.SFO)",
                    serial=serial, path=str(p))
    if not s["ok"]:
        return _err("bad_update", f"update folder {p} has no readable PARAM.SFO: {s['text']}",
                    serial=serial, path=str(p))
    have = s["keys"].get("APP_VER")
    res = {"ok": True, "serial": serial, "path": str(p), "app_ver": have}
    if want is not None and (have is None or not _same_ver(have, want)):
        return _err("wrong_version", f"update is {have}, wanted {want}",
                    serial=serial, path=str(p), app_ver=have, want=want)
    return res


def fonts_dir(root) -> dict:
    c = _cand_for(root)
    p = Path(c["dev_flash"]) / "data" / "font"
    if p.is_dir():
        return {"ok": True, "path": str(p)}
    return {"ok": False, "code": "missing_fonts", "text": "no dev_flash/data/font folder", "path": str(p)}


def saves(root, serial_prefix) -> dict:
    c = _cand_for(root)
    home = Path(c["dev_hdd0"]) / "home"
    users = {}
    try:
        uids = sorted(os.listdir(home))
    except OSError:
        return {"ok": True, "users": users}
    for uid in uids:
        sd = home / uid / "savedata"
        try:
            names = sorted(n for n in os.listdir(sd) if n.startswith(serial_prefix))
        except OSError:
            continue
        for n in names:
            p = sd / n
            if not p.is_dir():
                continue
            e = {"user": uid, "name": n, "path": str(p),
                 "mtime": p.stat().st_mtime, "title_id": None}
            s = param_sfo(p)
            if s["ok"]:
                e["title_id"] = s["keys"].get("TITLE_ID")
            users.setdefault(uid, []).append(e)
    return {"ok": True, "users": users}


def eboot_kind(path) -> dict:
    """Reads only the first 4 bytes: "elf" (decrypted), "self" (encrypted SELF, SCE magic), else "unknown"."""
    try:
        with _open_ro(path, "rb") as f:
            magic = f.read(4)
    except (OSError, PermissionError) as e:
        return _err("missing", str(e), path=str(path))
    kind = "elf" if magic == b"\x7fELF" else "self" if magic == b"SCE\x00" else "unknown"
    return {"ok": True, "path": str(path), "kind": kind}


def decrypted_eboot(game_dirs) -> dict:
    """game_dirs in priority order: update dir, disc dir, then extras (dirs or files)."""
    found, seen = [], set()
    for d in game_dirs or []:
        if not d:
            continue
        dp = Path(d)
        cands = ([dp] if dp.is_file() else
                 [dp / "USRDIR" / "EBOOT.elf", dp / "PS3_GAME" / "USRDIR" / "EBOOT.elf"])
        for f in cands:
            if str(f) in seen:
                continue
            seen.add(str(f))
            if f.is_file():
                k = eboot_kind(f)
                if k.get("kind") != "elf":   # a SELF saved as .elf is not decrypted
                    continue
                found.append({"path": str(f), "size_hint": f.stat().st_size})
    if not found:
        return _err("no_decrypted_eboot",
                    "no decrypted EBOOT.elf found next to the game's EBOOT.BIN",
                    found=[])
    return {"ok": True, "path": found[0]["path"], "size_hint": found[0]["size_hint"],
            "found": found}


def sha256(path) -> dict:
    h = hashlib.sha256()
    n = 0
    try:
        with _open_ro(path, "rb") as f:
            while chunk := f.read(1 << 20):
                h.update(chunk)
                n += len(chunk)
    except PermissionError as e:
        return _err("forbidden", str(e))
    except OSError as e:
        return _err("unreadable", f"cannot read {path}: {e}")
    return {"ok": True, "path": str(path), "sha256": h.hexdigest(), "size": n}


# --- redaction -------------------------------------------------------------

def _fs(s):
    return str(s).replace("\\", "/")


def redact(path, roots=None) -> str:
    """Forward-slash string with the RPCS3 root as <RPCS3>, home as <home>, other
    users' home folders as <user>."""
    s = _fs(path)
    reps = []
    for r in (roots or []):
        r = r["root"] if isinstance(r, dict) else r
        reps.append((_fs(r).rstrip("/"), "<RPCS3>"))
    home = _fs(Path.home()).rstrip("/")
    reps.append((home, "<home>"))
    reps.append(("Z:" + home, "<home>"))
    wh = _wine_home()
    if wh:
        reps.append((_fs(wh).rstrip("/"), "<home>"))
    reps.sort(key=lambda t: -len(t[0]))
    for pre, tag in reps:
        if pre and (s.lower() == pre.lower() or s.lower().startswith(pre.lower() + "/")):
            return tag + s[len(pre):]
    return re.sub(r"^(?:[A-Za-z]:)?/(?:home|Users)/[^/]+", "<user>", s, flags=re.I)


# --- copying saves ----------------------------------------------------------

def copy_save(src, dst, overwrite=False, dry_run=True) -> dict:
    s, d = Path(src), Path(dst)
    if not s.is_dir():
        return _err("not_found", f"save folder {s} not found")
    if _forbidden(s):
        return _err("forbidden", f"refusing {s}")
    find_rpcs3()  # registers the default RPCS3 roots
    dn = _nc(d)
    for root, hdd0 in _ROOTS:
        if _under(dn, root) is not None or _under(dn, hdd0) is not None:
            return _err("dst_in_rpcs3", f"{d} is inside an RPCS3 folder; refusing", dst=str(d))
    if d.exists() and not overwrite:
        return _err("exists", f"{d} already exists", dst=str(d))
    try:
        d.resolve().relative_to(s.resolve())
        return _err("bad_dst", "destination is inside the source")
    except ValueError:
        pass
    files, skipped = [], []
    for dirpath, dirnames, filenames in os.walk(s):
        dirnames[:] = [x for x in dirnames if not _forbidden(Path(dirpath) / x)]
        for fn in sorted(filenames):
            fp = Path(dirpath) / fn
            rel = fp.relative_to(s).as_posix()
            if _forbidden(fp) or fp.is_symlink():
                skipped.append(rel)
                continue
            files.append({"rel": rel, "size": fp.stat().st_size})
    # overwrite merges: nothing in dst is deleted. replaces = same path in dst,
    # stale = only in dst and left behind.
    replaces, stale = [], []
    if d.is_dir():
        new = {f["rel"] for f in files}
        for dirpath, _dn, filenames in os.walk(d):
            for fn in sorted(filenames):
                fp = Path(dirpath) / fn
                if _forbidden(fp):
                    continue
                rel = fp.relative_to(d).as_posix()
                (replaces if rel in new else stale).append(rel)
    res = {"ok": True, "dry_run": bool(dry_run), "src": str(s), "dst": str(d),
           "files": files, "skipped": skipped, "replaces": sorted(replaces),
           "stale": sorted(stale), "copied": 0}
    if dry_run:
        return res
    try:
        for f in files:
            t = d / f["rel"]
            t.parent.mkdir(parents=True, exist_ok=True)
            with _open_ro(s / f["rel"], "rb") as a, open(t, "wb") as b:
                shutil.copyfileobj(a, b, 1 << 20)
            res["copied"] += 1
        d.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        return _err("copy_failed", f"copy failed: {e}", copied=res["copied"])
    return res


# --- one call ----------------------------------------------------------------

def _problem(code, text):
    return {"code": code, "text": text}


def _parse_want(want):
    """-> (mode, version): mode "disc" (no update expected), "update" or "any" (no version)."""
    if want is None or str(want).strip() == "":
        return "any", None
    w = str(want).strip()
    if w.lower().startswith("disc:"):
        return "disc", w[5:].strip() or None
    return "update", w


def locate(rpcs3_dir=None, wants=None, extra_eboots=None) -> dict:
    """wants: {id: want}; want is "VER" (update required), "disc:VER" (disc only,
    no update expected) or None (report an update only if one exists)."""
    out = {"ok": False, "root": None, "config_dir": None, "dev_hdd0": None,
           "dev_flash": None, "fonts": None, "source": None,
           "candidates": [], "titles": {}, "problems": []}
    if not wants:
        out["code"] = "no_titles"
        out["problems"].append(_problem("no_titles", "no titles were asked for"))
        return out
    cands = find_rpcs3(rpcs3_dir)
    out["candidates"] = [c["root"] for c in cands if c.get("root")]
    if cands and cands[0].get("code") == "not_rpcs3":
        out["code"] = "not_rpcs3"
        out["problems"].append(_problem("not_rpcs3", cands[0]["text"]))
        return out
    if not cands:
        out["code"] = "not_found"
        out["problems"].append(_problem("not_found", "no RPCS3 folder found"))
        return out
    c = cands[0]
    fd = fonts_dir(c)
    out.update(ok=True, root=c["root"], config_dir=c["config_dir"], dev_hdd0=c["dev_hdd0"],
               dev_flash=c["dev_flash"], source=c["source"], fonts=fd.get("path") if fd["ok"] else None)
    for tid, want in wants.items():
        mode, ver = _parse_want(want)
        probs = []
        disc = game_path(c, tid)
        if not disc["ok"]:
            probs.append(_problem("missing_disc", disc["text"]))
        # Discs differ in which PARAM.SFO key holds "the disc version" (R&C ToD: VERSION 02.00 / APP_VER 01.00;
        # KZ3: APP_VER 01.00 / VERSION 01.01). "disc:KEY=VER" names the key; a plain "disc:VER" matches either.
        dkey, dver, dmatch = None, None, True
        if disc["ok"] and ver:
            k, _, v = ver.partition("=") if "=" in ver else ("", "", ver)
            keys = {"VERSION": disc.get("version"), "APP_VER": disc.get("app_ver")}
            if k.upper() in keys:
                ver, dkey = v.strip(), k.upper()
                dver = keys[dkey]
                dmatch = dver is not None and _same_ver(dver, ver)
            else:
                hit = [kk for kk, vv in keys.items() if vv is not None and _same_ver(vv, ver)]
                dkey = hit[0] if hit else None
                dmatch = bool(hit) or all(vv is None for vv in keys.values())
                dver = "/".join(f"{kk} {vv}" for kk, vv in keys.items() if vv is not None) or None
        upd = None
        if mode != "disc":
            upd = update_dir(c, tid, ver)
            if not upd["ok"]:
                if upd["code"] == "wrong_version":
                    probs.append(_problem("wrong_version", upd["text"]))
                elif mode == "update":
                    probs.append(_problem("missing_update", upd["text"]))
        if ver and disc["ok"] and dver is not None and not dmatch and (
                mode == "disc" or (upd is not None and not upd["ok"]
                                   and upd["code"] != "wrong_version")):
            probs.append(_problem("wrong_version", f"the disc ({disc['disc_dir']}) is {dver}, wanted {ver}"))
        dirs = []
        if upd and upd.get("ok") or (upd and upd.get("code") == "wrong_version"):
            dirs.append(upd["path"])  # game data and bad folders are never EBOOT sources
        if disc["ok"]:
            dirs.append(disc["disc_dir"])
        dirs += list(extra_eboots or [])
        eb = decrypted_eboot(dirs)
        if not eb["ok"]:
            probs.append(_problem("no_decrypted_eboot", eb["text"]))
        out["titles"][tid] = {"disc": disc, "disc_ver_key": dkey, "update": upd, "installs": install_dirs(c, tid),
                              "saves": saves(c, tid), "decrypted_eboot": eb, "problems": probs}
    return out


def _redact_obj(o, roots):
    if isinstance(o, dict):
        return {k: _redact_obj(v, roots) for k, v in o.items()}
    if isinstance(o, list):
        return [_redact_obj(v, roots) for v in o]
    if isinstance(o, str) and (os.path.isabs(o) or re.match(r"^[A-Za-z]:[\\/]", o)):
        return redact(o, roots)
    return o


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Locate a player's RPCS3 setup (read-only).")
    ap.add_argument("--json", action="store_true", help="print the result as JSON (default)")
    ap.add_argument("--rpcs3", help="RPCS3 folder (portable install or config dir)")
    ap.add_argument("--title", action="append", required=True, metavar="ID[=VER|=disc:VER]")
    ap.add_argument("--eboot", action="append", default=[], help="extra EBOOT.elf path or game folder")
    ap.add_argument("--redacted", action="store_true", help="redact paths (for logs)")
    a = ap.parse_args(argv)
    wants = {}
    for t in a.title:
        k, _, v = t.partition("=")
        wants[k] = v or None
    res = locate(a.rpcs3, wants, a.eboot)
    if a.redacted:
        res = _redact_obj(res, find_rpcs3(a.rpcs3))
    json.dump(res, sys.stdout, indent=2)
    print()
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
