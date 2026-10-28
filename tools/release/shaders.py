#!/usr/bin/env python3
"""Prepare shaders: one unattended warm-up session of the built port (stage 1,
pass 5).

    shaders.py --root DIR [--force] [--window minimized|hidden|normal] [--print]
    shaders.py --route-script profile|first-boot      (the PAD_SCRIPT alone, for a Proton check of the route)

Why: a cold first session translates and compiles every shader on the renderer's walker thread (rsx r32a: 1,256
misses, 25.4 s, 739 hitches over 16 ms in the first level); a warm one (patch 2031: shader disk cache, pso_keys.bin,
pso_library.bin) has 6. Nothing of it can ship (it is derived from the game's shader microcode and the library is
GPU/driver-specific), so the player's machine makes it once, right after the build.

What it does:
  1. Skips when the cache is warm for this kz3.exe and port version (warmup.json in the shader folder, written only
     after a good session; --force runs anyway: the launcher's "Prepare shaders again").
  2. Starts build/kz3.exe with the launcher's own game environment (launch.game_env; launch.ensure_vfs makes the
     writable VFS root <install>/vfs first, pass 17) plus
       RSX_SHADER_CACHE_DIR = <data>/cache/shaders   (the same folder every normal start uses; <data> = the install
                                                      folder when portable, else %APPDATA%/Project Helghast; never the
                                                      dump: the runtime's default is <cwd>/scratch/rsx_shader_cache_v2)
       PAD_SCRIPT = a fixed route (route_script)      fast start through the menu into mission_0, a walk in the level
       PAD_NO_KEYBOARD=1                              typing in other windows cannot reach the game
       KZ3_ERROR_NOTICE_TIMEOUT=15                    a fatal-error notice closes itself (unattended)
     in its own working folder <data>/cache/shader-warmup, so cellSaveData's ./gamedata (the warm-up's profile) never
     touches the player's saves. Window minimized without focus (STARTUPINFO + a ShowWindow after it appears).
  3. Route: "first-boot" when the warm-up folder has no profile yet (the game's first boot cannot skip the opening
     cinematic: status log 2026-10-08), "profile" afterwards (tools/pad/menu-fast.pad's presses: level load ~38 s);
     before a "profile" run the warm-up profile's continue record is cleared (profile_edit.edit) so the main menu
     looks the same as on its first pass. ASSUMPTION: both routes' timings until the maintainer's warm-up run.
  4. Ends: after the route it waits until pso_library.bin is newer than the route's end (the runtime saves it every
     60 s on present when it grew) or SAVE_WAIT seconds, then posts WM_CLOSE. Since RSX pass 35 the runtime exits
     cleanly on WM_CLOSE (also X and Alt+F4): rsx_live_draw_shutdown saves the library, the log is closed, exit 0
     in ~45 ms (rsx runs r35-close1/2; the runtime's own KZ3_CLOSE_WATCHDOG exits 3 on a hang). CLOSE_GRACE stays
     as the safety net for an older build or a hung shutdown: after it the process is terminated. The shader files
     and pso_keys.bin are written per build (each record fclose'd), so even a terminate loses at most the library
     entries made after its last save; the runtime rebuilds those in the background next start. ASSUMPTION: with
     the clean exit SAVE_WAIT could shrink (the close itself saves the library); kept until a warm-up run shows it.
Progress: one "[elapsed/total] Preparing shaders" line every 2 s (firstrun's step parses it like ninja's [x/y]).
Exit: 0 = warm (done or skipped), 2 = the session failed (game ended early, no keys recorded; no marker written),
1 = cannot start (not built). The game's own output goes to <data>/cache/shader-warmup/warmup-game.log.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

MARKER = "warmup.json"
KEYS = "pso_keys.bin"
LIBRARY = "pso_library.bin"
KEYS_HEADER = 24                 # PSPL header (patch 2031): a file this size or smaller holds no record
SAVE_WAIT = 75                   # > the runtime's 60 s library save period
CLOSE_GRACE = 10
TICK = 2
PROFILE = "PROFILE.DAT"
CROSS, START = 0x4000, 0x0008

# Route timings (pad clock = seconds since the first pad poll). ASSUMPTION until the maintainer's warm-up run.
# profile: tools/pad/menu-fast.pad (fast-1/fast-2: level load 38.0 s, level start 121/124 s)
MENU_FAST = [(3, CROSS), (6, CROSS), (30, CROSS), (31, CROSS), (33, CROSS), (34, CROSS), (36, CROSS), (37, CROSS),
             (39, CROSS), (40, CROSS), (42, CROSS), (43, CROSS)]
PROFILE_LEVEL_START = 125
# first-boot: language/settings/gamma pages take CROSS (START at 25 s as tools/kz3-check.sh's pad), the opening
# cinematic plays in full (~10 min on 2026-10-08, at a lower frame rate than today), then title, main menu,
# difficulty and the level load as on the profile route. CROSS every 2 s until FIRST_MENU_END covers all of it.
FIRST_MENU_END = 720
FIRST_LEVEL_START = 810
IN_LEVEL = 180                   # seconds of play in mission_0 per route


def in_level(t0, secs):
    """A walk at the scanner gate (tools/pad/t2-walk.pad's moves, repeated): forward, look right, back, look left,
    a CROSS every 20 s as pad-r12; -> [(t, body)]."""
    ev, t = [], t0
    while t + 20 <= t0 + secs:
        ev += [(t, "0,-1,0,0/6"), (t + 7, "0,0,1,0/3"), (t + 11, "0,1,0,0/5"), (t + 17, "0,0,-1,0/3")]
        t += 20
    ev += [(t0 + k, f"{CROSS:#06x}") for k in range(10, secs, 20)]
    return ev


def route_script(route):
    """-> (PAD_SCRIPT text, planned seconds) for "profile" or "first-boot"."""
    if route == "profile":
        ev = [(t, f"{m:#06x}") for t, m in MENU_FAST]
        start = PROFILE_LEVEL_START
    elif route == "first-boot":
        ev = [(t, f"{CROSS:#06x}") for t in range(2, FIRST_MENU_END, 2)] + [(25, f"{START:#06x}")]
        start = FIRST_LEVEL_START
    else:
        raise ValueError(route)
    ev += in_level(start, IN_LEVEL)
    ev.sort(key=lambda e: e[0])
    return "\n".join(f"{t}:{b}" for t, b in ev) + "\n", start + IN_LEVEL


# ------------------------------------------------------------------------------------------------------ where
def paths_for(root, environ=None, appdata=None, localappdata=None):
    """-> dict of the folders this step uses, from kz3-install.ini (portable) and the launcher's rules."""
    environ = dict(os.environ if environ is None else environ)
    portable = la.read_ini(root)["portable"]
    data = la.data_dir(root, portable, environ, appdata)
    cache = la.cache_dir(root, portable, environ, localappdata, appdata)   # caches: local, not roaming (pass 32)
    shaders = la.shader_dir(cache)
    return {"portable": portable, "data": data, "cache": cache, "shaders": shaders,
            "warmup": os.path.join(cache, "cache", "shader-warmup"),
            "exe": os.path.join(root, "build", "kz3.exe")}


def exe_id(exe):
    """{size, sha256} of kz3.exe, or None."""
    try:
        h = hashlib.sha256()
        with open(exe, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return {"size": os.path.getsize(exe), "sha256": h.hexdigest()}
    except OSError:
        return None


def keys_recorded(shaders):
    try:
        return os.path.getsize(os.path.join(shaders, KEYS)) > KEYS_HEADER
    except OSError:
        return False


def warm_state(shaders, exe_ident, version):
    """-> (warm, reason): warm = a good session's marker for this exe and version, and the key file still there."""
    m = la.load_json(os.path.join(shaders, MARKER))
    if not m:
        return False, "no warm-up recorded"
    if m.get("exe") != exe_ident:
        return False, "kz3.exe changed since the last warm-up"
    if m.get("version") != version:
        return False, "the port version changed since the last warm-up"
    if not keys_recorded(shaders):
        return False, "the shader key file is missing"
    return True, f"warm since {m.get('finished', '?')}"


def find_profile(warmup):
    hits = sorted(glob.glob(os.path.join(warmup, "gamedata", "dev_hdd0", "home", "*", "savedata", "*", PROFILE)))
    return hits[0] if len(hits) == 1 else ("" if not hits else hits[0])


def prepare_profile(warmup, log=print):
    """-> route. A usable warm-up profile: continue record cleared, route "profile". A broken one: its save folder
    removed (only inside the warm-up folder), route "first-boot"."""
    p = find_profile(warmup)
    if not p:
        return "first-boot"
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "assets"))
        import profile_edit as pe
        with open(p, "rb") as f:
            data = bytearray(f.read())
        if len(data) != pe.SIZE:
            raise ValueError(f"size {len(data)}")
        if pe.continue_info(data)["rec_size"] or pe.continue_info(data)["level_id"] != -1:
            pe.edit(data, clear_continue=True)
            with open(p, "wb") as f:
                f.write(data)
            log(f"warm-up profile: continue record cleared ({p})")
        return "profile"
    except (ImportError, OSError, ValueError, KeyError) as e:
        save = os.path.dirname(p)
        w = os.path.realpath(warmup)
        if os.path.realpath(save).startswith(w + os.sep):
            shutil.rmtree(save, ignore_errors=True)
        log(f"warm-up profile unusable ({e!r}): removed, first-boot route")
        return "first-boot"


