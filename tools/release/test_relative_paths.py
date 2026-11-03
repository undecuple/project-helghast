#!/usr/bin/env python3
"""Host test (release pass 86, tester report item 5): kz3.ini and paths.json hold install-relative paths. A path below the
install folder is written relative to it (to build\\ for an exe that does not know the `root` key), others stay absolute;
a path the player edited by hand stays as written (and is used) across starts; a moved install keeps working with no
rebuild when only the stamp's root differs. No game data."""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr   # noqa: E402
import launch as la     # noqa: E402
import test_firstrun_check as tfc   # noqa: E402  (fixture: a complete update folder)

FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def edit(root, key, value):
    """Edit kz3.ini by hand: replace the key's value."""
    p = os.path.join(root, "build", "kz3.ini")
    lines = [(f"{key} = {value}" if ln.split("=")[0].strip() == key else ln) for ln in open(p).read().splitlines()]
    write(p, "\n".join(lines) + "\n")


def start(root, paths):
    """What the launcher does at each start: the game environment, then kz3.ini."""
    env = {"APPDATA": os.path.join(root, "ad"), "LOCALAPPDATA": os.path.join(root, "lad")}
    la_env = la.game_env(root, paths, True, env)[3]
    la.write_game_ini(root, la.game_ini(root, paths, True, env))
    return la_env, la.read_game_ini(os.path.join(root, "build", "kz3.ini"))


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-rel-")
    try:
        # ---- 1 relative write / read round trip
        R = os.path.join(tmp, "A", "Install")
        dump = os.path.join(R, "game", "disc")          # inside the install
        out_dump = os.path.join(tmp, "elsewhere", "dump")   # outside
        for d in (dump, out_dump):
            os.makedirs(os.path.join(d, "PS3_GAME"))
        upd = tfc.make_update(os.path.join(tmp, "u"))
        eb = os.path.join(R, "game", "EBOOT.elf")
        write(eb, "x")
        write(os.path.join(R, "portable.txt"), "")
        os.makedirs(os.path.join(R, "build"))
        paths = {"dump": dump, "update": upd, "eboot": eb}
        e, ini = start(R, paths)
        check(ini.get("vfs_root") == "../vfs" and ini.get("eboot") == "../game/EBOOT.elf" and "root" not in ini,
              f"1 old exe: in-install paths relative to build\\: {ini.get('vfs_root')} {ini.get('eboot')}")
        check(ini.get("update_dir", "").replace("\\", "/").endswith("/USRDIR") and os.path.isabs(ini["update_dir"]),
              "1 a path outside the install stays absolute")
        write(os.path.join(R, "port", "kz3", "kz3_ini.h"), '{"root", NULL, 1}')
        e, ini = start(R, paths)
        check(ini.get("root") == ".." and ini.get("vfs_root") == "vfs" and ini.get("eboot") == "game/EBOOT.elf"
              ,
              f"1b exe with root key: root=.. and values relative to the install {ini}")
        check(e["PS3_VFS_ROOT"] == os.path.join(R, "vfs") and e["PS3_EDAT_CACHE"] == os.path.join(R, "build", "cache", "edat"),
              "1b the game environment stays absolute")
        # the game's rule: base = exe folder + root, then value
        base = os.path.normpath(os.path.join(R, "build", ini["root"]))
        check(os.path.normpath(os.path.join(base, ini["vfs_root"])) == os.path.join(R, "vfs"), "1b read back: same folder")
        check(ini.get("shader_cache") and ini.get("edat_cache") == "build/cache/edat", f"1b edat cache {ini.get('edat_cache')}")

        # ---- 2 a hand edit survives starts and is used
        mine = os.path.join(R, "mydump")
        os.makedirs(mine)
        edit(R, "vfs_root", "mydump")
        e, ini = start(R, paths)
        check(ini["vfs_root"] == "mydump" and e["PS3_VFS_ROOT"] == mine, f"2 hand-edited relative vfs_root kept and used {ini['vfs_root']}")
        e, ini = start(R, paths)
        check(ini["vfs_root"] == "mydump" and e["PS3_VFS_ROOT"] == mine, "2 ... and again on the next start")
        edit(R, "edat_cache", os.path.join(out_dump, "edat"))
        e, ini = start(R, paths)
        check(ini["edat_cache"] == os.path.join(out_dump, "edat") and e["PS3_EDAT_CACHE"] == ini["edat_cache"],
              "2 a hand-edited absolute path counts too")
        edit(R, "edat_cache", "build/cache/edat")   # back to the launcher's own value: the launcher owns it again
        start(R, paths)
        edit(R, "vfs_root", "nowhere")
        e, ini = start(R, paths)
        check(ini["vfs_root"] == "vfs" and e["PS3_VFS_ROOT"] == os.path.join(R, "vfs"), "2 a folder that does not exist is not used")
        edit(R, "vfs_root", "mydump")
        start(R, paths)   # leave one hand edit in place for the move

        # ---- 3 paths.json relative, a moved install needs no rebuild
        j = la.paths_rel({"dump": dump, "update": upd, "eboot": eb, "facts": {}}, R)
        check(j["dump"] == "game/disc" and j["eboot"] == "game/EBOOT.elf" and j["update"] == upd, f"3 paths_rel {j}")
        real_normcase = os.path.normcase   # Windows' normcase lowercases: the relative path keeps its case anyway
        os.path.normcase = str.lower
        try:
            jc = la.paths_rel({"eboot": eb}, R)
        finally:
            os.path.normcase = real_normcase
        check(jc["eboot"] == "game/EBOOT.elf", f"3 paths_rel keeps the case under a case-folding normcase {jc}")
        write(os.path.join(R, "build", "paths.json"), json.dumps(j))
        ctx0 = fr.Ctx(root=R, build=os.path.join(R, "build"), dump=dump, update=upd, eboot=eb)
        write(os.path.join(R, "build", "build-stamp.json"),
              json.dumps({"parts": ctx0.parts(), "eboot_stat": fr.eboot_stat(eb), "root": R}))
        write(os.path.join(R, "build", "kz3.exe"), "x")
        check(la.decide(R)[0] == "run", f"3 decide in the build folder: {la.decide(R)}")
        B = os.path.join(tmp, "B", "Renamed Install")
        os.makedirs(os.path.dirname(B))
        shutil.move(R, B)
        act, why = la.decide(B)
        check(act == "run", f"3 moved install, no cmake cache: no rebuild: {act} {why}")
        pj = json.load(open(os.path.join(B, "build", "paths.json")))
        check(pj["dump"] == "game/disc", "3 paths.json still relative after the move")
        check(json.load(open(os.path.join(B, "build", "build-stamp.json")))["root"] == B, "3 the stamp records the new folder")
        pa = la.load_json(os.path.join(B, "build", "paths.json"))
        check(pa["dump"] == os.path.join(B, "game", "disc"), "3 load_json expands against the new folder")
        e, ini = start(B, pa)
        check(ini["vfs_root"] == "mydump" and e["PS3_VFS_ROOT"] == os.path.join(B, "mydump"), "3 the hand edit follows the move")
        check(ini["edat_cache"] == "build/cache/edat" and e["PS3_EDAT_CACHE"].startswith(B), "3 written values follow the move")
        # a real trigger stays: a cmake cache made elsewhere
        write(os.path.join(B, "build", "obj", "CMakeCache.txt"), f"CMAKE_HOME_DIRECTORY:INTERNAL={R}/port/kz3\n")
        check(la.decide(B)[0] == "rebuild", "3 a cmake cache from the old folder still forces the rebuild")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_relative_paths:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
