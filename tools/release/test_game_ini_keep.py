#!/usr/bin/env python3
"""Host test (release pass 61): kz3.ini keeps every key the player added (launch.write_game_ini, also through
firstrun.write_game_ini), launcher-owned keys still update, "Discord status on or off" (--discord-status) flips
discord_presence and reports the state, and installer/kz3.iss has the Start-menu icon line. No game data, no ISCC."""
import os
import re
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

ISS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


class UI:
    def __init__(self, yes):
        self.yes, self.said = yes, []

    def ask(self, t):
        self.said.append(t)
        return self.yes

    def info(self, t):
        self.said.append(t)


def mkroot(tmp, name):
    root = os.path.join(tmp, name)
    os.makedirs(os.path.join(root, "build"))
    return root, os.path.join(root, "build", "kz3.ini")


HAND = ("; my notes\n[game]\nvfs_root = C:\\old\nlang = fr\nmy_game_key = 7\n\n[display]\nwindow_size = 1080p\n"
        "[discord]\ndiscord_presence = 1\ndiscord_client_id = 123\n[other]\nunknown_key = \"x y\"\n")

with tempfile.TemporaryDirectory() as tmp:
    # launcher start: game_ini + write_game_ini
    root, p = mkroot(tmp, "a")
    open(p, "w", encoding="utf-8").write(HAND)
    paths = {"dump": os.path.join(tmp, "dump"), "update": os.path.join(tmp, "upd"), "eboot": os.path.join(tmp, "EBOOT.elf")}
    la.write_game_ini(root, la.game_ini(root, paths, True, {}))
    ex = la.extra_game_ini(p)
    got = {k: (s, line) for s, k, line in ex}
    check(got.get("discord_presence") == ("discord", "discord_presence = 1"), f"1 discord_presence = 1 kept in [discord] {got.get('discord_presence')}")
    check(got.get("discord_client_id", ("", ""))[1] == "discord_client_id = 123" and got.get("my_game_key", ("", ""))[0] == "game", "1 other keys kept, in their sections")
    check(got.get("unknown_key") == ("other", 'unknown_key = "x y"'), "1 unknown key in another section kept unchanged")
    gi = la.read_game_ini(p)
    check(gi.get("vfs_root") != "C:\\old" and gi.get("vfs_root", "").endswith("vfs"), f"1 owned vfs_root updated {gi.get('vfs_root')}")
    check(gi.get("lang") == "fr" and gi.get("window_size") == "1080p", "1 player choices lang/window_size still kept")
    check("[game]" in open(p).read() and open(p).read().count("[game]") == 1, "1 [game] written once")
    before = open(p, "rb").read()
    la.write_game_ini(root, la.game_ini(root, paths, True, {}))
    check(open(p, "rb").read() == before, "1 a second start changes nothing (stable)")

    # firstrun write
    root2, p2 = mkroot(tmp, "b")
    open(p2, "w", encoding="utf-8").write(HAND)
    c = types.SimpleNamespace(root=root2, build=os.path.join(root2, "build"), dump=paths["dump"], update=paths["update"],
                              eboot=paths["eboot"], b=lambda *a: os.path.join(root2, "build", *a))
    os.makedirs(os.path.join(root2, ".git"))
    fr.write_game_ini(c)
    g2 = {k: line for _, k, line in la.extra_game_ini(p2)}
    check(g2.get("discord_presence") == "discord_presence = 1" and g2.get("unknown_key") == 'unknown_key = "x y"', f"2 firstrun write keeps added keys {sorted(g2)}")
    check(la.read_game_ini(p2).get("vfs_root") != "C:\\old", "2 firstrun write updates owned keys")

    # toggle
    root3, p3 = mkroot(tmp, "c")
    open(p3, "w", encoding="utf-8").write("[game]\nvfs_root = D:\\x\nmy_game_key = 7\n[other]\nunknown_key = 1\n")
    ui = UI(True)
    la.main(["--root", root3, "--discord-status"], ui=ui, environ={})
    gi = la.read_game_ini(p3)
    check(gi.get("discord_presence") == "1" and "Discord status is off" in ui.said[0] and "Turn it on?" in ui.said[0]
          and "Discord app must be running" in ui.said[0] and ui.said[-1].startswith("Discord status is on."), f"3 switch on: {ui.said[-1]!r}")
    check(gi.get("vfs_root") == "D:\\x" and gi.get("my_game_key") == "7" and gi.get("unknown_key") == "1", "3 toggle keeps the other keys")
    ui = UI(False)
    la.main(["--root", root3, "--discord-status"], ui=ui, environ={})
    check(len(ui.said) == 1 and "Discord status is on" in ui.said[0] and la.read_game_ini(p3).get("discord_presence") == "1", "3 No changes nothing, state reported")
    ui = UI(True)
    la.main(["--root", root3, "--discord-status"], ui=ui, environ={})
    txt = open(p3).read()
    check(la.read_game_ini(p3).get("discord_presence") == "0" and txt.count("discord_presence") == 1 and ui.said[-1].startswith("Discord status is off."),
          "3 switch off rewrites the one line")
    root4, p4 = mkroot(tmp, "d")
    ui = UI(True)
    la.main(["--root", root4, "--discord-status"], ui=ui, environ={})
    check(la.read_game_ini(p4).get("discord_presence") == "1", "3 works before kz3.ini exists")

t = open(ISS, encoding="utf-8").read()
check(re.search(r'^Name: "\{group\}\\Discord status on or off"; Filename: "\{app\}\\\{#LauncherExe\}"; Parameters: "--discord-status"; WorkingDir: "\{app\}"\s*$', t, re.M),
      "4 iss: Start-menu entry 'Discord status on or off' runs the launcher with --discord-status")
print("test_game_ini_keep:", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)