def warm_env(root, environ=None, appdata=None, localappdata=None):
    """-> (argv, cwd, env, added, where) of the warm-up session; None when the port is not built."""
    environ = dict(os.environ if environ is None else environ)
    paths = la.load_json(os.path.join(root, "build", "paths.json")) or {}
    if not paths.get("dump") or not paths.get("eboot"):
        return None
    w = paths_for(root, environ, appdata, localappdata)
    argv, _, env, added = la.game_env(root, paths, w["portable"], environ, appdata, localappdata)
    extra = {"PAD_NO_KEYBOARD": "1", "KZ3_ERROR_NOTICE_TIMEOUT": "15"}
    env.update(extra)
    added.update(extra)
    return argv, w["warmup"], env, added, w


# ------------------------------------------------------------------------------------------------------- the run
def start_minimized(argv, cwd, env, out, window):
    """Popen with the window minimized and not activated (STARTUPINFO); posix: plain (host tests use fakes)."""
    kw = {}
    if os.name == "nt" and window != "normal":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0 if window == "hidden" else 7      # SW_HIDE / SW_SHOWMINNOACTIVE
        kw["startupinfo"] = si
    return subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT, **kw)


def _windows_of(pid):
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    u.EnumWindows(proto(cb), 0)
    return u, found


