#!/usr/bin/env python3
"""Host test of the version block the launcher writes into build/kz3.ini at each start (pass 81):
port_version always (names.VERSION), newer_version only with check_updates on, a finished check that found a newer
release, and that version not dismissed (or just declined); otherwise the line is removed.
The four cases: check off, no newer release, newer release, dismissed. Run: python3 tools/release/test_version_block.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import launch as la  # noqa: E402
import names  # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  {extra}" if extra and not cond else ""))
    if not cond:
        fails.append(name)


def has_line(text):
    """a newer_version = ... line (the file's header comment names the key too)"""
    return any(ln.strip().lower().startswith("newer_version") for ln in text.splitlines())


def result_file(d, line):
    p = os.path.join(d, "version-check.txt")
    with open(p, "w", encoding="utf-8") as f:
        f.write(line + "\n")
    return p


def ini_after_start(root, paths, check_updates, result_line, dismissed="", declined=False):
    """what main() does before writing kz3.ini, then the file's keys"""
    logs = os.path.join(root, "logs")
    os.makedirs(logs, exist_ok=True)
    p = result_file(logs, result_line) if result_line else os.path.join(logs, "none.txt")
    found = la.newer_from_result(p, dismissed)
    newer = la.newer_version_for(check_updates, found, declined)
    path = la.write_game_ini(root, la.game_ini(root, paths, True, {}, newer=newer))
    return la.read_game_ini(path), open(path, encoding="utf-8", errors="replace").read()


def main():
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "build"))
        paths = {"dump": os.path.join(root, "d"), "eboot": "x"}
        newer = "VERSION status=newer current=%s latest=9.9.9 url=https://example.invalid/r" % names.VERSION
        none = "VERSION status=current current=%s latest=%s url=-" % (names.VERSION, names.VERSION)

        # start 1: newer release found, check on -> both keys
        k, _ = ini_after_start(root, paths, True, newer)
        check("newer: port_version", k.get("port_version") == names.VERSION, str(k.get("port_version")))
        check("newer: newer_version", k.get("newer_version") == "9.9.9", str(k.get("newer_version")))
        # start 2: check turned off, the old result file is still there -> newer_version removed again
        k, text = ini_after_start(root, paths, False, newer)
        check("off: port_version stays", k.get("port_version") == names.VERSION)
        check("off: newer_version removed", "newer_version" not in k and not has_line(text), text[-200:])
        # start 3: check on, finished check found nothing newer
        la.write_game_ini(root, la.game_ini(root, paths, True, {}, newer="9.9.9"))
        k, text = ini_after_start(root, paths, True, none)
        check("no newer: newer_version removed", "newer_version" not in k and not has_line(text))
        check("no newer: port_version", k.get("port_version") == names.VERSION)
        # start 4: found, but the player dismissed exactly that version earlier
        la.write_game_ini(root, la.game_ini(root, paths, True, {}, newer="9.9.9"))
        k, text = ini_after_start(root, paths, True, newer, dismissed="9.9.9")
        check("dismissed: newer_version removed", "newer_version" not in k and not has_line(text))
        # a different, later version is not covered by the dismissal
        k, _ = ini_after_start(root, paths, True, newer, dismissed="9.9.8")
        check("dismissed an older one: shown", k.get("newer_version") == "9.9.9")
        # the player just answered No at this start's prompt
        k, text = ini_after_start(root, paths, True, newer, declined=True)
        check("declined now: newer_version removed", "newer_version" not in k)
        # a hand-added key survives, and no result file at all means no newer_version
        la.set_game_ini_key(root, "discord_presence", "1")
        k, _ = ini_after_start(root, paths, True, "")
        check("no result file: no newer_version, hand key kept",
              "newer_version" not in k and k.get("discord_presence") == "1", str(k))
    print("FAIL: " + ", ".join(fails) if fails else "all version-block checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
