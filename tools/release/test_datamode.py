#!/usr/bin/env python3
"""Host test for launch.py's data and cache folders (pass 32): data_dir / cache_dir portable vs not, the roaming-cache
migration, switching portable <-> %APPDATA% (verify before remove; a failed copy leaves the old data and the old mode),
--open-data, and pack_zip.zip_name's '/'. Synthetic folders only. Exit 0 = PASS.

    python3 tools/release/test_datamode.py
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402
import pack_zip as pz  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


class UI:
    def __init__(self, answers=()):
        self.answers, self.asked, self.said, self.opened = list(answers), [], [], []

    def ask(self, t):
        self.asked.append(t)
        return self.answers.pop(0)

    def info(self, t):
        self.said.append(t)

    def open_folder(self, p):
        self.opened.append(p)

    def open_url(self, u):
        pass


def main():
    # The environment dict stands for APPDATA / LOCALAPPDATA; on Windows the real known folders would win over it
    # (launch.user_folder, by design), so they are switched off for this test (test_launch section 12 covers them).
    la.known_folder = lambda name: ""
    tmp = tempfile.mkdtemp(prefix="dm-")
    try:
        root, ad, lad = (os.path.join(tmp, n) for n in ("root", "Roaming", "Local"))
        env = {"APPDATA": ad, "LOCALAPPDATA": lad}
        S = la.SAVE_FOLDER
        # 1. folders
        check(la.data_dir(root, True, env) == root and la.data_dir(root, False, env) == os.path.join(ad, S), "1 data_dir")
        check(la.cache_dir(root, True, env) == root and la.cache_dir(root, False, env) == os.path.join(lad, S), "1 cache_dir")
        check(la.cache_dir(root, False, {"APPDATA": ad}) == os.path.join(ad, S), "1 no LOCALAPPDATA -> data folder")
        env_paths = la.game_env(root, {"dump": "d", "eboot": "e"}, False, env)[3]
        check(env_paths["RSX_SHADER_CACHE_DIR"] == os.path.join(lad, S, "cache", "shaders"), "1 shader cache is local")
        env_paths = la.game_env(root, {"dump": "d", "eboot": "e"}, True, env)[3]
        check(env_paths["RSX_SHADER_CACHE_DIR"] == os.path.join(root, "cache", "shaders"), "1 portable shader cache")
        # 2. roaming cache -> local, once
        write(os.path.join(ad, S, "cache", "shaders", "pso_keys.bin"), "pso")
        write(os.path.join(ad, S, "keybinds.ini"), "keys")
        msg = la.migrate_caches(env)
        check(read(os.path.join(lad, S, "cache", "shaders", "pso_keys.bin")) == "pso"
              and not os.path.exists(os.path.join(ad, S, "cache")) and os.path.isfile(os.path.join(ad, S, "keybinds.ini"))
              and "moved" in msg, f"2 cache moved, saves stay: {msg}")
        check(la.migrate_caches(env) == "" and la.migrate_caches({}) == "", "2 second start: nothing")
        # 3. switch to portable and back, with data
        os.makedirs(root)
        fr.write_install_ini(root, False, False)
        write(os.path.join(ad, S, "gamedata", "SAVE", "PARAM.SFO"), "save")
        ok, m = la.switch_data(root, True, env)
        check(ok and la.read_ini(root)["portable"] and read(os.path.join(root, "gamedata", "SAVE", "PARAM.SFO")) == "save"
              and read(os.path.join(root, "keybinds.ini")) == "keys" and read(os.path.join(root, "cache", "shaders", "pso_keys.bin")) == "pso"
              and not os.path.exists(os.path.join(ad, S, "gamedata")) and not os.path.exists(os.path.join(lad, S, "cache")), f"3 to portable: {m}")
        check(not la.switch_data(root, True, env)[0], "3 already portable")
        ok, m = la.switch_data(root, False, env)
        check(ok and not la.read_ini(root)["portable"] and read(os.path.join(ad, S, "gamedata", "SAVE", "PARAM.SFO")) == "save"
              and os.path.isfile(os.path.join(lad, S, "cache", "shaders", "pso_keys.bin"))
              and not os.path.exists(os.path.join(root, "gamedata")), f"3 round trip back: {m}")
        # 4. a failed copy leaves everything, in either direction
        import shutil as sh
        real = sh.copytree

        def boom(a, b, *x, **k):
            real(a, b, *x, **k)
            raise OSError("disk full")
        sh.copytree = boom
        try:
            ok, m = la.switch_data(root, True, env)
        finally:
            sh.copytree = real
        left = [n for n in os.listdir(root) if n.endswith(".copying") or n in ("gamedata", "keybinds.ini")]
        check(not ok and not la.read_ini(root)["portable"] and read(os.path.join(ad, S, "gamedata", "SAVE", "PARAM.SFO")) == "save"
              and not left and "unchanged" in m, f"4 failed copy: old data and mode intact, no partial copy {left} {m}")
        # a copy that silently differs is caught by the check
        ok, m = la.move_entries(("gamedata",), os.path.join(ad, S), os.path.join(tmp, "x"), copy=lambda a, b: os.makedirs(b))
        check(not ok and os.path.isdir(os.path.join(ad, S, "gamedata")) and not os.path.exists(os.path.join(tmp, "x", "gamedata"))
              and not os.path.exists(os.path.join(tmp, "x", "gamedata.copying")), f"4 mismatching copy rejected: {m}")
        # an existing entry at the target is never overwritten
        write(os.path.join(root, "keybinds.ini"), "other keys")
        ok, m = la.switch_data(root, True, env)
        check(not ok and read(os.path.join(root, "keybinds.ini")) == "other keys" and read(os.path.join(ad, S, "keybinds.ini")) == "keys"
              and not la.read_ini(root)["portable"], f"4 clash refused: {m}")
        os.remove(os.path.join(root, "keybinds.ini"))
        # 5. main(): --open-data and --switch-data (No / Yes)
        ui = UI()
        la.main(["--root", root, "--open-data"], ui=ui, environ=env)
        check(ui.opened == [os.path.join(ad, S)], "5 --open-data opens %APPDATA% folder")
        ui = UI([False])
        la.main(["--root", root, "--switch-data"], ui=ui, environ=env)
        check(not la.read_ini(root)["portable"] and os.path.isdir(os.path.join(ad, S, "gamedata")) and "Close the game" in ui.asked[0], "5 No moves nothing")
        ui = UI([True])
        rc = la.main(["--root", root, "--switch-data"], ui=ui, environ=env)
        check(rc == 0 and la.read_ini(root)["portable"] and os.path.isfile(os.path.join(root, "gamedata", "SAVE", "PARAM.SFO"))
              and ui.said and ui.said[0].startswith("Done"), "5 Yes switches")
        ui = UI()
        la.main(["--root", root, "--open-data"], ui=ui, environ=env)
        check(ui.opened == [root], "5 portable: opens the install folder")
        # 6. zip names
        check(pz.zip_name("Top", "a" + os.sep + "b.txt") == "Top/a/b.txt" and pz.zip_name("Top", "a\\b.txt") == "Top/a/b.txt",
              "6 zip_name converts os.sep and backslashes")
        # 7. precedence: flag > portable.txt > ini > default; rule strings; --switch-data keeps the file in step
        r7 = os.path.join(tmp, "r7")
        os.makedirs(r7)
        ini7 = os.path.join(r7, "kz3-install.ini")
        marker = os.path.join(r7, "portable.txt")
        check(la.data_mode(r7) == (False, "default"), "7 default is AppData")
        write(ini7, "[install]\nportable=1\n")
        check(la.read_ini(r7)["portable"] and la.read_ini(r7)["rule"] == "kz3-install.ini", "7 ini=1")
        write(ini7, "[install]\nportable=0\n")
        check(la.read_ini(r7)["portable"] is False and la.read_ini(r7)["rule"] == "kz3-install.ini", "7 ini=0")
        write(marker, "ignored content")
        check(la.read_ini(r7) == {**la.read_ini(r7), "portable": True, "rule": "portable.txt"}, "7 portable.txt beats ini=0")
        check(la.read_ini(r7, "appdata")["portable"] is False and la.read_ini(r7, "appdata")["rule"] == "--appdata flag",
              "7 --appdata beats portable.txt")
        os.remove(marker)
        write(ini7, "[install]\nportable=1\n")
        check(la.read_ini(r7, "appdata")["portable"] is False, "7 --appdata beats ini=1")
        check(la.read_ini(r7, "portable") == {**la.read_ini(r7, "portable"), "portable": True, "rule": "--portable flag"},
              "7 --portable rule")
        check(la.data_line(True, "portable.txt") == "data: portable (portable.txt)"
              and la.data_line(False, "default") == "data: AppData (default)", "7 data_line")
        # the flags change no file, and main logs / shows the rule
        write(ini7, "[install]\nportable=0\n")
        ui7 = UI()
        la.main(["--root", r7, "--open-data", "--portable"], ui=ui7, environ=env)
        check(ui7.opened == [r7] and ui7.said and ui7.said[0].startswith("data: portable (--portable flag)"), "7 open-data names rule")
        check(read(ini7) == "[install]\nportable=0\n" and not os.path.exists(marker), "7 flag changed no file")
        la.main(["--root", r7, "--open-data"], ui=ui7, environ=env)
        check(ui7.said[-1].startswith("data: AppData (kz3-install.ini)"), "7 open-data rule from ini")
        # --switch-data: to portable creates portable.txt, back removes it, the ini follows
        ui7 = UI([True, True])
        check(la.main(["--root", r7, "--switch-data"], ui=ui7, environ=env) == 0 and os.path.isfile(marker)
              and la.read_ini(r7)["portable"] and "portable = 1" in read(ini7), "7 switch to portable creates portable.txt")
        check(la.main(["--root", r7, "--switch-data"], ui=ui7, environ=env) == 0 and not os.path.exists(marker)
              and not la.read_ini(r7)["portable"] and "portable = 0" in read(ini7), "7 switch back removes portable.txt")
        # portable.txt without the ini change (user-dropped file): switching goes to AppData and removes it
        write(marker, "")
        ui7 = UI([True])
        check(la.main(["--root", r7, "--switch-data"], ui=ui7, environ=env) == 0 and not os.path.exists(marker)
              and not la.read_ini(r7)["portable"], "7 switch with a user-dropped portable.txt")
        real_sep = os.sep
        os.sep = "\\"
        try:
            check(pz.zip_name("Top", "a\\b") == "Top/a/b", "6 zip_name with a Windows separator")
        finally:
            os.sep = real_sep
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_datamode:", "FAIL " + ", ".join(FAILS) if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