def minimize_windows(pid, window):
    """ASSUMPTION: the D3D12 backend's WS_VISIBLE CreateWindowEx may not honour STARTUPINFO, so minimize (or hide)
    its windows again once they exist. Windows only; -> number of windows touched."""
    if os.name != "nt" or window == "normal":
        return 0
    try:
        u, found = _windows_of(pid)
        for h in found:
            u.ShowWindow(h, 0 if window == "hidden" else 7)
        return len(found)
    except Exception:   # noqa: BLE001 (cosmetic: the session works with the window shown)
        return 0


def close_politely(proc):
    """WM_CLOSE to the game's windows (Windows), SIGTERM elsewhere."""
    if os.name == "nt":
        try:
            u, found = _windows_of(proc.pid)
            for h in found:
                u.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
        except Exception:   # noqa: BLE001
            pass
    else:
        proc.terminate()


def mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


class Session:
    """The timed session; clock/sleep/start injectable for the host test."""

    def __init__(self, out=sys.stdout, clock=time.monotonic, wall=time.time, sleep=time.sleep, start=start_minimized,
                 window="minimized"):
        self.out, self.clock, self.wall, self.sleep, self.start, self.window = out, clock, wall, sleep, start, window

    def say(self, line):
        print(line, file=self.out, flush=True)

    def bar(self, el, total):
        self.say(f"[{int(min(el, total))}/{int(total)}] Preparing shaders")

    def run(self, argv, cwd, env, shaders, planned, log_path):
        """-> (ok, reason, facts)."""
        total = planned + SAVE_WAIT
        lib = os.path.join(shaders, LIBRARY)
        with open(log_path, "w", encoding="utf-8", errors="replace") as out:
            try:
                proc = self.start(argv, cwd, env, out, self.window)
            except OSError as e:
                return False, f"cannot start kz3.exe: {e}", {}
            t0 = self.clock()
            shown, end_wall, saved, why = 0, None, False, ""
            try:
                while True:
                    el = self.clock() - t0
                    if proc.poll() is not None:
                        why = f"the game ended early (exit {proc.returncode}) after {int(el)} s"
                        break
                    if shown < 3 and int(el) % 10 == 0:
                        shown += minimize_windows(proc.pid, self.window) and 1
                    if el >= planned and end_wall is None:
                        end_wall = self.wall()
                        self.say(f"route done after {int(el)} s; waiting for the pipeline library save")
                    if end_wall is not None:
                        if mtime(lib) >= end_wall:
                            saved = True
                            break
                        if el >= total:
                            break
                    self.bar(el, total)
                    self.sleep(TICK)
            finally:
                if proc.poll() is None:
                    close_politely(proc)
                    try:
                        proc.wait(CLOSE_GRACE)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
        el = self.clock() - t0
        self.bar(total, total)
        facts = {"seconds": round(el, 1), "library_saved_after_route": saved,
                 "keys_bytes": os.path.getsize(os.path.join(shaders, KEYS)) if keys_recorded(shaders) else 0}
        if why:
            return False, why, facts
        if not keys_recorded(shaders):
            return False, "no shader keys were recorded (did the game render? see warmup-game.log)", facts
        return True, "library saved" if saved else f"no library save within {SAVE_WAIT} s (keys and shaders kept)", facts


def main(argv=None, out=sys.stdout, session=None, environ=None, appdata=None, localappdata=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--force", action="store_true", help="run even when the cache is warm (Prepare shaders again)")
    ap.add_argument("--window", choices=("minimized", "hidden", "normal"), default="minimized")
    ap.add_argument("--print", action="store_true", help="show the decision, route and environment as JSON")
    ap.add_argument("--route-script", choices=("profile", "first-boot"),
                    help="print that route's PAD_SCRIPT and exit (for a Proton check of the route)")
    a = ap.parse_args(argv)
    if a.route_script:
        text, planned = route_script(a.route_script)
        print(f"# {a.route_script} route: {planned} s planned, + up to {SAVE_WAIT} s for the library save", file=out)
        print(text, end="", file=out)
        return 0
    root = os.path.abspath(a.root)
    we = warm_env(root, environ, appdata, localappdata)
    if we is None:
        print("The port is not built yet: build it first.", file=out)
        return 1
    gargv, cwd, env, added, w = we
    version = fr.port_version(root)
    ident = exe_id(w["exe"])
    if ident is None:
        print(f"{w['exe']} not found: build the port first.", file=out)
        return 1
    warm, reason = warm_state(w["shaders"], ident, version)
    route = "profile" if find_profile(w["warmup"]) else "first-boot"
    script, planned = route_script(route)
    if a.print:
        print(json.dumps({"warm": warm, "reason": reason, "run": a.force or not warm, "route": route,
                          "planned_seconds": planned, "total_seconds": planned + SAVE_WAIT, "cwd": cwd,
                          "argv": gargv, "env": added, "shaders": w["shaders"]}, indent=1), file=out)
        return 0
    if warm and not a.force:
        print(f"Shaders already prepared for this build ({reason}); skipped. --force runs it again.", file=out)
        return 0
    print(f"Preparing shaders ({reason}): {route} route, about {(planned + SAVE_WAIT + 59) // 60} minutes; the game "
          "runs minimised and plays the first level by itself.", file=out, flush=True)
    for d in (cwd, w["shaders"], added["PS3_EDAT_CACHE"]):
        os.makedirs(d, exist_ok=True)
    _, err = la.ensure_vfs(root, la.load_json(os.path.join(root, "build", "paths.json"))["dump"], la.link_free(root))
    if err:
        print(err, file=out)
        return 1
    route = prepare_profile(cwd, log=lambda t: print(t, file=out))
    script, planned = route_script(route)
    env["PAD_SCRIPT"] = script
    try:
        os.remove(os.path.join(w["shaders"], MARKER))
    except OSError:
        pass
    s = session or Session(out=out, window=a.window)
    ok, why, facts = s.run(gargv, cwd, env, w["shaders"], planned, os.path.join(cwd, "warmup-game.log"))
    print(f"warm-up {'done' if ok else 'failed'}: {why}; {json.dumps(facts)}", file=out, flush=True)
    if not ok:
        return 2
    with open(os.path.join(w["shaders"], MARKER), "w", encoding="utf-8") as f:
        json.dump({"exe": ident, "version": version, "route": route, "result": why, **facts,
                   "finished": datetime.datetime.now().isoformat(timespec="seconds")}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
